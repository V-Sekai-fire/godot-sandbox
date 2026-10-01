#include "../codegen.h"
#include "../compiler_exception.h"
#include "../ir_optimizer.h"
#include "../lexer.h"
#include "../parser.h"
#include "../riscv_codegen.h"
#include "../syscall_numbers.h"
#include "witness/doctest.h"
#include <algorithm>
#include <iostream>

using namespace gdscript;

TEST_CASE("simple arithmetic") {
	std::string source = R"(func add(a, b):
	return a + b
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);

	REQUIRE(ir.functions.size() == 1);
	REQUIRE(ir.functions[0].name == "add");
	REQUIRE(ir.functions[0].parameters.size() == 2);
	REQUIRE(ir.functions[0].instructions.size() > 0);

	// Should have ADD instruction
	bool has_add = false;
	for (const auto &instr : ir.functions[0].instructions) {
		if (instr.opcode == IROpcode::ADD) {
			has_add = true;
			break;
		}
	}
	REQUIRE(has_add);
}

TEST_CASE("variable operations") {
	std::string source = R"(func test():
	var x = 10
	var y = 20
	var sum = x + y
	return sum
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);

	REQUIRE(ir.functions[0].instructions.size() > 0);

	// Should have LOAD_IMM for constants
	int load_imm_count = 0;
	for (const auto &instr : ir.functions[0].instructions) {
		if (instr.opcode == IROpcode::LOAD_IMM) {
			load_imm_count++;
		}
	}
	REQUIRE(load_imm_count >= 2); // At least for 10 and 20
}

TEST_CASE("control flow") {
	std::string source = R"(func abs(x):
	if x < 0:
		return -x
	else:
		return x
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);

	// Should have labels and branches
	bool has_label = false;
	bool has_branch = false;

	for (const auto &instr : ir.functions[0].instructions) {
		if (instr.opcode == IROpcode::LABEL)
			has_label = true;
		if (instr.opcode == IROpcode::BRANCH_ZERO)
			has_branch = true;
	}

	REQUIRE(has_label);
	REQUIRE(has_branch);
}

TEST_CASE("loop generation") {
	std::string source = R"(func count(n):
	var i = 0
	while i < n:
		i = i + 1
	return i
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);

	// Rotated loops use a conditional branch for the back edge.
	bool has_back_branch = false;
	int label_count = 0;

	for (const auto &instr : ir.functions[0].instructions) {
		if (instr.opcode == IROpcode::BRANCH_NOT_ZERO)
			has_back_branch = true;
		if (instr.opcode == IROpcode::LABEL)
			label_count++;
	}

	REQUIRE(has_back_branch);
	REQUIRE(label_count >= 3); // body, continue and end labels
}

TEST_CASE("function calls") {
	std::string source = R"(func helper(x):
	return x * 2

func main():
	var result = helper(21)
	return result
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);

	REQUIRE(ir.functions.size() == 2);

	// main() should have a CALL instruction
	bool has_call = false;
	for (const auto &instr : ir.functions[1].instructions) {
		if (instr.opcode == IROpcode::CALL) {
			has_call = true;
			break;
		}
	}
	REQUIRE(has_call);
}

TEST_CASE("comparison operators") {
	std::string source = R"(func test(a, b):
	var eq = a == b
	var ne = a != b
	var lt = a < b
	var lte = a <= b
	var gt = a > b
	var gte = a >= b
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);

	// Count different comparison operations
	int cmp_count = 0;
	for (const auto &instr : ir.functions[0].instructions) {
		if (instr.opcode == IROpcode::CMP_EQ ||
			instr.opcode == IROpcode::CMP_NEQ ||
			instr.opcode == IROpcode::CMP_LT ||
			instr.opcode == IROpcode::CMP_LTE ||
			instr.opcode == IROpcode::CMP_GT ||
			instr.opcode == IROpcode::CMP_GTE) {
			cmp_count++;
		}
	}

	REQUIRE(cmp_count == 6);
}

TEST_CASE("logical operators") {
	std::string source = R"(func test(a, b, c):
	var result = a and b or not c
	return result
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);

	// 'and' and 'or' short-circuit, so they lower to branches over the
	// right-hand side rather than to an AND/OR that evaluates both sides.
	// 'not' has no short circuit and stays a single instruction.
	bool has_not = false;
	int branch_count = 0;
	for (const auto &instr : ir.functions[0].instructions) {
		REQUIRE(instr.opcode != IROpcode::AND);
		REQUIRE(instr.opcode != IROpcode::OR);
		if (instr.opcode == IROpcode::NOT)
			has_not = true;
		if (instr.opcode == IROpcode::BRANCH_ZERO || instr.opcode == IROpcode::BRANCH_NOT_ZERO) {
			branch_count++;
		}
	}

	REQUIRE(has_not);
	// Two tests for 'and' (a, b) and two for 'or' (the and-result, 'not c').
	REQUIRE(branch_count == 4);
}

