// Match patterns beyond a plain value.
//
// Dense-integer dispatch is tests/test_switch.cpp's subject. This file covers
// the non-value pattern kinds and the guard that can decline an arm after its
// pattern matched:
//
//   _              matches anything, binds nothing
//   var name       matches anything, binds it for the guard and the body
//   [a, b, ..]     an Array of that shape, elementwise
//   {"k": p, ..}   a Dictionary with those keys
//
// Three properties, one test each: an arm runs only when every part of its
// pattern matched; a binding is a copy, not an alias of the subject; and arms
// are tried in source order whichever lowering carried them.
//
// Container patterns need the host, so those tests assert the emitted shape --
// type test, size, element and key syscalls -- and leave execution to the Godot
// integration tests, where a real Array exists. Everything the IR interpreter
// can execute is executed here.
#include "../codegen.h"
#include "../compiler_exception.h"
#include "../ir_interpreter.h"
#include "../ir_optimizer.h"
#include "../ir_verifier.h"
#include "../lexer.h"
#include "../parser.h"
#include "../riscv_codegen.h"
#include "witness/doctest.h"
#include <iostream>
#include <string>
#include <vector>

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

// Syscalls emitted by a container pattern, counted by number. Trailing operands
// identify the Dictionary_Op of a dictionary call.
static int count_syscall(const IRFunction &func, int64_t number, int64_t op = -1) {
	int count = 0;
	for (const auto &instr : func.instructions) {
		if (instr.opcode != IROpcode::CALL_SYSCALL || instr.operands.size() < 2) {
			continue;
		}
		if (instr.operands[1].immediate() != number) {
			continue;
		}
		if (op >= 0 && (instr.operands.size() < 3 || instr.operands[2].immediate() != op)) {
			continue;
		}
		count++;
	}
	return count;
}

static int64_t call_int(const IRProgram &ir, const std::string &function,
						const std::vector<IRInterpreter::Value> &args = {}) {
	IRInterpreter interp(ir);
	return std::get<int64_t>(interp.call(function, args));
}

// Every arm must answer the same before and after the optimizer, and the IR must
// stay verifiable through the pipeline: a pattern's tests jump between arms, and
// a pass that dropped one of those labels would leave a branch into nothing.
static void check_unoptimized_and_optimized(const std::string &source,
											const std::string &function,
											const std::vector<int64_t> &inputs) {
	IRProgram plain = compile_to_ir(source, /*optimize=*/false);
	for (const auto &func : plain.functions) {
		ir_verify(func, "codegen");
	}
	IRProgram optimized = compile_to_ir(source, /*optimize=*/true);
	for (const auto &func : optimized.functions) {
		ir_verify(func, "the optimizer");
	}
	for (int64_t input : inputs) {
		const int64_t before = call_int(plain, function, { input });
		const int64_t after = call_int(optimized, function, { input });
		REQUIRE((before == after && "the optimizer changed what a match answers"));
	}
}

// The CompilerException message a source is expected to raise.
static std::string compile_error(const std::string &source) {
	try {
		compile_to_ir(source);
	} catch (const CompilerException &e) {
		return e.what();
	}
	return "";
}

// -= Bindings =-

TEST_CASE("a binding names the subject") {
	const std::string source = R"(
func classify(n):
	match n:
		0:
			return 100
		var v:
			return v * 2

func test():
	return classify(0) * 1000 + classify(7)
)";

	IRProgram ir = compile_to_ir(source);
	REQUIRE(call_int(ir, "classify", { int64_t(0) }) == 100);
	REQUIRE(call_int(ir, "classify", { int64_t(7) }) == 14);
	REQUIRE(call_int(ir, "test") == 100014);

	// A binding matches everything, so its arm is the fall-through arm: no test
	// is emitted before it.
	check_unoptimized_and_optimized(source, "classify", { 0, 7, -3 });
}

TEST_CASE("a binding is a copy") {
	// Binding and subject must not share a register: writing the name in the
	// body would otherwise change the subject, visible to later arms and to
	// everything after the match.
	const std::string source = R"(
func test():
	var subject = 5
	match subject:
		var v:
			v = 99
	return subject
)";

	REQUIRE(call_int(compile_to_ir(source), "test") == 5);
	REQUIRE(call_int(compile_to_ir(source, /*optimize=*/true), "test") == 5);
}

TEST_CASE("a binding lives only in its arm") {
	// Each arm is its own scope, so two arms may bind the same name; a shared
	// scope would make the second a redeclaration error.
	const std::string source = R"(
func pick(n):
	match n:
		1:
			return 1
		var v when v > 10:
			return v
		var v:
			return -v

func test():
	return pick(1) + pick(20) + pick(3)
)";

	IRProgram ir = compile_to_ir(source);
	REQUIRE(call_int(ir, "pick", { int64_t(20) }) == 20);
	REQUIRE(call_int(ir, "pick", { int64_t(3) }) == -3);
	REQUIRE(call_int(ir, "test") == 1 + 20 - 3);
}

