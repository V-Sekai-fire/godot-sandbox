// A method called on an untyped value reaches the class the instance was built
// from, and only an instance this program sealed is trusted to name its class.
#include "../codegen.h"
#include "../ir_verifier.h"
#include "../lexer.h"
#include "../parser.h"
#include "witness/doctest.h"
#include <algorithm>
#include <string>
#include <vector>

using namespace gdscript;

static IRProgram compile_to_ir(const std::string &source) {
	Lexer lexer(source);
	lexer.set_extensions(false);
	Parser parser(lexer.tokenize());
	parser.set_extensions(false);
	Program program = parser.parse();
	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);
	ir_verify(ir);
	return ir;
}

static const IRFunction &find_function(const IRProgram &ir, const std::string &name) {
	for (const IRFunction &func : ir.functions) {
		if (func.name == name) {
			return func;
		}
	}
	FAIL("function not found: " << name);
	return ir.functions.front();
}

static std::vector<std::string> called(const IRProgram &ir, const IRFunction &func) {
	std::vector<std::string> names;
	for (const IRInstruction &instr : func.instructions) {
		if (instr.opcode == IROpcode::CALL) {
			names.push_back(ir.strings[instr.operands[0].string_id]);
		}
	}
	return names;
}

static int count_opcode(const IRFunction &func, IROpcode opcode) {
	return int(std::count_if(func.instructions.begin(), func.instructions.end(),
							 [&](const IRInstruction &instr) { return instr.opcode == opcode; }));
}

static int seal_index(const IRProgram &ir) {
	for (size_t i = 0; i < ir.globals.size(); i++) {
		if (ir.globals[i].name == "@seal") {
			return int(i);
		}
	}
	return -1;
}

static bool loads_global(const IRFunction &func, int index) {
	for (const IRInstruction &instr : func.instructions) {
		if (instr.opcode == IROpcode::LOAD_GLOBAL && instr.operands[1].immediate() == index) {
			return true;
		}
	}
	return false;
}

static const std::string CHAIN =
		"class Base:\n"
		"\tvar n = 1\n"
		"\tfunc get_n():\n"
		"\t\treturn n\n"
		"\tfunc name():\n"
		"\t\treturn \"base\"\n"
		"class Derived:\n"
		"\textends Base\n"
		"\tfunc name():\n"
		"\t\treturn \"derived\"\n"
		"class Other:\n"
		"\tvar u = 0\n"
		"func f(c):\n"
		"\treturn c.name()\n"
		"func g(c):\n"
		"\treturn c.get_n()\n";

TEST_CASE("an untyped receiver dispatches on the class it was built from") {
	const IRProgram ir = compile_to_ir(CHAIN);
	const std::vector<std::string> f_calls = called(ir, find_function(ir, "f"));
	CHECK(std::find(f_calls.begin(), f_calls.end(), "@Base.name") != f_calls.end());
	CHECK(std::find(f_calls.begin(), f_calls.end(), "@Derived.name") != f_calls.end());
	CHECK(count_opcode(find_function(ir, "f"), IROpcode::VCALL) == 1);

	// Derived inherits get_n, so both arms call Base's.
	const std::vector<std::string> g_calls = called(ir, find_function(ir, "g"));
	CHECK(std::count(g_calls.begin(), g_calls.end(), "@Base.get_n") == 2);
}

TEST_CASE("a method no class declares is the generic call") {
	const IRProgram ir = compile_to_ir(CHAIN + "func h(c):\n\treturn c.size()\n");
	CHECK(called(ir, find_function(ir, "h")).empty());
	CHECK(count_opcode(find_function(ir, "h"), IROpcode::VCALL) == 1);
}

TEST_CASE("only this program's seal lets a Dictionary name its class") {
	const IRProgram ir = compile_to_ir(CHAIN + "func t(x):\n\treturn x is Base\n");
	const int seal = seal_index(ir);
	REQUIRE(seal >= 0);
	CHECK(ir.globals[size_t(seal)].holds_object);
	CHECK(loads_global(find_function(ir, "f"), seal));
	CHECK(loads_global(find_function(ir, "t"), seal));
	bool created = false;
	for (const IRInstruction &instr : ir.global_init.instructions) {
		created = created || (instr.opcode == IROpcode::STORE_GLOBAL && instr.operands[0].immediate() == seal);
	}
	CHECK(created);
}

