#include "../codegen.h"
#include "../compiler.h"
#include "../ir_interpreter.h"
#include "../ir_optimizer.h"
#include "../lexer.h"
#include "../parser.h"
#include "witness/doctest.h"
#include <algorithm>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace gdscript;

// Helper function to compile code to IR and return the IRFunction
IRFunction compile_to_ir(const std::string &source, const std::string &function_name = "test") {
	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();
	CodeGenerator codegen;
	IRProgram ir_program = codegen.generate(program);

	// Find the function
	for (auto &func : ir_program.functions) {
		if (func.name == function_name) {
			return func;
		}
	}

	throw std::runtime_error("Function not found: " + function_name);
}

IRProgram compile_program(const std::string &source, bool optimize) {
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

// Helper to count instruction types
int count_instructions(const IRFunction &func, IROpcode opcode) {
	int count = 0;
	for (const auto &instr : func.instructions) {
		if (instr.opcode == opcode) {
			count++;
		}
	}
	return count;
}

// Helper to get IR as string for debugging
std::string ir_to_string(const IRFunction &func) {
	std::stringstream ss;
	ss << "Function: " << func.name << " (max_registers: " << func.max_registers << ")\n";
	for (size_t i = 0; i < func.instructions.size(); i++) {
		const auto &instr = func.instructions[i];
		ss << "  " << i << ": ";
		switch (instr.opcode) {
			case IROpcode::LOAD_IMM:
				ss << "LOAD_IMM r" << instr.operands[0].reg_index()
				   << ", " << instr.operands[1].immediate();
				break;
			case IROpcode::LOAD_FLOAT_IMM:
				ss << "LOAD_FLOAT_IMM r" << instr.operands[0].reg_index()
				   << ", " << instr.operands[1].float_number();
				break;
			case IROpcode::MOVE:
				ss << "MOVE r" << instr.operands[0].reg_index()
				   << ", r" << instr.operands[1].reg_index();
				break;
			case IROpcode::ADD:
			case IROpcode::SUB:
			case IROpcode::MUL:
			case IROpcode::DIV:
			case IROpcode::MOD: {
				const char *op_name = "";
				switch (instr.opcode) {
					case IROpcode::ADD:
						op_name = "ADD";
						break;
					case IROpcode::SUB:
						op_name = "SUB";
						break;
					case IROpcode::MUL:
						op_name = "MUL";
						break;
					case IROpcode::DIV:
						op_name = "DIV";
						break;
					case IROpcode::MOD:
						op_name = "MOD";
						break;
					default:
						break;
				}
				ss << op_name << " r" << instr.operands[0].reg_index()
				   << ", r" << instr.operands[1].reg_index()
				   << ", r" << instr.operands[2].reg_index();
				break;
			}
			default:
				ss << "opcode_" << static_cast<int>(instr.opcode);
				break;
		}
		ss << "\n";
	}
	return ss.str();
}

TEST_CASE("pattern a basic") {
	// Pattern A: MOVE tmp1, src1; MOVE tmp2, src2; OP dst, tmp1, tmp2; MOVE result, dst
	//          -> OP result, src1, src2
	std::string source = R"(
func test(a, b):
	var c = a + b
	return c
)";

	// Compile without optimization
	IRFunction func_no_opt = compile_to_ir(source);
	int move_count_no_opt = count_instructions(func_no_opt, IROpcode::MOVE);
	int add_count_no_opt = count_instructions(func_no_opt, IROpcode::ADD);

	// Compile with optimization
	Compiler compiler;
	CompilerOptions options;
	options.dump_ir = true;

	IRFunction func = compile_to_ir(source);
	IROptimizer optimizer;
	optimizer.optimize_function(func);

	int move_count_opt = count_instructions(func, IROpcode::MOVE);
	int add_count_opt = count_instructions(func, IROpcode::ADD);

	// Pattern A should reduce some MOVEs
	std::cout << "  MOVEs: " << move_count_no_opt << " -> " << move_count_opt << std::endl;
	std::cout << "  ADDs: " << add_count_no_opt << " -> " << add_count_opt << std::endl;
}

