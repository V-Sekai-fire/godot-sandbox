// Inline member stores (VSET_INLINE) and lvalue write-back chains.
// Value types (Vector2/3/4, Color) need copy-back; handles do not.
// Untyped paths branch on the tag at run time.
#include "../codegen.h"
#include "../compiler_exception.h"
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

// Count ECALL_DICTIONARY_OPS with GET.
// Tag tests, of either shape: a group of types that share an arm is tested
// with one TYPE_TEST_MASK rather than one TYPE_TEST each.
static int count_tag_tests(const IRFunction &func) {
	return count_opcode(func, IROpcode::TYPE_TEST) +
			count_opcode(func, IROpcode::TYPE_TEST_MASK);
}

static int count_dict_gets(const IRFunction &func) {
	int count = count_opcode(func, IROpcode::DICT_GET_CONST);
	for (const auto &instr : func.instructions) {
		if (instr.opcode == IROpcode::CALL_SYSCALL && instr.operands.size() >= 3 &&
			instr.operands[1].immediate() == ECALL_DICTIONARY_OPS &&
			instr.operands[2].immediate() == 0) {
			count++;
		}
	}
	return count;
}

static int count_dict_sets(const IRFunction &func) {
	return count_opcode(func, IROpcode::DICT_SET) +
			count_opcode(func, IROpcode::DICT_SET_CONST) +
			count_opcode(func, IROpcode::DICT_SET_CONST_STR);
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

static bool refuses(const std::string &source) {
	try {
		compile_to_ir(source);
	} catch (const CompilerException &) {
		return true;
	}
	return false;
}

// The register a VSET_INLINE writes into.
static int inline_set_target(const IRFunction &func) {
	for (const auto &instr : func.instructions) {
		if (instr.opcode == IROpcode::VSET_INLINE) {
			return instr.operands[0].reg_index();
		}
	}
	return -1;
}

// The type hint a VSET_INLINE was given.
static int64_t inline_set_type(const IRFunction &func) {
	for (const auto &instr : func.instructions) {
		if (instr.opcode == IROpcode::VSET_INLINE) {
			return instr.operands[2].immediate();
		}
	}
	return -1;
}

// Machine code, so that a lowering nothing emits is caught here and not by a
// "not yet implemented" throw in someone's project.
static std::vector<uint8_t> compile_to_riscv(const std::string &source) {
	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();
	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);
	IROptimizer optimizer;
	optimizer.optimize(ir);
	RISCVCodeGen backend;
	return backend.generate(ir);
}

// -= A known inline type =-

TEST_CASE("known inline member write") {
	const IRProgram vec2 = compile_to_ir(
			"func test():\n\tvar v : Vector2 = Vector2(1, 2)\n\tv.x = 5.0\n\treturn v\n");
	const IRFunction &f = find_function(vec2, "test");
	REQUIRE(count_opcode(f, IROpcode::VSET_INLINE) == 1);
	// VSET (ECALL_OBJ_PROP_SET) throws on Vector2.
	REQUIRE(count_opcode(f, IROpcode::VSET) == 0);
	REQUIRE(inline_set_type(f) == Variant::VECTOR2);

	// Store target is the MAKE_VECTOR2 register, not a dropped copy.
	int target = inline_set_target(f);
	bool target_is_the_variable = false;
	for (const auto &instr : f.instructions) {
		if (instr.opcode == IROpcode::MAKE_VECTOR2 &&
			instr.operands[0].reg_index() == target) {
			target_is_the_variable = true;
		}
	}
	REQUIRE(target_is_the_variable);

	// Integer vectors and Color differ only in payload slot.
	const IRProgram vec3i = compile_to_ir(
			"func test():\n\tvar v : Vector3i = Vector3i(1, 2, 3)\n\tv.z = 9\n\treturn v\n");
	REQUIRE(inline_set_type(find_function(vec3i, "test")) == Variant::VECTOR3I);

	const IRProgram color = compile_to_ir(
			"func test():\n\tvar c : Color = Color(1, 0, 0)\n\tc.g = 0.5\n\treturn c\n");
	REQUIRE(inline_set_type(find_function(color, "test")) == Variant::COLOR);
	REQUIRE(count_opcode(find_function(color, "test"), IROpcode::VSET) == 0);

	// Compound assignment desugars to VGET_INLINE + VSET_INLINE.
	const IRProgram compound = compile_to_ir(
			"func test():\n\tvar v : Vector2 = Vector2(1, 2)\n\tv.y += 1.0\n\treturn v\n");
	REQUIRE(count_opcode(find_function(compound, "test"), IROpcode::VSET_INLINE) == 1);
	REQUIRE(count_opcode(find_function(compound, "test"), IROpcode::VGET_INLINE) == 1);
}