// -= Guards =-

TEST_CASE("a guard can decline an arm") {
	const std::string source = R"(
func classify(n):
	match n:
		var v when v < 0:
			return 0
		var v when v == 0:
			return 1
		var v when v < 10:
			return 2
		_:
			return 3

func test():
	return classify(-5) * 1000 + classify(0) * 100 + classify(4) * 10 + classify(50)
)";

	IRProgram ir = compile_to_ir(source);
	REQUIRE(call_int(ir, "classify", { int64_t(-5) }) == 0);
	REQUIRE(call_int(ir, "classify", { int64_t(0) }) == 1);
	REQUIRE(call_int(ir, "classify", { int64_t(4) }) == 2);
	REQUIRE(call_int(ir, "classify", { int64_t(50) }) == 3);
	REQUIRE(call_int(ir, "test") == 123);

	check_unoptimized_and_optimized(source, "classify", { -5, 0, 4, 50 });
}

TEST_CASE("a guarded value pattern falls through") {
	// The same value twice, told apart by the guard. The second arm is reachable
	// only if a declining guard continues the chain instead of leaving the match.
	const std::string source = R"(
func pick(n, flag):
	match n:
		1 when flag:
			return 10
		1:
			return 20
		_:
			return 30

func test():
	return pick(1, true) + pick(1, false) + pick(2, true)
)";

	IRProgram ir = compile_to_ir(source);
	REQUIRE(call_int(ir, "test") == 60);
}

TEST_CASE("a guard disqualifies the jump table") {
	// Six dense integer patterns, the table's target case, but one arm can
	// decline and a table entry cannot test that: the whole match keeps the chain.
	const std::string source = R"(
func dispatch(op : int, flag) -> int:
	match op:
		0:
			return 100
		1:
			return 101
		2 when flag:
			return 102
		3:
			return 103
		4:
			return 104
		_:
			return -1
)";

	const IRProgram ir = compile_to_ir(source);
	const IRFunction &func = find_function(ir, "dispatch");
	REQUIRE(count_opcode(func, IROpcode::SWITCH) == 0);
	REQUIRE(count_opcode(func, IROpcode::CMP_EQ) == 5);

	// Without the guard the same six arms do become a table, so the case above
	// is about the guard, not the density.
	const std::string ungarded = R"(
func dispatch(op : int) -> int:
	match op:
		0:
			return 100
		1:
			return 101
		2:
			return 102
		3:
			return 103
		4:
			return 104
		_:
			return -1
)";
	const IRProgram ungarded_ir = compile_to_ir(ungarded);
	REQUIRE(count_opcode(find_function(ungarded_ir, "dispatch"), IROpcode::SWITCH) == 1);
}

TEST_CASE("a guarded wildcard is not the default") {
	// `_ when c` can decline, so an unmatched subject must not jump straight to
	// it: the arm below stays reachable, and a subject matching neither leaves
	// the match with the variable untouched.
	const std::string source = R"(
func pick(n, flag):
	var out = -1
	match n:
		_ when flag:
			out = 1
		2:
			out = 2
	return out

func test():
	return pick(5, true) * 100 + pick(2, false) * 10 + pick(9, false)
)";

	IRProgram ir = compile_to_ir(source);
	REQUIRE(call_int(ir, "test") == 100 + 20 - 1);
}

// -= Array patterns =-

TEST_CASE("an array pattern asks type length and elements") {
	const std::string source = R"(
func shape(v):
	match v:
		[]:
			return 0
		[1, var x]:
			return x
		[var a, var b, ..]:
			return a + b
		_:
			return -1
)";

	const IRProgram ir = compile_to_ir(source);
	const IRFunction &func = find_function(ir, "shape");

	// One type test per array pattern: a Dictionary or integer is never asked
	// for a length.
	REQUIRE(count_opcode(func, IROpcode::TYPE_TEST) == 3);
	// One length per pattern, and one element fetch per element named.
	REQUIRE(count_syscall(func, 523) == 3);
	REQUIRE(count_syscall(func, 522) == 4);
	// `..` is the one pattern of the three whose length test is not equality.
	REQUIRE(count_opcode(func, IROpcode::CMP_GTE) == 1);
}

TEST_CASE("an array pattern reaches riscv") {
	const std::string source = R"(
func shape(v):
	match v:
		[1, var x]:
			return x
		[var a, [var b, var c]]:
			return a + b + c
		{"kind": "circle", "r": var r}:
			return r
		{"kind"}:
			return 1
		{..}:
			return 2
		_:
			return 0
)";

	IRProgram ir = compile_to_ir(source, /*optimize=*/true);
	for (const auto &func : ir.functions) {
		ir_verify(func, "the optimizer");
	}
	RISCVCodeGen codegen;
	const std::vector<uint8_t> code = codegen.generate(ir);
	REQUIRE(!code.empty());
}

