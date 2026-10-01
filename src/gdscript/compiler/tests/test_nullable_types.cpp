#include "../codegen.h"
#include "../compiler.h"
#include "../compiler_exception.h"
#include "../ir_interpreter.h"
#include "../ir_verifier.h"
#include "../lexer.h"
#include "../parser.h"
#include "witness/doctest.h"
#include <iostream>
#include <string>

using namespace gdscript;

static IRProgram compile_to_ir(const std::string &source) {
	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();
	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);
	ir_verify(ir, "nullable test");
	return ir;
}

static std::string rejection(const std::string &source) {
	try {
		compile_to_ir(source);
	} catch (const CompilerException &error) {
		return error.what();
	}
	return {};
}

static const IRFunction &function(const IRProgram &ir, const std::string &name) {
	for (const IRFunction &candidate : ir.functions) {
		if (candidate.name == name)
			return candidate;
	}
	throw std::runtime_error("missing function " + name);
}

static const IRGlobalVar &global(const IRProgram &ir, const std::string &name) {
	for (const IRGlobalVar &candidate : ir.globals) {
		if (candidate.name == name)
			return candidate;
	}
	throw std::runtime_error("missing global " + name);
}

static int count(const IRFunction &func, IROpcode opcode) {
	int result = 0;
	for (const IRInstruction &instruction : func.instructions) {
		result += instruction.opcode == opcode;
	}
	return result;
}

static int typed_count(const IRFunction &func, IROpcode opcode,
					   IRInstruction::TypeHint type) {
	int result = 0;
	for (const IRInstruction &instruction : func.instructions) {
		result += instruction.opcode == opcode && instruction.type_hint == type;
	}
	return result;
}

TEST_CASE("parser and spelling") {
	Lexer lexer(
			"var value: Vector2?\n"
			"var array: Array[int]?\n"
			"func f(v: Node? = null) -> Vector3?:\n"
			"\treturn null\n"
			"func accepts(x):\n"
			"\treturn x is Vector2?\n");
	Parser parser(lexer.tokenize());
	Program program = parser.parse();
	REQUIRE(program.globals[0].type_hint.names == std::vector<std::string>({ "Vector2" }));
	REQUIRE(program.globals[0].type_hint.nullable);
	REQUIRE(program.globals[0].type_hint.spelled_nullable);
	REQUIRE(program.globals[0].type_hint.to_string() == "Vector2?");
	REQUIRE(program.globals[1].type_hint.to_string() == "Array[int]?");
	REQUIRE(program.functions[0].parameters[0].type_hint.to_string() == "Node?");
	REQUIRE(program.functions[0].return_type.to_string() == "Vector3?");
	auto *returned = dynamic_cast<ReturnStmt *>(program.functions[1].body[0].get());
	auto *tested = dynamic_cast<TypeTestExpr *>(returned->value.get());
	REQUIRE((tested != nullptr && tested->type.to_string() == "Vector2?"));

	const IRProgram suffix = compile_to_ir("func f(x: Vector2?):\n\treturn x\n");
	const IRProgram union_form = compile_to_ir("func f(x: Vector2 | null):\n\treturn x\n");
	REQUIRE(function(suffix, "f").param_sets == function(union_form, "f").param_sets);
}

TEST_CASE("diagnostics") {
	REQUIRE(rejection("func f():\n\tvar x: Variant?\n").find("already includes") != std::string::npos);
	REQUIRE(rejection("func f():\n\tvar x: null?\n").find("already includes") != std::string::npos);
	REQUIRE(rejection("func f():\n\tvar x: int??\n").find("second") != std::string::npos);
	REQUIRE(rejection("func f():\n\tvar x: int | String?\n").find("union member") != std::string::npos);
	REQUIRE(rejection("func f():\n\tvar x: int? | String\n").find("come last") != std::string::npos);
	REQUIRE(rejection("func f():\n\tvar x: int ?\n").find("directly follow") != std::string::npos);
	REQUIRE(rejection("func f(x):\n\treturn x as Node?\n").find("already nullable") != std::string::npos);
	REQUIRE(rejection("func f():\n\tvar x: Vector2 = null\n").find("Cannot assign null") != std::string::npos);
	REQUIRE(rejection("var x: Vector2 = null\n").find("Cannot assign null") != std::string::npos);
	compile_to_ir("var x: Node = null\nfunc f():\n\tvar y: Node = null\n");
}