TEST_CASE("pattern b operand1") {
	// Pattern B: MOVE tmp, src; OP dst, tmp, other; MOVE result, dst
	//          -> OP result, src, other
	std::string source = R"(
func test(a, b):
	var c = a
	var d = c + b
	return d
)";

	IRFunction func = compile_to_ir(source);
	IROptimizer optimizer;
	optimizer.optimize_function(func);
}

TEST_CASE("pattern c operand2") {
	// Pattern C: MOVE tmp, src; OP dst, other, tmp; MOVE result, dst
	//          -> OP result, other, src
	std::string source = R"(
func test(a, b):
	var c = b
	var d = a + c
	return d
)";

	IRFunction func = compile_to_ir(source);
	IROptimizer optimizer;
	optimizer.optimize_function(func);
}

TEST_CASE("pattern d move after op") {
	// Pattern D: OP dst, ...; MOVE result, dst
	//          -> OP result, ...
	std::string source = R"(
func test(a, b):
	return a + b
)";

	IRFunction func = compile_to_ir(source);
	IROptimizer optimizer;
	optimizer.optimize_function(func);
}

TEST_CASE("pattern e increment") {
	// Pattern E: MOVE tmp, var; LOAD_IMM/LOAD_FLOAT_IMM const; OP dst, tmp, const; MOVE var, dst
	//          -> LOAD_IMM/LOAD_FLOAT_IMM const; OP var, var, const
	std::string source = R"(
func test(x):
	var i = x
	i += 1
	return i
)";

	IRFunction func = compile_to_ir(source);

	// Count instructions before optimization
	int move_count_before = count_instructions(func, IROpcode::MOVE);
	int load_imm_count_before = count_instructions(func, IROpcode::LOAD_IMM);
	int add_count_before = count_instructions(func, IROpcode::ADD);

	std::cout << "  Before optimization:" << std::endl;
	std::cout << ir_to_string(func);

	IROptimizer optimizer;
	optimizer.optimize_function(func);

	// Count instructions after optimization
	int move_count_after = count_instructions(func, IROpcode::MOVE);
	int load_imm_count_after = count_instructions(func, IROpcode::LOAD_IMM);
	int add_count_after = count_instructions(func, IROpcode::ADD);

	std::cout << "  After optimization:" << std::endl;
	std::cout << ir_to_string(func);

	std::cout << "  MOVEs: " << move_count_before << " -> " << move_count_after << std::endl;
	std::cout << "  LOAD_IMM: " << load_imm_count_before << " -> " << load_imm_count_after << std::endl;
	std::cout << "  ADDs: " << add_count_before << " -> " << add_count_after << std::endl;

	// Pattern E should reduce at least 2 MOVEs (MOVE tmp,var and MOVE var,dst)
	// while keeping the LOAD_IMM (needed for the constant)
	REQUIRE((move_count_after < move_count_before && "Pattern E should reduce MOVEs"));
	REQUIRE((load_imm_count_after <= load_imm_count_before && "Pattern E should keep LOAD_IMM"));
	REQUIRE((add_count_after == add_count_before && "Pattern E should keep ADD count"));
}

TEST_CASE("pattern e float increment") {
	std::string source = R"(
func test(x):
	var i = x
	i += 1.5
	return i
)";

	IRFunction func = compile_to_ir(source);

	int move_count_before = count_instructions(func, IROpcode::MOVE);
	int load_float_count_before = count_instructions(func, IROpcode::LOAD_FLOAT_IMM);

	std::cout << "  Before optimization:" << std::endl;
	std::cout << ir_to_string(func);

	IROptimizer optimizer;
	optimizer.optimize_function(func);

	int move_count_after = count_instructions(func, IROpcode::MOVE);
	int load_float_count_after = count_instructions(func, IROpcode::LOAD_FLOAT_IMM);

	std::cout << "  After optimization:" << std::endl;
	std::cout << ir_to_string(func);

	std::cout << "  MOVEs: " << move_count_before << " -> " << move_count_after << std::endl;
	std::cout << "  LOAD_FLOAT_IMM: " << load_float_count_before << " -> " << load_float_count_after << std::endl;

	REQUIRE((move_count_after < move_count_before && "Pattern E should reduce MOVEs for floats"));
}

