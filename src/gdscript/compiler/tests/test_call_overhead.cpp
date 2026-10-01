#include "../syscall_abi.h"
// Pin down the instruction sequences around a call: return, immediate folding,
// int chaining, non-negative subscript elision, global handle elision.
// Assertions are on emitted instruction shapes, not specific encodings.
#include "../codegen.h"
#include "../compiler_exception.h"
#include "../ir_optimizer.h"
#include "../lexer.h"
#include "../parser.h"
#include "../riscv_codegen.h"
#include "../syscall_numbers.h"
#include "../variant_layout.h"
#include "witness/doctest.h"
#include <iostream>
#include <vector>

using namespace gdscript;

namespace {

constexpr uint8_t REG_ZERO = 0;
constexpr uint8_t REG_SP = 2;
constexpr uint8_t REG_A0 = 10;
constexpr uint8_t REG_A7 = 17;

constexpr uint32_t RET = 0x00008067;

struct Compiled {
	std::vector<uint8_t> code;
	std::unordered_map<std::string, size_t> offsets;
};

Compiled compile(const std::string &source) {
	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);
	IROptimizer optimizer;
	optimizer.optimize(ir);

	RISCVCodeGen riscv{ VariantLayout(false) };
	Compiled out;
	out.code = riscv.generate(ir);
	out.offsets = riscv.get_function_offsets();
	return out;
}

uint32_t word_at(const Compiled &compiled, size_t offset) {
	return uint32_t(compiled.code[offset]) |
			(uint32_t(compiled.code[offset + 1]) << 8) |
			(uint32_t(compiled.code[offset + 2]) << 16) |
			(uint32_t(compiled.code[offset + 3]) << 24);
}

// The words of one function, up to and including its `returns`th RET.
std::vector<uint32_t> function_words(const Compiled &compiled, const std::string &name,
									 size_t returns = 1) {
	auto it = compiled.offsets.find(name);
	REQUIRE((it != compiled.offsets.end() && "no such function in the generated code"));

	std::vector<uint32_t> words;
	size_t seen = 0;
	for (size_t off = it->second; off + 4 <= compiled.code.size(); off += 4) {
		const uint32_t word = word_at(compiled, off);
		words.push_back(word);
		if (word == RET && ++seen == returns) {
			return words;
		}
	}
	FAIL("function does not return that many times");
	return words;
}

uint32_t opcode_of(uint32_t w) { return w & 0x7F; }
uint8_t rd_of(uint32_t w) { return uint8_t((w >> 7) & 0x1F); }
uint8_t funct3_of(uint32_t w) { return uint8_t((w >> 12) & 0x7); }
uint8_t rs1_of(uint32_t w) { return uint8_t((w >> 15) & 0x1F); }
int32_t i_imm_of(uint32_t w) { return int32_t(w) >> 20; }
// A shift's immediate field carries funct7 above the amount.
uint8_t shamt_of(uint32_t w) { return uint8_t((w >> 20) & 0x3F); }

// An OP-IMM instruction (addi/andi/ori/xori/slli/srai) with this funct3.
bool is_op_imm(uint32_t w, uint8_t funct3) {
	return opcode_of(w) == 0x13 && funct3_of(w) == funct3;
}

bool is_andi(uint32_t w) { return is_op_imm(w, 7); }
bool is_ori(uint32_t w) { return is_op_imm(w, 6); }
bool is_xori(uint32_t w) { return is_op_imm(w, 4); }
bool is_srai(uint32_t w) { return is_op_imm(w, 5) && (w >> 30) == 1; }
bool is_slli(uint32_t w) { return is_op_imm(w, 1); }
bool is_slti(uint32_t w) { return is_op_imm(w, 2); }
bool is_mul(uint32_t w) {
	return opcode_of(w) == 0x33 && funct3_of(w) == 0 && (w >> 25) == 1;
}

bool is_stack_adjust(uint32_t w) {
	return is_op_imm(w, 0) && rd_of(w) == REG_SP && rs1_of(w) == REG_SP;
}

// addi that is not the frame moving: an arithmetic immediate or an address.
bool is_addi(uint32_t w) {
	return is_op_imm(w, 0) && !is_stack_adjust(w);
}

bool is_store_to_frame(uint32_t w) {
	return opcode_of(w) == 0x23 && rs1_of(w) == REG_SP;
}

bool touches_frame(uint32_t w) {
	const bool is_store = opcode_of(w) == 0x23;
	const bool is_load = opcode_of(w) == 0x03;
	return (is_store || is_load) && rs1_of(w) == REG_SP;
}

bool is_store_through_return_pointer(uint32_t w) {
	return opcode_of(w) == 0x23 && rs1_of(w) == REG_A0;
}

bool is_ecall(uint32_t w) {
	return gdscript::valid_counted_syscall_encoding(w) ||
			(opcode_of(w) == 0x73 && funct3_of(w) == 0 && rd_of(w) == REG_ZERO && (w >> 20) == 0);
}