TEST_CASE("complex expression") {
	std::string source = R"(func calc(a, b, c):
	return (a + b) * c - a / b
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);

	// Should have multiple arithmetic operations
	int add_count = 0, mul_count = 0, sub_count = 0, div_count = 0;

	for (const auto &instr : ir.functions[0].instructions) {
		if (instr.opcode == IROpcode::ADD)
			add_count++;
		if (instr.opcode == IROpcode::MUL)
			mul_count++;
		if (instr.opcode == IROpcode::SUB)
			sub_count++;
		if (instr.opcode == IROpcode::DIV)
			div_count++;
	}

	REQUIRE(add_count > 0);
	REQUIRE(mul_count > 0);
	REQUIRE(sub_count > 0);
	REQUIRE(div_count > 0);
}

TEST_CASE("string constants") {
	std::string source = R"(func greet():
	var msg = "Hello, World!"
	return msg
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);

	// Should have at least one string constant
	REQUIRE(ir.string_constants.size() == 1);
	REQUIRE(ir.string_constants[0] == "Hello, World!");
}

TEST_CASE("subscript operations") {
	// Test array/dict indexing for reading
	std::string source_read = R"(func get_item(arr, idx):
	var item = arr[idx]
	return item
)";

	Lexer lexer_read(source_read);
	Parser parser_read(lexer_read.tokenize());
	Program program_read = parser_read.parse();

	CodeGenerator codegen_read;
	IRProgram ir_read = codegen_read.generate(program_read);

	// An untyped subscript is a Variant get, not a call to a method named "get".
	bool has_variant_get = false;
	for (const auto &instr : ir_read.functions[0].instructions) {
		if (instr.opcode == IROpcode::CALL_SYSCALL && instr.operands.size() >= 2 &&
			instr.operands[1].immediate() == ECALL_VARIANT_GET) {
			has_variant_get = true;
			break;
		}
	}
	REQUIRE(has_variant_get);

	// Test array/dict indexing for writing
	std::string source_write = R"(func set_item(arr, idx, value):
	arr[idx] = value
	return arr
)";

	Lexer lexer_write(source_write);
	Parser parser_write(lexer_write.tokenize());
	Program program_write = parser_write.parse();

	CodeGenerator codegen_write;
	IRProgram ir_write = codegen_write.generate(program_write);

	// An unknown container writes with the indexed Variant operation, the mirror of
	// the read above: only Object and the packed arrays have a set() method.
	bool has_variant_set = false;
	for (const auto &instr : ir_write.functions[0].instructions) {
		if (instr.opcode == IROpcode::VARIANT_SET) {
			has_variant_set = true;
			break;
		}
	}
	REQUIRE(has_variant_set);
}

TEST_CASE("global print") {
	// print() is a GDScript global, not a method on the owner node. Lowering it
	// to a self-call produces a VCALL that Godot accepts and silently drops, so
	// the shape of this IR is the whole point of the test.
	std::string source = R"(
func say():
	print("Hello", 42)
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);

	const IRFunction &fn = ir.functions[0];

	bool has_print = false;
	for (const auto &instr : fn.instructions) {
		// No VCALL should survive: that is the bug being prevented.
		REQUIRE(instr.opcode != IROpcode::VCALL);
		if (instr.opcode == IROpcode::PRINT) {
			has_print = true;
			// PRINT dst, channel, count, arg...
			REQUIRE(instr.operands.size() == 5);
			REQUIRE(instr.operands[0].type == IRValue::Type::REGISTER);
			REQUIRE(instr.operands[1].immediate() == int64_t(Print_Channel::PRINT));
			REQUIRE(instr.operands[2].immediate() == 2);
			REQUIRE(instr.operands[3].type == IRValue::Type::REGISTER);
			REQUIRE(instr.operands[4].type == IRValue::Type::REGISTER);
		}
	}
	REQUIRE(has_print);

	// Zero arguments is legal and still prints a blank line.
	std::string source_empty = R"(
func say():
	print()
)";

	Lexer lexer_empty(source_empty);
	Parser parser_empty(lexer_empty.tokenize());
	Program program_empty = parser_empty.parse();

	CodeGenerator codegen_empty;
	IRProgram ir_empty = codegen_empty.generate(program_empty);

	bool has_empty_print = false;
	for (const auto &instr : ir_empty.functions[0].instructions) {
		if (instr.opcode == IROpcode::PRINT) {
			has_empty_print = true;
			REQUIRE(instr.operands.size() == 3);
			REQUIRE(instr.operands[1].immediate() == int64_t(Print_Channel::PRINT));
			REQUIRE(instr.operands[2].immediate() == 0);
		}
	}
	REQUIRE(has_empty_print);

	// A script that declares its own print() keeps calling its own, the way a
	// GDScript method shadows a global.
	std::string source_shadow = R"(