TEST_CASE("coercion defaults and storage") {
	const IRProgram coercions = compile_to_ir(
			"func f(value):\n"
			"\tvar number: float? = 1\n"
			"\tvar packed: PackedInt32Array? = [1, 2]\n"
			"\tvar guarded: Vector2? = value\n"
			"\treturn number\n");
	const IRFunction &f = function(coercions, "f");
	REQUIRE(typed_count(f, IROpcode::CONVERT, Variant::FLOAT) == 1);
	REQUIRE(count(f, IROpcode::MAKE_PACKED_INT32_ARRAY) == 1);
	REQUIRE(count(f, IROpcode::TYPE_TEST_MASK) == 1);
	REQUIRE(count(f, IROpcode::THROW) == 1);
	REQUIRE(rejection("func f():\n\tvar x: Vector2? = \"bad\"\n").find("Vector2?") != std::string::npos);

	// A nullable source proves itself: widening it into a union that lists both
	// of its tags, directly or through a local or a call, guards only the entry.
	const IRProgram widened = compile_to_ir(
			"func find_by(key: String) -> Node?:\n\treturn null\n"
			"func widen(found: Node?) -> Node | String | null:\n\treturn found\n"
			"func via_local(key: String) -> Node | String | null:\n"
			"\tvar found: Node? = find_by(key)\n\treturn found\n"
			"func via_call(key: String) -> Node | String | null:\n\treturn find_by(key)\n");
	REQUIRE(count(function(widened, "widen"), IROpcode::TYPE_TEST_MASK) == 1);
	REQUIRE(count(function(widened, "via_local"), IROpcode::TYPE_TEST_MASK) == 0);
	REQUIRE(count(function(widened, "via_call"), IROpcode::TYPE_TEST_MASK) == 0);

	const IRProgram defaults = compile_to_ir(
			"struct Point:\n\tvar x: int = 0\n"
			"var vector: Vector2?\n"
			"var point: Point?\n"
			"func f():\n"
			"\tvar local: Vector3?\n"
			"\tvar shape: Point?\n"
			"\tshape = Point(7)\n"
			"\treturn shape.x\n");
	REQUIRE(global(defaults, "vector").init_type == IRGlobalVar::InitType::NULL_VAL);
	REQUIRE(global(defaults, "vector").value_type == IRInstruction::TypeHint_NONE);
	REQUIRE(global(defaults, "vector").declared_set != 0);
	REQUIRE(global(defaults, "point").init_type == IRGlobalVar::InitType::NULL_VAL);
	REQUIRE(count(function(defaults, "f"), IROpcode::LOAD_NIL) >= 2);
	REQUIRE(count(function(defaults, "f"), IROpcode::VGET_INLINE) == 0);
}