TEST_CASE("rect and plane members") {
	const IRProgram rect = compile_to_ir(
			"func test():\n\tvar r : Rect2 = Rect2(1, 2, 3, 4)\n\treturn r.size.x\n");
	const IRFunction &r = find_function(rect, "test");
	REQUIRE(count_opcode(r, IROpcode::VGET_INLINE) == 2);
	REQUIRE(count_opcode(r, IROpcode::VGET) == 0);

	const IRProgram plane = compile_to_ir(
			"func test():\n\tvar p : Plane = Plane(0, 1, 0, 2)\n\treturn p.normal.z + p.d\n");
	const IRFunction &p = find_function(plane, "test");
	REQUIRE(count_opcode(p, IROpcode::VGET_INLINE) == 3);
	REQUIRE(count_opcode(p, IROpcode::VGET) == 0);

	const IRProgram write = compile_to_ir(
			"func test():\n\tvar r : Rect2 = Rect2(1, 2, 3, 4)\n\tr.position = Vector2(9, 9)\n\treturn r\n");
	const IRFunction &w = find_function(write, "test");
	REQUIRE(count_opcode(w, IROpcode::VSET_INLINE) == 1);
	REQUIRE(count_opcode(w, IROpcode::VSET) == 0);
	REQUIRE(inline_set_type(w) == Variant::RECT2);

	const IRProgram chained = compile_to_ir(
			"func test():\n\tvar r : Rect2i = Rect2i(1, 2, 3, 4)\n\tr.size.y = 7\n\treturn r\n");
	const IRFunction &c = find_function(chained, "test");
	REQUIRE(count_opcode(c, IROpcode::VGET_INLINE) == 1);
	REQUIRE(count_opcode(c, IROpcode::VSET_INLINE) == 2);
	REQUIRE(count_opcode(c, IROpcode::VSET) == 0);

	const IRProgram spelled = compile_to_ir(
			"func test():\n\tvar p : Plane = Plane(0, 1, 0, 2)\n\tp.y = 0.5\n\treturn p.y\n");
	REQUIRE(count_opcode(find_function(spelled, "test"), IROpcode::VSET) == 0);
	REQUIRE(count_opcode(find_function(spelled, "test"), IROpcode::VGET) == 0);

	const IRProgram computed_program = compile_to_ir(
			"func test():\n\tvar r : Rect2 = Rect2(1, 2, 3, 4)\n\treturn r.end\n");
	const IRFunction &computed = find_function(computed_program, "test");
	REQUIRE(count_opcode(computed, IROpcode::VGET) == 0);
	REQUIRE(count_opcode(computed, IROpcode::CALL_SYSCALL) == 1);

	REQUIRE(count_opcode(find_function(compile_to_ir(
											   "func test():\n\tvar a : AABB = AABB()\n\treturn a.end\n"),
									   "test"),
						 IROpcode::VGET) == 1);

	REQUIRE(!compile_to_riscv(
					 "func test():\n\tvar r : Rect2 = Rect2(1, 2, 3, 4)\n\tr.position = Vector2(9, 9)\n"
					 "\treturn r.size.x + r.position.y\n")
					 .empty());
	REQUIRE(!compile_to_riscv(
					 "func test():\n\tvar p : Plane = Plane(0, 1, 0, 2)\n\tp.normal = Vector3(1, 0, 0)\n"
					 "\treturn p.normal.x\n")
					 .empty());
	REQUIRE(!compile_to_riscv(
					 "func test():\n\tvar r : Rect2i = Rect2i(1, 2, 3, 4)\n\tr.size.y = 7\n\treturn r.size\n")
					 .empty());
}

