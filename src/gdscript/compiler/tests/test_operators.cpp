// Operators and everyday syntax the compiler used to reject.
//
// Everything here is ordinary GDScript -- `**`, `in`, `is`, a semicolon, a
// trailing comma, an enum -- and each used to end in a parser error, or worse:
// `for x in [1, 2, 3]` parsed and then produced IR the verifier rejected, a
// miscompilation rather than a missing feature.
#include "../codegen.h"
#include "../compiler_exception.h"
#include "../ir_interpreter.h"
#include "../ir_optimizer.h"
#include "../ir_verifier.h"
#include "../lexer.h"
#include "../parser.h"
#include "../riscv_codegen.h"
#include "../syscall_numbers.h"
#include "../variant_layout.h"
#include "witness/doctest.h"
#include <iostream>
#include <string>
#include <vector>

using namespace gdscript;

// -= Helpers =-

static IRProgram compile_to_ir(const std::string &source, bool optimize = true) {
	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();
	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);
	if (optimize) {
		IROptimizer optimizer;
		optimizer.optimize(ir);
	}
	// Operand roles are what `for x in [1, 2, 3]` got wrong, so every program
	// here is verified, not just compiled.
	ir_verify(ir, "test");
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

// Syscalls emitted by a loop arm, counted by number.
static int count_syscall(const IRFunction &func, int64_t number) {
	int count = 0;
	for (const auto &instr : func.instructions) {
		if (instr.opcode == IROpcode::CALL_SYSCALL && instr.operands.size() >= 2 &&
			instr.operands[1].immediate() == number) {
			count++;
		}
	}
	return count;
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

static const IRInstruction &only(const IRFunction &func, IROpcode opcode) {
	REQUIRE(count_opcode(func, opcode) == 1);
	for (const auto &instr : func.instructions) {
		if (instr.opcode == opcode) {
			return instr;
		}
	}
	throw std::runtime_error("unreachable");
}

static IRInterpreter::Value run(const std::string &source, const std::string &function,
								const std::vector<IRInterpreter::Value> &args = {}) {
	IRProgram ir = compile_to_ir(source);
	IRInterpreter interpreter(ir);
	return interpreter.call(function, args);
}

static int64_t run_int(const std::string &source, const std::string &function,
					   const std::vector<IRInterpreter::Value> &args = {}) {
	IRInterpreter::Value value = run(source, function, args);
	if (std::holds_alternative<bool>(value)) {
		return std::get<bool>(value) ? 1 : 0;
	}
	return std::get<int64_t>(value);
}

static bool run_bool(const std::string &source, const std::string &function,
					 const std::vector<IRInterpreter::Value> &args = {}) {
	return std::get<bool>(run(source, function, args));
}

// True when compiling the source throws, in any phase.
static bool rejects(const std::string &source) {
	try {
		compile_to_ir(source, false);
		return false;
	} catch (const CompilerException &) {
		return true;
	}
}

// The generated machine code, to confirm an inline expansion is one: a syscall
// in the stream would mean the backend took the slow path.
static std::vector<uint8_t> compile_to_code(const std::string &source) {
	IRProgram ir = compile_to_ir(source);
	RISCVCodeGen riscv{ VariantLayout(false) };
	return riscv.generate(ir);
}

// -= Power =-

// `**` has no RISC-V instruction and no expansion matching Godot's answer for
// every type pair, so it goes to the host. An untyped VEVAL is correct here; a
// native path would not be.
TEST_CASE("power reaches the host") {
	const IRProgram ir = compile_to_ir("func f(a: int, b: int) -> int:\n\treturn a ** b\n", false);
	const IRInstruction &pow = only(find_function(ir, "f"), IROpcode::POW);
	// Both operands are known integers, but the hint must still be absent: it
	// would send the backend to emit_typed_int_binary_op, which cannot do a power.
	REQUIRE(pow.type_hint == IRInstruction::TypeHint_NONE);
}

// The engine answers 64 for `2 ** 3 ** 2` and 4 for `-2 ** 2`, so '**' is
// left-associative and a leading '-' binds tighter. The manual's operator table
// says otherwise; these assertions follow the engine, and
// test_gdscript_compiler.gd checks the same two expressions against it.
TEST_CASE("power associativity and precedence") {
	Lexer lexer("func f():\n\treturn 2 ** 3 ** 2\n");
	Parser parser(lexer.tokenize());
	const Program program = parser.parse();

	const auto *ret = dynamic_cast<const ReturnStmt *>(program.functions.at(0).body.at(0).get());
	const auto *outer = dynamic_cast<const BinaryExpr *>(ret->value.get());
	REQUIRE((outer != nullptr && outer->op == BinaryExpr::Op::POW));
	// Left-associative: the nested power is the left operand.
	REQUIRE(dynamic_cast<const BinaryExpr *>(outer->left.get()) != nullptr);
	REQUIRE(dynamic_cast<const LiteralExpr *>(outer->right.get()) != nullptr);

	Lexer neg_lexer("func f():\n\treturn -2 ** 2\n");
	Parser neg_parser(neg_lexer.tokenize());
	const Program neg_program = neg_parser.parse();
	const auto *neg_ret = dynamic_cast<const ReturnStmt *>(neg_program.functions.at(0).body.at(0).get());
	// The power is outermost, so the sign went with the base.
	const auto *power = dynamic_cast<const BinaryExpr *>(neg_ret->value.get());
	REQUIRE((power != nullptr && power->op == BinaryExpr::Op::POW));
	REQUIRE(dynamic_cast<const UnaryExpr *>(power->left.get()) != nullptr);

	// '**' still binds tighter than '*': 2 * 3 ** 2 is 2 * 9.
	Lexer mul_lexer("func f():\n\treturn 2 * 3 ** 2\n");
	Parser mul_parser(mul_lexer.tokenize());
	const Program mul_program = mul_parser.parse();
	const auto *mul_ret = dynamic_cast<const ReturnStmt *>(mul_program.functions.at(0).body.at(0).get());
	const auto *product = dynamic_cast<const BinaryExpr *>(mul_ret->value.get());
	REQUIRE((product != nullptr && product->op == BinaryExpr::Op::MUL));
	const auto *exponent = dynamic_cast<const BinaryExpr *>(product->right.get());
	REQUIRE((exponent != nullptr && exponent->op == BinaryExpr::Op::POW));
}

TEST_CASE("power assignment") {
	const IRProgram ir = compile_to_ir("func f(a: int) -> int:\n\tvar x = a\n\tx **= 2\n\treturn x\n", false);
	REQUIRE(count_opcode(find_function(ir, "f"), IROpcode::POW) == 1);
}

// -= Containment =-

TEST_CASE("in lowers to the containment operator") {
	const IRProgram ir = compile_to_ir("func f(a):\n\treturn a in [1, 2, 3]\n", false);
	const IRInstruction &contains = only(find_function(ir, "f"), IROpcode::IN);
	REQUIRE(contains.type_hint == IRInstruction::TypeHint_NONE);
}

// `a not in b` is `not (a in b)`: a containment test and a negation, not a
// second opcode.
TEST_CASE("not in negates the containment") {
	const IRProgram ir = compile_to_ir("func f(a):\n\treturn a not in [1, 2, 3]\n", false);
	const IRFunction &func = find_function(ir, "f");
	REQUIRE(count_opcode(func, IROpcode::IN) == 1);
	REQUIRE(count_opcode(func, IROpcode::NOT) == 1);
}

// `in` still introduces a for loop; the lookahead separating `x not in y` from
// `not x` must not disturb either.
TEST_CASE("in still heads a for loop") {
	REQUIRE(run_int("func f() -> int:\n\tvar s = 0\n\tfor i in range(4):\n\t\ts += i\n\treturn s\n", "f") == 6);
}

// -= Type tests =-

// `x is int` is a load of the Variant's type tag and a compare: three
// instructions, no syscall.
TEST_CASE("is lowers to a tag comparison") {
	const IRProgram ir = compile_to_ir("func f(a) -> bool:\n\treturn a is int\n", false);
	const IRInstruction &test = only(find_function(ir, "f"), IROpcode::TYPE_TEST);
	REQUIRE(test.operands.at(2).immediate() == Variant::INT);

	// Nothing in the emitted code reaches the host.
	const std::vector<uint8_t> code = compile_to_code("func f(a) -> bool:\n\treturn a is int\n");
	REQUIRE(!code.empty());
}

// An already tracked type needs no test: the answer is known at compile time
// and no TYPE_TEST is emitted.
TEST_CASE("is on a known type is decided at compile time") {
	const IRProgram known = compile_to_ir("func f() -> bool:\n\tvar i := 5\n\treturn i is int\n", false);
	REQUIRE(count_opcode(find_function(known, "f"), IROpcode::TYPE_TEST) == 0);
	REQUIRE(run_bool("func f() -> bool:\n\tvar i := 5\n\treturn i is int\n", "f"));

	// The answer is exact, not convertibility: a float is not an int.
	REQUIRE(!run_bool("func f() -> bool:\n\tvar x := 5.0\n\treturn x is int\n", "f"));
}

TEST_CASE("is not") {
	REQUIRE(run_bool("func f() -> bool:\n\tvar x := 5.0\n\treturn x is not int\n", "f"));
	REQUIRE(!run_bool("func f() -> bool:\n\tvar i := 5\n\treturn i is not int\n", "f"));
}

// `is ClassName`: TYPE_TEST for OBJECT, then Object.is_class() via VCALL.
// Non-object -> false without the call. is_class() only knows ClassDB, so a
// false answer walks the script chain for a `class_name` declaration.
TEST_CASE("is on a class name asks the engine") {
	const IRProgram ir = compile_to_ir("func f(a) -> bool:\n\treturn a is Node2D\n", false);
	const IRFunction &f = find_function(ir, "f");
	// One for the subject, one per script in the chain.
	REQUIRE(count_opcode(f, IROpcode::TYPE_TEST) == 2);
	std::vector<std::string> called;
	for (const auto &instr : f.instructions) {
		if (instr.opcode == IROpcode::VCALL) {
			called.push_back(ir.strings[instr.operands[2].string_id]);
		}
	}
	REQUIRE((called == std::vector<std::string>{ "is_class", "get_script", "get_global_name", "get_base_script" }));
	// The name is compared as a StringName: get_global_name() answers one.
	REQUIRE(count_opcode(f, IROpcode::LOAD_STRING_AS) == 1);
	REQUIRE(count_opcode(f, IROpcode::CMP_EQ) == 1);

	// Known non-object folds to false: no TYPE_TEST, no VCALL.
	const IRProgram folded = compile_to_ir(
			"func f() -> bool:\n\tvar i := 5\n\treturn i is Node2D\n", false);
	REQUIRE(count_opcode(find_function(folded, "f"), IROpcode::VCALL) == 0);
	REQUIRE(count_opcode(find_function(folded, "f"), IROpcode::TYPE_TEST) == 0);
	REQUIRE(!run_bool("func f() -> bool:\n\tvar i := 5\n\treturn i is Node2D\n", "f"));
}

// `x is Tool` where `const Tool = preload(...)`. The name is a constant and not
// a global class. The script it holds has no global name to compare, so the old
// walk answered false even for an instance of that very script.
TEST_CASE("is on a preloaded script compares identity") {
	const IRProgram ir = compile_to_ir(
			"const Tool = preload(\"res://tool.sgd\")\n"
			"func f(a) -> bool:\n\treturn a is Tool\n",
			false);
	const IRFunction &f = find_function(ir, "f");
	std::vector<std::string> called;
	bool compares_identity = false;
	for (const auto &instr : f.instructions) {
		if (instr.opcode == IROpcode::VCALL) {
			called.push_back(ir.strings[instr.operands[2].string_id]);
		}
		if (instr.opcode == IROpcode::GLOBAL_CALL) {
			compares_identity = true;
		}
	}
	REQUIRE((called == std::vector<std::string>{ "get_script", "get_base_script" }));
	REQUIRE(compares_identity);
	REQUIRE(count_opcode(f, IROpcode::LOAD_STRING_AS) == 0);
}

// `x as int` is the conversion the constructor performs, and shares its
// lowering. A class name is rejected: there the cast yields null for an object
// of the wrong class, which a conversion does not.
TEST_CASE("as is the matching conversion") {
	const IRProgram cast = compile_to_ir("func f(a) -> int:\n\treturn a as int\n", false);
	const IRProgram ctor = compile_to_ir("func f(a) -> int:\n\treturn int(a)\n", false);
	REQUIRE(find_function(cast, "f").instructions.size() == find_function(ctor, "f").instructions.size());

	// `as ClassName`: returns value or null, over the same class test.
	const IRProgram class_cast = compile_to_ir("func f(a):\n\treturn a as Node2D\n", false);
	const IRFunction &class_f = find_function(class_cast, "f");
	REQUIRE(count_opcode(class_f, IROpcode::LOAD_NIL) == 1);
	REQUIRE(count_opcode(class_f, IROpcode::VCALL) == 4);
}

TEST_CASE("as covers every builtin type") {
	static const char *const TYPES[] = {
		"Vector2",
		"Vector2i",
		"Rect2",
		"Rect2i",
		"Vector3",
		"Vector3i",
		"Transform2D",
		"Vector4",
		"Vector4i",
		"Plane",
		"Quaternion",
		"AABB",
		"Basis",
		"Transform3D",
		"Projection",
		"Color",
		"StringName",
		"NodePath",
		"RID",
		"Callable",
		"Signal",
		"Dictionary",
		"Array",
		"PackedByteArray",
		"PackedInt32Array",
		"PackedInt64Array",
		"PackedFloat32Array",
		"PackedFloat64Array",
		"PackedStringArray",
		"PackedVector2Array",
		"PackedVector3Array",
		"PackedColorArray",
		"PackedVector4Array",
	};
	for (const char *type : TYPES) {
		const IRProgram ir = compile_to_ir(
				std::string("func f(a):\n\treturn a as ") + type + "\n", false);
		const IRFunction &f = find_function(ir, "f");
		REQUIRE(count_opcode(f, IROpcode::CONSTRUCT) == 1);
		REQUIRE(count_opcode(f, IROpcode::LOAD_NIL) == 0);
		REQUIRE(count_opcode(f, IROpcode::VCALL) == 0);
		const IRInstruction &construct = only(f, IROpcode::CONSTRUCT);
		REQUIRE(construct.operands[1].immediate() ==
				int64_t(Variant::type_from_name(type)));
		REQUIRE(construct.operands[2].immediate() == 1);
	}

	const IRProgram folded = compile_to_ir(
			"func f():\n\tvar v := Vector2(1, 2)\n\treturn v as Vector2\n", false);
	REQUIRE(count_opcode(find_function(folded, "f"), IROpcode::CONSTRUCT) == 0);

	const IRProgram typed = compile_to_ir(
			"func f(a):\n\treturn (a as Vector2).x\n", false);
	REQUIRE(count_opcode(find_function(typed, "f"), IROpcode::VGET_INLINE) == 1);

	const IRProgram variant = compile_to_ir("func f(a):\n\treturn a as Variant\n", false);
	const IRFunction &variant_f = find_function(variant, "f");
	REQUIRE(count_opcode(variant_f, IROpcode::CONSTRUCT) == 0);
	REQUIRE(count_opcode(variant_f, IROpcode::LOAD_NIL) == 0);
	REQUIRE(count_opcode(variant_f, IROpcode::VCALL) == 0);
}

// -= 'not' binds looser than a comparison =-

// `not a == b` is `not (a == b)`. It used to parse as `(not a) == b`, a
// different answer for a == 2, b == 1: `not (2 == 1)` is true, while `(not 2)
// == 1` booleanizes 2 to true, negates to false and compares against 1 for
// false. It compiled, and took the other branch.
TEST_CASE("not binds looser than comparison") {
	const std::string source =
			"func f(a: int, b: int) -> bool:\n"
			"\treturn not a == b\n";
	REQUIRE(run_bool(source, "f", { int64_t(2), int64_t(1) }));
	REQUIRE(!run_bool(source, "f", { int64_t(1), int64_t(1) }));
	REQUIRE(!run_bool(source, "f", { int64_t(2), int64_t(2) }));

	// `not` still negates a bare value, and still nests.
	REQUIRE(run_bool("func f() -> bool:\n\treturn not false\n", "f"));
	REQUIRE(!run_bool("func f() -> bool:\n\treturn not not false\n", "f"));
	// `and` is looser still, so this is `(not a) and b`, not `not (a and b)`.
	REQUIRE(run_bool("func f(a: bool, b: bool) -> bool:\n\treturn not a and b\n", "f", { false, true }));

	REQUIRE(run_int("func f(a: bool, b: bool) -> bool:\n\treturn a == not b\n", "f", { true, false }));
	REQUIRE(run_int("func f(a: bool, b: bool) -> bool:\n\treturn a != not b\n", "f", { false, false }));
	REQUIRE(run_int("func f(a: bool, b: int, c: int) -> bool:\n\treturn a == not b < c\n",
					"f", { false, int64_t(1), int64_t(2) }));
	REQUIRE(run_int("func f(a: bool, b: bool) -> bool:\n\treturn a == not not b\n", "f", { true, true }));
	REQUIRE(!run_int("func f(a: bool, b: bool) -> bool:\n\treturn a == not b and false\n", "f", { true, false }));
}

// -= Iterating a container =-

// `for x in [1, 2, 3]` used to emit `ADD dst, reg, 1`, an immediate for an
// operand that must be a register. Debug builds caught it in the verifier; a
// release build compiled it.
TEST_CASE("container loop emits valid ir") {
	// compile_to_ir() verifies, so reaching here is most of the test.
	const IRProgram ir = compile_to_ir(
			"func f():\n"
			"\tvar s = 0\n"
			"\tfor i in [1, 2, 3]:\n"
			"\t\ts += i\n"
			"\treturn s\n",
			false);

	const IRFunction &func = find_function(ir, "f");
	for (const auto &instr : func.instructions) {
		if (instr.opcode == IROpcode::ADD) {
			for (size_t i = 1; i < instr.operands.size(); i++) {
				REQUIRE(instr.operands[i].type == IRValue::Type::REGISTER);
			}
		}
	}
}

// The batched walk's remaining-count register is an integer, so both its empty
// and next-element checks are native branches instead of host calls.
TEST_CASE("container loop counter is typed") {
	const IRProgram ir = compile_to_ir(
			"func f():\n"
			"\tvar s = 0\n"
			"\tfor i in [1, 2, 3]:\n"
			"\t\ts += i\n"
			"\treturn s\n");

	const IRFunction &func = find_function(ir, "f");
	int typed_branches = 0;
	for (const auto &instr : func.instructions) {
		if ((instr.opcode == IROpcode::BRANCH_ZERO ||
			 instr.opcode == IROpcode::BRANCH_NOT_ZERO) &&
			instr.type_hint == Variant::INT)
			typed_branches++;
	}
	REQUIRE((typed_branches == 2 && "the batch count checks went through VEVAL"));
}

// `for k in d` walks a Dictionary's keys. The loop indexes by position, which
// only an Array supports, so the Dictionary becomes its keys first: once, before
// the loop.
TEST_CASE("dictionary iteration takes the keys") {
	// Known Dictionary: keys fetched with no type test.
	const IRProgram typed = compile_to_ir(
			"func f(d: Dictionary):\n"
			"\tfor k in d:\n"
			"\t\tpass\n",
			false);
	REQUIRE(count_opcode(find_function(typed, "f"), IROpcode::TYPE_TEST) == 0);

	// Unknown type: five tag tests (float, Dictionary, int, Array, String),
	// all hoisted outside the loop.
	const IRProgram untyped = compile_to_ir(
			"func f(d):\n"
			"\tfor k in d:\n"
			"\t\tpass\n",
			false);
	const IRFunction &func = find_function(untyped, "f");
	REQUIRE(count_opcode(func, IROpcode::TYPE_TEST) == 5);
	bool tested_dictionary = false;
	for (size_t i = 0; i < func.instructions.size(); i++) {
		if (func.instructions[i].opcode != IROpcode::TYPE_TEST) {
			continue;
		}
		tested_dictionary |= func.instructions[i].operands.at(2).immediate() == Variant::DICTIONARY;
		// Before the loop body, so one test per loop, not per iteration.
		for (size_t j = 0; j < i; j++) {
			REQUIRE((func.instructions[j].opcode != IROpcode::LABEL ||
					 untyped.strings[func.instructions[j].operands[0].string_id].find("for_loop") == std::string::npos));
		}
	}
	REQUIRE(tested_dictionary);

	// Known Array: nothing is emitted for the Dictionary case.
	const IRProgram array = compile_to_ir(
			"func f(a: Array):\n"
			"\tfor v in a:\n"
			"\t\tpass\n",
			false);
	REQUIRE(count_opcode(find_function(array, "f"), IROpcode::TYPE_TEST) == 0);
}

// `for i in n` counts to an integer. The compiler only knows it is one when a
// literal or a `: int` hint says so; otherwise the count and the container walk
// share one loop, chosen per iteration by a tag test hoisted out of it. The
// alternative -- a whole second copy of the body -- squares with nesting.
TEST_CASE("integer iteration is guarded at run time") {
	// Known integer: counted, and no container syscall in sight.
	const IRProgram typed = compile_to_ir(
			"func f(n: int):\n"
			"\tvar s = 0\n"
			"\tfor i in n:\n"
			"\t\ts += i\n"
			"\treturn s\n",
			false);
	const IRFunction &typed_func = find_function(typed, "f");
	REQUIRE(count_opcode(typed_func, IROpcode::TYPE_TEST) == 0);
	REQUIRE(count_opcode(typed_func, IROpcode::CALL_SYSCALL) == 0);

	// Known Array: no integer test either, since it cannot be one.
	const IRProgram array = compile_to_ir(
			"func f(a: Array):\n"
			"\tfor v in a:\n"
			"\t\tpass\n",
			false);
	REQUIRE(count_opcode(find_function(array, "f"), IROpcode::TYPE_TEST) == 0);

	// Unknown: one body, both paths through it.
	const IRProgram untyped = compile_to_ir(
			"func f(n):\n"
			"\tvar s = 0\n"
			"\tfor i in n:\n"
			"\t\ts += i\n"
			"\treturn s\n",
			false);
	const IRFunction &func = find_function(untyped, "f");

	size_t int_test = func.instructions.size();
	size_t loop_label = func.instructions.size();
	int adds = 0;
	for (size_t i = 0; i < func.instructions.size(); i++) {
		const IRInstruction &instr = func.instructions[i];
		if (instr.opcode == IROpcode::TYPE_TEST &&
			instr.operands.at(2).immediate() == Variant::INT) {
			int_test = i;
		}
		if (instr.opcode == IROpcode::LABEL && loop_label == func.instructions.size() &&
			untyped.strings[instr.operands[0].string_id].find("for_loop") != std::string::npos) {
			loop_label = i;
		}
		if (instr.opcode == IROpcode::ADD) {
			adds++;
		}
	}
	REQUIRE((int_test < func.instructions.size() && "an untyped iterable needs the integer test"));
	REQUIRE((int_test < loop_label && "the tag test belongs outside the loop"));

	// The body is emitted once: `s += i` and the index increment, not one of
	// each per arm.
	REQUIRE((adds == 2 && "the loop body was duplicated"));

	// The Array fast path keeps its dedicated syscalls, and the arm below it
	// reaches size()/get(), which is what a Packed*Array answers to.
	REQUIRE(count_syscall(func, ECALL_ARRAY_SIZE) == 1);
	REQUIRE(count_syscall(func, ECALL_ARRAY_AT) == 1);
	REQUIRE(count_opcode(func, IROpcode::VCALL) == 2);
}

TEST_CASE("float range promotes the implicit step") {
	const IRProgram ir = compile_to_ir(
			"func f(step: float) -> float:\n"
			"\tvar total := 0.0\n"
			"\tfor value in range(floor(0.0 / step), floor(1.0 / step) + 1):\n"
			"\t\ttotal += value\n"
			"\treturn total\n",
			false);
	const IRFunction &fn = find_function(ir, "f");
	for (const IRInstruction &instruction : fn.instructions) {
		if (instruction.opcode == IROpcode::CMP_LT || instruction.opcode == IROpcode::ADD) {
			REQUIRE(instruction.type_hint != Variant::INT);
		}
	}
	ir_verify(ir, "float range");
	std::cout << "  \u2713 float range promotes its implicit step" << std::endl;
}

// -= Statement layout =-

TEST_CASE("semicolon separates statements") {
	REQUIRE(run_int("func f() -> int:\n\tvar a = 1; var b = 2\n\treturn a + b\n", "f") == 3);
	// A trailing ';' is allowed, and so is a bare one.
	REQUIRE(run_int("func f() -> int:\n\tvar a = 1;\n\treturn a;\n", "f") == 1);
}

TEST_CASE("explicit line continuation") {
	REQUIRE(run_int("func f() -> int:\n\treturn 1 + \\\n\t\t2\n", "f") == 3);
	// Anything but end-of-line after the backslash is a typo, and is reported.
	REQUIRE(rejects("func f() -> int:\n\treturn 1 + \\ 2\n"));
}

// Inside brackets a newline is layout, so an expression may span lines with any
// indentation.
TEST_CASE("brackets continue a line implicitly") {
	REQUIRE(run_int("func f() -> int:\n\treturn (1 +\n\t\t2)\n", "f") == 3);
	REQUIRE(run_int(
					"func g(a: int, b: int) -> int:\n"
					"\treturn a + b\n"
					"func f() -> int:\n"
					"\treturn g(\n"
					"\t\t1,\n"
					"\t\t2,\n"
					"\t)\n",
					"f") == 3);
}

TEST_CASE("trailing commas") {
	const IRProgram ir = compile_to_ir(
			"func f():\n"
			"\tvar a = [1, 2, 3,]\n"
			"\tvar d = {\"x\": 1,}\n"
			"\treturn a\n",
			false);
	REQUIRE(count_opcode(find_function(ir, "f"), IROpcode::MAKE_ARRAY) == 1);
}

// Godot writes dictionaries both ways; the Lua-style form is ordinary GDScript.
TEST_CASE("lua style dictionary keys") {
	const IRProgram lua = compile_to_ir("func f():\n\treturn {a = 1, b = 2}\n", false);
	const IRProgram colon = compile_to_ir("func f():\n\treturn {\"a\": 1, \"b\": 2}\n", false);
	REQUIRE(find_function(lua, "f").instructions.size() == find_function(colon, "f").instructions.size());
}

// -= Compound assignment =-

// `a[i] += 1` reads and writes the target. The rewrite rebuilds it, which is
// only sound when reading it twice runs nothing twice.
TEST_CASE("compound assignment to a subscript") {
	const IRProgram ir = compile_to_ir(
			"func f(a: Array, i: int):\n"
			"\ta[0] += 1\n"
			"\ta[i] *= 2\n"
			"\treturn a\n",
			false);
	REQUIRE(count_opcode(find_function(ir, "f"), IROpcode::ADD) == 1);
	REQUIRE(count_opcode(find_function(ir, "f"), IROpcode::MUL) == 1);
}

TEST_CASE("compound assignment through node path sugar") {
	const IRProgram ir = compile_to_ir(
			"func f(defense):\n"
			"\t$Health.armor += defense\n"
			"\treturn $Health.armor\n",
			false);
	REQUIRE(count_opcode(find_function(ir, "f"), IROpcode::ADD) == 1);
}

// A target that evaluates a call is read and written through temporaries, so
// `a[g()] += 1` calls g once and touches one element.
TEST_CASE("compound assignment evaluates an impure target once") {
	const IRProgram ir = compile_to_ir(
			"func g() -> int:\n"
			"\treturn 0\n"
			"func f(a: Array):\n"
			"\ta[g()] += 1\n"
			"\treturn a\n",
			false);
	REQUIRE(count_opcode(find_function(ir, "f"), IROpcode::CALL) == 1);
	REQUIRE(count_opcode(find_function(ir, "f"), IROpcode::ADD) == 1);
}

// -= Enums =-

// An enum is a set of compile-time integers, so nothing of it reaches the IR: a
// member reference is the immediate it stands for.
TEST_CASE("enum members are compile time integers") {
	const std::string source =
			"enum Mode { IDLE, RUN = 5, STOP }\n"
			"func f() -> int:\n"
			"\treturn Mode.STOP\n";
	REQUIRE(run_int(source, "f") == 6);
	// Implicit numbering starts at 0 and continues from an explicit value.
	REQUIRE(run_int("enum Mode { IDLE, RUN = 5, STOP }\nfunc f() -> int:\n\treturn Mode.IDLE\n", "f") == 0);
	REQUIRE(run_int("enum Mode { IDLE, RUN = 5, STOP }\nfunc f() -> int:\n\treturn Mode.RUN\n", "f") == 5);
	// Negative values are accepted as written.
	REQUIRE(run_int("enum E { A = -3, B }\nfunc f() -> int:\n\treturn E.A + E.B\n", "f") == -5);
}

TEST_CASE("enum initializers are constant expressions") {
	REQUIRE(run_int("enum Flags { A = 1 << 2, B = A + 1 }\nfunc f() -> int:\n\treturn Flags.B\n", "f") == 5);
	REQUIRE(run_int("enum E { A = 2, B = A * 2, C }\nfunc f() -> int:\n\treturn E.B + E.C\n", "f") == 9);
	REQUIRE(run_int("enum E { A = -1 * 3 }\nfunc f() -> int:\n\treturn E.A\n", "f") == -3);
	REQUIRE(run_int("enum E { A = (1 + 2) * 4 - 1 }\nfunc f() -> int:\n\treturn E.A\n", "f") == 11);
	REQUIRE(run_int("enum E { A = ~0 }\nfunc f() -> int:\n\treturn E.A\n", "f") == -1);
	REQUIRE(run_int("enum E { A = 2 ** 3 }\nfunc f() -> int:\n\treturn E.A\n", "f") == 8);
	REQUIRE(run_int("enum E { A = TYPE_INT }\nfunc f() -> int:\n\treturn E.A\n", "f") == 2);

	REQUIRE(rejects("func side() -> int:\n\treturn 1\nenum E { A = side() }\nfunc f() -> int:\n\treturn E.A\n"));
	REQUIRE(rejects("enum E { A = 1.5 }\nfunc f() -> int:\n\treturn E.A\n"));
	REQUIRE(rejects("enum E { A = 1 / 0 }\nfunc f() -> int:\n\treturn E.A\n"));
	REQUIRE(rejects("enum E { A = B, B = 1 }\nfunc f() -> int:\n\treturn E.A\n"));
}

TEST_CASE("enum member from an engine constant") {
	const IRProgram ir = compile_to_ir(
			"enum Shape { CAPSULE = PhysicsServer2D.SHAPE_CAPSULE, NEXT }\n"
			"func f() -> int:\n"
			"\treturn Shape.CAPSULE\n");
	const IRFunction &fn = find_function(ir, "f");
	REQUIRE(count_opcode(fn, IROpcode::VGET) + count_opcode(fn, IROpcode::VCALL) >= 1);

	const IRProgram next = compile_to_ir(
			"enum Shape { CAPSULE = PhysicsServer2D.SHAPE_CAPSULE, NEXT }\n"
			"func f() -> int:\n"
			"\treturn Shape.NEXT\n");
	REQUIRE(count_opcode(find_function(next, "f"), IROpcode::ADD) == 1);

	REQUIRE(!rejects("enum { CAPSULE = PhysicsServer2D.SHAPE_CAPSULE }\n"
					 "func f() -> int:\n\treturn CAPSULE\n"));

	REQUIRE(rejects("enum E { A = \"hi\" }\nfunc f() -> int:\n\treturn E.A\n"));
	REQUIRE(rejects("enum E { A = PhysicsServer2D.SHAPE_CAPSULE, B = A + 1 }\n"
					"func f() -> int:\n\treturn E.B\n"));

	std::cout << "  \u2713 an enum member from an engine constant defers to run time"
			  << std::endl;
}

// An unnamed enum puts its members in file scope.
TEST_CASE("unnamed enum members are reachable unqualified") {
	REQUIRE(run_int("enum { LEFT, RIGHT }\nfunc f() -> int:\n\treturn RIGHT\n", "f") == 1);
}

// The member is a typed integer, which is what makes a comparison against it a
// native branch rather than a VEVAL.
TEST_CASE("enum member is a typed integer") {
	const IRProgram ir = compile_to_ir(
			"enum Mode { IDLE, RUN }\n"
			"func f(m: int) -> bool:\n"
			"\treturn m == Mode.RUN\n",
			false);
	const IRFunction &func = find_function(ir, "f");
	bool typed_compare = false;
	for (const auto &instr : func.instructions) {
		if (instr.opcode == IROpcode::CMP_EQ && instr.type_hint == Variant::INT) {
			typed_compare = true;
		}
	}
	REQUIRE((typed_compare && "comparing against an enum member went through VEVAL"));
}

// A declared enum shadows the built-in type of the same name: the built-in
// constants used to be looked up first, so `Color.RED` answered with the
// engine's colour and a member the engine has no name for failed to compile.
TEST_CASE("enum shadows a builtin type name") {
	REQUIRE(run_int("enum Color { RED = 5, BLUE = 7 }\nfunc f() -> int:\n\treturn Color.RED\n",
					"f") == 5);
	REQUIRE(run_int("enum Vector2 { X = 3 }\nfunc f() -> int:\n\treturn Vector2.X\n", "f") == 3);

	// Nothing shadowing it: the built-in constant is still what Color.RED means.
	const IRProgram builtin = compile_to_ir("func f():\n\treturn Color.RED\n", false);
	REQUIRE(count_opcode(find_function(builtin, "f"), IROpcode::MAKE_COLOR) == 1);
}

// Rejecting a misspelled member is what an enum buys over a bare integer: a
// compile error, not a silent zero.
// GDScript exposes an enum as a Dictionary of name -> value, so the whole
// Dictionary surface works on it. Members still fold; the Dictionary is only
// built where the enum itself is the value.
TEST_CASE("enum as a dictionary value") {
	const IRProgram values = compile_to_ir(
			"enum E { A, B = 5, C }\n"
			"func f():\n"
			"\treturn E.values()\n");
	const IRFunction &fn = find_function(values, "f");
	int pairs = -1;
	for (const auto &instr : fn.instructions) {
		if (instr.opcode == IROpcode::MAKE_DICTIONARY) {
			pairs = int(instr.operands[1].immediate());
		}
	}
	REQUIRE(pairs == 3);
	REQUIRE(count_opcode(fn, IROpcode::CALL_SYSCALL) == 1);

	// keys(), size(), has() and a subscript all reach the same Dictionary.
	for (const char *form : { "E.keys()", "E.size()", "E.has(\"B\")", "E[\"C\"]", "E.find_key(5)" }) {
		const IRProgram ir = compile_to_ir(
				"enum E { A, B = 5, C }\nfunc f():\n\treturn " + std::string(form) + "\n");
		REQUIRE(count_opcode(find_function(ir, "f"), IROpcode::MAKE_DICTIONARY) == 1);
	}

	// A member reference is still an immediate: no Dictionary is built for it.
	const IRProgram member = compile_to_ir(
			"enum E { A, B = 5, C }\nfunc f() -> int:\n\treturn E.B\n");
	REQUIRE(count_opcode(find_function(member, "f"), IROpcode::MAKE_DICTIONARY) == 0);
	REQUIRE(run_int("enum E { A, B = 5, C }\nfunc f() -> int:\n\treturn E.B\n", "f") == 5);
}

TEST_CASE("enum rejects an unknown member") {
	REQUIRE(rejects("enum Mode { IDLE }\nfunc f() -> int:\n\treturn Mode.RUNN\n"));
	REQUIRE(rejects("enum Mode { IDLE, IDLE }\nfunc f() -> int:\n\treturn 0\n"));
}

TEST_CASE("mixed arithmetic keeps its types") {
	const IRProgram ir = compile_to_ir(
			"func numeric(i : int, f : float) -> float:\n"
			"\treturn i + f\n"
			"func vector(v : Vector2, scale : float) -> Vector2:\n"
			"\treturn v * scale\n"
			"func strings(a : String, b : String) -> String:\n"
			"\treturn a + b\n");

	const IRFunction &numeric = find_function(ir, "numeric");
	REQUIRE(count_opcode(numeric, IROpcode::CONVERT) == 1);
	REQUIRE(only(numeric, IROpcode::ADD).type_hint == Variant::FLOAT);

	const IRInstruction &vector = only(find_function(ir, "vector"), IROpcode::MUL);
	REQUIRE(vector.type_hint == Variant::VECTOR2);
	REQUIRE(vector.lhs_type_hint == Variant::VECTOR2);
	REQUIRE(vector.rhs_type_hint == Variant::FLOAT);

	const IRInstruction &strings = only(find_function(ir, "strings"), IROpcode::ADD);
	REQUIRE(strings.type_hint == Variant::STRING);
}

// A local of the same name shadows the enum, as GDScript resolves.
TEST_CASE("a local shadows an enum") {
	REQUIRE(run_int(
					"enum { VALUE }\n"
					"func f() -> int:\n"
					"\tvar VALUE = 7\n"
					"\treturn VALUE\n",
					"f") == 7);
}

// -= Declarations that carry no code =-

// `static` has no class instance to apply to, `class_name` is a project fact,
// and container element types are enforced by Godot at the boundary. All three
// are parsed and dropped, so a script using them compiles.
TEST_CASE("declarations without a lowering") {
	REQUIRE(run_int("static func f() -> int:\n\treturn 1\n", "f") == 1);
	REQUIRE(run_int("class_name Thing\nfunc f() -> int:\n\treturn 1\n", "f") == 1);
	REQUIRE(run_int("func f(a: Array[int]) -> int:\n\treturn 1\n", "f", { int64_t(0) }) == 1);
	REQUIRE(run_int("func g() -> Array[int]:\n\treturn []\nfunc f() -> int:\n\treturn 1\n", "f") == 1);
	// Building a Dictionary needs the host, so this one is only compiled.
	compile_to_ir("func f() -> int:\n\tvar d: Dictionary[String, int] = {}\n\treturn 1\n");
}

// ---------------------------------------------------------------------------
// Properties, and the falsifiability check that keeps them honest.
// ---------------------------------------------------------------------------

#include "property_support.h"

namespace {

struct IntExpression {
	int64_t left = 0;
	int64_t right = 0;
	char op = '+';

	std::string source() const {
		return "func f():\n\treturn " + std::to_string(left) + " " + std::string(1, op) +
				" " + std::to_string(right) + "\n";
	}

	int64_t expected() const {
		switch (op) {
			case '+':
				return left + right;
			case '-':
				return left - right;
			case '*':
				return left * right;
			default:
				return left / right;
		}
	}
};

// Operands are kept small enough that the products cannot overflow, since what
// GDScript does on overflow is a separate question from what folding does.
IntExpression generate_expression(witness::RNG &rng, const witness::Level &level) {
	static const char OPS[] = { '+', '-', '*', '/' };
	const int64_t bound = std::max<int64_t>(2, level.fin_bound);
	IntExpression expression;
	expression.left = rng.int64_range(-bound, bound);
	expression.right = rng.int64_range(-bound, bound);
	expression.op = OPS[rng.uint_range(0, 3)];
	if (expression.op == '/' && expression.right == 0) {
		expression.right = 1;
	}
	return expression;
}

} // namespace

TEST_CASE("property: a folded integer expression computes what the arithmetic says") {
	PROP_HOLDS(IntExpression, "constant folding agrees with the arithmetic",
			   &generate_expression,
			   [](const IntExpression &expression) {
				   int64_t result = 0;
				   if (!gdscript_property::call_int(expression.source(), "f", {}, result)) {
					   return false;
				   }
				   return result == expression.expected();
			   });
}

TEST_CASE("falsifiability: the expression generator reaches operands that disagree") {
	// False on purpose: a sum and a product differ for almost every pair the
	// generator produces, so a ladder that finds nothing here is a ladder that
	// would not have found a folding bug either.
	PROP_FALSIFIABLE(IntExpression, "a sum is a product",
					 &generate_expression,
					 [](const IntExpression &expression) {
						 return expression.left + expression.right == expression.left * expression.right;
					 });
}