TEST_CASE("narrowing") {
	const IRProgram ir = compile_to_ir(
			"func checked(x: Vector2?):\n"
			"\tif x == null:\n\t\treturn 0.0\n"
			"\treturn x.x\n"
			"func truthy(x: Vector2?):\n"
			"\tif x:\n\t\treturn x.x\n"
			"\treturn x.x\n"
			"func asserted(x: Vector2?):\n"
			"\tassert(x != null)\n"
			"\treturn x.x\n"
			"func conjunction(x: Vector2?):\n"
			"\treturn x != null and x.x > 0.0\n"
			"func by_is(x: int?):\n"
			"\tif x is int:\n\t\treturn x + 1\n"
			"\treturn 0\n"
			"func matched(x: Vector2?):\n"
			"\tmatch x:\n"
			"\t\tnull:\n\t\t\treturn 0.0\n"
			"\t\t_:\n\t\t\treturn x.x\n"
			"func after_break(x: Vector2?):\n"
			"\twhile true:\n"
			"\t\tif x == null:\n\t\t\t\tbreak\n"
			"\t\treturn x.x\n"
			"\treturn 0.0\n"
			"func reassigned(x: Vector2?):\n"
			"\tx = Vector2(2, 3)\n"
			"\treturn x.x\n");
	REQUIRE(count(function(ir, "checked"), IROpcode::VGET_INLINE) == 1);
	// The then arm has one direct Vector2 read; the rest goes through the
	// untyped multi-type dispatch because a falsy Vector2.ZERO is not NIL.
	REQUIRE(count(function(ir, "truthy"), IROpcode::VGET_INLINE) > 1);
	REQUIRE(count(function(ir, "asserted"), IROpcode::VGET_INLINE) == 1);
	REQUIRE(count(function(ir, "conjunction"), IROpcode::VGET_INLINE) == 1);
	REQUIRE(typed_count(function(ir, "by_is"), IROpcode::ADD, Variant::INT) == 1);
	REQUIRE(count(function(ir, "matched"), IROpcode::VGET_INLINE) == 1);
	REQUIRE(count(function(ir, "after_break"), IROpcode::VGET_INLINE) == 1);
	REQUIRE(count(function(ir, "reassigned"), IROpcode::VGET_INLINE) == 1);

	const IRProgram nullable_is = compile_to_ir("func f(x):\n\treturn x is Vector2?\n");
	REQUIRE(count(function(nullable_is, "f"), IROpcode::TYPE_TEST_MASK) == 1);
}

TEST_CASE("reflection") {
	const IRProgram ir = compile_to_ir(
			"@export var value: Vector2?\n"
			"@export var texture: Texture2D?\n"
			"func f(v: Vector2?, n: Node?) -> Vector3?:\n"
			"\treturn null\n");
	REQUIRE(ir.signatures[0].parameters[0].type == FunctionParameter::ANY_TYPE);
	REQUIRE(ir.signatures[0].parameters[1].type == Variant::OBJECT);
	REQUIRE(ir.signatures[0].return_type == FunctionParameter::ANY_TYPE);

	Compiler compiler;
	CompilerOptions options;
	options.extensions = true;
	options.output_elf = false;
	compiler.compile(
			"@export var value: Vector2?\n"
			"@export var texture: Texture2D?\n",
			options);
	REQUIRE(!compiler.get_error_info().has_error);
	const auto &properties = compiler.get_property_signatures();
	REQUIRE(properties.size() == 2);
	REQUIRE(properties[0].type == -1);
	REQUIRE(properties[0].default_kind == PropertyDefaultKind::NIL);
	REQUIRE(properties[1].type == Variant::OBJECT);
	REQUIRE(properties[1].class_name == "Texture2D");
}

// The mask a `T?` parameter is guarded with, or 0 when it is not guarded.
static uint64_t guard_mask(const IRFunction &func) {
	for (const IRInstruction &instruction : func.instructions) {
		if (instruction.opcode == IROpcode::TYPE_TEST_MASK) {
			return uint64_t(instruction.operands.at(2).immediate());
		}
	}
	return 0;
}