TEST_CASE("a container pattern a type rules out costs nothing") {
	// The subject is an integer, so no length or element is ever needed: the arm
	// is one jump, not destructuring behind a test known to fail.
	const std::string source = R"(
func f(n : int):
	match n:
		[1, 2]:
			return 1
		{"a": 1}:
			return 2
		var v:
			return v
)";

	const IRProgram ir = compile_to_ir(source);
	const IRFunction &func = find_function(ir, "f");
	REQUIRE(count_opcode(func, IROpcode::TYPE_TEST) == 0);
	REQUIRE(count_syscall(func, 523) == 0);
	REQUIRE(count_syscall(func, 522) == 0);
	REQUIRE(count_syscall(func, 524) == 0);
	REQUIRE(call_int(ir, "f", { int64_t(7) }) == 7);
}

// -= Dictionary patterns =-

TEST_CASE("a dictionary pattern asks size keys and values") {
	constexpr int64_t DICT_GET = 0;
	constexpr int64_t DICT_HAS = 3;
	constexpr int64_t DICT_GET_SIZE = 6;

	const std::string source = R"(
func shape(v):
	match v:
		{"kind": "circle", "r": var r}:
			return r
		{"kind"}:
			return 1
		{..}:
			return 2
		_:
			return 0
)";

	const IRProgram ir = compile_to_ir(source);
	const IRFunction &func = find_function(ir, "shape");

	REQUIRE(count_opcode(func, IROpcode::TYPE_TEST) == 3);
	// A closed pattern constrains the size: `{"kind"}` does not match a
	// Dictionary with a second key, while `{..}` does.
	REQUIRE(count_syscall(func, 524, DICT_GET_SIZE) == 2);
	// Every named key is asked for, and only the ones with a pattern are read.
	REQUIRE(count_syscall(func, 524, DICT_HAS) == 3);
	REQUIRE(count_syscall(func, 524, DICT_GET) == 2);
}

// -= Order, and the rest of the grammar =-

TEST_CASE("the first matching arm wins") {
	const std::string source = R"(
func pick(n):
	match n:
		1, 2, 3:
			return 10
		3, 4:
			return 20
		var v:
			return v

func test():
	return pick(3) * 1000 + pick(4) * 10 + pick(9)
)";

	IRProgram ir = compile_to_ir(source);
	REQUIRE(call_int(ir, "pick", { int64_t(3) }) == 10);
	REQUIRE(call_int(ir, "pick", { int64_t(4) }) == 20);
	REQUIRE(call_int(ir, "pick", { int64_t(9) }) == 9);
	REQUIRE(call_int(ir, "test") == 10 * 1000 + 20 * 10 + 9);

	check_unoptimized_and_optimized(source, "pick", { 1, 2, 3, 4, 9 });
}

TEST_CASE("an arm after the wildcard is dead but legal") {
	// GDScript warns rather than rejects; the arms below the wildcard never run.
	const std::string source = R"(
func pick(n):
	match n:
		1:
			return 1
		_:
			return 2
		3:
			return 3

func test():
	return pick(3)
)";

	REQUIRE(call_int(compile_to_ir(source), "test") == 2);
}

TEST_CASE("break and continue inside an arm") {
	// A match is not a loop and has no fallthrough: `break` and `continue` in an
	// arm belong to the enclosing loop.
	const std::string source = R"(
func test():
	var total = 0
	var i = 0
	while i < 10:
		i = i + 1
		match i:
			3:
				continue
			var v when v > 7:
				break
			_:
				total = total + i
	return total
)";

	// 1 + 2 + 4 + 5 + 6 + 7 -- 3 is skipped, and 8 leaves the loop.
	REQUIRE(call_int(compile_to_ir(source), "test") == 25);
	REQUIRE(call_int(compile_to_ir(source, /*optimize=*/true), "test") == 25);
}

TEST_CASE("the grammar is enforced") {
	// A binding cannot share an arm: the other pattern may be the one that
	// matched, leaving the name unbound.
	const std::string shared_binding = R"(
func f(n):
	match n:
		1, var v:
			return v
)";
	REQUIRE(compile_error(shared_binding).find("cannot share an arm") != std::string::npos);

	// `..` means "and the rest", so nothing may follow it.
	const std::string rest_in_the_middle = R"(
func f(n):
	match n:
		[.., 1]:
			return 1
)";
	REQUIRE(compile_error(rest_in_the_middle).find("last entry") != std::string::npos);

	const std::string dictionary_rest_in_the_middle = R"(
func f(n):
	match n:
		{.., "a": 1}:
			return 1
)";
	REQUIRE(compile_error(dictionary_rest_in_the_middle).find("last entry") != std::string::npos);

	const std::string nameless_binding = R"(
func f(n):
	match n:
		var:
			return 1
)";
	REQUIRE(!compile_error(nameless_binding).empty());
}
