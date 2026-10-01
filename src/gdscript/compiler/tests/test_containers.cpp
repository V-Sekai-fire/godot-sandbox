// Element access on an Array or a Dictionary.
//
// `a[i]`, `a[i] = v` and `a.append(v)` used to be Variant::call("get"), ("set")
// and ("append"): a StringName built from the method name and a builtin-method
// lookup, per element touched, which is where a fetch-decode-execute loop over
// an Array spends nearly all of its time. On a known Array with a known int
// index all three lower to one system call instead.
//
// What "known" means is the subject of half of this file: the backend reads the
// container's scoped index and the element index straight out of the Variants,
// with no type check, so anything less certain has to use the generic Variant
// get syscall.
//
// A dynamic negative index -- `a[i]` where i may be -1 -- is normalised in the
// guest. A negative literal is marked for the host to wrap in ECALL_ARRAY_AT.
// That part is machine code rather than IR, so it is covered in
// tests/tests/test_gdscript_compiler.gd against the engine's own answer.
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

// ECALL_DICTIONARY_OPS calls, by Dictionary_Op.
static int count_dict_ops(const IRFunction &func, int64_t op) {
	int count = 0;
	for (const auto &instr : func.instructions) {
		if (instr.opcode == IROpcode::CALL_SYSCALL && instr.operands.size() >= 3 &&
			instr.operands[1].immediate() == 524 &&
			instr.operands[2].immediate() == op) {
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

// The whole pipeline, so an opcode the backend cannot expand fails here rather
// than in a Godot project.
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

TEST_CASE("array element access") {
	const std::string source =
			"func read(a : Array, i : int):\n"
			"\treturn a[i]\n"
			"\n"
			"func write(a : Array, i : int, v):\n"
			"\ta[i] = v\n"
			"\n"
			"func both(a : Array, i : int, j : int):\n"
			"\ta[i] = a[j]\n";

	const IRProgram ir = compile_to_ir(source);

	REQUIRE(count_opcode(find_function(ir, "read"), IROpcode::ARRAY_GET) == 1);
	REQUIRE(count_vcalls(ir, find_function(ir, "read"), "get") == 0);

	REQUIRE(count_opcode(find_function(ir, "write"), IROpcode::ARRAY_SET) == 1);
	REQUIRE(count_vcalls(ir, find_function(ir, "write"), "set") == 0);

	const IRFunction &both = find_function(ir, "both");
	REQUIRE(count_opcode(both, IROpcode::ARRAY_GET) == 1);
	REQUIRE(count_opcode(both, IROpcode::ARRAY_SET) == 1);

	// An Array literal is as known as a declared one.
	const IRProgram literal = compile_to_ir(
			"func test(i : int):\n"
			"\tvar a = [1, 2, 3]\n"
			"\treturn a[i]\n");
	REQUIRE(count_opcode(find_function(literal, "test"), IROpcode::ARRAY_GET) == 1);

	compile_to_machine_code(source);
}

TEST_CASE("unknown container uses variant get") {
	// The backend reads the container's scoped index out of the Variant without
	// checking the type tag, so a container of unknown type may not take that
	// path.
	const IRProgram untyped_object = compile_to_ir(
			"func test(a, i : int):\n\treturn a[i]\n");
	REQUIRE(count_opcode(find_function(untyped_object, "test"), IROpcode::ARRAY_GET) == 0);
	REQUIRE(count_syscalls(find_function(untyped_object, "test"), ECALL_VARIANT_GET) == 1);
	REQUIRE(count_vcalls(untyped_object, find_function(untyped_object, "test"), "get") == 0);

	// Same for the index, which is read as an int64 out of its payload.
	const IRProgram untyped_index = compile_to_ir(
			"func test(a : Array, i):\n\treturn a[i]\n");
	REQUIRE(count_opcode(find_function(untyped_index, "test"), IROpcode::ARRAY_GET) == 0);
	REQUIRE(count_syscalls(find_function(untyped_index, "test"), ECALL_VARIANT_GET) == 1);
	REQUIRE(count_vcalls(untyped_index, find_function(untyped_index, "test"), "get") == 0);

	// Packed array: uses the same generic indexed Variant operation.
	const IRProgram packed_index = compile_to_ir(
			"func test(p : PackedInt32Array, i : int):\n\treturn p[i]\n");
	REQUIRE(count_opcode(find_function(packed_index, "test"), IROpcode::ARRAY_GET) == 0);
	REQUIRE(count_syscalls(find_function(packed_index, "test"), ECALL_VARIANT_GET) == 1);
	REQUIRE(count_vcalls(packed_index, find_function(packed_index, "test"), "get") == 0);

	// String: own syscall, no get(). See test_strings.cpp.
	const IRProgram string_index = compile_to_ir(
			"func test(s : String, i : int):\n\treturn s[i]\n");
	REQUIRE(count_vcalls(string_index, find_function(string_index, "test"), "get") == 0);

	// And a write to an unknown container: the mirror of the read, since only Object
	// and the packed arrays answer a set() call.
	const IRProgram untyped_write = compile_to_ir(
			"func test(a, i : int, v):\n\ta[i] = v\n");
	REQUIRE(count_opcode(find_function(untyped_write, "test"), IROpcode::ARRAY_SET) == 0);
	REQUIRE(count_opcode(find_function(untyped_write, "test"), IROpcode::VARIANT_SET) == 1);
	REQUIRE(count_vcalls(untyped_write, find_function(untyped_write, "test"), "set") == 0);
}

TEST_CASE("array append") {
	const std::string source =
			"func test(v):\n"
			"\tvar a : Array = []\n"
			"\ta.append(v)\n"
			"\ta.push_back(v)\n"
			"\treturn a\n";

	const IRProgram ir = compile_to_ir(source);
	const IRFunction &test = find_function(ir, "test");
	REQUIRE(count_opcode(test, IROpcode::ARRAY_APPEND) == 2);
	REQUIRE(count_vcalls(ir, test, "append") == 0);
	REQUIRE(count_vcalls(ir, test, "push_back") == 0);

	// Any other method, and any receiver that is not a known Array, is a VCALL.
	const IRProgram mixed = compile_to_ir(
			"func test(a : Array, b, v):\n"
			"\ta.sort()\n"
			"\tb.append(v)\n");
	const IRFunction &other = find_function(mixed, "test");
	REQUIRE(count_opcode(other, IROpcode::ARRAY_APPEND) == 0);
	REQUIRE(count_vcalls(mixed, other, "sort") == 1);
	REQUIRE(count_vcalls(mixed, other, "append") == 1);

	compile_to_machine_code(source);
}

TEST_CASE("array iteration is batched") {
	const std::string source =
			"func sum(a : Array):\n"
			"\tvar total = 0\n"
			"\tfor value in a:\n"
			"\t\ttotal += value\n"
			"\treturn total\n";
	const IRProgram ir = compile_to_ir(source, true);
	const IRFunction &sum = find_function(ir, "sum");
	REQUIRE(count_syscalls(sum, ECALL_ARRAY_BATCH) == 1);
	REQUIRE(count_syscalls(sum, ECALL_ARRAY_SIZE) == 0);
	REQUIRE(count_syscalls(sum, ECALL_ARRAY_AT) == 0);
	REQUIRE(count_opcode(sum, IROpcode::BATCH_GET) == 1);
	// The scope whose release must drop the batch's sixteen slots is named on
	// the function, not inferred from what sits next to the syscall.
	REQUIRE(sum.array_batch_scopes.size() == 1);
	bool batch_scope_released = false;
	for (const auto &instr : sum.instructions) {
		if (instr.opcode == IROpcode::SCOPE_RELEASE &&
			int(instr.operands.at(0).immediate()) == sum.array_batch_scopes[0].first) {
			batch_scope_released = true;
		}
	}
	REQUIRE(batch_scope_released);
	compile_to_machine_code(source);
}

TEST_CASE("dictionary element access") {
	constexpr int64_t DICT_OP_GET = 0;

	const std::string source =
			"func read(d : Dictionary, k):\n"
			"\treturn d[k]\n"
			"\n"
			"func write(d : Dictionary, k, v):\n"
			"\td[k] = v\n"
			"\n"
			"func by_name(d : Dictionary):\n"
			"\td.count = 1\n"
			"\treturn d.count\n";

	const IRProgram ir = compile_to_ir(source);

	// A Dictionary is keyed by any Variant, so unlike an Array index the key
	// needs no type of its own: the host is handed a pointer either way.
	REQUIRE(count_dict_ops(find_function(ir, "read"), DICT_OP_GET) == 1);
	REQUIRE(count_vcalls(ir, find_function(ir, "read"), "get") == 0);

	REQUIRE(count_opcode(find_function(ir, "write"), IROpcode::DICT_SET) == 1);
	REQUIRE(count_vcalls(ir, find_function(ir, "write"), "set") == 0);

	// `d.key` means `d["key"]` in GDScript, and takes the same path.
	const IRFunction &by_name = find_function(ir, "by_name");
	REQUIRE(count_opcode(by_name, IROpcode::DICT_GET_CONST) == 1);
	REQUIRE(count_opcode(by_name, IROpcode::DICT_SET_CONST) == 1);
	REQUIRE(count_opcode(by_name, IROpcode::VGET) == 0);
	REQUIRE(count_opcode(by_name, IROpcode::VSET) == 0);

	compile_to_machine_code(source);
}

TEST_CASE("element access survives the optimizer") {
	// An unread write is still a write, and a read that throws on an
	// out-of-range index is still a read: IR_SIDE_EFFECTS keeps DCE off both.
	const std::string source =
			"func test(a : Array, d : Dictionary, i : int):\n"
			"\ta[i] = 1\n"
			"\td[i] = 2\n"
			"\ta.append(3)\n"
			"\tvar unused = a[i]\n"
			"\treturn 0\n";

	const IRProgram ir = compile_to_ir(source, true);
	const IRFunction &test = find_function(ir, "test");
	REQUIRE(count_opcode(test, IROpcode::ARRAY_SET) == 1);
	REQUIRE(count_opcode(test, IROpcode::DICT_SET) == 1);
	REQUIRE(count_opcode(test, IROpcode::ARRAY_APPEND) == 1);
	REQUIRE(count_opcode(test, IROpcode::ARRAY_GET) == 1);

	compile_to_machine_code(source);
}

// `{k: v}` evaluates k; `{k = v}` is Lua-style (key is the string "k").
TEST_CASE("dictionary literal keys") {
	// `{k: 2}` evaluates k; "k" not in the string pool.
	{
		const IRProgram ir = compile_to_ir(
				"func f():\n"
				"\tvar k = 7\n"
				"\treturn {k: 2}\n");
		const IRFunction &f = find_function(ir, "f");
		REQUIRE(count_opcode(f, IROpcode::MAKE_DICTIONARY) == 1);
		for (const auto &str : ir.string_constants) {
			REQUIRE(str != "k");
		}
	}

	// `{k = 2}` is Lua-style; "k" is in the string pool.
	{
		const IRProgram ir = compile_to_ir(
				"func f():\n"
				"\tvar k = 7\n"
				"\treturn {k = 2}\n");
		const IRFunction &f = find_function(ir, "f");
		REQUIRE(count_opcode(f, IROpcode::MAKE_DICTIONARY) == 1);
		REQUIRE(count_opcode(f, IROpcode::LOAD_STRING) == 1);
		bool found = false;
		for (const auto &str : ir.string_constants) {
			found = found || str == "k";
		}
		REQUIRE(found);
	}

	// Expression keys (not just identifiers).
	compile_to_machine_code(
			"func f():\n"
			"\tvar k = 7\n"
			"\treturn {k: 1, \"s\": 2, 1 + 1: 3, Vector2.ZERO: 4}\n");

	// String Lua-style key + trailing comma.
	compile_to_machine_code(
			"func f():\n"
			"\treturn {a = 1, \"b c\" = 2,}\n");

	// Mixing styles is refused.
	{
		bool refused = false;
		try {
			compile_to_ir("func f():\n\tvar v = 1\n\treturn {a = 1, v: 2}\n");
		} catch (const CompilerException &) {
			refused = true;
		}
		REQUIRE(refused);
	}
	{
		bool refused = false;
		try {
			compile_to_ir("func f():\n\tvar v = 1\n\treturn {v: 2, a = 1}\n");
		} catch (const CompilerException &) {
			refused = true;
		}
		REQUIRE(refused);
	}
	// A Lua-style key has to be a name, not any expression.
	{
		bool refused = false;
		try {
			compile_to_ir("func f():\n\treturn {1 = 2}\n");
		} catch (const CompilerException &) {
			refused = true;
		}
		REQUIRE(refused);
	}

	std::cout << "  dictionary literal keys OK" << std::endl;
}

TEST_CASE("known container methods lower to syscalls") {
	const std::string source =
			"func sizes(a : Array, d : Dictionary, s : String):\n"
			"\treturn a.size() + d.size() + s.length()\n";
	const IRProgram sizes_ir = compile_to_ir(source);
	const IRFunction &sizes = find_function(sizes_ir, "sizes");
	REQUIRE(count_vcalls(sizes_ir, sizes, "size") == 0);
	REQUIRE(count_vcalls(sizes_ir, sizes, "length") == 0);
	REQUIRE(count_syscalls(sizes, ECALL_ARRAY_SIZE) == 1);
	REQUIRE(count_syscalls(sizes, ECALL_STRING_SIZE) == 1);
	REQUIRE(count_dict_ops(sizes, dictionary_op(Dictionary_Op::GET_SIZE)) == 1);

	const std::string dict_source =
			"func query(d : Dictionary):\n"
			"\tif d.has(1):\n"
			"\t\treturn d.get(1)\n"
			"\tvar n = d.keys().size() + d.values().size()\n"
			"\tvar fallback = d.get(2, 0)\n"
			"\td.clear()\n"
			"\treturn n + fallback\n";
	const IRProgram query_ir = compile_to_ir(dict_source);
	const IRFunction &query = find_function(query_ir, "query");
	REQUIRE(count_opcode(query, IROpcode::VCALL) == 0);
	REQUIRE(count_dict_ops(query, dictionary_op(Dictionary_Op::HAS)) == 1);
	REQUIRE(count_dict_ops(query, dictionary_op(Dictionary_Op::GET)) == 1);
	REQUIRE(count_dict_ops(query, dictionary_op(Dictionary_Op::GET_OR_DEFAULT)) == 1);
	REQUIRE(count_dict_ops(query, dictionary_op(Dictionary_Op::GET_KEYS)) == 1);
	REQUIRE(count_dict_ops(query, dictionary_op(Dictionary_Op::GET_VALUES)) == 1);
	REQUIRE(count_dict_ops(query, dictionary_op(Dictionary_Op::CLEAR)) == 1);
	REQUIRE(count_syscalls(query, ECALL_ARRAY_SIZE) == 2);

	const std::string empty_source =
			"func empty(a : Array, d : Dictionary, s : String):\n"
			"\treturn a.is_empty() and d.is_empty() and s.is_empty()\n";
	const IRProgram empty_ir = compile_to_ir(empty_source);
	const IRFunction &empty = find_function(empty_ir, "empty");
	REQUIRE(count_vcalls(empty_ir, empty, "is_empty") == 0);
	REQUIRE(count_syscalls(empty, ECALL_ARRAY_SIZE) == 1);
	REQUIRE(count_syscalls(empty, ECALL_STRING_SIZE) == 1);
	REQUIRE(count_dict_ops(empty, dictionary_op(Dictionary_Op::GET_SIZE)) == 1);
	REQUIRE(count_opcode(empty, IROpcode::CMP_EQ) == 3);

	compile_to_machine_code(source);
	compile_to_machine_code(dict_source);
	compile_to_machine_code(empty_source);

	std::cout << "  \u2713 known container methods skip the VCALL" << std::endl;
}

TEST_CASE("unknown receiver keeps the vcall") {
	const IRProgram untyped_ir = compile_to_ir("func size_of(a):\n\treturn a.size()\n");
	const IRFunction &untyped = find_function(untyped_ir, "size_of");
	REQUIRE(count_vcalls(untyped_ir, untyped, "size") == 1);
	REQUIRE(count_syscalls(untyped, ECALL_ARRAY_SIZE) == 0);

	// Untyped receiver keeps the VCALL.
	const IRProgram defaulted_ir = compile_to_ir("func get_or(d):\n\treturn d.get(1, 0)\n");
	const IRFunction &defaulted = find_function(defaulted_ir, "get_or");
	REQUIRE(count_vcalls(defaulted_ir, defaulted, "get") == 1);
	REQUIRE(count_dict_ops(defaulted, dictionary_op(Dictionary_Op::GET)) == 0);
	REQUIRE(count_dict_ops(defaulted, dictionary_op(Dictionary_Op::GET_OR_DEFAULT)) == 0);

	const IRProgram popped_ir = compile_to_ir("func pop(a : Array):\n\treturn a.pop_back()\n");
	const IRFunction &popped = find_function(popped_ir, "pop");
	REQUIRE(count_vcalls(popped_ir, popped, "pop_back") == 1);

	std::cout << "  \u2713 an untyped receiver keeps the VCALL" << std::endl;
}

// ---------------------------------------------------------------------------
// Properties, and the falsifiability check that keeps them honest.
// ---------------------------------------------------------------------------

#include "property_support.h"

namespace {

// An array literal of n integers, and the function that adds them up.
struct IntArray {
	std::vector<int64_t> values;

	std::string source() const {
		std::string literal = "[";
		for (size_t i = 0; i < values.size(); i++) {
			literal += (i == 0 ? "" : ", ") + std::to_string(values[i]);
		}
		literal += "]";
		return "func f():\n\tvar a = " + literal +
				"\n\tvar total = 0\n\tfor v in a:\n\t\ttotal += v\n\treturn total\n";
	}
};

// An array literal is built by one MAKE_ARRAY, whatever its length. The IR
// interpreter cannot run these programs -- MAKE_ARRAY needs the host Variant
// API -- so the property is about what the compiler emits rather than what the
// program computes.
size_t make_array_count(const std::string &source) {
	Compiler compiler;
	CompilerOptions options;
	options.output_elf = false;
	auto ir = compiler.compile_to_ir(source, options);
	if (!ir.has_value()) {
		return SIZE_MAX;
	}
	size_t count = 0;
	for (const IRFunction &function : ir->functions) {
		if (function.name != "f") {
			continue;
		}
		for (const IRInstruction &instruction : function.instructions) {
			if (instruction.opcode == IROpcode::MAKE_ARRAY) {
				count++;
			}
		}
	}
	return count;
}

IntArray generate_int_array(witness::RNG &rng, const witness::Level &level) {
	const uint32_t count = rng.uint_range(0, std::max<uint32_t>(1, uint32_t(level.fin_bound) / 32));
	const int64_t bound = std::max<int64_t>(2, level.fin_bound);
	IntArray array;
	for (uint32_t i = 0; i < count; i++) {
		array.values.push_back(rng.int64_range(-bound, bound));
	}
	return array;
}

} // namespace

TEST_CASE("property: an array literal of any length is built by one MAKE_ARRAY") {
	PROP_HOLDS(IntArray, "one array literal is one MAKE_ARRAY",
			   &generate_int_array,
			   [](const IntArray &array) { return make_array_count(array.source()) == 1; });
}

TEST_CASE("falsifiability: the array generator reaches arrays with elements in them") {
	// False on purpose: a generator that only ever produced the empty array
	// would satisfy the property above by iterating nothing.
	PROP_FALSIFIABLE(IntArray, "every generated array is empty",
					 &generate_int_array,
					 [](const IntArray &array) { return array.values.empty(); });
}