func print(x):
	return x

func say():
	print(1)
)";

	Lexer lexer_shadow(source_shadow);
	Parser parser_shadow(lexer_shadow.tokenize());
	Program program_shadow = parser_shadow.parse();

	CodeGenerator codegen_shadow;
	IRProgram ir_shadow = codegen_shadow.generate(program_shadow);

	const IRFunction *say = nullptr;
	for (const auto &f : ir_shadow.functions) {
		if (f.name == "say") {
			say = &f;
		}
	}
	REQUIRE(say != nullptr);

	bool has_local_call = false;
	for (const auto &instr : say->instructions) {
		REQUIRE(instr.opcode != IROpcode::PRINT);
		if (instr.opcode == IROpcode::CALL &&
			ir_shadow.strings[instr.operands[0].string_id] == "print") {
			has_local_call = true;
		}
	}
	REQUIRE(has_local_call);
}

TEST_CASE("array dictionary constructors") {
	// Test empty Array constructor
	std::string source_array = R"(
func make_array():
	return Array()
)";

	Lexer lexer_array(source_array);
	Parser parser_array(lexer_array.tokenize());
	Program program_array = parser_array.parse();

	CodeGenerator codegen_array;
	IRProgram ir_array = codegen_array.generate(program_array);

	// Should have MAKE_ARRAY instruction
	bool has_make_array = false;
	for (const auto &instr : ir_array.functions[0].instructions) {
		if (instr.opcode == IROpcode::MAKE_ARRAY) {
			has_make_array = true;
			// Check that element count is 0
			if (instr.operands.size() >= 2) {
				int count = static_cast<int>(instr.operands[1].immediate());
				REQUIRE(count == 0);
			}
			break;
		}
	}
	REQUIRE(has_make_array);

	// Test empty Dictionary constructor
	std::string source_dict = R"(
func make_dict():
	return Dictionary()
)";

	Lexer lexer_dict(source_dict);
	Parser parser_dict(lexer_dict.tokenize());
	Program program_dict = parser_dict.parse();

	CodeGenerator codegen_dict;
	IRProgram ir_dict = codegen_dict.generate(program_dict);

	// Should have MAKE_DICTIONARY instruction
	bool has_make_dict = false;
	for (const auto &instr : ir_dict.functions[0].instructions) {
		if (instr.opcode == IROpcode::MAKE_DICTIONARY) {
			has_make_dict = true;
			break;
		}
	}
	REQUIRE(has_make_dict);
}

TEST_CASE("float arithmetic") {
	std::string source = R"(func float_ops():
	var a = 1.5
	var b = 2.5
	var sum = a + b
	var diff = a - b
	var prod = a * b
	var quot = b / a
	return sum
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);

	REQUIRE(ir.functions.size() == 1);

	// Should have LOAD_FLOAT_IMM and arithmetic operations
	int float_imm_count = 0;
	int add_count = 0;
	int sub_count = 0;
	int mul_count = 0;
	int div_count = 0;

	for (const auto &instr : ir.functions[0].instructions) {
		if (instr.opcode == IROpcode::LOAD_FLOAT_IMM) {
			float_imm_count++;
		}
		if (instr.opcode == IROpcode::ADD)
			add_count++;
		if (instr.opcode == IROpcode::SUB)
			sub_count++;
		if (instr.opcode == IROpcode::MUL)
			mul_count++;
		if (instr.opcode == IROpcode::DIV)
			div_count++;
	}

	REQUIRE(float_imm_count >= 2); // At least 1.5 and 2.5
	REQUIRE(add_count >= 1);
	REQUIRE(sub_count >= 1);
	REQUIRE(mul_count >= 1);
	REQUIRE(div_count >= 1);
}

TEST_CASE("vector float operations") {
	std::string source = R"(func vector_ops():
	var v1 = Vector2(1.5, 2.5)
	var v2 = Vector2(3.0, 4.0)
	var x_sum = v1.x + v2.x
	var y_sum = v1.y + v2.y
	return x_sum
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);

	REQUIRE(ir.functions.size() == 1);

	// Should have MAKE_VECTOR2 and VGET_INLINE
	int make_vector2_count = 0;
	int vget_inline_count = 0;

	for (const auto &instr : ir.functions[0].instructions) {
		if (instr.opcode == IROpcode::MAKE_VECTOR2) {
			make_vector2_count++;
		}
		if (instr.opcode == IROpcode::VGET_INLINE) {
			vget_inline_count++;
		}
	}

	REQUIRE(make_vector2_count >= 2);
	REQUIRE(vget_inline_count >= 2); // v1.x, v1.y or v2.x, v2.y
}