// A member of more than one component (Rect2.size is a Vector2) is filled by
// copying the value's own components out of its Variant, and a numeric scalar
// has none. GDScript rejects that assignment; so must the IR, because a backend
// that keeps scalars in machine registers has no Variant to copy from.
TEST_CASE("a scalar cannot fill a multi component member") {
	bool rejected = false;
	try {
		compile_to_ir("func test(v: float):\n\tvar r : Rect2 = Rect2(1, 2, 3, 4)\n\tr.size = v\n");
	} catch (const CompilerException &) {
		rejected = true;
	}
	REQUIRE((rejected && "a float written to Rect2.size is not a program"));

	// An unknown tag: no arm can take the float, so none is emitted and the
	// write goes to the host, which reports it the way Godot does.
	const IRProgram dynamic = compile_to_ir("func test(o, v: float):\n\to.size = v\n");
	const IRFunction &d = find_function(dynamic, "test");
	REQUIRE(count_opcode(d, IROpcode::VSET_INLINE) == 0);
	REQUIRE(count_opcode(d, IROpcode::VSET) >= 1);

	// The arms are still there for a value that can fill them.
	const IRProgram vector = compile_to_ir("func test(o, v: Vector2):\n\to.size = v\n");
	REQUIRE(count_opcode(find_function(vector, "test"), IROpcode::VSET_INLINE) > 0);
}

TEST_CASE("a computed member write goes to the host") {
	const IRProgram color = compile_to_ir(
			"func test():\n\tvar c : Color = Color()\n\tc.r8 = 128\n\treturn c\n");
	const IRFunction &f = find_function(color, "test");
	REQUIRE(count_opcode(f, IROpcode::VARIANT_SET) == 1);
	REQUIRE(count_opcode(f, IROpcode::VSET) == 0);
	REQUIRE(count_opcode(f, IROpcode::VSET_INLINE) == 0);

	// Pointer-backed: the subject really is a handle, so the named syscall stays.
	const IRProgram aabb = compile_to_ir(
			"func test():\n\tvar a : AABB = AABB()\n\ta.end = Vector3(1, 2, 3)\n\treturn a\n");
	const IRFunction &b = find_function(aabb, "test");
	REQUIRE(count_opcode(b, IROpcode::VSET) == 1);
	REQUIRE(count_opcode(b, IROpcode::VARIANT_SET) == 0);

	// Unknown tag: the inline arm is chosen at run time, the rest still goes to VSET.
	const IRProgram dynamic = compile_to_ir("func test(o, v):\n\to.r8 = v\n");
	const IRFunction &d = find_function(dynamic, "test");
	REQUIRE(count_opcode(d, IROpcode::VARIANT_SET) == 1);
	REQUIRE(count_opcode(d, IROpcode::VSET) == 1);

	REQUIRE(!compile_to_riscv(
					 "func test():\n\tvar c : Color = Color()\n\tc.r8 = 128\n\treturn c.r8\n")
					 .empty());
}

TEST_CASE("an element write is a variant operation") {
	// The mirror of an element read: only Object and the packed arrays have a set()
	// method, so a VCALL left every other built-in with "Nonexistent function 'set'".
	const IRProgram vector = compile_to_ir(
			"func test():\n\tvar v : Vector3 = Vector3()\n\tv[1] = 9.0\n\treturn v\n");
	const IRFunction &f = find_function(vector, "test");
	REQUIRE(count_opcode(f, IROpcode::VARIANT_SET) == 1);
	REQUIRE(count_vcalls(vector, f, "set") == 0);

	// The containers keep their own opcodes.
	const IRProgram array = compile_to_ir(
			"func test():\n\tvar a : Array = [1, 2]\n\ta[0] = 3\n\treturn a\n");
	const IRFunction &a = find_function(array, "test");
	REQUIRE(count_opcode(a, IROpcode::ARRAY_SET) == 1);
	REQUIRE(count_opcode(a, IROpcode::VARIANT_SET) == 0);
	const IRProgram dict = compile_to_ir(
			"func test(k):\n\tvar d : Dictionary = {}\n\td[k] = 3\n\treturn d\n");
	REQUIRE(count_opcode(find_function(dict, "test"), IROpcode::VARIANT_SET) == 0);

	// A value read out of a chain is a copy, so the element write travels back the
	// same way a member write does. Without it the assignment was simply lost.
	const IRProgram nested = compile_to_ir(
			"func test():\n\tvar t : Transform3D = Transform3D()\n"
			"\tt.basis[0] = Vector3(1, 2, 3)\n\treturn t\n");
	const IRFunction &n = find_function(nested, "test");
	REQUIRE(count_opcode(n, IROpcode::VARIANT_SET) == 1);
	REQUIRE(count_opcode(n, IROpcode::VSET) == 1);

	// Compound assignment resolves the same chain and writes back too.
	const IRProgram compound = compile_to_ir(
			"func test():\n\tvar t : Transform3D = Transform3D()\n"
			"\tt.basis[0] += Vector3(1, 2, 3)\n\treturn t\n");
	const IRFunction &c = find_function(compound, "test");
	REQUIRE(count_opcode(c, IROpcode::VARIANT_SET) == 1);
	REQUIRE(count_opcode(c, IROpcode::VSET) == 1);

	REQUIRE(!compile_to_riscv(
					 "func test():\n\tvar b : Basis = Basis()\n\tb[0] = Vector3(1, 2, 3)\n"
					 "\treturn b[0].x\n")
					 .empty());
}