TEST_CASE("pattern f redundant swap") {
	// Pattern F: MOVE tmp, src; MOVE src, tmp -> eliminate both
	std::string source = R"(
func test(a):
	var b = a
	var c = b
	return c
)";

	IRFunction func = compile_to_ir(source);
	int move_count_before = count_instructions(func, IROpcode::MOVE);

	std::cout << "  Before optimization: " << move_count_before << " MOVEs" << std::endl;

	IROptimizer optimizer;
	optimizer.optimize_function(func);

	int move_count_after = count_instructions(func, IROpcode::MOVE);
	std::cout << "  After optimization: " << move_count_after << " MOVEs" << std::endl;
}

TEST_CASE("constant folding") {
	std::string source = R"(
func test():
	return 5 + 3
)";

	IRFunction func = compile_to_ir(source);

	std::cout << "  Before optimization:" << std::endl;
	std::cout << ir_to_string(func);

	IROptimizer optimizer;
	optimizer.optimize_function(func);

	std::cout << "  After optimization:" << std::endl;
	std::cout << ir_to_string(func);

	// Should be optimized to just LOAD_IMM r0, 8
	int move_count = count_instructions(func, IROpcode::MOVE);
	int add_count = count_instructions(func, IROpcode::ADD);
	int load_imm_count = count_instructions(func, IROpcode::LOAD_IMM);

	std::cout << "  Final: " << load_imm_count << " LOAD_IMM, " << add_count << " ADD, " << move_count << " MOVE" << std::endl;

	REQUIRE((add_count == 0 && "Constant folding should eliminate ADD"));
	REQUIRE((load_imm_count == 1 && "Constant folding should result in single LOAD_IMM"));
}

TEST_CASE("combined optimizations") {
	std::string source = R"(
func test():
	var sum = 0
	for i in range(10):
		sum += i
	return sum
)";

	IRFunction func = compile_to_ir(source);

	int move_count_before = count_instructions(func, IROpcode::MOVE);
	int add_count_before = count_instructions(func, IROpcode::ADD);

	std::cout << "  Before optimization: " << move_count_before << " MOVEs, " << add_count_before << " ADDs" << std::endl;

	IROptimizer optimizer;
	optimizer.optimize_function(func);

	int move_count_after = count_instructions(func, IROpcode::MOVE);
	int add_count_after = count_instructions(func, IROpcode::ADD);

	std::cout << "  After optimization: " << move_count_after << " MOVEs, " << add_count_after << " ADDs" << std::endl;
	std::cout << "  Reduced " << (move_count_before - move_count_after) << " MOVEs" << std::endl;
}

TEST_CASE("register pressure reduction") {
	std::string source = R"(
func test():
	var a = 1
	var b = 2
	var c = 3
	var d = 4
	var e = 5
	var f = 6
	return a + b + c + d + e + f
)";

	IRFunction func = compile_to_ir(source);

	std::cout << "  Max registers before optimization: " << func.max_registers << std::endl;

	IROptimizer optimizer;
	optimizer.optimize_function(func);

	std::cout << "  Max registers after optimization: " << func.max_registers << std::endl;
}

TEST_CASE("copy propagation") {
	std::string source = R"(
func test():
	var a = 5
	var b = a
	var c = b
	return c
)";

	IRFunction func = compile_to_ir(source);

	std::cout << "  Before optimization:" << std::endl;
	std::cout << ir_to_string(func);

	IROptimizer optimizer;
	optimizer.optimize_function(func);

	std::cout << "  After optimization:" << std::endl;
	std::cout << ir_to_string(func);
}

TEST_CASE("dead code elimination") {
	std::string source = R"(
func test():
	var a = 5
	var b = 10
	var c = 15
	return a + c
)";

	IRFunction func = compile_to_ir(source);

	int instr_count_before = func.instructions.size();

	std::cout << "  Instructions before: " << instr_count_before << std::endl;

	IROptimizer optimizer;
	optimizer.optimize_function(func);

	int instr_count_after = func.instructions.size();

	std::cout << "  Instructions after: " << instr_count_after << std::endl;
}

