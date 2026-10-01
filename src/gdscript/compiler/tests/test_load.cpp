#include "../syscall_abi.h"
// LOAD_RESOURCE vs LOAD_RESOURCE_VAR lowering and DCE immunity.
#include "../codegen.h"
#include "../compiler_exception.h"
#include "../ir_optimizer.h"
#include "../ir_verifier.h"
#include "../lexer.h"
#include "../parser.h"
#include "../riscv_codegen.h"
#include "../syscall_numbers.h"
#include "witness/doctest.h"
#include <cstring>
#include <iostream>
#include <string>

using namespace gdscript;

static IRProgram compile_to_ir(const std::string &source, bool optimize = false,
							   const std::string &source_path = {}) {
	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();
	CodeGenerator codegen;
	codegen.set_source_path(source_path);
	IRProgram ir = codegen.generate(program);
	if (optimize) {
		IROptimizer optimizer;
		optimizer.optimize(ir);
		ir_verify(ir, "the optimizer");
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

static bool refuses(const std::string &source) {
	try {
		compile_to_ir(source);
	} catch (const CompilerException &) {
		return true;
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

static std::string embedded_path(const IRProgram &ir, const IRFunction &func) {
	for (const auto &instr : func.instructions) {
		if (instr.opcode == IROpcode::LOAD_RESOURCE) {
			return ir.strings[instr.operands[1].string_id];
		}
	}
	throw std::runtime_error("No LOAD_RESOURCE in " + func.name);
}

TEST_CASE("relative constant path uses the script directory") {
	const IRProgram ir = compile_to_ir(
			"func test():\n\treturn preload(\"Bullet.tscn\")\n", false,
			"res://player/bullet/bullet_spawner.gd");
	REQUIRE(embedded_path(ir, find_function(ir, "test")) ==
			"res://player/bullet/Bullet.tscn");

	const IRProgram parent = compile_to_ir(
			"func test():\n\treturn load(\"../shared/icon.svg\")\n", false,
			"res://player/bullet/bullet_spawner.gd");
	REQUIRE(embedded_path(parent, find_function(parent, "test")) ==
			"res://player/shared/icon.svg");
}

static std::vector<uint8_t> machine_code(const std::string &source) {
	IRProgram ir = compile_to_ir(source, /*optimize=*/true);
	RISCVCodeGen backend;
	std::vector<uint8_t> code = backend.generate(ir);
	REQUIRE(!code.empty());
	return code;
}

static int count_instruction(const std::vector<uint8_t> &code, uint32_t instruction) {
	int count = 0;
	for (size_t i = 0; i + 4 <= code.size(); i += 4) {
		uint32_t word = 0;
		std::memcpy(&word, &code[i], 4);
		if (word == instruction) {
			count++;
		}
	}
	return count;
}

static constexpr uint32_t li(int rd, int imm) {
	return (static_cast<uint32_t>(imm & 0xFFF) << 20) | (static_cast<uint32_t>(rd) << 7) | 0x13;
}

static constexpr int REG_A1 = 11;
static constexpr int REG_A7 = 17;

TEST_CASE("literal path is embedded") {
	const std::string source = "func test():\n\treturn load(\"res://icon.svg\")\n";
	const IRProgram ir = compile_to_ir(source);
	const IRFunction &test = find_function(ir, "test");

	REQUIRE(count_opcode(test, IROpcode::LOAD_RESOURCE) == 1);
	REQUIRE(embedded_path(ir, test) == "res://icon.svg");

	REQUIRE(count_opcode(test, IROpcode::LOAD_STRING) == 0);
	REQUIRE(count_opcode(test, IROpcode::LOAD_RESOURCE_VAR) == 0);
	REQUIRE(count_opcode(test, IROpcode::VCALL) == 0);
	REQUIRE(count_opcode(test, IROpcode::CALL) == 0);

	for (const auto &instr : test.instructions) {
		if (instr.opcode == IROpcode::LOAD_RESOURCE) {
			REQUIRE(instr.type_hint == Variant::OBJECT);
		}
	}
}

TEST_CASE("const path is embedded") {
	const IRProgram ir = compile_to_ir(
			"const ICON = \"res://icon.svg\"\n"
			"func test():\n"
			"\treturn load(ICON)\n");
	const IRFunction &test = find_function(ir, "test");

	REQUIRE(count_opcode(test, IROpcode::LOAD_RESOURCE) == 1);
	REQUIRE(embedded_path(ir, test) == "res://icon.svg");
	REQUIRE(count_opcode(test, IROpcode::LOAD_RESOURCE_VAR) == 0);

	// Local shadows the const.
	const IRProgram shadowed = compile_to_ir(
			"const ICON = \"res://icon.svg\"\n"
			"func test():\n"
			"\tvar ICON = \"res://other.svg\"\n"
			"\treturn load(ICON)\n");
	const IRFunction &shadowed_fn = find_function(shadowed, "test");
	REQUIRE(count_opcode(shadowed_fn, IROpcode::LOAD_RESOURCE) == 0);
	REQUIRE(count_opcode(shadowed_fn, IROpcode::LOAD_RESOURCE_VAR) == 1);

	const IRProgram number = compile_to_ir(
			"const N = 5\n"
			"func test():\n"
			"\treturn load(N)\n");
	REQUIRE(count_opcode(find_function(number, "test"), IROpcode::LOAD_RESOURCE) == 0);
	REQUIRE(count_opcode(find_function(number, "test"), IROpcode::LOAD_RESOURCE_VAR) == 1);
}

TEST_CASE("runtime path is a variant") {
	static const char *sources[] = {
		"func test(p):\n\treturn load(p)\n",
		"func test(dir):\n\treturn load(dir + \"/icon.svg\")\n",
		"func test(a : Array):\n\treturn load(a[0])\n",
	};

	for (const char *source : sources) {
		const IRProgram ir = compile_to_ir(source);
		const IRFunction &test = find_function(ir, "test");
		REQUIRE(count_opcode(test, IROpcode::LOAD_RESOURCE_VAR) == 1);
		REQUIRE(count_opcode(test, IROpcode::LOAD_RESOURCE) == 0);
		for (const auto &instr : test.instructions) {
			if (instr.opcode == IROpcode::LOAD_RESOURCE_VAR) {
				REQUIRE(instr.type_hint == Variant::OBJECT);
			}
		}
	}
}

TEST_CASE("call survives the optimizer") {
	// Side-effectful: DCE must not delete either form.
	const IRProgram literal = compile_to_ir(
			"func test():\n\tload(\"res://icon.svg\")\n\treturn 1\n", /*optimize=*/true);
	REQUIRE(count_opcode(find_function(literal, "test"), IROpcode::LOAD_RESOURCE) == 1);

	const IRProgram computed = compile_to_ir(
			"func test(p):\n\tload(p)\n\treturn 1\n", /*optimize=*/true);
	REQUIRE(count_opcode(find_function(computed, "test"), IROpcode::LOAD_RESOURCE_VAR) == 1);

	// Duplicate paths are distinct calls.
	const IRProgram twice = compile_to_ir(
			"func test():\n"
			"\tvar a = load(\"res://icon.svg\")\n"
			"\tvar b = load(\"res://icon.svg\")\n"
			"\treturn a == b\n",
			/*optimize=*/true);
	REQUIRE(count_opcode(find_function(twice, "test"), IROpcode::LOAD_RESOURCE) == 2);
}

TEST_CASE("emitted syscall") {
	const std::vector<uint8_t> literal =
			machine_code("func test():\n\treturn load(\"res://icon.svg\")\n");
	REQUIRE(count_instruction(literal, gdscript::encode_counted_syscall(ECALL_LOAD, 3, 0, false)) == 1);
	// A1 = length (characters).
	REQUIRE(count_instruction(literal, li(REG_A1, 14)) == 1);
	REQUIRE(count_instruction(literal, li(REG_A1, -1)) == 0);

	const std::vector<uint8_t> computed = machine_code("func test(p):\n\treturn load(p)\n");
	REQUIRE(count_instruction(computed, gdscript::encode_counted_syscall(ECALL_LOAD, 3, 0, false)) == 1);
	// A1 = -1 (Variant path).
	REQUIRE(count_instruction(computed, li(REG_A1, -1)) == 1);
}

TEST_CASE("refusals") {
	REQUIRE(refuses("func test():\n\treturn load()\n"));
	REQUIRE(refuses("func test():\n\treturn load(\"res://a.tscn\", \"res://b.tscn\")\n"));

	const IRProgram preloaded = compile_to_ir(
			"func test():\n\treturn preload(\"res://a.tscn\")\n");
	REQUIRE(count_opcode(find_function(preloaded, "test"), IROpcode::LOAD_RESOURCE) == 1);
	REQUIRE(refuses("func test(p):\n\treturn preload(p)\n"));
	REQUIRE(refuses("func test():\n\treturn preload()\n"));

	// Local function shadows the global.
	const IRProgram own = compile_to_ir(
			"func load(x):\n\treturn x\n"
			"func test():\n\treturn load(\"res://icon.svg\")\n");
	const IRFunction &test = find_function(own, "test");
	REQUIRE(count_opcode(test, IROpcode::LOAD_RESOURCE) == 0);
	REQUIRE(count_opcode(test, IROpcode::LOAD_RESOURCE_VAR) == 0);
	REQUIRE(count_opcode(test, IROpcode::CALL) == 1);
}