// -= An unknown type =-

TEST_CASE("unknown member write tests the tag") {
	const IRProgram chained = compile_to_ir(
			"func test(n):\n\tn.position.x = 5\n");
	const IRFunction &f = find_function(chained, "test");

	const int arms = count_opcode(f, IROpcode::VSET_INLINE) + count_opcode(f, IROpcode::VGET_INLINE);
	REQUIRE(count_opcode(f, IROpcode::VSET_INLINE) > 0);
	// Per store and load: a Dictionary test, and a mask that catches every inline
	// payload before the handle-taking VSET/VGET.
	REQUIRE(count_tag_tests(f) == arms + 6);
	// One VSET for the Object fallback, one to write `position` back.
	REQUIRE(count_opcode(f, IROpcode::VSET) == 2);
	// Chain evaluated once: one VGET for `position`.
	REQUIRE(count_opcode(f, IROpcode::VGET) == 1);
	// Dictionary arms: one element read, two element writes.
	REQUIRE(count_dict_gets(f) == 1);
	REQUIRE(count_dict_sets(f) == 2);

	// Read path: VGET_INLINE branches, same issue (VGET throws on Vector2).
	const IRProgram read = compile_to_ir("func test(n):\n\treturn n.position.x\n");
	const IRFunction &r = find_function(read, "test");
	REQUIRE(count_opcode(r, IROpcode::VGET_INLINE) > 0);
	REQUIRE(count_tag_tests(r) == count_opcode(r, IROpcode::VGET_INLINE) + 4);
	// VGET fallback for Objects that carry `.x` as a property.
	REQUIRE(count_opcode(r, IROpcode::VGET) == 2);
	REQUIRE(count_dict_gets(r) == 2);

	// Non-inline member: Dictionary arm + VSET fallback only.
	const IRProgram plain_ir = compile_to_ir("func test(n):\n\tn.visible = true\n");
	const IRFunction &plain = find_function(plain_ir, "test");
	REQUIRE(count_tag_tests(plain) == 2);
	REQUIRE(count_dict_sets(plain) == 1);
	REQUIRE(count_opcode(plain, IROpcode::VSET) == 1);
	REQUIRE(count_opcode(plain, IROpcode::VSET_INLINE) == 0);
}

// -= Write-back through each kind of container =-

TEST_CASE("the copy travels back") {
	// Array element: ARRAY_GET, mutate, ARRAY_SET.
	const IRProgram element = compile_to_ir(
			"func test():\n\tvar a : Array = []\n\tvar i : int = 0\n\ta[i].x = 1.0\n");
	const IRFunction &e = find_function(element, "test");
	REQUIRE(count_opcode(e, IROpcode::ARRAY_GET) == 1);
	REQUIRE(count_opcode(e, IROpcode::ARRAY_SET) == 1);

	// Dictionary value: DICT_SET for the element store + DICT_SET for the write-back.
	const IRProgram entry = compile_to_ir(
			"func test():\n\tvar d : Dictionary = {}\n\td[\"p\"].x = 1.0\n");
	REQUIRE(count_dict_sets(find_function(entry, "test")) == 2);

	// Global inline type: write-back via STORE_GLOBAL.
	const IRProgram global = compile_to_ir(
			"var origin : Vector2 = Vector2(0, 0)\nfunc test():\n\torigin.x = 1.0\n");
	const IRFunction &g = find_function(global, "test");
	REQUIRE(count_opcode(g, IROpcode::VSET_INLINE) == 1);
	REQUIRE(count_opcode(g, IROpcode::STORE_GLOBAL) == 1);

	// Call result: no write-back (Object handle); call evaluated once.
	const IRProgram call_base = compile_to_ir(
			"func node():\n\treturn 1\nfunc test():\n\tnode().position = 5\n");
	REQUIRE(count_opcode(find_function(call_base, "test"), IROpcode::CALL) == 1);
}