TEST_CASE("dead code elimination keeps stored globals") {
	// Regression test: STORE_GLOBAL reads its value from operand 1, but the
	// liveness analysis used to only consider a whitelist of opcodes. Because
	// STORE_GLOBAL was not on it, the LOAD_IMM defining the stored register
	// looked dead and was deleted, leaving the global initialised from an
	// uninitialised register.
	std::string source = R"(
var g = 0
var h = 0
func test():
	g = 5
	h = 7
)";

	IRFunction func = compile_to_ir(source);

	IROptimizer optimizer;
	optimizer.optimize_function(func);

	// Collect every register that a STORE_GLOBAL reads
	std::vector<int> stored_regs;
	for (const auto &instr : func.instructions) {
		if (instr.opcode == IROpcode::STORE_GLOBAL && instr.operands.size() > 1 &&
			instr.operands[1].type == IRValue::Type::REGISTER) {
			stored_regs.push_back(instr.operands[1].reg_index());
		}
	}
	REQUIRE(stored_regs.size() == 2);

	// Each of them must still be defined before it is stored
	for (int reg : stored_regs) {
		bool defined = false;
		for (const auto &instr : func.instructions) {
			if (instr.opcode == IROpcode::STORE_GLOBAL) {
				continue;
			}
			if (!instr.operands.empty() && instr.operands[0].type == IRValue::Type::REGISTER &&
				instr.operands[0].reg_index() == reg) {
				defined = true;
				break;
			}
		}
		REQUIRE((defined && "STORE_GLOBAL reads a register that is never defined"));
	}
}

// A chain of copies has to collapse onto its original source, and the copies
// that nothing reads any more have to go.
//
// Both halves used to fail, and each hid the other. Copy propagation recorded a
// MOVE as a copy and then immediately erased that record when it invalidated
// the register the MOVE had written, so no later instruction ever saw a copy to
// propagate. And dead-code elimination refused to delete any instruction that
// read a register, which a MOVE always does, so the copies left behind by the
// front end stayed to the end. A `return` at the end of a loop reached the
// backend as three Variant copies where one would do.
TEST_CASE("copy chains collapse") {
	std::cout << "Test: copy chains collapse to one move" << std::endl;

	const std::string source = R"(
func test(n):
	var total = 0
	for i in range(n):
		total += i
	return total
)";

	IRFunction unoptimized = compile_to_ir(source);
	IRFunction optimized = compile_to_ir(source);
	IROptimizer optimizer;
	optimizer.optimize_function(optimized);

	const int before = count_instructions(unoptimized, IROpcode::MOVE);
	const int after = count_instructions(optimized, IROpcode::MOVE);
	std::cout << "  MOVE instructions: " << before << " -> " << after << std::endl;
	REQUIRE((after < before && "the copy chain into the return register survived"));

	// Whatever survives has to end with the value reaching the return register,
	// which RETURN reads without naming it.
	bool defines_return_register = false;
	for (const auto &instr : optimized.instructions) {
		if (ir_destination_register(instr) == IRFunction::RETURN_REGISTER) {
			defines_return_register = true;
		}
	}
	REQUIRE((defines_return_register && "nothing writes the value that gets returned"));

	// The registers the deleted copies used are gone with them.
	std::cout << "  max_registers: " << unoptimized.max_registers
			  << " -> " << optimized.max_registers << std::endl;
	REQUIRE(optimized.max_registers < unoptimized.max_registers);

	std::cout << "  PASSED" << std::endl;
}

// Whether any instruction sits between an unconditional transfer of control and
// the next label, which is code nothing can reach.
bool has_unreachable_tail(const IRFunction &func) {
	for (size_t i = 0; i + 1 < func.instructions.size(); i++) {
		if (ir_has_effect(func.instructions[i].opcode, IR_TERMINATOR) &&
			!ir_has_effect(func.instructions[i + 1].opcode, IR_LABEL)) {
			return true;
		}
	}
	return false;
}