// A nullable slot is one Variant that holds either T or NIL, so the boundary
// guard is a two-tag mask -- and once a null check has ruled NIL out, the value
// lowers exactly like the plain type it was declared with.
TEST_CASE("nullable containers and structs") {
	const IRProgram ir = compile_to_ir(
			"struct Point:\n\tvar x = 0\n"
			"func arrays(a: Array[int]?):\n"
			"\tif a == null:\n\t\treturn 0\n"
			"\tvar total = 0\n\tfor v in a:\n\t\ttotal += v\n\treturn total\n"
			"func dictionaries(d: Dictionary?):\n"
			"\tif d == null:\n\t\treturn 0\n\treturn d.size()\n"
			"func structs(p: Point?):\n"
			"\tif p == null:\n\t\treturn 0\n\treturn p.x\n");

	const uint64_t nil = uint64_t(1) << Variant::NIL;
	REQUIRE(guard_mask(function(ir, "arrays")) == (nil | (uint64_t(1) << Variant::ARRAY)));
	REQUIRE(guard_mask(function(ir, "dictionaries")) == (nil | (uint64_t(1) << Variant::DICTIONARY)));
	// A struct is a Dictionary, so that is the tag its nullable form admits.
	REQUIRE(guard_mask(function(ir, "structs")) == (nil | (uint64_t(1) << Variant::DICTIONARY)));

	// After the null check each one is the plain lowering: a batched Array walk,
	// a Dictionary operation, a direct field read.
	REQUIRE(count(function(ir, "arrays"), IROpcode::CALL_SYSCALL) == 1);
	REQUIRE(count(function(ir, "arrays"), IROpcode::BATCH_GET) == 1);
	REQUIRE(count(function(ir, "dictionaries"), IROpcode::CALL_SYSCALL) == 1);
	REQUIRE(count(function(ir, "structs"), IROpcode::DICT_GET_CONST) == 1);
	// NOTE: the tag mask is the whole boundary check for a nullable struct --
	// the exact-shape guard a plain `p: Point` parameter carries is not emitted
	// here, so any Dictionary satisfies `Point?`.
	REQUIRE(count(function(ir, "structs"), IROpcode::STRUCT_CHECK) == 0);

	// A nullable struct without an initializer is NIL rather than a fresh
	// instance: the slot's value is the declaration, not the struct.
	const IRProgram declared = compile_to_ir(
			"struct Point:\n\tvar x = 0\n"
			"func f():\n\tvar plain: Point\n\tvar maybe: Point?\n\treturn maybe\n");
	REQUIRE(count(function(declared, "f"), IROpcode::MAKE_DICTIONARY_KEYED) == 1);
	REQUIRE(count(function(declared, "f"), IROpcode::LOAD_NIL) == 1);
}

// Narrowing is a fact about a value, so it travels with the value: a local
// copied out of a narrowed one is narrowed too. A member is different -- it can
// change under any call -- which is why its storage stays untyped.
TEST_CASE("nullable narrowing travels") {
	const IRProgram ir = compile_to_ir(
			"func copied(x: int?):\n"
			"\tif x == null:\n\t\treturn 0\n"
			"\tvar y = x\n\treturn y + 1\n"
			"func through_assert(x: Vector2?):\n"
			"\tassert(x is Vector2)\n"
			"\tvar y = x\n\treturn y.x\n"
			"func early_return(x: int?):\n"
			"\tif x == null:\n\t\treturn 0\n"
			"\treturn x + 1\n");
	REQUIRE(typed_count(function(ir, "copied"), IROpcode::ADD, Variant::INT) == 1);
	REQUIRE(count(function(ir, "through_assert"), IROpcode::VGET_INLINE) == 1);
	REQUIRE(typed_count(function(ir, "early_return"), IROpcode::ADD, Variant::INT) == 1);

	// A nullable member keeps untyped storage even where it demonstrably holds
	// T: a concrete slot type plus a stored NIL is what corrupts host reads.
	const IRProgram member = compile_to_ir(
			"var maybe: int?\n"
			"var plain: int = 0\n"
			"func f():\n\tmaybe = 5\n\treturn maybe + 1\n"
			"func g():\n\tplain = 5\n\treturn plain + 1\n");
	REQUIRE(global(member, "maybe").value_type == IRInstruction::TypeHint_NONE);
	REQUIRE(global(member, "maybe").init_type == IRGlobalVar::InitType::NULL_VAL);
	REQUIRE(typed_count(function(member, "f"), IROpcode::ADD, Variant::INT) == 0);
	// The same member without the '?' keeps its declared type through the store.
	REQUIRE(global(member, "plain").value_type == Variant::INT);
	REQUIRE(typed_count(function(member, "g"), IROpcode::ADD, Variant::INT) == 1);
}