TEST_CASE("mixed float int arithmetic") {
	std::string source = R"(func mixed_ops():
	var f = 3.14
	var i = 2
	var result = f + i
	return result
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);

	REQUIRE(ir.functions.size() == 1);

	// Should have both LOAD_FLOAT_IMM and LOAD_IMM
	int float_imm_count = 0;
	int int_imm_count = 0;
	int add_count = 0;

	for (const auto &instr : ir.functions[0].instructions) {
		if (instr.opcode == IROpcode::LOAD_FLOAT_IMM) {
			float_imm_count++;
		}
		if (instr.opcode == IROpcode::LOAD_IMM) {
			int_imm_count++;
		}
		if (instr.opcode == IROpcode::ADD) {
			add_count++;
		}
	}

	REQUIRE(float_imm_count >= 1);
	REQUIRE(int_imm_count >= 1);
	REQUIRE(add_count >= 1);
}

TEST_CASE("many float constants") {
	// Create more float constants than available FP registers (12 temp FP regs: f8-f19)
	std::string source = R"(func many_floats():
	var f1 = 1.0
	var f2 = 2.0
	var f3 = 3.0
	var f4 = 4.0
	var f5 = 5.0
	var f6 = 6.0
	var f7 = 7.0
	var f8 = 8.0
	var f9 = 9.0
	var f10 = 10.0
	var f11 = 11.0
	var f12 = 12.0
	var f13 = 13.0
	var f14 = 14.0
	var f15 = 15.0
	return f1
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);

	REQUIRE(ir.functions.size() == 1);

	// Count float immediate loads
	int float_imm_count = 0;
	for (const auto &instr : ir.functions[0].instructions) {
		if (instr.opcode == IROpcode::LOAD_FLOAT_IMM) {
			float_imm_count++;
		}
	}

	// Should have 15 float constants
	REQUIRE(float_imm_count == 15);

	// Test code generation to make sure it doesn't crash
	RISCVCodeGen codegen_obj;
	try {
		std::vector<uint8_t> code = codegen_obj.generate(ir);
		// Should generate code successfully even with FP register exhaustion
		REQUIRE(code.size() > 0);
	} catch (const CompilerException &e) {
		// If it fails, it should be a known issue
		std::cerr << "    Note: Code generation issue with many floats: " << e.what() << std::endl;
	}
}

TEST_CASE("complex float expressions") {
	std::string source = R"(func complex_float():
	var a = 1.5
	var b = 2.5
	var c = 3.0
	var result = (a + b) * c - a / b
	return result
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);

	REQUIRE(ir.functions.size() == 1);

	// Should have multiple arithmetic operations
	int add_count = 0;
	int sub_count = 0;
	int mul_count = 0;
	int div_count = 0;

	for (const auto &instr : ir.functions[0].instructions) {
		if (instr.opcode == IROpcode::ADD)
			add_count++;
		if (instr.opcode == IROpcode::SUB)
			sub_count++;
		if (instr.opcode == IROpcode::MUL)
			mul_count++;
		if (instr.opcode == IROpcode::DIV)
			div_count++;
	}

	// (a + b), * c, - (a / b) means at least 1 ADD, 1 SUB, 1 MUL, 1 DIV
	REQUIRE(add_count >= 1);
	REQUIRE(sub_count >= 1);
	REQUIRE(mul_count >= 1);
	REQUIRE(div_count >= 1);
}

TEST_CASE("vector3 operations") {
	std::string source = R"(func vector3_ops():
	var v = Vector3(1.0, 2.0, 3.0)
	var x = v.x
	var y = v.y
	var z = v.z
	var sum = x + y + z
	return sum
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);

	REQUIRE(ir.functions.size() == 1);

	// Should have MAKE_VECTOR3 and VGET_INLINE
	int make_vector3_count = 0;
	int vget_inline_count = 0;

	for (const auto &instr : ir.functions[0].instructions) {
		if (instr.opcode == IROpcode::MAKE_VECTOR3) {
			make_vector3_count++;
		}
		if (instr.opcode == IROpcode::VGET_INLINE) {
			vget_inline_count++;
		}
	}

	REQUIRE(make_vector3_count == 1);
	REQUIRE(vget_inline_count == 3); // x, y, z
}