// Whether any jump or branch targets the label that immediately follows it.
bool has_branch_to_next(const IRFunction &func) {
	for (size_t i = 0; i + 1 < func.instructions.size(); i++) {
		const auto &instr = func.instructions[i];
		if (instr.opcode == IROpcode::SWITCH) {
			continue;
		}
		if (instr.opcode != IROpcode::JUMP && !ir_has_effect(instr.opcode, IR_BRANCH)) {
			continue;
		}
		const IRValue *target = nullptr;
		for (const auto &operand : instr.operands) {
			if (operand.type == IRValue::Type::LABEL) {
				target = &operand;
			}
		}
		if (target == nullptr) {
			continue;
		}
		for (size_t j = i + 1; j < func.instructions.size(); j++) {
			if (!ir_has_effect(func.instructions[j].opcode, IR_LABEL)) {
				break;
			}
			if (func.instructions[j].operands[0].string_id == target->string_id) {
				return true;
			}
		}
	}
	return false;
}

// Whether the function loads `value` into some register as an integer immediate.
bool loads_int_immediate(const IRFunction &func, int64_t value) {
	for (const auto &instr : func.instructions) {
		if (instr.opcode == IROpcode::LOAD_IMM &&
			instr.operands[1].immediate() == value) {
			return true;
		}
	}
	return false;
}

// Every label a jump or branch names has to exist: a pass that removes code
// must not leave a target behind.
void assert_labels_resolve(const IRFunction &func) {
	std::vector<uint32_t> defined;
	for (const auto &instr : func.instructions) {
		if (ir_has_effect(instr.opcode, IR_LABEL)) {
			defined.push_back(instr.operands[0].string_id);
		}
	}
	for (const auto &instr : func.instructions) {
		if (ir_has_effect(instr.opcode, IR_LABEL)) {
			continue;
		}
		for (const auto &operand : instr.operands) {
			if (operand.type != IRValue::Type::LABEL) {
				continue;
			}
			REQUIRE((std::find(defined.begin(), defined.end(), operand.string_id) != defined.end() &&
					 "branch target survived but its label did not"));
		}
	}
}

// A label is a join point, and clearing the constant state at one used to end
// constant folding at the first `if` in a function. Neither `a` nor `b` is
// touched by the branch, so both are still known where they are added.
TEST_CASE("constants survive a label") {
	std::string source = R"(
func test(n):
	var a = 4
	var b = 9
	if n:
		pass
	return a + b
)";

	IRFunction func = compile_to_ir(source);
	IROptimizer optimizer;
	optimizer.optimize_function(func);

	std::cout << ir_to_string(func);

	REQUIRE((count_instructions(func, IROpcode::ADD) == 0 &&
			 "the addition is of two constants and should have folded"));
	REQUIRE((loads_int_immediate(func, 13) && "4 + 9 should have folded to 13"));

	std::cout << "  \u2713 Constants survive a label" << std::endl;
}

// A branch whose condition folded to a constant is not a branch, and the arm it
// guarded is not code.
TEST_CASE("constant branch folds") {
	std::string source = R"(
func test(x):
	var flag = false
	if flag:
		return 4
	else:
		return x
)";

	IRFunction func = compile_to_ir(source);
	IROptimizer optimizer;
	optimizer.optimize_function(func);

	std::cout << ir_to_string(func);

	for (const auto &instr : func.instructions) {
		REQUIRE((!ir_has_effect(instr.opcode, IR_BRANCH) &&
				 "a branch on a known condition should not survive"));
	}
	REQUIRE((!loads_int_immediate(func, 4) &&
			 "the arm the condition rules out should not be emitted"));
	assert_labels_resolve(func);

	std::cout << "  \u2713 Constant branch folded away" << std::endl;
}

// The same, taken the other way: a condition that is true leaves the else arm
// unreachable rather than the then arm.
TEST_CASE("constant branch folds when taken") {
	std::string source = R"(
func test(x):
	var flag = 7
	if flag:
		return 4
	else:
		return x
)";

	IRFunction func = compile_to_ir(source);
	IROptimizer optimizer;
	optimizer.optimize_function(func);

	std::cout << ir_to_string(func);

	for (const auto &instr : func.instructions) {
		REQUIRE((!ir_has_effect(instr.opcode, IR_BRANCH) &&
				 "a branch on a known condition should not survive"));
	}
	REQUIRE((loads_int_immediate(func, 4) && "the arm the condition selects has to stay"));
	assert_labels_resolve(func);

	std::cout << "  \u2713 Constant branch folded away, other arm dropped" << std::endl;
}