TEST_CASE("if var null only binding") {
	const IRProgram ir = compile_to_ir(
			"func enters(x):\n"
			"\tif var value := x:\n"
			"\t\treturn 1\n"
			"\treturn 0\n"
			"func typed(x):\n"
			"\tif var value: int = x:\n"
			"\t\treturn value + 1\n"
			"\telse:\n"
			"\t\treturn -1\n");

	IRInterpreter interpreter(ir);
	REQUIRE(std::get<int64_t>(interpreter.call("enters", { std::monostate{} })) == 0);
	REQUIRE(std::get<int64_t>(interpreter.call("enters", { int64_t(0) })) == 1);
	REQUIRE(std::get<int64_t>(interpreter.call("enters", { false })) == 1);
	REQUIRE(std::get<int64_t>(interpreter.call("enters", { std::string() })) == 1);
	REQUIRE(std::get<int64_t>(interpreter.call("typed", { std::monostate{} })) == -1);
	REQUIRE(std::get<int64_t>(interpreter.call("typed", { int64_t(4) })) == 5);
	REQUIRE(typed_count(function(ir, "typed"), IROpcode::ADD, Variant::INT) == 1);
	const uint64_t nil_or_int = (uint64_t(1) << Variant::NIL) |
			(uint64_t(1) << Variant::INT);
	REQUIRE(guard_mask(function(ir, "typed")) == nil_or_int);

	const std::string escaped = rejection(
			"func f(x):\n"
			"\tif var value = x:\n"
			"\t\tpass\n"
			"\treturn value\n");
	REQUIRE(escaped.find("Undefined variable: value") != std::string::npos);

	const std::string redeclared = rejection(
			"func f(x):\n"
			"\tif var value = x:\n"
			"\t\tvar value = 1\n");
	REQUIRE(redeclared.find("already declared in this scope") != std::string::npos);

	// The initializer is outside the binding scope. This common shadow-unwrapping
	// form must therefore capture the outer value when used in a lambda.
	compile_to_ir(
			"func make(value: int?):\n"
			"\treturn func():\n"
			"\t\tif var value := value:\n"
			"\t\t\treturn value + 1\n"
			"\t\treturn 0\n");

	compile_to_ir(
			"func f(v):\n"
			"\treturn func():\n"
			"\t\tif var value := v:\n"
			"\t\t\tvar g = func(): return value\n"
			"\t\t\treturn g.call()\n"
			"\t\treturn 0\n");

	const std::string mistyped = rejection(
			"func f():\n"
			"\tvar s := \"hi\"\n"
			"\tif var value: int = s:\n"
			"\t\tpass\n");
	REQUIRE(mistyped.find("variable 'value' of type int") != std::string::npos);
	REQUIRE(mistyped.find("int | null") == std::string::npos);

	const IRProgram thrower = compile_to_ir(
			"func f(x):\n"
			"\tif var value: int? = x:\n"
			"\t\treturn value\n"
			"\treturn 0\n");
	for (const IRInstruction &instr : function(thrower, "f").instructions) {
		if (instr.opcode != IROpcode::THROW)
			continue;
		REQUIRE(thrower.strings[instr.operands[1].string_id].find("of type int?") !=
				std::string::npos);
	}

	Compiler compiler;
	CompilerOptions options;
	options.extensions = true;
	options.output_elf = false;
	compiler.compile(
			"func f(x):\n"
			"\tif var value: int = x:\n"
			"\t\treturn value + 1\n"
			"\treturn 0\n",
			options);
	REQUIRE(!compiler.get_error_info().has_error);
}

// `?.` and `??` write out the null check that `if x != null:` spells, and lower
// to the same NIL tag test. GDScript has neither.
// A member read on an untyped value tests for DICTIONARY on its own, so only a
// test against the NIL tag is a null guard.
static int nil_tests(const IRFunction &func) {
	int result = 0;
	for (const IRInstruction &instruction : func.instructions) {
		result += instruction.opcode == IROpcode::TYPE_TEST &&
				instruction.operands.size() > 2 &&
				instruction.operands[2].type == IRValue::Type::IMMEDIATE &&
				instruction.operands[2].imm_value == static_cast<int64_t>(Variant::NIL);
	}
	return result;
}

static size_t index_of(const IRFunction &func, IROpcode opcode) {
	for (size_t i = 0; i < func.instructions.size(); i++) {
		if (func.instructions[i].opcode == opcode)
			return i;
	}
	return SIZE_MAX;
}