// -= Through what the script extends =-

TEST_CASE("a base property travels back") {
	const IRProgram owned = compile_to_ir(
			"extends Node2D\nfunc test():\n\tvelocity.y = 1.0\n");
	const IRFunction &o = find_function(owned, "test");
	REQUIRE(count_opcode(o, IROpcode::GET_NODE) == 1);
	REQUIRE(count_opcode(o, IROpcode::VGET) == 1);
	REQUIRE(count_opcode(o, IROpcode::VSET) == 2);

	const IRProgram plain = compile_to_ir(
			"extends Node2D\nfunc test():\n\tvisible = true\n");
	const IRFunction &p = find_function(plain, "test");
	REQUIRE(count_opcode(p, IROpcode::VGET) == 0);
	REQUIRE(count_opcode(p, IROpcode::VSET) == 1);

	const IRProgram field = compile_to_ir(
			"class Foo:\n\tvar v : Vector2 = Vector2(0, 0)\n\tfunc bump():\n\t\tv.x = 1.0\n"
			"func test():\n\tFoo.new().bump()\n");
	const IRFunction &b = find_function(field, "@Foo.bump");
	REQUIRE(count_opcode(b, IROpcode::VSET_INLINE) == 1);
	REQUIRE(count_dict_sets(b) == 1);
}

// -= The optimizer =-

TEST_CASE("the optimizer keeps the copy") {
	// Copy propagation must not fold the MOVE into VSET_INLINE's INOUT operand;
	// doing so would alias `copy` back onto `v`.
	const IRProgram aliased = compile_to_ir(
			"func test():\n\tvar v : Vector2 = Vector2(1.0, 1.0)\n"
			"\tvar copy : Vector2 = v\n\tcopy.x = 5.0\n\treturn v\n",
			/*optimize=*/true);
	const IRFunction &f = find_function(aliased, "test");

	int source = -1;
	for (const auto &instr : f.instructions) {
		if (instr.opcode == IROpcode::MAKE_VECTOR2) {
			source = instr.operands[0].reg_index();
		}
	}
	REQUIRE(source >= 0);
	REQUIRE(inline_set_target(f) != source);

	// Verifier: INOUT must pass both def-before-read and operand-role checks.
	ir_verify(aliased, "optimized");
}

// -= The backend =-

TEST_CASE("the backend emits the store") {
	// Verify backend emits VSET_INLINE (previously threw "not yet implemented").
	REQUIRE(!compile_to_riscv(
					 "func test():\n\tvar v : Vector2 = Vector2(1, 2)\n\tv.x = 5.0\n\treturn v\n")
					 .empty());
	REQUIRE(!compile_to_riscv(
					 "func test():\n\tvar v : Vector4i = Vector4i(1, 2, 3, 4)\n\tv.w = 9\n\treturn v\n")
					 .empty());
	REQUIRE(!compile_to_riscv(
					 "func test():\n\tvar c : Color = Color(0, 0, 0)\n\tc.a = 0.25\n\treturn c\n")
					 .empty());
	REQUIRE(!compile_to_riscv("func test(n):\n\tn.position.x = 5\n").empty());
	// Cross-type stores: int->real and real->int convert, not reinterpret.
	REQUIRE(!compile_to_riscv(
					 "func test():\n\tvar v : Vector2 = Vector2(0, 0)\n\tv.x = 3\n\treturn v\n")
					 .empty());
	REQUIRE(!compile_to_riscv(
					 "func test():\n\tvar v : Vector2i = Vector2i(0, 0)\n\tv.x = 3.5\n\treturn v\n")
					 .empty());
}

// -= Freestanding calls =-