// `if/return/else` leaves a jump after every return, and the join at the end of
// the chain is reached by nothing at all.
TEST_CASE("unreachable code removed") {
	std::string source = R"(
func test(n):
	if n < 0:
		return 1
	elif n == 0:
		return 2
	else:
		return 3
)";

	IRFunction unoptimized = compile_to_ir(source);
	REQUIRE((has_unreachable_tail(unoptimized) &&
			 "the reproduction needs code after a terminator to remove"));

	IRFunction func = compile_to_ir(source);
	IROptimizer optimizer;
	optimizer.optimize_function(func);

	std::cout << ir_to_string(func);

	REQUIRE((!has_unreachable_tail(func) && "nothing may follow a terminator but a label"));
	assert_labels_resolve(func);

	std::cout << "  \u2713 Unreachable code removed" << std::endl;
}

// The last arm of a match jumps to the end label that immediately follows it.
TEST_CASE("branch to next removed") {
	std::string source = R"(
func test(op):
	var r = 0
	match op:
		1:
			r = 10
		2:
			r = 20
		_:
			r = 30
	return r
)";

	IRFunction unoptimized = compile_to_ir(source);
	REQUIRE((has_branch_to_next(unoptimized) &&
			 "the reproduction needs a jump to the following label to remove"));

	IRFunction func = compile_to_ir(source);
	IROptimizer optimizer;
	optimizer.optimize_function(func);

	std::cout << ir_to_string(func);

	REQUIRE((!has_branch_to_next(func) && "a jump to the next instruction is a no-op"));
	assert_labels_resolve(func);

	std::cout << "  \u2713 Branch to the next instruction removed" << std::endl;
}

TEST_CASE("move after call folds into call destination") {
	IRFunction func = compile_to_ir(
			"func source():\n"
			"\treturn 4\n"
			"func test():\n"
			"\tvar value = source()\n"
			"\treturn value\n");
	IROptimizer optimizer;
	optimizer.optimize_function(func);

	REQUIRE(count_instructions(func, IROpcode::CALL) == 1);
	REQUIRE(count_instructions(func, IROpcode::MOVE) == 0);
	for (const IRInstruction &instr : func.instructions) {
		if (instr.opcode == IROpcode::CALL) {
			REQUIRE(ir_destination_register(instr) == IRFunction::RETURN_REGISTER);
		}
	}
}

TEST_CASE("struct scalar replacement snapshots fields") {
	IRProgram ir = compile_program(
			"struct Point:\n"
			"\tvar x = 0\n"
			"\tvar y = 0\n\n"
			"func test():\n"
			"\tvar i = 4\n"
			"\tvar p = Point(i, 0)\n"
			"\ti += 1\n"
			"\treturn p.x\n\n"
			"func test_set():\n"
			"\tvar i = 4\n"
			"\tvar p = Point(0, 0)\n"
			"\tp.x = i\n"
			"\ti += 1\n"
			"\treturn p.x\n",
			true);
	const IRFunction &func = ir.functions.front();
	REQUIRE(count_instructions(func, IROpcode::MAKE_DICTIONARY_KEYED) == 0);
	REQUIRE(count_instructions(func, IROpcode::DICT_GET_CONST) == 0);
	const IRFunction &set_func = ir.functions.back();
	REQUIRE(count_instructions(set_func, IROpcode::MAKE_DICTIONARY_KEYED) == 0);
	REQUIRE(count_instructions(set_func, IROpcode::DICT_SET_CONST) == 0);
	IRInterpreter interpreter(ir);
	REQUIRE((std::get<int64_t>(interpreter.call("test")) == 4 &&
			 "a constructed field must retain its source value"));
	REQUIRE((std::get<int64_t>(interpreter.call("test_set")) == 4 &&
			 "an assigned field must retain its source value"));
}