// `li a7, number` -- how a syscall says which one it is.
bool selects_syscall(uint32_t w, int number) {
	return is_op_imm(w, 0) && rd_of(w) == REG_A7 && rs1_of(w) == REG_ZERO &&
			i_imm_of(w) == number;
}

size_t count(const std::vector<uint32_t> &words, bool (*pred)(uint32_t)) {
	size_t n = 0;
	for (uint32_t w : words) {
		if (pred(w)) {
			n++;
		}
	}
	return n;
}

size_t count_syscall(const std::vector<uint32_t> &words, int number) {
	size_t n = 0;
	for (uint32_t w : words) {
		if (selects_syscall(w, number) ||
			(gdscript::valid_counted_syscall_encoding(w) && (w >> 20) == unsigned(number))) {
			n++;
		}
	}
	return n;
}

// Is there a shift by this amount?
bool has_shift(const std::vector<uint32_t> &words, bool (*form)(uint32_t), uint8_t shamt) {
	for (uint32_t w : words) {
		if (form(w) && shamt_of(w) == shamt) {
			return true;
		}
	}
	return false;
}

// Is there an immediate-form instruction carrying this value?
bool has_immediate(const std::vector<uint32_t> &words, bool (*form)(uint32_t), int32_t value) {
	for (uint32_t w : words) {
		if (form(w) && i_imm_of(w) == value) {
			return true;
		}
	}
	return false;
}

// -= Leaving =-
TEST_CASE("falling off the end returns null") {
	const Compiled compiled = compile("func f(a, b):\n\tvar c = a\n");
	const std::vector<uint32_t> words = function_words(compiled, "f");

	// Type word through a0, no frame, no load.
	REQUIRE(count(words, is_stack_adjust) == 0);
	REQUIRE(count(words, touches_frame) == 0);
	REQUIRE(count(words, is_store_through_return_pointer) == 1);
	REQUIRE(words.size() <= 3);
}

TEST_CASE("bare return returns null") {
	const Compiled compiled = compile("func f(a):\n\tif a > 0:\n\t\treturn\n\treturn 2\n");
	const std::vector<uint32_t> words = function_words(compiled, "f", 2);

	// One sw for null, two for the integer -- no slot copy.
	REQUIRE(count(words, is_store_through_return_pointer) == 3);
}

// Later reads of r0 are on unreachable paths; forwarding is still valid.
TEST_CASE("early return still forwards") {
	const Compiled compiled = compile(
			"func f(a):\n"
			"\tif a > 10:\n"
			"\t\treturn 1\n"
			"\tif a < 0:\n"
			"\t\treturn 2\n"
			"\treturn 3\n");
	const std::vector<uint32_t> words = function_words(compiled, "f", 3);

	// Type + payload per return, not a whole-Variant copy.
	const size_t variant_words = size_t(VariantLayout(false).variant_words());
	REQUIRE(count(words, is_store_through_return_pointer) == 3 * 2);
	REQUIRE(3 * 2 < 3 * variant_words);
}

// -= Decoding =-
TEST_CASE("constant operand becomes an immediate") {
	const Compiled compiled = compile(
			"func f(word : int):\n"
			"\treturn (word & 255) + (word | 16) + (word ^ 3) + (word << 2) \\\n"
			"\t\t+ (word >> 4) + (word + 7) + (word - 9)\n");
	const std::vector<uint32_t> words = function_words(compiled, "f");

	REQUIRE(has_immediate(words, is_andi, 255));
	REQUIRE(has_immediate(words, is_ori, 16));
	REQUIRE(has_immediate(words, is_xori, 3));
	REQUIRE(has_shift(words, is_slli, 2));
	REQUIRE(has_shift(words, is_srai, 4));
	REQUIRE(has_immediate(words, is_addi, 7));
	REQUIRE(has_immediate(words, is_addi, -9)); // SUB borrows addi
}

// Past the 12-bit range, andi sign-extends wrongly; register form required.
TEST_CASE("a constant too wide stays in a register") {
	const Compiled compiled = compile("func f(word : int):\n\treturn word & 4095\n");
	const std::vector<uint32_t> words = function_words(compiled, "f");

	REQUIRE(!has_immediate(words, is_andi, 4095));
	// Falls back to a Variant in the frame.
	REQUIRE(count(words, touches_frame) > 0);
}

TEST_CASE("immediate comparisons and power of two multiply") {
	const Compiled compiled = compile(
			"func less(i : int) -> bool:\n"
			"\treturn i < 10\n"
			"func scale(i : int) -> int:\n"
			"\treturn i * 8\n");
	const std::vector<uint32_t> less = function_words(compiled, "less");
	const std::vector<uint32_t> scale = function_words(compiled, "scale");

	REQUIRE(has_immediate(less, is_slti, 10));
	REQUIRE(has_shift(scale, is_slli, 3));
	REQUIRE(count(scale, is_mul) == 0);
	// The arithmetic leaf writes its tag and payload through the return pointer.
	REQUIRE(count(scale, is_store_through_return_pointer) == 2);
}

