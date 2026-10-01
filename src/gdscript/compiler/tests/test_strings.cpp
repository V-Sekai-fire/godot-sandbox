// String concatenation folding, ECALL_STRING_SIZE, ECALL_STRING_AT, and `for c in s`.
#include "../codegen.h"
#include "../compiler_exception.h"
#include "../globals.h"
#include "../ir_optimizer.h"
#include "../ir_verifier.h"
#include "../lexer.h"
#include "../parser.h"
#include "../riscv_codegen.h"
#include "../syscall_numbers.h"
#include "witness/doctest.h"
#include <iostream>
#include <string>

using namespace gdscript;

// -= Helpers =-

static IRProgram compile_to_ir(const std::string &source, bool optimize = false) {
	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();
	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);
	if (optimize) {
		IROptimizer optimizer;
		optimizer.optimize(ir);
	}
	return ir;
}

static const IRFunction &find_function(const IRProgram &ir, const std::string &name) {
	for (const auto &func : ir.functions) {
		if (func.name == name) {
			return func;
		}
	}
	throw std::runtime_error("Function not found: " + name);
}

static int count_opcode(const IRFunction &func, IROpcode opcode) {
	int count = 0;
	for (const auto &instr : func.instructions) {
		if (instr.opcode == opcode) {
			count++;
		}
	}
	return count;
}

static int count_vcalls(const IRProgram &ir, const IRFunction &func, const std::string &method) {
	int count = 0;
	for (const auto &instr : func.instructions) {
		if (instr.opcode == IROpcode::VCALL && instr.operands.size() >= 3 &&
			ir.strings[instr.operands[2].string_id] == method) {
			count++;
		}
	}
	return count;
}

static int count_syscalls(const IRFunction &func, int64_t number) {
	int count = 0;
	for (const auto &instr : func.instructions) {
		if (instr.opcode == IROpcode::CALL_SYSCALL && instr.operands.size() >= 2 &&
			instr.operands[1].immediate() == number) {
			count++;
		}
	}
	return count;
}

static std::vector<int> str_call_arities(const IRFunction &func) {
	std::vector<int> arities;
	for (const auto &instr : func.instructions) {
		if (instr.opcode == IROpcode::GLOBAL_CALL && instr.operands.size() >= 4 &&
			instr.operands[1].immediate() == static_cast<int64_t>(GlobalFn::STR)) {
			arities.push_back(static_cast<int>(instr.operands[3].immediate()));
		}
	}
	return arities;
}

// Full pipeline through the RISC-V backend.
static void compile_to_machine_code(const std::string &source) {
	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();
	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);
	IROptimizer optimizer;
	optimizer.optimize(ir);
	ir_verify(ir, "the optimizer");
	RISCVCodeGen backend;
	const std::vector<uint8_t> code = backend.generate(ir);
	REQUIRE(!code.empty());
}

// -= Tests =-

TEST_CASE("concatenation folds into str") {
	const std::string source =
			"func literal(i):\n"
			"\treturn \"n=\" + str(i)\n"
			"\n"
			"func both(i, j):\n"
			"\treturn str(i) + str(j)\n"
			"\n"
			"func leading(i, s : String):\n"
			"\treturn str(i) + s\n";

	const IRProgram ir = compile_to_ir(source);

	const IRFunction &literal = find_function(ir, "literal");
	REQUIRE(str_call_arities(literal) == std::vector<int>{ 2 });
	REQUIRE(count_opcode(literal, IROpcode::ADD) == 0);

	const IRFunction &both = find_function(ir, "both");
	REQUIRE(str_call_arities(both) == std::vector<int>{ 2 });
	REQUIRE(count_opcode(both, IROpcode::ADD) == 0);

	const IRFunction &leading = find_function(ir, "leading");
	REQUIRE(str_call_arities(leading) == std::vector<int>{ 2 });
	REQUIRE(count_opcode(leading, IROpcode::ADD) == 0);

	compile_to_machine_code(source);
}

TEST_CASE("a chain folds into one call") {
	const std::string source =
			"func test(i, j, k):\n"
			"\treturn \"a\" + str(i) + \"b\" + str(j) + str(k)\n";

	const IRProgram ir = compile_to_ir(source);
	const IRFunction &test = find_function(ir, "test");

	REQUIRE(str_call_arities(test) == std::vector<int>{ 5 });
	REQUIRE(count_opcode(test, IROpcode::ADD) == 0);

	compile_to_machine_code(source);
}