TEST_CASE("immutable struct scalar replacement crosses a loop") {
	IRProgram ir = compile_program(
			"struct Point:\n"
			"\tvar x = 0\n"
			"\tvar y = 0\n\n"
			"func test(n):\n"
			"\tvar i = 0\n"
			"\tvar acc = 0\n"
			"\twhile i < n:\n"
			"\t\tvar p = Point(i, i + 1)\n"
			"\t\ti += 1\n"
			"\t\tacc += p.x\n"
			"\treturn acc\n",
			true);
	const IRFunction &func = ir.functions.front();
	REQUIRE(count_instructions(func, IROpcode::MAKE_DICTIONARY_KEYED) == 0);
	REQUIRE(count_instructions(func, IROpcode::DICT_GET_CONST) == 0);
	IRInterpreter interpreter(ir);
	REQUIRE(std::get<int64_t>(interpreter.call("test", { int64_t(5) })) == 10);
}

// ---------------------------------------------------------------------------
// Properties, and the falsifiability check that keeps them honest.
// ---------------------------------------------------------------------------

#include "property_support.h"

namespace {

// A small arithmetic function over one parameter, which both an optimized and
// an unoptimized build have to compute the same way.
struct ArithmeticProgram {
	std::string body;
	int64_t argument = 0;

	std::string source() const {
		return "func f(n):\n" + body + "\treturn total\n";
	}
};

ArithmeticProgram generate_arithmetic(witness::RNG &rng, const witness::Level &level) {
	static const char OPS[] = { '+', '-', '*' };
	const int64_t bound = std::max<int64_t>(2, level.fin_bound / 16);
	const uint32_t statements = rng.uint_range(1, std::max<uint32_t>(2, uint32_t(level.fin_bound) / 64));

	ArithmeticProgram program;
	program.argument = rng.int64_range(-bound, bound);
	program.body = "\tvar total = n\n";
	for (uint32_t i = 0; i < statements; i++) {
		const char op = OPS[rng.uint_range(0, 2)];
		const int64_t operand = rng.int64_range(-bound, bound);
		// Dead stores, folded constants and a live use of the parameter: the
		// shapes the optimizer's passes are supposed to act on.
		program.body += "\tvar dead" + std::to_string(i) + " = " +
				std::to_string(operand) + " * 2\n";
		program.body += "\ttotal = total " + std::string(1, op) + " " +
				std::to_string(operand) + "\n";
	}
	return program;
}

bool run_with(const ArithmeticProgram &program, bool optimize, int64_t &out) {
	Compiler compiler;
	CompilerOptions options;
	options.output_elf = false;
	options.optimize = optimize;
	auto ir = compiler.compile_to_ir(program.source(), options);
	if (!ir.has_value()) {
		return false;
	}
	IRInterpreter interpreter(*ir);
	const IRInterpreter::Value result = interpreter.call("f", { program.argument });
	if (!std::holds_alternative<int64_t>(result)) {
		return false;
	}
	out = std::get<int64_t>(result);
	return true;
}

size_t instruction_count(const ArithmeticProgram &program, bool optimize) {
	Compiler compiler;
	CompilerOptions options;
	options.output_elf = false;
	options.optimize = optimize;
	auto ir = compiler.compile_to_ir(program.source(), options);
	if (!ir.has_value()) {
		return SIZE_MAX;
	}
	size_t count = 0;
	for (const IRFunction &function : ir->functions) {
		count += function.instructions.size();
	}
	return count;
}

} // namespace

TEST_CASE("property: optimizing does not change what a program computes") {
	PROP_HOLDS(ArithmeticProgram, "the optimized and unoptimized runs agree",
			   &generate_arithmetic,
			   [](const ArithmeticProgram &program) {
				   int64_t plain = 0;
				   int64_t optimized = 0;
				   if (!run_with(program, false, plain) || !run_with(program, true, optimized)) {
					   return false;
				   }
				   return plain == optimized;
			   });
}

TEST_CASE("falsifiability: the generator reaches programs the optimizer shortens") {
	// False on purpose: every generated program has a dead store in it, so the
	// optimizer has something to remove. A ladder that finds no program where
	// the counts differ is a ladder generating programs with nothing to
	// optimize, and the property above would then be comparing two identical
	// builds.
	PROP_FALSIFIABLE(ArithmeticProgram, "optimizing never changes the instruction count",
					 &generate_arithmetic,
					 [](const ArithmeticProgram &program) {
						 return instruction_count(program, false) == instruction_count(program, true);
					 });
}
