// Property accessors: inline (`set(v): ... get: ...`) and named (`set = f, get = g`).
#include "../codegen.h"
#include "../compiler_exception.h"
#include "../ir_optimizer.h"
#include "../ir_verifier.h"
#include "../lexer.h"
#include "../parser.h"
#include "../riscv_codegen.h"
#include "witness/doctest.h"
#include <iostream>
#include <stdexcept>
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

static bool has_function(const IRProgram &ir, const std::string &name) {
	for (const auto &func : ir.functions) {
		if (func.name == name) {
			return true;
		}
	}
	return false;
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

// Direct calls to `name`, by the callee in operand 0.
static int count_calls(const IRProgram &ir, const IRFunction &func, const std::string &name) {
	int count = 0;
	for (const auto &instr : func.instructions) {
		if (instr.opcode == IROpcode::CALL && !instr.operands.empty() &&
			ir.strings[instr.operands[0].string_id] == name) {
			count++;
		}
	}
	return count;
}

// The callees of `func`, in order, so a get-then-set can be told from a
// set-then-get.
static std::vector<std::string> call_sequence(const IRProgram &ir, const IRFunction &func) {
	std::vector<std::string> names;
	for (const auto &instr : func.instructions) {
		if (instr.opcode == IROpcode::CALL && !instr.operands.empty()) {
			names.push_back(ir.strings[instr.operands[0].string_id]);
		}
	}
	return names;
}

static const IRGlobalVar &find_global(const IRProgram &ir, const std::string &name) {
	for (const auto &global : ir.globals) {
		if (global.name == name) {
			return global;
		}
	}
	throw std::runtime_error("Global not found: " + name);
}

// The whole pipeline, so an accessor the backend cannot lower fails here
// rather than in a Godot project.
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

static CompilerException compile_failure(const std::string &source) {
	try {
		compile_to_ir(source);
	} catch (const CompilerException &e) {
		return e;
	}
	FAIL("expected this source to be refused");
	throw std::runtime_error("unreachable");
}

// -= Tests =-

TEST_CASE("inline bodies are lifted") {
	const IRProgram ir = compile_to_ir(
			"var store = 0\n"
			"var hp = 10:\n"
			"\tset(v):\n"
			"\t\tstore = v\n"
			"\tget:\n"
			"\t\treturn store\n"
			"func test():\n"
			"\thp = 3\n"
			"\treturn hp\n");

	REQUIRE(has_function(ir, "@hp_setter"));
	REQUIRE(has_function(ir, "@hp_getter"));

	const IRFunction &test = find_function(ir, "test");
	REQUIRE(count_calls(ir, test, "@hp_setter") == 1);
	REQUIRE(count_calls(ir, test, "@hp_getter") == 1);
	REQUIRE(count_opcode(test, IROpcode::LOAD_GLOBAL) == 0);
	REQUIRE(count_opcode(test, IROpcode::STORE_GLOBAL) == 0);

	REQUIRE(find_function(ir, "@hp_setter").parameters.size() == 1);
	REQUIRE(find_function(ir, "@hp_getter").parameters.empty());

	std::cout << "  inline accessors are lifted" << std::endl;
}

TEST_CASE("named accessors") {
	const IRProgram ir = compile_to_ir(
			"var store = 0\n"
			"var mp = 5:\n"
			"\tset = _set_mp,\n"
			"\tget = _get_mp\n"
			"func _set_mp(v):\n"
			"\tstore = v\n"
			"func _get_mp():\n"
			"\treturn store\n"
			"func test():\n"
			"\tmp = 4\n"
			"\treturn mp\n");

	REQUIRE(!has_function(ir, "@mp_setter"));
	REQUIRE(!has_function(ir, "@mp_getter"));

	const IRFunction &test = find_function(ir, "test");
	REQUIRE(count_calls(ir, test, "_set_mp") == 1);
	REQUIRE(count_calls(ir, test, "_get_mp") == 1);

	// One-line spelling.
	const IRProgram one_line = compile_to_ir(
			"var store = 0\n"
			"var mp: get = _get_mp, set = _set_mp\n"
			"func _set_mp(v):\n"
			"\tstore = v\n"
			"func _get_mp():\n"
			"\treturn store\n"
			"func test():\n"
			"\tmp = 4\n"
			"\treturn mp\n");
	const IRFunction &one_line_test = find_function(one_line, "test");
	REQUIRE(count_calls(one_line, one_line_test, "_set_mp") == 1);
	REQUIRE(count_calls(one_line, one_line_test, "_get_mp") == 1);

	std::cout << "  named accessors are called as written" << std::endl;
}

TEST_CASE("a property means its storage inside its own accessor") {
	const IRProgram ir = compile_to_ir(
			"var hp = 1:\n"
			"\tset(v):\n"
			"\t\thp = v * 2\n"
			"\tget:\n"
			"\t\treturn hp + 1\n"
			"func test():\n"
			"\thp = 5\n"
			"\treturn hp\n");

	const IRFunction &setter = find_function(ir, "@hp_setter");
	REQUIRE(count_opcode(setter, IROpcode::STORE_GLOBAL) == 1);
	REQUIRE(count_calls(ir, setter, "@hp_setter") == 0);

	const IRFunction &getter = find_function(ir, "@hp_getter");
	REQUIRE(count_opcode(getter, IROpcode::LOAD_GLOBAL) == 1);
	REQUIRE(count_calls(ir, getter, "@hp_getter") == 0);

	// `set = f` also gets direct storage access.
	const IRProgram named = compile_to_ir(
			"var ind = 5:\n"
			"\tset = _si\n"
			"func _si(v):\n"
			"\tind = v\n"
			"func test():\n"
			"\tind = 1\n");
	const IRFunction &si = find_function(named, "_si");
	REQUIRE(count_opcode(si, IROpcode::STORE_GLOBAL) == 1);
	REQUIRE(count_calls(named, si, "_si") == 0);

	// Cross-property access goes through the accessor.
	const IRProgram other = compile_to_ir(
			"var a = 1:\n"
			"\tget:\n"
			"\t\treturn 7\n"
			"var b = 2:\n"
			"\tget:\n"
			"\t\treturn a\n");
	REQUIRE(count_calls(other, find_function(other, "@b_getter"), "@a_getter") == 1);

	std::cout << "  an accessor reaches the storage behind its own property" << std::endl;
}

TEST_CASE("the initializer does not run the setter") {
	const IRProgram ir = compile_to_ir(
			"var seen = 0\n"
			"var hp = 10:\n"
			"\tset(v):\n"
			"\t\tseen += 1\n"
			"\t\thp = v\n");
	REQUIRE(find_global(ir, "hp").init_type == IRGlobalVar::InitType::INT);
	REQUIRE(count_calls(ir, ir.member_init, "@hp_setter") == 0);

	const IRProgram runtime = compile_to_ir(
			"var base = 4\n"
			"var hp = base + 1:\n"
			"\tset(v):\n"
			"\t\thp = v\n");
	REQUIRE(find_global(runtime, "hp").init_type == IRGlobalVar::InitType::RUNTIME);
	REQUIRE(count_calls(runtime, runtime.member_init, "@hp_setter") == 0);
	REQUIRE(count_opcode(runtime.member_init, IROpcode::STORE_GLOBAL) == 1);

	std::cout << "  the declaration's own value is written, not set" << std::endl;
}

TEST_CASE("compound assignment gets then sets") {
	const IRProgram ir = compile_to_ir(
			"var store = 0\n"
			"var hp = 1:\n"
			"\tset(v):\n"
			"\t\tstore = v\n"
			"\tget:\n"
			"\t\treturn store\n"
			"func test():\n"
			"\thp += 5\n");

	const std::vector<std::string> calls = call_sequence(ir, find_function(ir, "test"));
	REQUIRE(calls.size() == 2);
	REQUIRE(calls[0] == "@hp_getter");
	REQUIRE(calls[1] == "@hp_setter");

	std::cout << "  a compound assignment reads before it writes" << std::endl;
}

TEST_CASE("one sided properties") {
	// Getter only: writes go to storage.
	const IRProgram getter_only = compile_to_ir(
			"var ro = 1:\n"
			"\tget:\n"
			"\t\treturn 99\n"
			"func test():\n"
			"\tro = 5\n"
			"\treturn ro\n");
	const IRFunction &ro_test = find_function(getter_only, "test");
	REQUIRE(count_opcode(ro_test, IROpcode::STORE_GLOBAL) == 1);
	REQUIRE(count_calls(getter_only, ro_test, "@ro_getter") == 1);

	// Setter only: reads come from storage.
	const IRProgram setter_only = compile_to_ir(
			"var wo = 2:\n"
			"\tset(v):\n"
			"\t\two = v * 3\n"
			"func test():\n"
			"\two = 4\n"
			"\treturn wo\n");
	const IRFunction &wo_test = find_function(setter_only, "test");
	REQUIRE(count_opcode(wo_test, IROpcode::LOAD_GLOBAL) == 1);
	REQUIRE(count_calls(setter_only, wo_test, "@wo_setter") == 1);

	std::cout << "  the missing half falls back to the storage" << std::endl;
}

TEST_CASE("an export publishes both accessors") {
	// Missing half is generated for the host.
	const IRProgram ir = compile_to_ir(
			"@export var hp = 10:\n"
			"\tset(v):\n"
			"\t\thp = v\n"
			"func test():\n"
			"\treturn hp\n");

	const IRGlobalVar &hp = find_global(ir, "hp");
	REQUIRE(hp.is_property);
	REQUIRE(hp.setter_function == "@hp_setter");
	REQUIRE(hp.getter_function == "@hp_getter");
	REQUIRE(has_function(ir, "@hp_getter"));

	// Guest still reads storage directly (no getter of its own).
	REQUIRE(count_opcode(find_function(ir, "test"), IROpcode::LOAD_GLOBAL) == 1);
	REQUIRE(count_calls(ir, find_function(ir, "test"), "@hp_getter") == 0);

	// No accessors: empty names, host uses direct path.
	const IRProgram plain = compile_to_ir("@export var speed = 1.0\n");
	REQUIRE(find_global(plain, "speed").is_property);
	REQUIRE(find_global(plain, "speed").setter_function.empty());
	REQUIRE(find_global(plain, "speed").getter_function.empty());

	std::cout << "  an exported property answers Godot both ways" << std::endl;
}

TEST_CASE("accessors reach machine code") {
	compile_to_machine_code(
			"var store = 0\n"
			"@export var hp: int = 10:\n"
			"\tset(v):\n"
			"\t\thp = v if v > 0 else 0\n"
			"\tget:\n"
			"\t\treturn hp\n"
			"@export var mp = 5:\n"
			"\tset = _set_mp\n"
			"var lazy:\n"
			"\tget(): return store + 1\n"
			"func _set_mp(v):\n"
			"\tstore = v\n"
			"func test():\n"
			"\thp += 1\n"
			"\tmp = 2\n"
			"\treturn [hp, mp, lazy]\n");

	// No type, no initializer: NIL is a valid initial storage value.
	const IRProgram ir = compile_to_ir(
			"var store = 1\n"
			"var lazy:\n"
			"\tget:\n"
			"\t\treturn store\n");
	REQUIRE(find_global(ir, "lazy").init_type == IRGlobalVar::InitType::NULL_VAL);

	std::cout << "  accessors survive the optimizer and the backend" << std::endl;
}

TEST_CASE("a one line accessor body") {
	const IRProgram one_line = compile_to_ir(
			"var store = 0\n"
			"var hp = 1: set(v): store = v\n"
			"func test():\n"
			"\thp = 3\n");
	const IRProgram block = compile_to_ir(
			"var store = 0\n"
			"var hp = 1:\n"
			"\tset(v):\n"
			"\t\tstore = v\n"
			"func test():\n"
			"\thp = 3\n");

	const IRFunction &one_line_setter = find_function(one_line, "@hp_setter");
	const IRFunction &block_setter = find_function(block, "@hp_setter");
	REQUIRE(one_line_setter.instructions.size() == block_setter.instructions.size());
	for (size_t i = 0; i < block_setter.instructions.size(); i++) {
		REQUIRE(one_line_setter.instructions[i].to_string() ==
				block_setter.instructions[i].to_string());
	}

	// `get(): ...` too, and both accessors on the declaration's own line.
	compile_to_machine_code(
			"var store = 0\n"
			"var hp = 1: set(v): store = v, get(): return store\n"
			"func test():\n"
			"\thp += 1\n"
			"\treturn hp\n");

	std::cout << "  a one-line accessor is the same body" << std::endl;
}

TEST_CASE("a named setter may suspend") {
	const IRProgram ir = compile_to_ir(
			"signal resume\n"
			"var active = false: set = set_active\n"
			"func set_active(value):\n"
			"\tactive = value\n"
			"\tawait resume\n"
			"func enable():\n"
			"\tactive = true\n");

	const IRFunction &enable = find_function(ir, "enable");
	REQUIRE(count_opcode(enable, IROpcode::CALL_HOSTED) == 1);
	REQUIRE(count_opcode(enable, IROpcode::CALL) == 0);
	REQUIRE(count_opcode(find_function(ir, "set_active"), IROpcode::STORE_GLOBAL) == 1);
	compile_to_machine_code(
			"signal resume\n"
			"var active = false: set = set_active\n"
			"func set_active(value):\n"
			"\tactive = value\n"
			"\tawait resume\n"
			"func enable():\n"
			"\tactive = true\n");

	std::cout << "  a property assignment starts its setter coroutine" << std::endl;
}

TEST_CASE("refusals") {
	// Named accessor: must exist with correct arity.
	compile_failure("var x = 1:\n\tset = missing\n");
	compile_failure("var x = 1:\n\tset = f\nfunc f(a, b):\n\treturn a\n");
	compile_failure("var x = 1:\n\tget = f\nfunc f(a):\n\treturn a\n");

	compile_failure("const C = 1:\n\tget:\n\t\treturn 2\n");

	// One of each, at most.
	compile_failure("var x = 1:\n\tget:\n\t\treturn 1\n\tget:\n\t\treturn 2\n");
	compile_failure("var x = 1:\n\tset(v):\n\t\tpass\n\tset(v):\n\t\tpass\n");

	compile_failure("var x = 1:\n\tset:\n\t\tpass\n");

	compile_failure("var x = 1:\n\tvalue = 2\n");
	compile_failure("var x = 1: pass\n");

	// No await inside an accessor.
	compile_failure("var x = 1:\n\tget:\n\t\treturn await something()\n");
	compile_failure(
			"var x = 1: get = read_x\n"
			"func read_x():\n"
			"\treturn await something()\n");

	std::cout << "  refusals are refusals" << std::endl;
}

TEST_CASE("a local shadows the property") {
	const IRProgram ir = compile_to_ir(
			"var hp = 1:\n"
			"\tset(v):\n"
			"\t\thp = v\n"
			"\tget:\n"
			"\t\treturn 9\n"
			"func test():\n"
			"\tvar hp = 2\n"
			"\thp = 3\n"
			"\treturn hp\n");

	const IRFunction &test = find_function(ir, "test");
	REQUIRE(call_sequence(ir, test).empty());

	std::cout << "  a local of the same name is a local" << std::endl;
}