static size_t global_index(const IRProgram &ir, const std::string &name) {
	for (size_t i = 0; i < ir.globals.size(); i++) {
		if (ir.globals[i].name == name)
			return i;
	}
	throw std::runtime_error("missing global " + name);
}

TEST_CASE("safe navigation") {
	// One guard for the whole postfix chain: `n?.a.b` is null rather than a
	// member access on null.
	const IRProgram chain = compile_to_ir(
			"func f(n):\n"
			"\treturn n?.transform.origin\n");
	REQUIRE(nil_tests(function(chain, "f")) == 1);
	REQUIRE(count(function(chain, "f"), IROpcode::LOAD_NIL) == 1);

	// An index at the end of the chain is skipped by the same branch.
	const IRProgram indexed = compile_to_ir(
			"func f(n):\n"
			"\treturn n?.items[0]\n");
	REQUIRE(nil_tests(function(indexed, "f")) == 1);
	REQUIRE(count(function(indexed, "f"), IROpcode::LOAD_NIL) == 1);

	// Two links, two tests: the second one guards what the first returned.
	const IRProgram twice = compile_to_ir(
			"func f(n):\n"
			"\treturn n?.parent?.name\n");
	REQUIRE(nil_tests(function(twice, "f")) == 2);
	REQUIRE(count(function(twice, "f"), IROpcode::LOAD_NIL) == 1);

	// A null receiver evaluates no arguments, so the guard precedes the call
	// that produces them.
	const IRProgram guarded = compile_to_ir(
			"func side() -> int:\n"
			"\treturn 1\n"
			"func f(n):\n"
			"\treturn n?.push(side())\n");
	REQUIRE(index_of(function(guarded, "f"), IROpcode::TYPE_TEST) <
			index_of(function(guarded, "f"), IROpcode::CALL));

	// A receiver that cannot be null costs nothing: no test, no join.
	const IRProgram proven = compile_to_ir(
			"func f() -> String:\n"
			"\tvar s := \"hi\"\n"
			"\treturn s?.to_upper()\n");
	REQUIRE(nil_tests(function(proven, "f")) == 0);
	REQUIRE(count(function(proven, "f"), IROpcode::LOAD_NIL) == 0);
	REQUIRE(count(function(proven, "f"), IROpcode::VCALL) == 1);

	// A nullable declaration narrows past the guard the way `if x != null:`
	// does, so the member read lowers as a plain Dictionary field.
	const IRProgram narrowed = compile_to_ir(
			"struct Point:\n"
			"\tvar x: int\n"
			"func f(p: Point?):\n"
			"\treturn p?.x\n");
	REQUIRE(count(function(narrowed, "f"), IROpcode::DICT_GET_CONST) == 1);
	REQUIRE(count(function(narrowed, "f"), IROpcode::VGET) == 0);

	// There is no place to write through a null receiver.
	REQUIRE(rejection("func f(n):\n\tn?.x = 1\n").find("'?.'") != std::string::npos);
	REQUIRE(rejection("func f(n):\n\tn?.x += 1\n").find("compound assignment") !=
			std::string::npos);
	REQUIRE(rejection("func f(n):\n\tn?.items[0] = 1\n").find("'?.'") != std::string::npos);

	// An enum or engine type on the left is a name, not a value that can be null.
	REQUIRE(rejection("func f():\n\treturn Vector2?.ZERO\n").find("is a name") !=
			std::string::npos);

	// Null short-circuits without reaching the host.
	const IRProgram reads = compile_to_ir("func f(n):\n\treturn n?.name\n");
	IRInterpreter interpreter(reads);
	REQUIRE(std::holds_alternative<std::monostate>(
			interpreter.call("f", { std::monostate{} })));
	std::cout << "  \u2713 '?.' guards a whole chain with one NIL test and no host call\n";
}

