#include "../codegen.h"
#include "../compiler_exception.h"
#include "../function_signature.h"
#include "../lexer.h"
#include "../parser.h"
#include "witness/doctest.h"
#include <cmath>
#include <iostream>
#include <string>

using namespace gdscript;

static IRProgram compile_to_ir(const std::string &source) {
	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();
	CodeGenerator codegen;
	return codegen.generate(program);
}

static const IRGlobalVar &global(const IRProgram &ir, const std::string &name) {
	for (const auto &entry : ir.globals) {
		if (entry.name == name) {
			return entry;
		}
	}
	throw std::runtime_error("Global not found: " + name);
}

static const FunctionSignature &find_signature(const IRProgram &ir, const std::string &name) {
	for (const auto &sig : ir.signatures) {
		if (sig.name == name) {
			return sig;
		}
	}
	throw std::runtime_error("Signature not found: " + name);
}

static const IRFunction &find_function(const IRProgram &ir, const std::string &name) {
	for (const auto &func : ir.functions) {
		if (func.name == name) {
			return func;
		}
	}
	throw std::runtime_error("Function not found: " + name);
}

static bool has_opcode(const IRFunction &func, IROpcode opcode) {
	for (const auto &instr : func.instructions) {
		if (instr.opcode == opcode) {
			return true;
		}
	}
	return false;
}

static std::string compile_error(const std::string &source) {
	try {
		compile_to_ir(source);
	} catch (const CompilerException &e) {
		return e.what();
	}
	return "";
}

TEST_CASE("an operator over constants is a constant") {
	const IRProgram ir = compile_to_ir(
			"const MASK = 1 << 3\n"
			"const HALF = 7 / 2\n"
			"const SCALED = 2.5 * 4\n"
			"const NAME = \"a\" + \"b\"\n"
			"const BOTH = true and false\n"
			"const SMALL = 3 < 4\n"
			"const CUBE = 2 ** 10\n");

	REQUIRE(global(ir, "MASK").init_type == IRGlobalVar::InitType::INT);
	REQUIRE(std::get<int64_t>(global(ir, "MASK").init_value) == 8);
	REQUIRE(std::get<int64_t>(global(ir, "HALF").init_value) == 3);
	REQUIRE(global(ir, "SCALED").init_type == IRGlobalVar::InitType::FLOAT);
	REQUIRE(std::get<double>(global(ir, "SCALED").init_value) == 10.0);
	REQUIRE(global(ir, "NAME").init_type == IRGlobalVar::InitType::STRING);
	REQUIRE(std::get<std::string>(global(ir, "NAME").init_value) == "ab");
	REQUIRE(global(ir, "BOTH").init_type == IRGlobalVar::InitType::BOOL);
	REQUIRE(std::get<bool>(global(ir, "BOTH").init_value) == false);
	REQUIRE(std::get<bool>(global(ir, "SMALL").init_value) == true);
	REQUIRE(std::get<int64_t>(global(ir, "CUBE").init_value) == 1024);

	REQUIRE(ir.global_init.instructions.empty());
}

TEST_CASE("a constant names another constant") {
	const IRProgram ir = compile_to_ir(
			"const A = 5\n"
			"const B = A * 2\n"
			"const C = B - A\n"
			"func f() -> int:\n"
			"\treturn C\n");

	REQUIRE(std::get<int64_t>(global(ir, "B").init_value) == 10);
	REQUIRE(std::get<int64_t>(global(ir, "C").init_value) == 5);
	REQUIRE(!has_opcode(find_function(ir, "f"), IROpcode::LOAD_GLOBAL));
}

TEST_CASE("engine constants fold") {
	const IRProgram ir = compile_to_ir(
			"const HALF_PI = PI / 2\n"
			"const ESCAPE = KEY_ESCAPE\n"
			"const LEFT = Side.SIDE_LEFT\n");

	REQUIRE(global(ir, "HALF_PI").init_type == IRGlobalVar::InitType::FLOAT);
	REQUIRE(std::fabs(std::get<double>(global(ir, "HALF_PI").init_value) - 1.5707963267948966) < 1e-12);
	REQUIRE(global(ir, "ESCAPE").init_type == IRGlobalVar::InitType::INT);
	REQUIRE(global(ir, "LEFT").init_type == IRGlobalVar::InitType::INT);
	REQUIRE(std::get<int64_t>(global(ir, "LEFT").init_value) == 0);
}

TEST_CASE("a ternary over constants folds") {
	const IRProgram ir = compile_to_ir(
			"const DEBUG = true\n"
			"const LIMIT = 100 if DEBUG else 10\n"
			"const EMPTY = 1 if \"\" else 2\n");

	REQUIRE(std::get<int64_t>(global(ir, "LIMIT").init_value) == 100);
	REQUIRE(std::get<int64_t>(global(ir, "EMPTY").init_value) == 2);
}

TEST_CASE("a script enum member folds into a constant") {
	const IRProgram ir = compile_to_ir(
			"enum Flags { A = 1, B = 2 }\n"
			"const BOTH = Flags.A | Flags.B\n");

	REQUIRE(std::get<int64_t>(global(ir, "BOTH").init_value) == 3);
}

