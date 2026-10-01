#include "../syscall_abi.h"
// Regression tests for compiler bugs that produced wrong code silently.
//
// Every test here pins down a behaviour that was previously wrong in a way that
// no existing test noticed: the program still compiled, it just did the wrong
// thing at run time. Each one names the mistake it guards against.
#include "../codegen.h"
#include "../compiler_exception.h"
#include "../instance_layout.h"
#include "../ir_interpreter.h"
#include "../ir_optimizer.h"
#include "../lexer.h"
#include "../parser.h"
#include "../riscv_codegen.h"
#include "../syscall_numbers.h"
#include "../variant_layout.h"
#include "witness/doctest.h"
#include <algorithm>
#include <climits>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace gdscript;

// -= Helpers =-

static IRProgram compile_to_ir(const std::string &source, bool optimize = true) {
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

static const IRGlobalVar &find_global(const IRProgram &ir, const std::string &name) {
	for (const auto &global : ir.globals) {
		if (global.name == name) {
			return global;
		}
	}
	throw std::runtime_error("Global not found: " + name);
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

static IRInterpreter::Value run(const std::string &source, const std::string &function,
								const std::vector<IRInterpreter::Value> &args = {}) {
	IRProgram ir = compile_to_ir(source);
	IRInterpreter interpreter(ir);
	return interpreter.call(function, args);
}

static int64_t run_int(const std::string &source, const std::string &function,
					   const std::vector<IRInterpreter::Value> &args = {}) {
	IRInterpreter::Value value = run(source, function, args);
	if (std::holds_alternative<bool>(value)) {
		return std::get<bool>(value) ? 1 : 0;
	}
	return std::get<int64_t>(value);
}

// Returns true when compiling the source throws a CompilerException.
static bool rejects(const std::string &source) {
	try {
		compile_to_ir(source, false);
		return false;
	} catch (const CompilerException &) {
		return true;
	}
}

static std::vector<uint8_t> compile_to_code(const std::string &source) {
	IRProgram ir = compile_to_ir(source);
	RISCVCodeGen riscv{ VariantLayout(false) };
	return riscv.generate(ir);
}

static uint32_t word_at(const std::vector<uint8_t> &code, size_t off) {
	return uint32_t(code[off]) | (uint32_t(code[off + 1]) << 8) |
			(uint32_t(code[off + 2]) << 16) | (uint32_t(code[off + 3]) << 24);
}

// -= Tests =-

// Locals shadow globals. The lookup used to consult the global table first, so a
// local sharing a name with a global was never seen: reads and writes both went
// to the global, which the function then also corrupted.
TEST_CASE("locals shadow globals") {
	const std::string source =
			"var counter = 10\n"
			"func test():\n"
			"\tvar counter = 1\n"
			"\tcounter = counter + 1\n"
			"\treturn counter\n";

	const IRProgram ir = compile_to_ir(source);
	const IRFunction &test = find_function(ir, "test");
	REQUIRE(count_opcode(test, IROpcode::LOAD_GLOBAL) == 0);
	REQUIRE(count_opcode(test, IROpcode::STORE_GLOBAL) == 0);
	REQUIRE(run_int(source, "test") == 2);

	// Parameters shadow globals too.
	const std::string param_source =
			"var value = 100\n"
			"func test(value):\n"
			"\treturn value\n";
	REQUIRE(count_opcode(find_function(compile_to_ir(param_source), "test"), IROpcode::LOAD_GLOBAL) == 0);

	// Without a local of that name the global is still reached.
	const std::string global_source =
			"var counter = 10\n"
			"func test():\n"
			"\tcounter = counter + 1\n"
			"\treturn counter\n";
	REQUIRE(count_opcode(find_function(compile_to_ir(global_source), "test"), IROpcode::STORE_GLOBAL) == 1);

	// Assigning to a global const is an error, as it is for a local const.
	REQUIRE(rejects("const LIMIT = 3\nfunc test():\n\tLIMIT = 4\n\treturn LIMIT\n"));
}

// The operand-role table. Passes used to assume "operand 0 is the destination,
// every other operand is a source", which is wrong for CALL (destination in
// operand 1) and for VSET/STORE_GLOBAL/RETURN/branches (operand 0 is read).
TEST_CASE("operand roles") {
	IRStringTable strings;

	{
		// CALL name, dst, argc, args...
		IRInstruction call(IROpcode::CALL);
		call.operands.push_back(IRValue::str(strings.intern("f")));
		call.operands.push_back(IRValue::reg(3)); // destination
		call.operands.push_back(IRValue::imm(1));
		call.operands.push_back(IRValue::reg(7)); // argument
		REQUIRE(ir_destination_register(call) == 3);
		REQUIRE(!ir_reads_operand(call, 1));
		REQUIRE(ir_reads_operand(call, 3));
	}

	{
		// VSET obj, name_idx, name_len, value - no destination, operand 0 is read.
		IRInstruction vset(IROpcode::VSET);
		vset.operands.push_back(IRValue::reg(2));
		vset.operands.push_back(IRValue::imm(0));
		vset.operands.push_back(IRValue::imm(4));
		vset.operands.push_back(IRValue::reg(5));
		REQUIRE(ir_destination_register(vset) == -1);
		REQUIRE(ir_reads_operand(vset, 0));
		REQUIRE(ir_reads_operand(vset, 3));
	}

	{
		// STORE_GLOBAL index, value - operand 0 is an immediate, not a register.
		IRInstruction store(IROpcode::STORE_GLOBAL, IRValue::imm(0), IRValue::reg(4));
		REQUIRE(ir_destination_register(store) == -1);
		REQUIRE(!ir_reads_operand(store, 0));
		REQUIRE(ir_reads_operand(store, 1));
	}

	{
		// A bare RETURN reads r0 implicitly.
		std::vector<int> reads;
		ir_collect_read_registers(IRInstruction(IROpcode::RETURN), reads);
		REQUIRE((reads.size() == 1 && reads[0] == 0));
	}
}

// A pending MOVE must not be delayed past an instruction that reads the register
// it writes. VSET reads its object out of operand 0, which the dead-store pass
// used to skip, so the VSET saw whatever the register held beforehand.
TEST_CASE("store not delayed past vset") {
	IRFunction func;
	func.name = "test";
	func.instructions.emplace_back(IROpcode::LOAD_IMM, IRValue::reg(0), IRValue::imm(1));
	func.instructions.emplace_back(IROpcode::MOVE, IRValue::reg(1), IRValue::reg(0));
	IRInstruction vset(IROpcode::VSET);
	vset.operands.push_back(IRValue::reg(1)); // object being written into
	vset.operands.push_back(IRValue::imm(0));
	vset.operands.push_back(IRValue::imm(1));
	vset.operands.push_back(IRValue::reg(0)); // value
	func.instructions.push_back(vset);
	func.instructions.emplace_back(IROpcode::RETURN);
	func.max_registers = 2;

	IROptimizer optimizer;
	optimizer.optimize_function(func);

	// Whatever else the optimizer does, r1 has to be defined before the VSET.
	size_t move_index = SIZE_MAX;
	size_t vset_index = SIZE_MAX;
	for (size_t i = 0; i < func.instructions.size(); i++) {
		const auto &instr = func.instructions[i];
		if (instr.opcode == IROpcode::VSET) {
			vset_index = i;
		} else if (ir_destination_register(instr) == 1) {
			move_index = i;
		}
	}
	REQUIRE(vset_index != SIZE_MAX);
	REQUIRE(move_index != SIZE_MAX);
	REQUIRE(move_index < vset_index);
}

// Copy propagation has to kill the register a CALL defines. Because CALL keeps
// its destination in operand 1, the kill used to be skipped and a constant that
// had previously lived in that register was propagated over the call result.
TEST_CASE("call result kills constant") {
	IRStringTable strings;
	IRFunction func;
	func.name = "test";
	func.instructions.emplace_back(IROpcode::LOAD_IMM, IRValue::reg(1), IRValue::imm(42));
	IRInstruction call(IROpcode::CALL);
	call.operands.push_back(IRValue::str(strings.intern("side")));
	call.operands.push_back(IRValue::reg(1)); // overwrites r1 with the call result
	call.operands.push_back(IRValue::imm(0));
	func.instructions.push_back(call);
	func.instructions.emplace_back(IROpcode::MOVE, IRValue::reg(0), IRValue::reg(1));
	func.instructions.emplace_back(IROpcode::RETURN);
	func.max_registers = 2;

	IROptimizer optimizer;
	optimizer.optimize_function(func);

	// The MOVE must not have become "LOAD_IMM r0, 42".
	for (const auto &instr : func.instructions) {
		if (instr.opcode == IROpcode::LOAD_IMM && ir_destination_register(instr) == 0) {
			FAIL("call result was replaced by a stale constant");
		}
	}
}

// Register types are per-function. Virtual register numbers restart at 0 in
// every function, so a type left on r0 by one function used to be inherited by
// the next, sending the backend down a native path for the wrong Variant type.
TEST_CASE("register types do not leak between functions") {
	const std::string source =
			"func first():\n"
			"\treturn 1\n"
			"func second(a):\n"
			"\tif a:\n"
			"\t\treturn 1\n"
			"\treturn 0\n";

	const IRProgram ir = compile_to_ir(source);
	const IRFunction &second = find_function(ir, "second");
	for (const auto &instr : second.instructions) {
		if (instr.opcode == IROpcode::BRANCH_ZERO) {
			// 'a' is an untyped parameter, so nothing is known about its type.
			REQUIRE(instr.type_hint == IRInstruction::TypeHint_NONE);
		}
	}
}

// A global's address is `.globals + index * sizeof(Variant)`. Emitting that as
// `la` followed by `addi` truncates to a 12-bit signed immediate, so every
// global past #85 (or #51 in a double-precision build) addressed the wrong slot.
TEST_CASE("large global offsets") {
	std::string source;
	const int global_count = 200;
	for (int i = 0; i < global_count; i++) {
		source += "static var g" + std::to_string(i) + " = " + std::to_string(i) + "\n";
	}
	source += "func test():\n\treturn g199\n";

	const std::vector<uint8_t> code = compile_to_code(source);

	// No ADDI may carry a truncated global offset: every offset is folded into
	// the AUIPC/ADDI pair by the relocation instead. `static var` so the slots
	// are in the data area; a member is addressed off the base register, where
	// a wide offset is materialized by LUI + ADDI and 680 is a legitimate half.
	for (size_t off = 0; off + 4 <= code.size(); off += 4) {
		const uint32_t instr = word_at(code, off);
		if ((instr & 0x7F) != 0x13 || ((instr >> 12) & 7) != 0) {
			continue; // not an ADDI
		}
		const int32_t imm = int32_t(instr) >> 20;
		// 199 * 24 = 4776 does not fit a signed 12-bit immediate; it wraps to
		// 4776 - 4096 = 680. Finding that as an ADDI immediate would mean a
		// global offset had been silently truncated.
		REQUIRE(imm != 680);
	}

	// The same for members: no ADDI off the base register may carry the wrap.
	{
		std::string members;
		for (int i = 0; i < global_count; i++) {
			members += "var g" + std::to_string(i) + " = " + std::to_string(i) + "\n";
		}
		members += "func test():\n\treturn g199\n";
		const std::vector<uint8_t> member_code = compile_to_code(members);
		constexpr uint8_t REG_TP = 4;
		for (size_t off = 0; off + 4 <= member_code.size(); off += 4) {
			const uint32_t instr = word_at(member_code, off);
			if ((instr & 0x7F) != 0x13 || ((instr >> 12) & 7) != 0) {
				continue;
			}
			if (((instr >> 15) & 0x1F) != REG_TP) {
				continue;
			}
			REQUIRE((int32_t(instr) >> 20) != 680);
		}
	}

	// The data area covers all 200 Variants.
	IRProgram ir = compile_to_ir(source);
	RISCVCodeGen riscv{ VariantLayout(false) };
	riscv.generate(ir);
	REQUIRE(riscv.get_global_data_size() >= size_t(global_count) * 24);
}

// An int constant is materialized by LUI + a 12-bit add. Rounding the split to
// the nearest 4K carries into bit 31 for anything above 0x7FFFF7FF, which
// overflowed the int32 the carry was computed in: the upper half became
// 0x80000000, LUI sign-extended it to a 64-bit negative, and a plain ADDI left
// the value 2^32 too small. `return 0x7FFFFFFF` handed back -2147483649.
TEST_CASE("int32 immediates at the lui carry boundary") {
	// The 64-bit value an LUI, alone or followed by ADDI/ADDIW on the same
	// register, leaves in that register — RV64 semantics, sign extension and all.
	const auto materialized_values = [](const std::vector<uint8_t> &code) {
		std::vector<int64_t> values;
		for (size_t off = 0; off + 4 <= code.size(); off += 4) {
			const uint32_t lui = word_at(code, off);
			if ((lui & 0x7F) != 0x37) {
				continue;
			}
			const uint8_t rd = (lui >> 7) & 0x1F;
			// LUI's immediate is bits 31:12 of a sign-extended 32-bit value.
			int64_t value = int64_t(int32_t(lui & 0xFFFFF000));
			if (off + 8 <= code.size()) {
				const uint32_t next = word_at(code, off + 4);
				const uint8_t opcode = next & 0x7F;
				const bool addi = opcode == 0x13 || opcode == 0x1B;
				if (addi && ((next >> 12) & 7) == 0 &&
					((next >> 15) & 0x1F) == rd && ((next >> 7) & 0x1F) == rd) {
					value += int64_t(int32_t(next) >> 20);
					if (opcode == 0x1B) { // ADDIW truncates to 32 signed bits
						value = int64_t(int32_t(uint32_t(value)));
					}
				}
			}
			values.push_back(value);
		}
		return values;
	};

	// 0x7FFFF7FF is the last value the carry does not overflow; 0x80000000 is
	// past int32 and travels in the constant pool instead. Everything between
	// was wrong by 2^32.
	const int64_t constants[] = {
		2147481599, // 0x7FFFF7FF
		2147481600, // 0x7FFFF800, first value that carries into bit 31
		2147483646,
		2147483647, // INT32_MAX
		-2147481600,
		-2147483648, // INT32_MIN
		131072, // an ordinary LUI-only value, no low half
		1048577, // ordinary LUI + positive low half
		-1048577, // ordinary LUI + negative low half
	};
	for (const int64_t constant : constants) {
		const std::vector<uint8_t> code =
				compile_to_code("func test():\n\treturn " + std::to_string(constant) + "\n");
		const std::vector<int64_t> values = materialized_values(code);
		REQUIRE(std::find(values.begin(), values.end(), constant) != values.end());
	}
}

// Global initializers that are not compile-time constants. Every one of these
// used to fall through the initializer matcher silently and leave the global
// NIL, with no diagnostic.
TEST_CASE("global initializer forms") {
	// Unary minus over a literal is a constant, not a runtime expression.
	{
		const IRProgram ir = compile_to_ir("var x = -5\nvar f = -2.5\nfunc test():\n\treturn x\n");
		const IRGlobalVar &x = find_global(ir, "x");
		REQUIRE(x.init_type == IRGlobalVar::InitType::INT);
		REQUIRE(std::get<int64_t>(x.init_value) == -5);
		const IRGlobalVar &f = find_global(ir, "f");
		REQUIRE(f.init_type == IRGlobalVar::InitType::FLOAT);
		REQUIRE(std::get<double>(f.init_value) == -2.5);
		REQUIRE(!ir.has_global_init); // both fold, so nothing runs at startup
	}

	// A reference to an earlier const folds to that const's value.
	{
		const IRProgram ir = compile_to_ir("const M = 10\nvar y = M\nfunc test():\n\treturn y\n");
		const IRGlobalVar &y = find_global(ir, "y");
		REQUIRE(y.init_type == IRGlobalVar::InitType::INT);
		REQUIRE(std::get<int64_t>(y.init_value) == 10);
	}

	// Referring to a global declared later would read NIL, so it is rejected.
	// A member read before its declaration holds its type's default, null here, as
	// GDScript runs initializers in order; the typed cases are in test_gdscript_syntax.
	REQUIRE(!rejects("var a = b\nvar b = 1\nfunc test():\n\treturn a\n"));

	// Non-empty containers, nesting and packed arrays run at startup.
	{
		const IRProgram ir = compile_to_ir(
				"var a = [1, 2, 3]\n"
				"var d = {\"k\": 1}\n"
				"var n = [[1, 2], {\"k\": 3}]\n"
				"var p = PackedInt32Array()\n"
				"func test():\n"
				"\treturn a\n");
		// Members, so they are built per instance rather than at startup.
		REQUIRE(ir.has_member_init);
		REQUIRE(!ir.has_global_init);
		for (const char *name : { "a", "d", "n", "p" }) {
			REQUIRE(find_global(ir, name).init_type == IRGlobalVar::InitType::RUNTIME);
		}
		// Untyped containers still get a Variant type for @export registration.
		REQUIRE(find_global(ir, "a").value_type == Variant::ARRAY);
		REQUIRE(find_global(ir, "d").value_type == Variant::DICTIONARY);
		REQUIRE(find_global(ir, "p").value_type == Variant::PACKED_INT32_ARRAY);
		REQUIRE(count_opcode(ir.member_init, IROpcode::STORE_GLOBAL) == 4);
	}

	// Empty containers stay compile-time constants: no startup code for them.
	{
		const IRProgram ir = compile_to_ir("var a = []\nvar d = {}\nfunc test():\n\treturn a\n");
		REQUIRE(find_global(ir, "a").init_type == IRGlobalVar::InitType::EMPTY_ARRAY);
		REQUIRE(find_global(ir, "d").init_type == IRGlobalVar::InitType::EMPTY_DICT);
		REQUIRE(!ir.has_global_init);
		REQUIRE(!ir.has_member_init);
	}

	// A type-hinted global with no initializer gets its type's default value,
	// the way GDScript does, rather than staying NIL.
	{
		const IRProgram ir = compile_to_ir(
				"var a: Array\nvar s: String\nvar n: int\nvar f: float\nvar p: PackedByteArray\n"
				"func test():\n\treturn n\n");
		REQUIRE(find_global(ir, "a").init_type == IRGlobalVar::InitType::EMPTY_ARRAY);
		REQUIRE(find_global(ir, "s").init_type == IRGlobalVar::InitType::STRING);
		REQUIRE(find_global(ir, "n").init_type == IRGlobalVar::InitType::INT);
		REQUIRE(find_global(ir, "f").init_type == IRGlobalVar::InitType::FLOAT);
		REQUIRE(find_global(ir, "p").init_type == IRGlobalVar::InitType::RUNTIME);
	}

	// Declaring the same global twice is an error rather than a silent shadow.
	REQUIRE(rejects("var a = 1\nvar a = 2\nfunc test():\n\treturn a\n"));
}

// The global init function runs before any @export property is registered, so a
// property is registered holding the value it was declared with.
TEST_CASE("global init runs before property registration") {
	const std::string source =
			"@export var items = [1, 2]\n"
			"func test():\n"
			"\treturn items\n";

	const IRProgram ir = compile_to_ir(source);
	REQUIRE(ir.has_member_init);
	REQUIRE(find_global(ir, "items").is_property);
	REQUIRE(find_global(ir, "items").value_type == Variant::ARRAY);

	// The init function is internal: it must not be exported as a callable.
	RISCVCodeGen riscv{ VariantLayout(false) };
	riscv.generate(ir);
	REQUIRE(riscv.get_function_offsets().count("__init_globals") == 0);
	REQUIRE(riscv.get_function_offsets().count(".init_globals") == 0);
	REQUIRE(riscv.get_function_offsets().count("__init_members") == 0);

	// One member, the shared return slot the initializers write into, and the
	// blob describing the record.
	REQUIRE(riscv.get_global_data_size() == 2 * 24 + size_t(InstanceLayout::BLOB_SIZE));
}

// 'and' and 'or' short-circuit in GDScript: the right-hand side is not evaluated
// when the left already decides the result. Lowering them to a binary IR op ran
// the right side's side effects unconditionally.
TEST_CASE("logical short circuit") {
	const std::string source =
			"func side():\n"
			"\treturn 1\n"
			"func test(a):\n"
			"\tif a and side():\n"
			"\t\treturn 1\n"
			"\treturn 0\n";

	const IRProgram ir = compile_to_ir(source);
	const IRFunction &test = find_function(ir, "test");
	REQUIRE(count_opcode(test, IROpcode::AND) == 0);
	REQUIRE(count_opcode(test, IROpcode::OR) == 0);

	// The call has to sit between the two short-circuit tests, not before them.
	size_t first_branch = SIZE_MAX;
	size_t call_index = SIZE_MAX;
	for (size_t i = 0; i < test.instructions.size(); i++) {
		if (test.instructions[i].opcode == IROpcode::BRANCH_ZERO && first_branch == SIZE_MAX) {
			first_branch = i;
		} else if (test.instructions[i].opcode == IROpcode::CALL) {
			call_index = i;
		}
	}
	REQUIRE(first_branch != SIZE_MAX);
	REQUIRE(call_index != SIZE_MAX);
	REQUIRE(first_branch < call_index);

	// Values are what GDScript produces, and both operators are bools.
	REQUIRE(run_int("func test():\n\treturn 1 and 1\n", "test") == 1);
	REQUIRE(run_int("func test():\n\treturn 1 and 0\n", "test") == 0);
	REQUIRE(run_int("func test():\n\treturn 0 and 1\n", "test") == 0);
	REQUIRE(run_int("func test():\n\treturn 0 or 0\n", "test") == 0);
	REQUIRE(run_int("func test():\n\treturn 0 or 1\n", "test") == 1);
	REQUIRE(run_int("func test():\n\treturn 1 or 0\n", "test") == 1);
	REQUIRE(std::holds_alternative<bool>(run("func test():\n\treturn 1 and 1\n", "test")));

	// 5 and 3 is true, not 3: the operators booleanize.
	REQUIRE(run_int("func test():\n\treturn 5 and 3\n", "test") == 1);
}

// Truthiness follows Variant::booleanize(). Testing only the payload's low byte
// makes 256 false, and makes any scoped index whose low byte is zero false too.
TEST_CASE("truthiness") {
	REQUIRE(run_int("func test():\n\tif 256:\n\t\treturn 1\n\treturn 0\n", "test") == 1);
	REQUIRE(run_int("func test():\n\tif 0:\n\t\treturn 1\n\treturn 0\n", "test") == 0);
	REQUIRE(run_int("func test():\n\tif 0.5:\n\t\treturn 1\n\treturn 0\n", "test") == 1);
	REQUIRE(run_int("func test():\n\tif 0.0:\n\t\treturn 1\n\treturn 0\n", "test") == 0);

	// A comparison result is a bool, not an int of the operand's type. Tracking
	// it as INT made the backend read eight bytes of a one-byte payload.
	// Unoptimized: the optimizer fuses a comparison and its branch into a
	// BRANCH_LT, which is exactly the branch this is not about.
	const IRProgram ir = compile_to_ir(
			"func test(a: int, b: int):\n\tvar c = a < b\n\tif c:\n\t\treturn 1\n\treturn 0\n", false);
	const IRFunction &test = find_function(ir, "test");
	bool checked = false;
	for (const auto &instr : test.instructions) {
		if (instr.opcode == IROpcode::BRANCH_ZERO && instr.type_hint != IRInstruction::TypeHint_NONE) {
			REQUIRE(instr.type_hint == Variant::BOOL);
			checked = true;
		}
	}
	REQUIRE(checked);
}

// A global whose every read takes its address directly has no frame copy. The
// branch's truthiness test dropped that base register and handed the host an
// SP-relative pointer anyway, so `if platform:` booleanized whatever sat at the
// bottom of the frame -- reliably false for an object member.
TEST_CASE("truthiness of a global read in place") {
	// Declared by class name, so the slot carries no Variant type and the branch
	// has to ask the host. `r` keeps the branch off the return register, which is
	// excluded from the frame-copy elision.
	const std::string source =
			"var platform: Sprite2D\n"
			"func test(a):\n"
			"\tvar r = a\n"
			"\tif platform:\n"
			"\t\tr = 1\n"
			"\treturn r\n";

	REQUIRE(count_opcode(find_function(compile_to_ir(source), "test"), IROpcode::BRANCH_ZERO) == 1);

	constexpr uint8_t REG_SP = 2;
	constexpr uint8_t REG_A1 = 11;
	constexpr uint8_t REG_A7 = 17;
	const auto is_addi = [](uint32_t instr) {
		return (instr & 0x7F) == 0x13 && ((instr >> 12) & 7) == 0;
	};
	const auto rd_of = [](uint32_t instr) { return uint8_t((instr >> 7) & 0x1F); };
	const auto rs1_of = [](uint32_t instr) { return uint8_t((instr >> 15) & 0x1F); };

	const std::vector<uint8_t> code = compile_to_code(source);
	bool checked = false;
	for (size_t off = 0; off + 4 <= code.size(); off += 4) {
		const uint32_t instr = word_at(code, off);
		// li a7, ECALL_VEVAL
		if (!(gdscript::valid_counted_syscall_encoding(instr) && (instr >> 20) == ECALL_VEVAL) &&
			(!is_addi(instr) || rd_of(instr) != REG_A7 || rs1_of(instr) != 0 ||
			 (int32_t(instr) >> 20) != ECALL_VEVAL)) {
			continue;
		}
		// a1 is the operand pointer: the last write of it before the syscall.
		bool found = false;
		for (size_t back = off; back >= 4 && !found; back -= 4) {
			const uint32_t prev = word_at(code, back - 4);
			if (!is_addi(prev) || rd_of(prev) != REG_A1) {
				continue;
			}
			REQUIRE(rs1_of(prev) != REG_SP);
			found = true;
		}
		REQUIRE(found);
		checked = true;
	}
	REQUIRE(checked);
}

// A declared type is a promise about the value, not just a label on it.
TEST_CASE("declared type coercion") {
	// `var f: float = 0` holds 0.0. Folding it as an INT while registering the
	// global as FLOAT hands Godot a Variant of the wrong type.
	{
		const IRProgram ir = compile_to_ir("var f: float = 0\nfunc test():\n\treturn f\n");
		const IRGlobalVar &f = find_global(ir, "f");
		REQUIRE(f.init_type == IRGlobalVar::InitType::FLOAT);
		REQUIRE(std::get<double>(f.init_value) == 0.0);
		REQUIRE(f.value_type == Variant::FLOAT);
	}

	// The same for locals, where the mismatch made the backend read an integer
	// payload as a double.
	{
		const IRProgram ir = compile_to_ir("func test():\n\tvar g: float = 2\n\treturn g\n", false);
		const IRFunction &test = find_function(ir, "test");
		REQUIRE(count_opcode(test, IROpcode::CONVERT) == 1);
	}
	REQUIRE(std::get<double>(run("func test():\n\tvar g: float = 2\n\treturn g\n", "test")) == 2.0);

	// Mismatches GDScript rejects are rejected here rather than reinterpreted.
	REQUIRE(rejects("var s: String = 5\nfunc test():\n\treturn s\n"));
	REQUIRE(rejects("func test():\n\tvar i: int = 1.5\n\treturn i\n"));
}

// Bitwise and shift operators. These are implemented but were never covered, so
// the integer-only rules and the fallback path went unchecked.
TEST_CASE("bitwise and shifts") {
	REQUIRE(run_int("func test():\n\treturn 12 & 10\n", "test") == 8);
	REQUIRE(run_int("func test():\n\treturn 12 | 10\n", "test") == 14);
	REQUIRE(run_int("func test():\n\treturn 12 ^ 10\n", "test") == 6);
	REQUIRE(run_int("func test():\n\treturn ~0\n", "test") == -1);
	REQUIRE(run_int("func test():\n\treturn 1 << 10\n", "test") == 1024);
	REQUIRE(run_int("func test():\n\treturn 1024 >> 3\n", "test") == 128);
	REQUIRE(run_int("func test():\n\treturn -8 >> 1\n", "test") == -4);

	// Compound assignment forms lower to the same operators.
	REQUIRE(run_int("func test():\n\tvar x = 12\n\tx ^= 10\n\treturn x\n", "test") == 6);
	REQUIRE(run_int("func test():\n\tvar x = 1\n\tx <<= 4\n\treturn x\n", "test") == 16);
	REQUIRE(run_int("func test():\n\tvar x = 64\n\tx >>= 4\n\treturn x\n", "test") == 4);
	REQUIRE(run_int("func test():\n\tvar x = 12\n\tx &= 10\n\treturn x\n", "test") == 8);
	REQUIRE(run_int("func test():\n\tvar x = 12\n\tx |= 10\n\treturn x\n", "test") == 14);

	// Only two known integers take the native path; anything else falls back to
	// the host, which reports a type error at run time rather than producing
	// nonsense from a float payload.
	{
		const IRProgram typed_ir = compile_to_ir("func test(a: int, b: int):\n\treturn a ^ b\n", false);
		const IRFunction &typed = find_function(typed_ir, "test");
		for (const auto &instr : typed.instructions) {
			if (instr.opcode == IROpcode::BIT_XOR) {
				REQUIRE(instr.type_hint == Variant::INT);
			}
		}

		const IRProgram untyped_ir = compile_to_ir("func test(a, b):\n\treturn a ^ b\n", false);
		const IRFunction &untyped = find_function(untyped_ir, "test");
		for (const auto &instr : untyped.instructions) {
			if (instr.opcode == IROpcode::BIT_XOR) {
				REQUIRE(instr.type_hint == IRInstruction::TypeHint_NONE);
			}
		}
	}
}

// Uninitialized globals hold an "empty" payload of INT32_MIN, which is the only
// value VASSIGN treats as "no destination yet". Leaving it 0 makes the first
// assignment into a complex global assign through scoped variant 0 instead.
TEST_CASE("globals start empty") {
	IRProgram ir = compile_to_ir("var a: Callable\nfunc test():\n\treturn a\n");
	RISCVCodeGen riscv{ VariantLayout(false) };
	const std::vector<uint8_t> code = riscv.generate(ir);
	const size_t data_size = riscv.get_global_data_size();
	REQUIRE(data_size >= 24);

	const size_t data_start = code.size() - data_size;
	int64_t payload = 0;
	for (int i = 7; i >= 0; i--) {
		payload = (payload << 8) | code[data_start + 8 + i];
	}
	REQUIRE(payload == int64_t(INT32_MIN));
}

// Loop-invariant code motion hoisted definitions that only run on one path
// through the loop body. A loop containing `if c: x = 1` and `else: x = 0` has
// two invariant definitions of the same register; hoisting either makes the
// register hold that value on both paths. A generated program found it, and it
// produced a wrong answer with nothing else to see.
TEST_CASE("licm leaves conditional definitions alone") {
	// `or` lowers to exactly that shape: a register set to 1 on the
	// short-circuit path and to 0 on the other, both inside the loop body.
	const std::string source = R"(
func test():
	var taken = 0
	var i = 4
	while i > 0:
		i = i - 1
		if true or false:
			taken = taken + 1
	return taken
)";

	REQUIRE(run_int(source, "test") == 4);

	// The same answer with loop-invariant code motion as the only pass, so that
	// a later pass cannot be the one making it right.
	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program parsed = parser.parse();
	CodeGenerator codegen;
	IRProgram unoptimized = codegen.generate(parsed);

	IRProgram hoisted = unoptimized;
	IROptimizer optimizer;
	optimizer.set_enabled_passes({ "licm" });
	optimizer.optimize(hoisted);

	IRInterpreter interpreter(hoisted);
	const IRInterpreter::Value value = interpreter.call("test");
	REQUIRE(std::get<int64_t>(value) == 4);

	// Rotated loops have an entry test before the body label.  LICM may retain
	// an immediate in that entry path instead of moving it across the label;
	// the interpreter result above is the semantic guard for the conditional
	// definition this regression covers.
}

TEST_CASE("typed entry survives unused parameters") {
	const std::string unused =
			"func test():\n"
			"\thelper(1.0)\n"
			"func helper(_d: float):\n"
			"\tprint(1)\n";

	const IRProgram ir = compile_to_ir(unused);
	const IRFunction &caller = find_function(ir, "test");
	bool trusted = false;
	for (const auto &instr : caller.instructions) {
		if (instr.opcode == IROpcode::CALL && instr.trusted_internal_call) {
			trusted = true;
		}
	}
	REQUIRE(trusted);
	REQUIRE(count_opcode(find_function(ir, "helper"), IROpcode::COERCE) == 0);
	REQUIRE(!compile_to_code(unused).empty());

	REQUIRE(!compile_to_code(
					 "func test():\n"
					 "\thelper(1.0, 2.0)\n"
					 "func helper(_a: float, b: float):\n"
					 "\tprint(b)\n")
					 .empty());

	REQUIRE(!compile_to_code(
					 "func test():\n"
					 "\thelper(1.0, 2.0)\n"
					 "func helper(a: float, _b: float):\n"
					 "\tprint(a)\n")
					 .empty());

	REQUIRE(!compile_to_code(
					 "func test():\n"
					 "\thelper(1, 2.0, true)\n"
					 "func helper(_a: int, _b: float, _c: bool):\n"
					 "\tprint(1)\n")
					 .empty());

	std::cout << "  \u2713 Typed entry points survive unused parameters" << std::endl;
}

TEST_CASE("vector int float conversion") {
	const struct {
		const char *value;
		const char *declared;
		IRInstruction::TypeHint type;
	} cases[] = {
		{ "Vector2i(1, 2)", "Vector2", Variant::VECTOR2 },
		{ "Vector2(1.5, 2.5)", "Vector2i", Variant::VECTOR2I },
		{ "Vector3i(1, 2, 3)", "Vector3", Variant::VECTOR3 },
		{ "Vector3(1.5, 2.5, 3.5)", "Vector3i", Variant::VECTOR3I },
		{ "Vector4i(1, 2, 3, 4)", "Vector4", Variant::VECTOR4 },
		{ "Vector4(1.5, 2.5, 3.5, 4.5)", "Vector4i", Variant::VECTOR4I },
		{ "Rect2i(1, 2, 3, 4)", "Rect2", Variant::RECT2 },
		{ "Rect2(1.5, 2.5, 3.5, 4.5)", "Rect2i", Variant::RECT2I },
	};

	for (const auto &item : cases) {
		const std::string source = std::string("func test():\n\tvar v: ") + item.declared +
				" = " + item.value + "\n\treturn v\n";
		const IRProgram ir = compile_to_ir(source, false);
		const IRFunction &test = find_function(ir, "test");
		bool converted = false;
		for (const auto &instr : test.instructions) {
			if (instr.opcode == IROpcode::CONSTRUCT && instr.type_hint == item.type) {
				converted = true;
			}
		}
		REQUIRE(converted);

		const std::string returned = std::string("func test() -> ") + item.declared +
				":\n\treturn " + item.value + "\n";
		REQUIRE(!rejects(returned));

		const std::string reassigned = std::string("func test():\n\tvar v: ") + item.declared +
				" = " + item.declared + "()\n\tv = " + item.value + "\n\treturn v\n";
		REQUIRE(!rejects(reassigned));
	}

	REQUIRE(!rejects(
			"func helper(v: Vector2) -> Vector2i:\n"
			"\treturn Vector2i(v)\n"
			"func test():\n"
			"\tvar from: Vector2 = helper(Vector2(1, 2))\n"
			"\treturn from\n"));

	const struct {
		const char *from;
		const char *to;
	} engine_pairs[] = {
		{ "StringName", "String" },
		{ "NodePath", "String" },
		{ "String", "StringName" },
		{ "NodePath", "StringName" },
		{ "String", "NodePath" },
		{ "StringName", "NodePath" },
		{ "String", "Color" },
		{ "Quaternion", "Basis" },
		{ "Basis", "Quaternion" },
		{ "Transform3D", "Transform2D" },
		{ "Transform2D", "Transform3D" },
		{ "Quaternion", "Transform3D" },
		{ "Basis", "Transform3D" },
		{ "Projection", "Transform3D" },
		{ "Transform3D", "Projection" },
		{ "PackedInt32Array", "Array" },
		{ "PackedStringArray", "Array" },
		{ "Array", "PackedInt32Array" },
		{ "Array", "PackedStringArray" },
	};
	for (const auto &pair : engine_pairs) {
		const std::string source = std::string("func take(value: ") + pair.from + ") -> " +
				pair.to + ":\n\treturn value\n";
		REQUIRE(!rejects(source));
	}

	{
		const IRProgram ir = compile_to_ir(
				"var member: Vector2 = Vector2i(1, 2)\nfunc test():\n\treturn member\n", false);
		bool converted = false;
		for (const auto &instr : ir.member_init.instructions) {
			if (instr.opcode == IROpcode::CONSTRUCT && instr.type_hint == Variant::VECTOR2) {
				converted = true;
			}
		}
		REQUIRE(converted);
		REQUIRE(find_global(ir, "member").value_type == Variant::VECTOR2);
	}
	REQUIRE(rejects("var member: Vector2 = Color(1, 1, 1)\nfunc test():\n\treturn member\n"));
	{
		const IRProgram ir = compile_to_ir(
				"var member: Vector2 = Vector2(0, 0)\n"
				"func test():\n\tmember = Vector2i(1, 2)\n\treturn member\n",
				false);
		REQUIRE(count_opcode(find_function(ir, "test"), IROpcode::CONSTRUCT) == 1);
	}

	REQUIRE(rejects("func test():\n\tvar v: Vector2 = Color(1, 1, 1)\n\treturn v\n"));
	REQUIRE(rejects("func test():\n\tvar v: Vector2 = Vector3(1, 2, 3)\n\treturn v\n"));
	REQUIRE(rejects("func test():\n\tvar v: Rect2 = Vector2i(1, 2)\n\treturn v\n"));

	std::cout << "  \u2713 Vector and rect int/float conversion" << std::endl;
}

static void assert_labels_defined(const IRFunction &func) {
	std::vector<uint32_t> defined;
	for (const auto &instr : func.instructions) {
		if (instr.opcode == IROpcode::LABEL) {
			defined.push_back(instr.operands[0].string_id);
		}
	}
	for (const auto &instr : func.instructions) {
		if (instr.opcode == IROpcode::LABEL) {
			continue;
		}
		for (const auto &operand : instr.operands) {
			if (operand.type == IRValue::Type::LABEL) {
				REQUIRE(std::find(defined.begin(), defined.end(), operand.string_id) != defined.end());
			}
		}
	}
}

TEST_CASE("break after a nested batched loop") {
	const std::string source = R"(
func over_arrays(items: Array) -> int:
	var n: int = 0
	for a in items:
		if a == 1:
			for b in items:
				n += 1
			if n != 0:
				break
	return n

func over_strings(text: String) -> int:
	var n: int = 0
	for a in text:
		for b in text:
			n += 1
		if n > 2:
			break
	return n

func continue_after(items: Array) -> int:
	var n: int = 0
	for a in items:
		for b in items:
			n += 1
		if n > 100:
			continue
		n += 1
	return n
)";

	for (bool optimize : { false, true }) {
		IRProgram ir = compile_to_ir(source, optimize);
		assert_labels_defined(find_function(ir, "over_arrays"));
		assert_labels_defined(find_function(ir, "over_strings"));
		assert_labels_defined(find_function(ir, "continue_after"));
	}
}