TEST_CASE("two plain strings keep the add") {
	const std::string source =
			"func typed(a : String, b : String):\n"
			"\treturn a + b\n"
			"\n"
			"func literals():\n"
			"\tvar a = \"x\"\n"
			"\tvar b = \"y\"\n"
			"\treturn a + b\n";

	const IRProgram ir = compile_to_ir(source);

	const IRFunction &typed = find_function(ir, "typed");
	REQUIRE(str_call_arities(typed).empty());
	REQUIRE(count_opcode(typed, IROpcode::ADD) == 1);

	const IRFunction &literals = find_function(ir, "literals");
	REQUIRE(str_call_arities(literals).empty());
	REQUIRE(count_opcode(literals, IROpcode::ADD) == 1);

	compile_to_machine_code(source);
}

TEST_CASE("folding stops at a non string") {
	const std::string source =
			"func untyped(i, j):\n"
			"\treturn i + str(j)\n"
			"\n"
			"func number(i : int, j):\n"
			"\treturn str(j) + i\n";

	const IRProgram ir = compile_to_ir(source);

	const IRFunction &untyped = find_function(ir, "untyped");
	REQUIRE(str_call_arities(untyped) == std::vector<int>{ 1 });
	REQUIRE(count_opcode(untyped, IROpcode::ADD) == 1);

	const IRFunction &number = find_function(ir, "number");
	REQUIRE(str_call_arities(number) == std::vector<int>{ 1 });
	REQUIRE(count_opcode(number, IROpcode::ADD) == 1);

	compile_to_machine_code(source);
}

TEST_CASE("folding does not cross control flow") {
	const std::string source =
			"func test(a, i, j):\n"
			"\treturn (str(i) if a else str(j)) + str(i)\n";

	const IRProgram ir = compile_to_ir(source);
	const IRFunction &test = find_function(ir, "test");

	const std::vector<int> arities = str_call_arities(test);
	REQUIRE(arities.size() == 3);
	for (int arity : arities) {
		REQUIRE((arity == 1 || arity == 2));
	}
	REQUIRE(count_opcode(test, IROpcode::ADD) == 0);

	compile_to_machine_code(source);
}

TEST_CASE("string length is a syscall") {
	const std::string source =
			"func declared(s : String):\n"
			"\treturn s.length()\n"
			"\n"
			"func literal():\n"
			"\tvar s = \"hello\"\n"
			"\treturn s.length()\n"
			"\n"
			"func built(i):\n"
			"\treturn (\"n=\" + str(i)).length()\n"
			"\n"
			"func concatenated(a : String, b : String):\n"
			"\treturn (a + b).length()\n";

	const IRProgram ir = compile_to_ir(source);

	for (const char *name : { "declared", "literal", "built", "concatenated" }) {
		const IRFunction &func = find_function(ir, name);
		REQUIRE(count_syscalls(func, ECALL_STRING_SIZE) == 1);
		REQUIRE(count_vcalls(ir, func, "length") == 0);
	}

	compile_to_machine_code(source);
}

TEST_CASE("unknown receiver keeps the vcall") {
	const std::string source =
			"func untyped(x):\n"
			"\treturn x.length()\n"
			"\n"
			"func an_array(a : Array):\n"
			"\treturn a.size()\n";

	const IRProgram ir = compile_to_ir(source);

	const IRFunction &untyped = find_function(ir, "untyped");
	REQUIRE(count_syscalls(untyped, ECALL_STRING_SIZE) == 0);
	REQUIRE(count_vcalls(ir, untyped, "length") == 1);

	const IRFunction &an_array = find_function(ir, "an_array");
	REQUIRE(count_syscalls(an_array, ECALL_STRING_SIZE) == 0);

	compile_to_machine_code(source);
}

TEST_CASE("subscript is a syscall") {
	const std::string source =
			"func first(s : String):\n"
			"\treturn s[0]\n"
			"\n"
			"func at(s : String, i : int):\n"
			"\treturn s[i]\n"
			"\n"
			"func last(s : String):\n"
			"\treturn s[-1]\n"
			"\n"
			"func literal():\n"
			"\treturn \"hello\"[1]\n";

	const IRProgram ir = compile_to_ir(source);

	for (const char *name : { "first", "at", "last", "literal" }) {
		const IRFunction &func = find_function(ir, name);
		REQUIRE(count_syscalls(func, ECALL_STRING_AT) == 1);
		REQUIRE(count_vcalls(ir, func, "get") == 0);
		REQUIRE(count_opcode(func, IROpcode::ARRAY_GET) == 0);
	}

	// Negative index normalised by host; no guest-side ECALL_ARRAY_SIZE needed.
	REQUIRE(count_syscalls(find_function(ir, "last"), ECALL_ARRAY_SIZE) == 0);

	compile_to_machine_code(source);
}