TEST_CASE("null coalescing") {
	// The fallback is evaluated only when the left side is null.
	const IRProgram basic = compile_to_ir("func f(x):\n\treturn x ?? 5\n");
	REQUIRE(nil_tests(function(basic, "f")) == 1);
	IRInterpreter values(basic);
	REQUIRE(std::get<int64_t>(values.call("f", { std::monostate{} })) == 5);
	REQUIRE(std::get<int64_t>(values.call("f", { int64_t(3) })) == 3);
	// Unlike `or`, a falsy value is still a value.
	REQUIRE(std::get<bool>(values.call("f", { false })) == false);
	REQUIRE(std::get<int64_t>(values.call("f", { int64_t(0) })) == 0);

	const IRProgram effects = compile_to_ir(
			"var calls := 0\n"
			"func bump() -> int:\n"
			"\tcalls += 1\n"
			"\treturn 9\n"
			"func f(x):\n"
			"\treturn x ?? bump()\n");
	const size_t calls = global_index(effects, "calls");
	IRInterpreter side_effects(effects);
	REQUIRE(std::get<int64_t>(side_effects.call("f", { int64_t(3) })) == 3);
	REQUIRE(std::get<int64_t>(side_effects.global(calls)) == 0);
	REQUIRE(std::get<int64_t>(side_effects.call("f", { std::monostate{} })) == 9);
	REQUIRE(std::get<int64_t>(side_effects.global(calls)) == 1);

	// A left side that cannot be null leaves the fallback unlowered entirely.
	const IRProgram folded = compile_to_ir(
			"func side() -> int:\n"
			"\treturn 1\n"
			"func f() -> int:\n"
			"\tvar a := 5\n"
			"\treturn a ?? side()\n");
	REQUIRE(nil_tests(function(folded, "f")) == 0);
	REQUIRE(count(function(folded, "f"), IROpcode::CALL) == 0);

	// One declared non-null type on the left and the same type on the right make
	// the result typed, so the addition stays an integer add.
	const IRProgram typed = compile_to_ir(
			"func f(x: int?) -> int:\n"
			"\treturn (x ?? 7) + 1\n");
	REQUIRE(typed_count(function(typed, "f"), IROpcode::ADD, Variant::INT) == 1);

	// A chain of fallbacks reads left to right.
	const IRProgram chain = compile_to_ir("func f(a, b):\n\treturn a ?? b ?? 3\n");
	IRInterpreter chained(chain);
	REQUIRE(std::get<int64_t>(chained.call("f", { std::monostate{}, std::monostate{} })) == 3);
	REQUIRE(std::get<int64_t>(chained.call("f", { std::monostate{}, int64_t(2) })) == 2);
	REQUIRE(std::get<int64_t>(chained.call("f", { int64_t(1), int64_t(2) })) == 1);

	// `??` binds tighter than the conditional expression, so the fallback is
	// part of the branch's value and not the whole conditional.
	const IRProgram conditional = compile_to_ir("func f(x, c):\n\treturn x ?? 0 if c else 9\n");
	IRInterpreter ternary(conditional);
	REQUIRE(std::get<int64_t>(ternary.call("f", { std::monostate{}, true })) == 0);
	REQUIRE(std::get<int64_t>(ternary.call("f", { int64_t(5), false })) == 9);

	// `as` is looser than every operator here, and a cast still takes a fallback.
	REQUIRE(rejection("func f(x):\n\treturn x as Node ?? 0\n").empty());

	// `?.` produces the value `??` is there to replace.
	const IRProgram together = compile_to_ir(
			"func f(n) -> String:\n"
			"\treturn n?.name ?? \"none\"\n");
	REQUIRE(nil_tests(function(together, "f")) == 2);
	IRInterpreter fallback(together);
	REQUIRE(std::get<std::string>(fallback.call("f", { std::monostate{} })) == "none");

	// The operator needs a space after a type name, which is otherwise the
	// repeated nullable suffix.
	REQUIRE(rejection("func f():\n\tvar x: int??\n").find("second") != std::string::npos);
	REQUIRE(rejection("func f(x):\n\treturn x is int ?? false\n").empty());
	std::cout << "  \u2713 '??' answers with its left side unless that side is null\n";
}