TEST_CASE("a program without classes carries no seal") {
	const IRProgram ir = compile_to_ir("func f(c):\n\treturn c.name()\n");
	CHECK(seal_index(ir) == -1);
}

static const std::string GETTERS =
		"class Asset:\n"
		"\tvar _cache = \"\"\n"
		"\tvar key: String:\n"
		"\t\tget:\n"
		"\t\t\tif _cache.is_empty():\n"
		"\t\t\t\t_cache = \"k\"\n"
		"\t\t\treturn _cache\n"
		"\tvar own: int = 3:\n"
		"\t\tget:\n"
		"\t\t\treturn own + 1\n"
		"\tfunc describe():\n"
		"\t\treturn key\n"
		"func typed():\n"
		"\tvar a := Asset.new()\n"
		"\treturn a.key\n"
		"func untyped(a):\n"
		"\treturn a.key\n"
		"func plain(d):\n"
		"\treturn d.other\n";

TEST_CASE("a class getter is called for every read of its field") {
	const IRProgram ir = compile_to_ir(GETTERS);
	const std::vector<std::string> typed_calls = called(ir, find_function(ir, "typed"));
	CHECK(std::find(typed_calls.begin(), typed_calls.end(), "@Asset.@key_getter") != typed_calls.end());

	const std::vector<std::string> self_calls = called(ir, find_function(ir, "@Asset.describe"));
	CHECK(self_calls == std::vector<std::string>{ "@Asset.@key_getter" });

	const std::vector<std::string> untyped_calls = called(ir, find_function(ir, "untyped"));
	CHECK(untyped_calls == std::vector<std::string>{ "@Asset.@key_getter" });

	CHECK(called(ir, find_function(ir, "plain")).empty());
}

TEST_CASE("inside its own getter a field reads its storage") {
	const IRProgram ir = compile_to_ir(GETTERS);
	CHECK(called(ir, find_function(ir, "@Asset.@own_getter")).empty());
}

TEST_CASE("a class field setter is refused, not ignored") {
	Lexer lexer("class A:\n\tvar x: int:\n\t\tset(v):\n\t\t\tpass\n");
	lexer.set_extensions(false);
	Parser parser(lexer.tokenize());
	parser.set_extensions(false);
	CHECK_THROWS(parser.parse());
}

static int global_index(const IRProgram &ir, const std::string &name) {
	for (size_t i = 0; i < ir.globals.size(); i++) {
		if (ir.globals[i].name == name) {
			return int(i);
		}
	}
	return -1;
}

static const std::string VALUES =
		"var maker = null\n"
		"class Sentinel:\n"
		"\tvar n = 0\n"
		"class Box:\n"
		"\tvar inner = maker.new()\n"
		"class Pair:\n"
		"\tvar a\n"
		"\tfunc _init(x):\n"
		"\t\ta = x\n"
		"func as_value():\n"
		"\treturn Sentinel\n"
		"func make(c):\n"
		"\treturn c.new()\n"
		"func is_sentinel(x):\n"
		"\treturn x == Sentinel\n";

TEST_CASE("a class used as a value is its own object, and .new() on it builds the class") {
	const IRProgram ir = compile_to_ir(VALUES);
	const int sentinel = global_index(ir, "@class:Sentinel");
	REQUIRE(sentinel >= 0);
	CHECK(ir.globals[size_t(sentinel)].holds_object);
	CHECK(loads_global(find_function(ir, "as_value"), sentinel));
	CHECK(loads_global(find_function(ir, "is_sentinel"), sentinel));

	const std::vector<std::string> made = called(ir, find_function(ir, "make"));
	CHECK(std::find(made.begin(), made.end(), "@Sentinel.@new0") != made.end());
	CHECK(std::find(made.begin(), made.end(), "@Box.@new0") != made.end());
	// Pair's _init needs an argument, so a zero-argument .new() cannot be Pair.
	CHECK(std::find(made.begin(), made.end(), "@Pair.@new0") == made.end());
	CHECK(count_opcode(find_function(ir, "make"), IROpcode::VCALL) == 1);
	CHECK_NOTHROW(find_function(ir, "@Box.@new0"));
}