TEST_CASE("globals do not become self calls") {
	// Unimplemented globals must be refused (self-call fallback is silently dropped).
	REQUIRE(refuses("func test():\n\treturn print_debug(\"x\")\n"));
	REQUIRE(refuses("func test(x):\n\treturn weakref(x)\n"));
	REQUIRE(refuses("func test(p):\n\treturn preload(p)\n"));
	// Available normally, but refused by the restricted compilation policy.
	(void)compile_to_ir("func test():\n\trandomize()\n\tseed(1234)\n");
	for (const char *call : { "randomize()", "seed(1234)" }) {
		bool refused = false;
		try {
			Lexer lexer(std::string("func test():\n\t") + call + "\n");
			Parser parser(lexer.tokenize());
			CodeGenerator codegen;
			codegen.set_restricted(true);
			(void)codegen.generate(parser.parse());
		} catch (const CompilerException &) {
			refused = true;
		}
		REQUIRE(refused);
	}

	const IRProgram quat = compile_to_ir("func test():\n\treturn Quaternion(0, 0, 0, 1)\n");
	const IRFunction &quat_fn = find_function(quat, "test");
	REQUIRE(count_opcode(quat_fn, IROpcode::CONSTRUCT) == 1);
	REQUIRE(count_vcalls(quat, quat_fn, "Quaternion") == 0);
	REQUIRE(count_opcode(quat_fn, IROpcode::CALL) == 0);

	// Non-global name: legitimate self-call on the owner node.
	const IRProgram self_call = compile_to_ir("func test():\n\tqueue_free()\n");
	REQUIRE(count_vcalls(self_call, find_function(self_call, "test"), "queue_free") == 1);

	// Local function shadows the global.
	const IRProgram shadowed = compile_to_ir(
			"func weakref(x):\n\treturn x\nfunc test():\n\treturn weakref(1)\n");
	REQUIRE(count_opcode(find_function(shadowed, "test"), IROpcode::CALL) == 1);

	// typeof(): guest-side tag read via TYPE_OF opcode.
	const IRProgram type_of = compile_to_ir("func test(x):\n\treturn typeof(x)\n");
	const IRFunction &t = find_function(type_of, "test");
	REQUIRE(count_opcode(t, IROpcode::TYPE_OF) == 1);
	REQUIRE(count_vcalls(type_of, t, "typeof") == 0);
	REQUIRE(refuses("func test(x):\n\treturn typeof(x, 1)\n"));
}

// -= Containers the Array walk cannot reach =-

TEST_CASE("iterating a non array") {
	// Packed array: VCALL size()/get(), not ECALL_ARRAY_SIZE/AT (Array-only).
	const IRProgram packed = compile_to_ir(
			"func test():\n\tvar p = PackedInt32Array([1, 2])\n\tvar t = 0\n"
			"\tfor v in p:\n\t\tt += v\n\treturn t\n");
	const IRFunction &p = find_function(packed, "test");
	REQUIRE(count_vcalls(packed, p, "size") == 1);
	REQUIRE(count_vcalls(packed, p, "get") == 1);
	// No Array-only syscall: ECALL_ARRAY_SIZE/AT/BATCH would throw on a packed
	// array. (With fast arrays the walk may also have a region around it, whose
	// ECALL_PACKED_ACQUIRE/RELEASE are made for packed arrays.)
	int array_syscalls = 0;
	for (const IRInstruction &instr : p.instructions) {
		if (instr.opcode == IROpcode::CALL_SYSCALL && instr.operands.size() >= 2 &&
			(instr.operands[1].immediate() == ECALL_ARRAY_SIZE ||
			 instr.operands[1].immediate() == ECALL_ARRAY_AT ||
			 instr.operands[1].immediate() == ECALL_ARRAY_BATCH)) {
			array_syscalls++;
		}
	}
	REQUIRE(array_syscalls == 0);

	// Array uses syscalls, not VCALL.
	const IRProgram array = compile_to_ir(
			"func test():\n\tvar a : Array = [1, 2]\n\tfor v in a:\n\t\tpass\n");
	REQUIRE(count_vcalls(array, find_function(array, "test"), "size") == 0);

	// String: own syscalls, no VCALL. See test_strings.cpp.
	const IRProgram walked = compile_to_ir(
			"func test():\n\tvar s : String = \"hi\"\n\tfor c in s:\n\t\tpass\n");
	REQUIRE(count_vcalls(walked, find_function(walked, "test"), "size") == 0);
	REQUIRE(count_vcalls(walked, find_function(walked, "test"), "get") == 0);
}