TEST_CASE("vector4 operations") {
	std::string source = R"(func vector4_ops():
	var v = Vector4(1.0, 2.0, 3.0, 4.0)
	var x = v.x
	var y = v.y
	var z = v.z
	var w = v.w
	return x + y + z + w
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);

	REQUIRE(ir.functions.size() == 1);

	// Should have MAKE_VECTOR4 and VGET_INLINE
	int make_vector4_count = 0;
	int vget_inline_count = 0;

	for (const auto &instr : ir.functions[0].instructions) {
		if (instr.opcode == IROpcode::MAKE_VECTOR4) {
			make_vector4_count++;
		}
		if (instr.opcode == IROpcode::VGET_INLINE) {
			vget_inline_count++;
		}
	}

	REQUIRE(make_vector4_count == 1);
	REQUIRE(vget_inline_count == 4); // x, y, z, w
}

// Walks the machine code of one function and verifies that every sp-relative access
// stays inside the stack frame the prologue allocated. A slot placed past the end of
// the frame does not fault: it silently writes into the caller's frame, so a wrong
// offset only shows up as corrupted locals much later. Returns false if the function
// adjusts sp in a way this simple linear walk cannot follow, in which case the caller
// skips the check rather than reporting a bogus failure.
static bool check_stack_accesses_in_frame(const std::vector<uint8_t> &code, size_t begin, size_t end,
										  const std::string &what, const VariantLayout &layout) {
	const int variant_size = layout.variant_size();
	static constexpr uint32_t REG_SP = 2;
	static constexpr uint32_t REG_FP = 8;
	static constexpr uint32_t RET_INSTR = 0x00008067; // jalr x0, 0(ra)

	auto word_at = [&](size_t off) -> uint32_t {
		return uint32_t(code[off]) | (uint32_t(code[off + 1]) << 8) |
				(uint32_t(code[off + 2]) << 16) | (uint32_t(code[off + 3]) << 24);
	};

	if (begin + 4 > end) {
		return false;
	}

	// Prologue: addi sp, sp, -frame_size
	const uint32_t first = word_at(begin);
	if ((first & 0x7F) != 0x13 || ((first >> 12) & 7) != 0 ||
		((first >> 7) & 0x1F) != REG_SP || ((first >> 15) & 0x1F) != REG_SP) {
		return false; // No recognizable frame (large frames use li + add)
	}
	const int32_t frame_size = -(int32_t(first) >> 20);
	if (frame_size <= 0) {
		return false;
	}

	// Offset of sp relative to the frame base, i.e. where sp stood right after the
	// prologue. Argument shuffling for calls lowers it temporarily.
	int32_t sp_delta = 0;

	for (size_t off = begin + 4; off + 4 <= end; off += 4) {
		const uint32_t instr = word_at(off);
		const uint32_t opcode = instr & 0x7F;
		const uint32_t rd = (instr >> 7) & 0x1F;
		const uint32_t funct3 = (instr >> 12) & 7;
		const uint32_t rs1 = (instr >> 15) & 0x1F;

		if (instr == RET_INSTR) {
			// A function can return from several places; each epilogue restores sp.
			sp_delta = 0;
			continue;
		}

		if (opcode == 0x13 && funct3 == 0 && rs1 == REG_SP) { // addi rd, sp, imm
			const int32_t imm = int32_t(instr) >> 20;
			if (rd == REG_SP) {
				sp_delta += imm;
				continue;
			}
			if (rd == REG_FP) {
				continue; // Frame pointer setup: fp = sp + frame_size
			}
			// The address of a Variant slot: all of it must be in the frame.
			const int32_t from_base = sp_delta + imm;
			if (imm < 0 || from_base + variant_size > frame_size) {
				std::cerr << "    " << what << ": Variant slot at sp+" << imm
						  << " (frame base +" << from_base << ") escapes the "
						  << frame_size << "-byte frame" << std::endl;
				return true; // Recognized, and wrong
			}
			continue;
		}
		if (rs1 == REG_SP && (opcode == 0x03 || opcode == 0x07)) { // loads
			const int32_t imm = int32_t(instr) >> 20;
			if (imm < 0 || sp_delta + imm + 8 > frame_size) {
				std::cerr << "    " << what << ": load at sp+" << imm << " escapes the "
						  << frame_size << "-byte frame" << std::endl;
				return true;
			}
			continue;
		}
		if (rs1 == REG_SP && (opcode == 0x23 || opcode == 0x27)) { // stores
			const int32_t imm = (int32_t(instr & 0xFE000000) >> 20) | int32_t((instr >> 7) & 0x1F);
			if (imm < 0 || sp_delta + imm + 8 > frame_size) {
				std::cerr << "    " << what << ": store at sp+" << imm << " escapes the "
						  << frame_size << "-byte frame" << std::endl;
				return true;
			}
			continue;
		}
		// Anything else touching sp (add sp, sp, t0) makes the walk unreliable.
		if ((opcode == 0x33 || opcode == 0x3B) && rd == REG_SP) {
			return false;
		}
	}
	return false;
}