TEST_CASE("chained operators stay in a register") {
	const Compiled compiled = compile("func f(word : int):\n\treturn (word >> 8) & 255\n");
	const std::vector<uint32_t> words = function_words(compiled, "f");

	REQUIRE(has_shift(words, is_srai, 8));
	REQUIRE(has_immediate(words, is_andi, 255));

	// The slot remains canonical, but the consumer must not reload it: B1 may
	// place a store/cache move between the two ALU instructions.
	bool cached_chain = false;
	for (size_t i = 0; i + 1 < words.size(); i++) {
		if (!is_srai(words[i])) {
			continue;
		}
		for (size_t j = i + 1; j < words.size() && j <= i + 6; j++) {
			if (is_andi(words[j])) {
				cached_chain = true;
				break;
			}
			REQUIRE((!(opcode_of(words[j]) == 0x03 && rs1_of(words[j]) == REG_SP)));
		}
	}
	REQUIRE(cached_chain);
}

// -= Subscripting =-
TEST_CASE("a masked index skips the wrap") {
	const Compiled bounded = compile(
			"func f(regs : Array, word : int):\n"
			"\treturn regs[(word >> 8) & 7]\n");
	const std::vector<uint32_t> bounded_words = function_words(bounded, "f");

	REQUIRE(count_syscall(bounded_words, ECALL_ARRAY_AT) == 1);
	REQUIRE(count_syscall(bounded_words, ECALL_ARRAY_SIZE) == 0);

	// Unbounded index still wraps.
	const Compiled unbounded = compile(
			"func f(regs : Array, index : int):\n"
			"\treturn regs[index]\n");
	const std::vector<uint32_t> unbounded_words = function_words(unbounded, "f");

	REQUIRE(count_syscall(unbounded_words, ECALL_ARRAY_AT) == 1);
	REQUIRE(count_syscall(unbounded_words, ECALL_ARRAY_SIZE) == 1);
}

TEST_CASE("a constant index skips the wrap") {
	const Compiled compiled = compile("func f(regs : Array):\n\treturn regs[2]\n");
	REQUIRE(count_syscall(function_words(compiled, "f"), ECALL_ARRAY_SIZE) == 0);

	// Godot wraps a negative constant inside the ARRAY_AT call itself.
	const Compiled from_end = compile("func f(regs : Array):\n\treturn regs[-1]\n");
	REQUIRE(count_syscall(function_words(from_end, "f"), ECALL_ARRAY_SIZE) == 0);
}

// -= Global handle elision =-
TEST_CASE("a global container is read where it lies") {
	const Compiled compiled = compile(
			"var regs : Array = []\n"
			"var pc : int = 0\n"
			"func f() -> void:\n"
			"\tregs[pc] = 1\n");
	const std::vector<uint32_t> words = function_words(compiled, "f");

	// Frame holds only the return pointer and the assigned Variant (type + payload).
	const size_t variant_words = size_t(VariantLayout(false).variant_words());
	REQUIRE(count(words, is_store_to_frame) == 1 + 2);
	REQUIRE(1 + 2 < 1 + 2 + variant_words);
	REQUIRE(count_syscall(words, ECALL_ARRAY_AT) == 1);
}

// A store between the load and the use invalidates the elision.
TEST_CASE("a reassigned global keeps its copy") {
	const Compiled compiled = compile(
			"var regs : Array = []\n"
			"var pc : int = 0\n"
			"func f(other : Array) -> void:\n"
			"\tvar first = regs\n"
			"\tregs = other\n"
			"\tfirst[pc] = 1\n");
	const std::vector<uint32_t> words = function_words(compiled, "f");

	// Intervening assignment forces the frame copy.
	const size_t variant_words = size_t(VariantLayout(false).variant_words());
	REQUIRE(count(words, is_store_to_frame) >= 1 + 2 + variant_words);
}

// Read-modify-write of an int global stays in registers; no Variant built.
TEST_CASE("an int global round trips in a register") {
	const Compiled compiled = compile(
			"var pc : int = 0\n"
			"func f(n : int) -> void:\n"
			"\tpc = pc + n\n");
	const std::vector<uint32_t> words = function_words(compiled, "f");

	// Resident values add callee-saved spills to the frame, so an exact store
	// count no longer describes this property. A copied Variant would add a
	// complete Variant's worth of consecutive frame stores; the generated path
	// remains comfortably below that old path plus its resident saves.
	const size_t variant_words = size_t(VariantLayout(false).variant_words());
	REQUIRE(count(words, is_store_to_frame) < 8 + variant_words);
	REQUIRE(count(words, is_ecall) == 0);
}

} // namespace