TEST_CASE("class typed local that starts null") {
	const std::string source = R"(
func test(value):
	var n: Node3D = null
	n = value
	var five: int = 5
	return n == five

func without_initializer(value):
	var n: Node3D
	n = value
	var five: int = 5
	return n != five

func still_null():
	var n: Node3D = null
	return n == null
)";
	REQUIRE(run_int(source, "test", { int64_t(5) }) == 1);
	REQUIRE(run_int(source, "test", { int64_t(4) }) == 0);
	REQUIRE(run_int(source, "without_initializer", { int64_t(5) }) == 0);
	REQUIRE(run_int(source, "still_null") == 1);

	REQUIRE(!rejects("func test(o: Node3D):\n\tvar n: Node3D = null\n\tn = o\n\treturn n\n"));
	REQUIRE(!rejects("func test():\n\tvar n: Node3D = null\n\tn = self\n\treturn n\n"));
}

// A default is an expression in the callee's scope. It used to be evaluated at
// the call site in the caller's scope. A default naming an earlier parameter
// then failed to compile or silently read a caller's local of the same name.
TEST_CASE("defaults are evaluated in the callee's scope") {
	const std::string source = R"(
func f(a := 1, b := a + 10):
	return a * 100 + b

func shadowed():
	var a = 5
	return f()

func supplied():
	var a = 5
	return f(2) * 1000 + f(3, 4)

class Box:
	static func scaled(a := 3, b := a * 2):
		return a + b
	static func call_it():
		return scaled() * 100 + scaled(1)
)";
	REQUIRE(run_int(source, "shadowed") == 111);
	REQUIRE(run_int(source, "supplied") == 212 * 1000 + 304);
	REQUIRE(run_int(source, "@Box.call_it") == 9 * 100 + 3);

	// Constant defaults still fold at the call site without a wrapper or an extra call.
	const IRProgram ir = compile_to_ir(
			"func g(a := 1, b := 2):\n\treturn a + b\n"
			"func caller():\n\treturn g()\n");
	for (const auto &func : ir.functions) {
		REQUIRE(func.name.rfind("@defaults", 0) != 0);
	}

	// A method's default may read the receiver's fields.
	const IRProgram members = compile_to_ir(R"(
class Box:
	var base := 7
	func get_v(extra := base):
		return extra
func test():
	var base = 1
	return Box.new().get_v()
)");
	const IRFunction &wrapper = find_function(members, "@defaults1.Box.get_v");
	REQUIRE((wrapper.parameters.size() == 1 && wrapper.parameters[0] == "self"));
	bool calls_wrapper = false;
	for (const auto &instr : find_function(members, "test").instructions) {
		if (instr.opcode == IROpcode::CALL &&
			members.strings[instr.operands[0].string_id] == "@defaults1.Box.get_v") {
			calls_wrapper = true;
		}
	}
	REQUIRE(calls_wrapper);
	std::cout << "  \u2713 Defaults are evaluated in the callee's scope" << std::endl;
}