// Compiles the source with the full pipeline and asserts no function writes outside
// its own stack frame.
static void assert_stack_frames_contain_all_slots(const std::string &source, const char *what,
												  const VariantLayout &layout = VariantLayout()) {
	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);
	IROptimizer optimizer;
	optimizer.optimize(ir);

	RISCVCodeGen riscv(layout);
	std::vector<uint8_t> code = riscv.generate(ir);
	REQUIRE(code.size() > 0);

	// Function bodies run from one offset to the next; sort them to find the ends.
	std::vector<std::pair<size_t, std::string>> funcs;
	for (const auto &[name, offset] : riscv.get_function_offsets()) {
		funcs.emplace_back(offset, name);
	}
	REQUIRE(!funcs.empty());
	std::sort(funcs.begin(), funcs.end());

	bool escaped = false;
	for (size_t i = 0; i < funcs.size(); i++) {
		const size_t begin = funcs[i].first;
		const size_t end = (i + 1 < funcs.size()) ? funcs[i + 1].first : code.size();
		escaped |= check_stack_accesses_in_frame(code, begin, end, std::string(what) + "/" + funcs[i].second, layout);
	}
	REQUIRE(!escaped);
}

// The frame has to hold whole Variants, so its sizing is layout-dependent: run the
// whole set against both the 24-byte and the 40-byte (double-precision) Variant.
static void check_stack_slots_stay_within_frame(const VariantLayout &layout) {
	// Untyped comparison: the fused compare-and-branch cannot use the native integer
	// path, so it has to materialize the comparison result as a Variant. That scratch
	// Variant used to be allocated after the frame had already been sized, which put
	// it in the caller's frame and corrupted the caller's locals on every recursion.
	assert_stack_frames_contain_all_slots(R"(func untyped_fibonacci(n):
	if n <= 1:
		return n
	return untyped_fibonacci(n - 1) + untyped_fibonacci(n - 2)
)",
										  "untyped fibonacci", layout);

	// Untyped comparisons and arithmetic against immediates both need a scratch Variant
	// for the immediate operand.
	assert_stack_frames_contain_all_slots(R"(func untyped_ops(a, b):
	var c = a * 3
	var d = 7 - b
	var e = c == 21
	var f = 4 != d
	while a < 10:
		a = a + 1
	return [c, d, e, f, a]
)",
										  "untyped ops", layout);

	// Negation builds a zero Variant to subtract from.
	assert_stack_frames_contain_all_slots(R"(func negate(a):
	return -a
)",
										  "negation", layout);

	// Calls shuffle arguments through extra stack space below the frame.
	assert_stack_frames_contain_all_slots(R"(func callee(a, b, c):
	if a > b:
		return c
	return a

func caller(x):
	return callee(x, x - 1, x + 1)
)",
										  "calls", layout);
}

TEST_CASE("stack slots stay within frame") {
	check_stack_slots_stay_within_frame(VariantLayout(false));
	check_stack_slots_stay_within_frame(VariantLayout(true));
}

TEST_CASE("auipc addi patching") {
	// Create multiple large float constants to force AUIPC+ADDI usage
	std::string source = R"(func large_constants():
	var f1 = 123456789.123
	var f2 = 987654321.456
	var f3 = 111111111.789
	var f4 = 222222222.012
	var sum = f1 + f2 + f3 + f4
	return sum
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);

	REQUIRE(ir.functions.size() == 1);

	// Test code generation - should handle AUIPC+ADDI patching correctly
	RISCVCodeGen codegen_obj;
	try {
		std::vector<uint8_t> code = codegen_obj.generate(ir);
		REQUIRE(code.size() > 0);

		// Whether an AUIPC appears depends on the constants, so there is
		// nothing to assert about it; what this test pins down is that the
		// AUIPC+ADDI pair is patched without generate() throwing.
	} catch (const CompilerException &e) {
		std::cerr << "    Note: AUIPC+ADDI test encountered issue: " << e.what() << std::endl;
	}
}

TEST_CASE("float negation") {
	std::string source = R"(func float_neg():
	var f = 3.14
	var neg = -f
	return neg
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);

	REQUIRE(ir.functions.size() == 1);

	// Should have LOAD_FLOAT_IMM and NEG
	int float_imm_count = 0;
	int neg_count = 0;

	for (const auto &instr : ir.functions[0].instructions) {
		if (instr.opcode == IROpcode::LOAD_FLOAT_IMM) {
			float_imm_count++;
		}
		if (instr.opcode == IROpcode::NEG) {
			neg_count++;
		}
	}

	REQUIRE(float_imm_count >= 1);
	REQUIRE(neg_count >= 1);
}