TEST_CASE("a non integer subscript goes through int") {
	// Non-integer index goes through int() first.
	const IRProgram ir = compile_to_ir(
			"func f(s : String):\n"
			"\treturn s[1.0]\n"
			"\n"
			"func g(s : String, i):\n"
			"\treturn s[i]\n");

	for (const char *name : { "f", "g" }) {
		const IRFunction &func = find_function(ir, name);
		REQUIRE(count_syscalls(func, ECALL_STRING_AT) == 1);
		bool converted = false;
		for (const auto &instr : func.instructions) {
			converted |= instr.opcode == IROpcode::CONVERT || instr.opcode == IROpcode::GLOBAL_CALL;
		}
		REQUIRE(converted);
	}
}

TEST_CASE("an unknown subscript uses variant get") {
	// The generic Variant operation preserves the runtime distinction between a
	// String character lookup and every other indexed/keyed lookup.
	const IRProgram ir = compile_to_ir("func f(x, i : int):\n\treturn x[i]\n");
	const IRFunction &func = find_function(ir, "f");
	REQUIRE(count_syscalls(func, ECALL_VARIANT_GET) == 1);
	REQUIRE(count_syscalls(func, ECALL_STRING_AT) == 0);
	REQUIRE(count_vcalls(ir, func, "get") == 0);
	REQUIRE(count_opcode(func, IROpcode::TYPE_TEST) == 0);

	// Known Array/Dictionary: no tag test.
	for (const char *hint : { "Array", "Dictionary" }) {
		const IRProgram known = compile_to_ir(
				std::string("func f(c : ") + hint + ", i : int):\n\treturn c[i]\n");
		REQUIRE(count_syscalls(find_function(known, "f"), ECALL_STRING_AT) == 0);
		REQUIRE(count_opcode(find_function(known, "f"), IROpcode::TYPE_TEST) == 0);
	}

	compile_to_machine_code("func f(x, i : int):\n\treturn x[i]\n");
}