TEST_CASE("a constant folds into an enum") {
	const IRProgram ir = compile_to_ir(
			"const BASE = 4\n"
			"enum Codes { A = BASE, B, C = BASE * 2 }\n"
			"func f() -> int:\n"
			"\treturn Codes.B + Codes.C\n");

	const IRFunction &func = find_function(ir, "f");
	REQUIRE(!has_opcode(func, IROpcode::LOAD_GLOBAL));

	bool saw_five = false;
	bool saw_eight = false;
	for (const auto &instr : func.instructions) {
		if (instr.opcode != IROpcode::LOAD_IMM || instr.operands.size() < 2) {
			continue;
		}
		saw_five = saw_five || instr.operands[1].immediate() == 5;
		saw_eight = saw_eight || instr.operands[1].immediate() == 8;
	}
	REQUIRE((saw_five && saw_eight));
}

TEST_CASE("a class constant may be an expression") {
	const IRProgram ir = compile_to_ir(
			"const SHIFT = 3\n"
			"class Foo:\n"
			"\tconst BITS = 1 << SHIFT\n"
			"\tconst DOUBLE = BITS * 2\n"
			"\tfunc bits() -> int:\n"
			"\t\treturn DOUBLE\n");

	const IRFunction &func = find_function(ir, "@Foo.bits");
	bool saw_sixteen = false;
	for (const auto &instr : func.instructions) {
		if (instr.opcode == IROpcode::LOAD_IMM && instr.operands.size() > 1 &&
			instr.operands[1].immediate() == 16) {
			saw_sixteen = true;
		}
	}
	REQUIRE(saw_sixteen);
}

TEST_CASE("a constant default argument reaches the host") {
	const IRProgram ir = compile_to_ir(
			"const MASK = 1 << 3\n"
			"func f(a = MASK, b = MASK + 1):\n"
			"\treturn a\n");

	const FunctionSignature &sig = find_signature(ir, "f");
	REQUIRE(sig.required_arguments == 0);
	REQUIRE(sig.parameters[0].default_kind == FunctionParameter::DefaultKind::INT);
	REQUIRE(std::get<int64_t>(sig.parameters[0].default_value) == 8);
	REQUIRE(std::get<int64_t>(sig.parameters[1].default_value) == 9);
}

TEST_CASE("computed constants index a jump table") {
	const IRProgram ir = compile_to_ir(
			"const A = 1 << 0\n"
			"const B = 1 << 1\n"
			"const C = 1 << 1 | 1\n"
			"const D = 1 << 2\n"
			"func pick(v: int) -> int:\n"
			"\tmatch v:\n"
			"\t\tA: return 10\n"
			"\t\tB: return 20\n"
			"\t\tC: return 30\n"
			"\t\tD: return 40\n"
			"\t\t_: return 0\n");

	REQUIRE(has_opcode(find_function(ir, "pick"), IROpcode::SWITCH));
}

TEST_CASE("a local shadows a constant in a pattern") {
	const IRProgram ir = compile_to_ir(
			"const A = 1\n"
			"const B = 2\n"
			"const C = 3\n"
			"const D = 4\n"
			"func pick(v: int) -> int:\n"
			"\tvar A = 9\n"
			"\tmatch v:\n"
			"\t\tA: return 10\n"
			"\t\tB: return 20\n"
			"\t\tC: return 30\n"
			"\t\tD: return 40\n"
			"\t\t_: return 0\n");

	REQUIRE(!has_opcode(find_function(ir, "pick"), IROpcode::SWITCH));
}

TEST_CASE("a run time value is still a run time value") {
	const IRProgram ir = compile_to_ir(
			"const ZERO = Vector2.ZERO\n"
			"const BAD = 1 / 0\n"
			"const NAMED = &\"walk\"\n");

	REQUIRE(global(ir, "ZERO").init_type == IRGlobalVar::InitType::RUNTIME);
	REQUIRE(global(ir, "BAD").init_type == IRGlobalVar::InitType::RUNTIME);
	REQUIRE(global(ir, "NAMED").init_type == IRGlobalVar::InitType::RUNTIME);
}

TEST_CASE("a class constant still has to fold") {
	const std::string message = compile_error(
			"class Foo:\n"
			"\tconst SIZE = Vector2.ZERO\n"
			"\tfunc f():\n"
			"\t\treturn SIZE\n");

	REQUIRE(message.find("not a compile-time value") != std::string::npos);
}

TEST_CASE("a power folds the way the host evaluates it") {
	const IRProgram ir = compile_to_ir(
			"const SMALL = 2 ** 10\n"
			"const WIDE = 7 ** 20\n"
			"const NEGATIVE = 2 ** -1\n"
			"const HUGE = 2 ** 100000000000\n"
			"const OVER = 2 ** 64\n");

	REQUIRE(std::get<int64_t>(global(ir, "SMALL").init_value) == 1024);
	REQUIRE(std::get<int64_t>(global(ir, "WIDE").init_value) == 79792266297612000LL);
	REQUIRE(std::get<int64_t>(global(ir, "NEGATIVE").init_value) == 0);
	REQUIRE(global(ir, "HUGE").init_type == IRGlobalVar::InitType::RUNTIME);
	REQUIRE(global(ir, "OVER").init_type == IRGlobalVar::InitType::RUNTIME);

	const std::string message = compile_error("enum E { A = 2 ** 100000000000 }\n");
	REQUIRE(message.find("outside the range of an integer") != std::string::npos);
}