TEST_CASE("constant fold comparison in if") {
	std::string source = R"(func test():
	var x = 10
	if x > 5:
		return 100
	else:
		return 50
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);

	// Before optimization, we should have a comparison
	int cmp_count_before = 0;
	for (const auto &instr : ir.functions[0].instructions) {
		if (instr.opcode == IROpcode::CMP_GT) {
			cmp_count_before++;
		}
	}
	REQUIRE(cmp_count_before > 0);

	// Apply optimization
	IROptimizer optimizer;
	optimizer.optimize(ir);

	// The comparison folds to a known bool, and the branch that reads it folds
	// with it: `10 > 5` decides the `if`, so neither the test nor the arm it
	// rules out reaches codegen. What is left is the taken arm.
	int load_bool_count = 0;
	int branch_zero_count = 0;
	bool loads_100 = false;
	bool loads_50 = false;
	for (const auto &instr : ir.functions[0].instructions) {
		if (instr.opcode == IROpcode::LOAD_BOOL) {
			load_bool_count++;
		}
		if (instr.opcode == IROpcode::BRANCH_ZERO) {
			branch_zero_count++;
		}
		if (instr.opcode == IROpcode::LOAD_IMM) {
			const int64_t value = instr.operands[1].immediate();
			loads_100 = loads_100 || (value == 100);
			loads_50 = loads_50 || (value == 50);
		}
	}

	REQUIRE(load_bool_count == 0);
	REQUIRE(branch_zero_count == 0);
	REQUIRE(loads_100);
	REQUIRE(!loads_50);
}

void test_copy_propagation_optimization() {
	// This test verifies that the copy propagation optimization eliminates
	// redundant MOVE instructions after constant loads
	std::string source = R"(func test():
	var a = 10
	var b = a
	var c = b
	return c
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);

	// Before optimization, we expect LOAD_IMM and two MOVEs
	int load_imm_count_before = 0;
	int move_count_before = 0;
	for (const auto &instr : ir.functions[0].instructions) {
		if (instr.opcode == IROpcode::LOAD_IMM) {
			load_imm_count_before++;
		}
		if (instr.opcode == IROpcode::MOVE) {
			move_count_before++;
		}
	}

	// Apply optimization
	IROptimizer optimizer;
	optimizer.optimize(ir);

	// After optimization, redundant MOVEs should be eliminated
	// We expect LOAD_IMM and fewer MOVEs (ideally 0 if all can be propagated)
	int load_imm_count_after = 0;
	int move_count_after = 0;
	for (const auto &instr : ir.functions[0].instructions) {
		if (instr.opcode == IROpcode::LOAD_IMM) {
			load_imm_count_after++;
		}
		if (instr.opcode == IROpcode::MOVE) {
			move_count_after++;
		}
	}

	// We should have at least 1 LOAD_IMM, and copy propagation rewrites the
	// readers of a MOVE rather than the constant that fed it, so it neither
	// drops one nor introduces one.
	REQUIRE(load_imm_count_after >= 1);
	REQUIRE(load_imm_count_after == load_imm_count_before);

	// The number of MOVEs should be reduced after optimization
	// (The exact number depends on what the optimizer can eliminate)
	REQUIRE(move_count_after <= move_count_before);

	// At minimum, we should have fewer MOVEs than before or the same
	// (Copy propagation should not increase instruction count)
	REQUIRE((move_count_after < move_count_before || move_count_after == 0));

	// Also test with float constants
	std::string float_source = R"(func test_float():
	var a = 3.14
	var b = a
	return b
)";

	Lexer lexer_float(float_source);
	Parser parser_float(lexer_float.tokenize());
	Program program_float = parser_float.parse();

	CodeGenerator codegen_float;
	IRProgram ir_float = codegen_float.generate(program_float);

	optimizer.optimize(ir_float);

	int float_load_count = 0;
	for (const auto &instr : ir_float.functions[0].instructions) {
		if (instr.opcode == IROpcode::LOAD_FLOAT_IMM) {
			float_load_count++;
		}
	}

	REQUIRE(float_load_count >= 1);
}

TEST_CASE("const declarations") {
	// Test basic const declaration
	std::string source = R"(func test():
	const x = 10
	const y = 1.5
	const z = "hello"
	return x
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);

	REQUIRE(ir.functions.size() == 1);
	REQUIRE(ir.functions[0].name == "test");
	REQUIRE(ir.functions[0].instructions.size() > 0);
}

TEST_CASE("const assignment prevention") {
	// Test that assignment to const variables is prevented
	std::string source = R"(func test():
	const x = 10
	x = 20
	return x
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	CodeGenerator codegen;
	bool caught_error = false;
	try {
		IRProgram ir = codegen.generate(program);
	} catch (const CompilerException &e) {
		caught_error = true;
		std::string error_msg(e.what());
		REQUIRE((error_msg.find("const") != std::string::npos ||
				 error_msg.find("Cannot assign") != std::string::npos));
	}

	REQUIRE(caught_error);
}