TEST_CASE("walking a string") {
	const IRProgram known = compile_to_ir(
			"func f(s : String):\n"
			"\tvar n = 0\n"
			"\tfor c in s:\n"
			"\t\tn += c.length()\n"
			"\treturn n\n");
	const IRFunction &f = find_function(known, "f");
	// length() on the walk's UTF-32 code point folds to 1. Code-point-only
	// body uses the raw guest buffer: no scoped Strings, no release per refill.
	REQUIRE(count_syscalls(f, ECALL_STRING_CODEPOINT_BATCH) == 1);
	REQUIRE(count_syscalls(f, ECALL_STRING_BATCH) == 0);
	REQUIRE(count_syscalls(f, ECALL_STRING_SIZE) == 0);
	REQUIRE(count_syscalls(f, ECALL_STRING_AT) == 0);
	REQUIRE(count_opcode(f, IROpcode::CODEPOINT_GET) == 1);

	const IRProgram reassigned = compile_to_ir(
			"func f(s : String):\n"
			"\tvar n = 0\n"
			"\tfor c in s:\n"
			"\t\tc = \"two\"\n"
			"\t\tn += c.length()\n"
			"\treturn n\n");
	const IRFunction &reassigned_function = find_function(reassigned, "f");
	REQUIRE(count_syscalls(reassigned_function, ECALL_STRING_SIZE) == 1);
	REQUIRE(count_syscalls(reassigned_function, ECALL_STRING_BATCH) == 1);
	REQUIRE(count_syscalls(reassigned_function, ECALL_STRING_CODEPOINT_BATCH) == 0);

	// A body runs many times: the assignment below the use still precedes it on
	// the next pass, so the fold has to go for the whole loop, not from the
	// assignment onwards.
	const IRProgram reassigned_in_a_loop = compile_to_ir(
			"func f(s : String, k : int):\n"
			"\tvar n = 0\n"
			"\tfor c in s:\n"
			"\t\tvar d = c\n"
			"\t\tvar i = 0\n"
			"\t\twhile i < k:\n"
			"\t\t\tn += d.length()\n"
			"\t\t\td = d + \"x\"\n"
			"\t\t\ti += 1\n"
			"\treturn n\n");
	REQUIRE(count_syscalls(find_function(reassigned_in_a_loop, "f"), ECALL_STRING_SIZE) == 1);
	REQUIRE(count_opcode(f, IROpcode::MAKE_SCOPED) == 0);
	REQUIRE(count_syscalls(f, ECALL_ARRAY_SIZE) == 0);
	REQUIRE(count_syscalls(f, ECALL_ARRAY_AT) == 0);
	REQUIRE(count_vcalls(known, f, "size") == 0);
	REQUIRE(count_vcalls(known, f, "get") == 0);
	REQUIRE(count_opcode(f, IROpcode::TYPE_TEST) == 0);
	// Also verify the frame layout and RISC-V load, not just the IR.
	compile_to_machine_code(
			"func f(s : String):\n"
			"\tvar n = 0\n"
			"\tfor c in s:\n"
			"\t\tn += c.length()\n"
			"\treturn n\n");
	const IRProgram ordinal = compile_to_ir(
			"func f(s : String):\n"
			"\tvar n = 0\n"
			"\tfor c in s:\n"
			"\t\tn += ord(c)\n"
			"\treturn n\n");
	const IRFunction &ordinal_function = find_function(ordinal, "f");
	REQUIRE(count_syscalls(ordinal_function, ECALL_STRING_CODEPOINT_BATCH) == 1);
	REQUIRE(count_opcode(ordinal_function, IROpcode::GLOBAL_CALL) == 0);
	compile_to_machine_code(
			"func f(s : String):\n"
			"\tvar n = 0\n"
			"\tfor c in s:\n"
			"\t\tn += ord(c)\n"
			"\treturn n\n");

	// Literal String also qualifies. Unused walk variable needs no boxing.
	const IRProgram literal = compile_to_ir(
			"func f():\n"
			"\tfor c in \"hello\":\n"
			"\t\tpass\n");
	REQUIRE(count_syscalls(find_function(literal, "f"), ECALL_STRING_CODEPOINT_BATCH) == 1);

	// Untyped: all four arms present (int, Array, String, VCALL).
	const IRProgram untyped = compile_to_ir(
			"func f(it):\n"
			"\tvar n = 0\n"
			"\tfor c in it:\n"
			"\t\tn += 1\n"
			"\treturn n\n");
	const IRFunction &u = find_function(untyped, "f");
	REQUIRE(count_syscalls(u, ECALL_STRING_SIZE) == 1);
	REQUIRE(count_syscalls(u, ECALL_STRING_AT) == 1);
	REQUIRE(count_syscalls(u, ECALL_ARRAY_SIZE) == 1);
	REQUIRE(count_syscalls(u, ECALL_ARRAY_AT) == 1);
	REQUIRE(count_vcalls(untyped, u, "size") == 1);
	REQUIRE(count_vcalls(untyped, u, "get") == 1);

	compile_to_machine_code(
			"func f(it, s : String):\n"
			"\tvar out = []\n"
			"\tfor c in s:\n"
			"\t\tout.append(c)\n"
			"\tfor c in it:\n"
			"\t\tout.append(c)\n"
			"\treturn out\n");
}

// `for i in 2.5`: float counter, counts 0.0, 1.0, 2.0.
TEST_CASE("iterating a float") {
	// Known float: counted loop with float compare and step, no syscall.
	const IRProgram known = compile_to_ir(
			"func f():\n"
			"\tvar n = 0\n"
			"\tfor i in 2.5:\n"
			"\t\tn += 1\n"
			"\treturn n\n"
			"\n"
			"func g(x : float):\n"
			"\tvar n = 0\n"
			"\tfor i in x:\n"
			"\t\tn += 1\n"
			"\treturn n\n");

	for (const char *name : { "f", "g" }) {
		const IRFunction &func = find_function(known, name);
		REQUIRE(count_opcode(func, IROpcode::CALL_SYSCALL) == 0);
		REQUIRE(count_opcode(func, IROpcode::TYPE_TEST) == 0);
		bool float_bound = false;
		bool float_step = false;
		for (const auto &instr : func.instructions) {
			const bool compare = ir_has_effect(instr.opcode, IR_COMPARISON) ||
					ir_has_effect(instr.opcode, IR_FUSED_BRANCH);
			if (compare && instr.type_hint == Variant::FLOAT) {
				float_bound = true;
			}
			if (instr.opcode == IROpcode::ADD && instr.type_hint == Variant::FLOAT) {
				float_step = true;
			}
		}
		REQUIRE((float_bound && "the bound compare was not a float compare"));
		REQUIRE((float_step && "the step was not a float add"));
	}

	// Untyped: float detected via TYPE_TEST, ceil'd into the integer arm.
	const IRProgram untyped = compile_to_ir("func f(it):\n\tfor i in it:\n\t\tpass\n");
	const IRFunction &u = find_function(untyped, "f");
	int float_tests = 0;
	for (const auto &instr : u.instructions) {
		if (instr.opcode == IROpcode::TYPE_TEST &&
			instr.operands.at(2).immediate() == Variant::FLOAT) {
			float_tests++;
		}
	}
	REQUIRE(float_tests == 1);
	// ceil() hoisted outside the loop.
	int ceil_calls = 0;
	size_t loop_header = u.instructions.size();
	for (size_t i = 0; i < u.instructions.size(); i++) {
		if (u.instructions[i].opcode == IROpcode::LABEL &&
			untyped.strings[u.instructions[i].operands[0].string_id].find("for_loop") !=
					std::string::npos) {
			loop_header = std::min(loop_header, i);
		}
		if (u.instructions[i].opcode == IROpcode::GLOBAL_CALL) {
			ceil_calls++;
			REQUIRE((i < loop_header || loop_header == u.instructions.size()));
		}
	}
	REQUIRE(ceil_calls == 1);

	compile_to_machine_code(
			"func f(it, x : float):\n"
			"\tvar out = []\n"
			"\tfor i in 2.5:\n"
			"\t\tout.append(i)\n"
			"\tfor i in x:\n"
			"\t\tout.append(i)\n"
			"\tfor i in it:\n"
			"\t\tout.append(i)\n"
			"\treturn out\n");
}

