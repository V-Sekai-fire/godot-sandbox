// Dictionary access lowering: key form (codegen), emitted op (riscv_codegen),
// and scope planning. Engine-visible behaviour is in test_gdscript_compiler.gd.
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
		ir_verify(ir, "the optimizer");
	}
	return ir;
}

static const IRFunction &find_function(const IRProgram &ir, const std::string &name) {
	for (const auto &func : ir.functions) {
		if (func.name == name)
			return func;
	}
	throw std::runtime_error("Function not found: " + name);
}

static int count_opcode(const IRFunction &func, IROpcode opcode) {
	int count = 0;
	for (const auto &instr : func.instructions) {
		if (instr.opcode == opcode)
			count++;
	}
	return count;
}

static int count_dict_ops(const IRFunction &func, Dictionary_Op op) {
	int count = 0;
	for (const auto &instr : func.instructions) {
		if (instr.opcode == IROpcode::CALL_SYSCALL && instr.operands.size() >= 3 &&
			instr.operands[1].immediate() == ECALL_DICTIONARY_OPS &&
			instr.operands[2].immediate() == dictionary_op(op)) {
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

static std::vector<uint8_t> compile_to_machine_code(const std::string &source) {
	IRProgram ir = compile_to_ir(source, true);
	RISCVCodeGen backend;
	std::vector<uint8_t> code = backend.generate(ir);
	REQUIRE(!code.empty());
	return code;
}

// The backend picks the op, so scan the instruction stream for `li a0, <op>`.
static bool emits_li(const std::vector<uint8_t> &code, uint8_t reg, int32_t value) {
	for (size_t i = 0; i + 4 <= code.size(); i += 4) {
		uint32_t insn = uint32_t(code[i]) | uint32_t(code[i + 1]) << 8 |
				uint32_t(code[i + 2]) << 16 | uint32_t(code[i + 3]) << 24;
		if ((insn & 0x7f) != 0x13)
			continue; // OP-IMM
		if (((insn >> 12) & 0x7) != 0)
			continue; // ADDI
		if (((insn >> 15) & 0x1f) != 0)
			continue; // rs1 == zero
		if (((insn >> 7) & 0x1f) != reg)
			continue;
		if (int32_t(insn) >> 20 == value)
			return true;
	}
	return false;
}

static bool emits_dict_op(const std::vector<uint8_t> &code, Dictionary_Op op) {
	return emits_li(code, 10 /* a0 */, int32_t(dictionary_op(op)));
}

// -= Key forms =-

TEST_CASE("an integer key travels as a number") {
	// Int-key ops only fire inside loops (scalar residency requires a back edge).
	const std::vector<uint8_t> code = compile_to_machine_code(
			"func write(d : Dictionary, n : int, v):\n"
			"\tvar i : int = 0\n"
			"\twhile i < n:\n"
			"\t\td[i] = v\n"
			"\t\ti += 1\n"
			"\n"
			"func read(d : Dictionary, n : int):\n"
			"\tvar i : int = 0\n"
			"\twhile i < n:\n"
			"\t\td[i]\n"
			"\t\ti += 1\n"
			"\n"
			"func present(d : Dictionary, n : int) -> bool:\n"
			"\tvar i : int = 0\n"
			"\tvar found : bool = false\n"
			"\twhile i < n:\n"
			"\t\tfound = d.has(i)\n"
			"\t\ti += 1\n"
			"\treturn found\n");
	REQUIRE(emits_dict_op(code, Dictionary_Op::SET_INT_KEY));
	REQUIRE(emits_dict_op(code, Dictionary_Op::GET_INT_KEY));
	REQUIRE(emits_dict_op(code, Dictionary_Op::HAS_INT_KEY));

	// Float and untyped keys stay boxed.
	const std::vector<uint8_t> boxed = compile_to_machine_code(
			"func write(d : Dictionary, n : int, v):\n"
			"\tvar f : float = 0.0\n"
			"\twhile f < float(n):\n"
			"\t\td[f] = v\n"
			"\t\tf += 1.0\n"
			"\n"
			"func read(d : Dictionary, k, n : int):\n"
			"\tvar i : int = 0\n"
			"\twhile i < n:\n"
			"\t\td[k]\n"
			"\t\ti += 1\n");
	REQUIRE(!emits_dict_op(boxed, Dictionary_Op::SET_INT_KEY));
	REQUIRE(!emits_dict_op(boxed, Dictionary_Op::GET_INT_KEY));
	REQUIRE(emits_dict_op(boxed, Dictionary_Op::SET));
	REQUIRE(emits_dict_op(boxed, Dictionary_Op::GET));

	std::cout << "  \u2713 an integer key is not boxed" << std::endl;
}

TEST_CASE("a constant string key allocates nothing") {
	const IRProgram ir = compile_to_ir(
			"func read(d : Dictionary):\n"
			"\treturn d[\"hp\"]\n"
			"\n"
			"func write(d : Dictionary, v):\n"
			"\td[\"hp\"] = v\n"
			"\n"
			"func bump(d : Dictionary):\n"
			"\td[\"hp\"] += 1\n");

	const IRFunction &read = find_function(ir, "read");
	REQUIRE(count_opcode(read, IROpcode::DICT_GET_CONST) == 1);
	REQUIRE(count_opcode(read, IROpcode::LOAD_STRING) == 0);
	REQUIRE(count_dict_ops(read, Dictionary_Op::GET) == 0);

	const IRFunction &write = find_function(ir, "write");
	REQUIRE(count_opcode(write, IROpcode::DICT_SET_CONST_STR) == 1);
	REQUIRE(count_opcode(write, IROpcode::DICT_SET) == 0);
	REQUIRE(count_opcode(write, IROpcode::LOAD_STRING) == 0);

	// A compound assignment reads and writes through the same constant key.
	const IRFunction &bump = find_function(ir, "bump");
	REQUIRE(count_opcode(bump, IROpcode::DICT_GET_CONST) == 1);
	REQUIRE(count_opcode(bump, IROpcode::DICT_SET_CONST_STR) == 1);
	REQUIRE(count_opcode(bump, IROpcode::LOAD_STRING) == 0);

	// Plain Dictionary keys are Strings; struct keys are StringNames.
	const std::vector<uint8_t> code = compile_to_machine_code(
			"func write(d : Dictionary, v):\n\td[\"hp\"] = v\n");
	REQUIRE(emits_dict_op(code, Dictionary_Op::SET_RAW_STR));
	REQUIRE(!emits_dict_op(code, Dictionary_Op::SET_RAW));

	// &"hp" (StringName) and ^"hp" (NodePath) are not String keys.
	const IRProgram typed_literals = compile_to_ir(
			"func read(d : Dictionary):\n"
			"\treturn d[&\"hp\"]\n");
	REQUIRE(count_opcode(find_function(typed_literals, "read"), IROpcode::DICT_GET_CONST) == 0);

	std::cout << "  \u2713 a literal key skips its scoped String" << std::endl;
}

TEST_CASE("get with a default has its own op") {
	const IRProgram ir = compile_to_ir(
			"func fallback(d : Dictionary, k):\n"
			"\treturn d.get(k, 0)\n"
			"\n"
			"func unknown(d, k):\n"
			"\treturn d.get(k, 0)\n");

	const IRFunction &fallback = find_function(ir, "fallback");
	REQUIRE(count_dict_ops(fallback, Dictionary_Op::GET_OR_DEFAULT) == 1);
	REQUIRE(count_vcalls(ir, fallback, "get") == 0);
	// Not GET_OR_ADD: the default is answered, never inserted.
	REQUIRE(count_dict_ops(fallback, Dictionary_Op::GET_OR_ADD) == 0);

	// The receiver's type is what decides, not the argument count.
	const IRFunction &unknown = find_function(ir, "unknown");
	REQUIRE(count_vcalls(ir, unknown, "get") == 1);
	REQUIRE(count_dict_ops(unknown, Dictionary_Op::GET_OR_DEFAULT) == 0);

	compile_to_machine_code("func fallback(d : Dictionary, k):\n\treturn d.get(k, 0)\n");

	std::cout << "  \u2713 get(key, default) is not a VCALL" << std::endl;
}

// -= Scopes =-

TEST_CASE("a numeric read loop releases nothing") {
	const std::string source =
			"func total(d : Dictionary, n : int) -> int:\n"
			"\tvar acc : int = 0\n"
			"\tvar i : int = 0\n"
			"\twhile i < n:\n"
			"\t\tacc += d[i]\n"
			"\t\ti += 1\n"
			"\treturn acc\n";

	const IRProgram ir = compile_to_ir(source, true);
	const IRFunction &total = find_function(ir, "total");
	REQUIRE(count_opcode(total, IROpcode::SCOPE_MARK) >= 1);
	// COERCE keeps `acc` a fixed scalar across the loop.
	REQUIRE(count_opcode(total, IROpcode::COERCE) >= 1);

	compile_to_machine_code(source);

	std::cout << "  \u2713 a read loop derives its dirty flag" << std::endl;
}

TEST_CASE("a declared scalar converts an unknown value") {
	const IRProgram ir = compile_to_ir(
			"var stored : int = 0\n"
			"\n"
			"func into_local(d : Dictionary):\n"
			"\tvar hp : int = d[\"hp\"]\n"
			"\treturn hp\n"
			"\n"
			"func into_member(d : Dictionary):\n"
			"\tstored = d[\"hp\"]\n"
			"\n"
			"func out_of_return(d : Dictionary) -> int:\n"
			"\treturn d[\"hp\"]\n"
			"\n"
			"func untyped(d : Dictionary):\n"
			"\tvar hp = d[\"hp\"]\n"
			"\treturn hp\n");

	REQUIRE(count_opcode(find_function(ir, "into_local"), IROpcode::COERCE) == 1);
	REQUIRE(count_opcode(find_function(ir, "into_member"), IROpcode::COERCE) == 1);
	REQUIRE(count_opcode(find_function(ir, "out_of_return"), IROpcode::COERCE) == 1);
	// Nothing was declared, so nothing is converted.
	REQUIRE(count_opcode(find_function(ir, "untyped"), IROpcode::COERCE) == 0);

	std::cout << "  \u2713 an unknown value converts into a declared slot" << std::endl;
}

// ---------------------------------------------------------------------------
// Properties, and the falsifiability check that keeps them honest.
// ---------------------------------------------------------------------------

#include "property_support.h"

namespace {

// A dictionary literal with n distinct integer keys.
struct IntDictionary {
	std::vector<std::pair<int64_t, int64_t>> entries;

	std::string source() const {
		std::string literal = "{";
		for (size_t i = 0; i < entries.size(); i++) {
			literal += (i == 0 ? "" : ", ") + std::to_string(entries[i].first) + ": " +
					std::to_string(entries[i].second);
		}
		literal += "}";
		return "func f():\n\tvar d = " + literal + "\n\treturn d\n";
	}
};

IntDictionary generate_int_dictionary(witness::RNG &rng, const witness::Level &level) {
	const uint32_t count = rng.uint_range(0, std::max<uint32_t>(1, uint32_t(level.fin_bound) / 32));
	const int64_t bound = std::max<int64_t>(2, level.fin_bound);
	IntDictionary dictionary;
	for (uint32_t i = 0; i < count; i++) {
		// Keys are distinct by construction: a repeated key is a question about
		// overwriting, not about building the literal.
		dictionary.entries.push_back({ int64_t(i), rng.int64_range(-bound, bound) });
	}
	return dictionary;
}

// The IR interpreter cannot run these programs -- building a Dictionary goes
// through the host Variant API -- so the property is about what the compiler
// emits.
size_t make_dictionary_count(const std::string &source) {
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
			if (instruction.opcode == IROpcode::MAKE_DICTIONARY ||
				instruction.opcode == IROpcode::MAKE_DICTIONARY_KEYED) {
				count++;
			}
		}
	}
	return count;
}

} // namespace

TEST_CASE("property: a dictionary literal of any size is built once") {
	PROP_HOLDS(IntDictionary, "one dictionary literal is one construction",
			   &generate_int_dictionary,
			   [](const IntDictionary &dictionary) {
				   return make_dictionary_count(dictionary.source()) == 1;
			   });
}

TEST_CASE("falsifiability: the dictionary generator reaches literals with entries") {
	// False on purpose: a generator stuck on {} would satisfy the property
	// above without ever building a dictionary that holds anything.
	PROP_FALSIFIABLE(IntDictionary, "every generated dictionary is empty",
					 &generate_int_dictionary,
					 [](const IntDictionary &dictionary) { return dictionary.entries.empty(); });
}