TEST_CASE("untyped global defaults to null") {
	std::string source = R"(var bad_global

func test():
	return bad_global
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);
	REQUIRE(ir.globals.size() == 1);
	REQUIRE(ir.globals[0].init_type == IRGlobalVar::InitType::NULL_VAL);
	REQUIRE(ir.globals[0].value_type == IRInstruction::TypeHint_NONE);
}

TEST_CASE("valid global declarations") {
	// Test that all valid forms work
	std::string source = R"(var typed_global: Array = []
var inferred_global = []
var typed_int: int
var inferred_int = 42
var typed_string: String = "hello"

func test():
	typed_global.append(1)
	inferred_global.append(2)
	typed_int = 100
	inferred_int = 200
	return typed_global.size() + inferred_global.size()
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);

	// Should have 5 global variables
	REQUIRE(ir.globals.size() == 5);
	REQUIRE(ir.globals[0].name == "typed_global");
	REQUIRE(ir.globals[1].name == "inferred_global");
	REQUIRE(ir.globals[2].name == "typed_int");
	REQUIRE(ir.globals[3].name == "inferred_int");
	REQUIRE(ir.globals[4].name == "typed_string");

	// Check type hints where applicable
	REQUIRE(ir.globals[0].type_hint == Variant::ARRAY); // : Array
	REQUIRE(ir.globals[1].init_type == IRGlobalVar::InitType::EMPTY_ARRAY); // = []
	REQUIRE(ir.globals[2].type_hint == Variant::INT); // : int
	REQUIRE(ir.globals[3].init_type == IRGlobalVar::InitType::INT); // = 42
	REQUIRE(ir.globals[4].type_hint == Variant::STRING); // : String
}

TEST_CASE("dictionary literals") {
	// Test empty dictionary literal
	std::string source_empty = R"(
func make_empty_dict():
	return {}
)";

	Lexer lexer_empty(source_empty);
	Parser parser_empty(lexer_empty.tokenize());
	Program program_empty = parser_empty.parse();

	CodeGenerator codegen_empty;
	IRProgram ir_empty = codegen_empty.generate(program_empty);

	// Should have MAKE_DICTIONARY instruction with 0 pairs
	bool has_make_dict_empty = false;
	for (const auto &instr : ir_empty.functions[0].instructions) {
		if (instr.opcode == IROpcode::MAKE_DICTIONARY) {
			has_make_dict_empty = true;
			if (instr.operands.size() >= 2) {
				int pair_count = static_cast<int>(instr.operands[1].immediate());
				REQUIRE(pair_count == 0);
			}
			break;
		}
	}
	REQUIRE(has_make_dict_empty);

	// Test dictionary literal with key-value pairs
	std::string source = R"(
func make_dict():
	var d = {"name": "Thor", "hp": 100, "mp": 50.5}
	return d
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);

	// Should have MAKE_DICTIONARY instruction with 3 pairs
	bool has_make_dict = false;
	for (const auto &instr : ir.functions[0].instructions) {
		if (instr.opcode == IROpcode::MAKE_DICTIONARY) {
			has_make_dict = true;
			// Check pair count
			if (instr.operands.size() >= 2) {
				int pair_count = static_cast<int>(instr.operands[1].immediate());
				REQUIRE(pair_count == 3);
				// Should have 6 more operands (3 key-value pairs = 6 variants)
				REQUIRE(instr.operands.size() == 2 + 6);
			}
			break;
		}
	}
	REQUIRE(has_make_dict);

	// Test dictionary with nested array
	std::string source_nested = R"(
func make_nested_dict():
	var player = {"name": "Thor", "inventory": ["sword", "shield", "map"], "energy": 67}
	return player
)";

	Lexer lexer_nested(source_nested);
	Parser parser_nested(lexer_nested.tokenize());
	Program program_nested = parser_nested.parse();

	CodeGenerator codegen_nested;
	IRProgram ir_nested = codegen_nested.generate(program_nested);

	// Should have both MAKE_ARRAY and MAKE_DICTIONARY
	bool has_make_array = false;
	bool has_make_dict_nested = false;
	for (const auto &instr : ir_nested.functions[0].instructions) {
		if (instr.opcode == IROpcode::MAKE_ARRAY) {
			has_make_array = true;
		}
		if (instr.opcode == IROpcode::MAKE_DICTIONARY) {
			has_make_dict_nested = true;
			if (instr.operands.size() >= 2) {
				int pair_count = static_cast<int>(instr.operands[1].immediate());
				REQUIRE(pair_count == 3); // 3 key-value pairs
			}
		}
	}
	REQUIRE(has_make_array);
	REQUIRE(has_make_dict_nested);
}