TEST_CASE("writing a character is refused") {
	// String is a scoped handle; per-character write would alias.
	bool refused = false;
	try {
		compile_to_ir("func f(s : String):\n\ts[0] = \"x\"\n");
	} catch (const CompilerException &) {
		refused = true;
	}
	REQUIRE(refused);
}

TEST_CASE("string ops survive the optimizer") {
	const std::string source =
			"func test(i, n : int):\n"
			"\tvar acc : int = 0\n"
			"\tvar k : int = 0\n"
			"\twhile k < n:\n"
			"\t\tvar s : String = \"value \" + str(k)\n"
			"\t\tacc += s.length()\n"
			"\t\tk += 1\n"
			"\treturn acc\n";

	const IRProgram ir = compile_to_ir(source, true);
	const IRFunction &test = find_function(ir, "test");

	REQUIRE(str_call_arities(test) == std::vector<int>{ 2 });
	REQUIRE(count_syscalls(test, ECALL_STRING_SIZE) == 1);
	REQUIRE(count_vcalls(ir, test, "length") == 0);
	// Every remaining ADD is typed-int (no VEVAL fallback).
	for (const auto &instr : test.instructions) {
		if (instr.opcode == IROpcode::ADD) {
			REQUIRE(instr.type_hint == Variant::INT);
		}
	}

	compile_to_machine_code(source);
}

// ---------------------------------------------------------------------------
// Properties, and the falsifiability check that keeps them honest.
// ---------------------------------------------------------------------------

#include "../lexer.h"
#include "property_support.h"

namespace {

// Printable characters that need no escaping, so the literal in the source is
// the string the lexer has to hand back.
std::string generate_plain_text(witness::RNG &rng, const witness::Level &level) {
	const uint32_t length = rng.uint_range(0, std::max<uint32_t>(1, uint32_t(level.fin_bound) / 16));
	std::string text;
	for (uint32_t i = 0; i < length; i++) {
		char c = char(rng.int_range(32, 126));
		while (c == '"' || c == '\\') {
			c = char(rng.int_range(32, 126));
		}
		text.push_back(c);
	}
	return text;
}

// What the lexer says the literal `"<text>"` contains: the decoded value, not
// the quoted source text the lexeme keeps.
std::string lexed_literal(const std::string &text) {
	Lexer lexer("var s = \"" + text + "\"\n");
	for (const Token &token : lexer.tokenize()) {
		if (token.type == TokenType::STRING) {
			return std::get<std::string>(token.value);
		}
	}
	return "\x01no string token";
}

} // namespace

TEST_CASE("property: a plain string literal is the text it was written as") {
	PROP_HOLDS(std::string, "the literal round-trips through the lexer",
			   &generate_plain_text,
			   [](const std::string &text) { return lexed_literal(text) == text; });
}

TEST_CASE("falsifiability: the text generator reaches strings past a few characters") {
	// False on purpose: a generator stuck on the empty string would pass the
	// property above without ever lexing a character.
	PROP_FALSIFIABLE(std::string, "every literal is at most three characters",
					 &generate_plain_text,
					 [](const std::string &text) { return text.size() <= 3; });
}
