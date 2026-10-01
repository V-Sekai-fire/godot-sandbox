// Dispatch: what a `match` on an integer opcode lowers to.
//
// A logic CPU in GDScript is a fetch-decode-execute loop whose execute step is
// one `match` on the opcode, so the cost of that `match` is the machine's cost.
// Two former costs are pinned down here:
//
//   - `const OP_ADD = 3` reached the arm as a LOAD_GLOBAL, which carries no
//     type, so each arm compared two untyped Variants through
//     Variant::evaluate(): one host syscall per arm.
//   - the arms were a chain of equality tests, so a sixteen-opcode machine paid
//     eight compares per instruction on average.
//
// A const now folds to a typed immediate, and a dense integer match lowers to
// SWITCH: a jump table entered in constant time.
//
// The table is a second lowering, not a replacement, so the key cases here are
// what it must not decide alone: a subject that is not an integer, or is out of
// range, must reach the same arm the compare chain would.
#include "../codegen.h"
#include "../compiler_exception.h"
#include "../ir_interpreter.h"
#include "../ir_optimizer.h"
#include "../ir_verifier.h"
#include "../lexer.h"
#include "../parser.h"
#include "../riscv_codegen.h"
#include "witness/doctest.h"
#include <cstring>
#include <iostream>
#include <set>
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

static int count_opcode(const IRFunction &func, IROpcode opcode) {
	int count = 0;
	for (const auto &instr : func.instructions) {
		if (instr.opcode == opcode) {
			count++;
		}
	}
	return count;
}

static const IRInstruction &only_switch(const IRFunction &func) {
	const IRInstruction *found = nullptr;
	for (const auto &instr : func.instructions) {
		if (instr.opcode == IROpcode::SWITCH) {
			REQUIRE((found == nullptr && "more than one SWITCH in the function"));
			found = &instr;
		}
	}
	REQUIRE((found != nullptr && "expected a SWITCH"));
	return *found;
}

// The label a SWITCH sends `value` to, or INVALID_ID if the value is out of range.
static uint32_t switch_target(const IRInstruction &sw, int64_t value) {
	const int64_t base = sw.operands.at(1).immediate();
	const int64_t count = sw.operands.at(2).immediate();
	if (value < base || value >= base + count) {
		return IRStringTable::INVALID_ID;
	}
	return sw.operands.at(3 + (value - base)).string_id;
}

static int64_t call_int(const IRProgram &ir, const std::string &function,
						const std::vector<IRInterpreter::Value> &args = {}) {
	IRInterpreter interp(ir);
	return std::get<int64_t>(interp.call(function, args));
}

// Every optimizer pass must leave the IR verifiable. SWITCH table entries are
// branch targets like any other: a pass that deleted one of those labels would
// leave a jump into nothing.
static void verify_through_the_pipeline(const std::string &source) {
	IRProgram ir = compile_to_ir(source, /*optimize=*/false);
	for (const auto &func : ir.functions) {
		ir_verify(func, "codegen");
	}
	IROptimizer optimizer;
	optimizer.optimize(ir);
	for (const auto &func : ir.functions) {
		ir_verify(func, "the optimizer");
	}
}

// -= Constants =-

TEST_CASE("const global folds to an immediate") {
	const std::string source = R"(
const LIMIT = 42
const NAME = "cpu"
const RATIO = 0.5
const ENABLED = true

func test():
	return LIMIT
)";

	const IRProgram ir = compile_to_ir(source);
	const IRFunction &func = find_function(ir, "test");

	// The type matters more than the saved load: LOAD_GLOBAL carries no type,
	// and every native path downstream needs to know this is an integer.
	REQUIRE(count_opcode(func, IROpcode::LOAD_GLOBAL) == 0);
	REQUIRE(count_opcode(func, IROpcode::LOAD_IMM) == 1);
	for (const auto &instr : func.instructions) {
		if (instr.opcode == IROpcode::LOAD_IMM) {
			REQUIRE(instr.operands.at(1).immediate() == 42);
			REQUIRE(instr.type_hint == Variant::INT);
		}
	}

	REQUIRE(call_int(ir, "test") == 42);
}

TEST_CASE("const container stays a global") {
	// A container is a handle: every read must yield the same container, so
	// materialising a fresh one per read would be a different program.
	const std::string source = R"(
const TABLE = []

func test():
	return TABLE
)";

	const IRProgram ir = compile_to_ir(source);
	const IRFunction &func = find_function(ir, "test");
	REQUIRE(count_opcode(func, IROpcode::LOAD_GLOBAL) == 1);
}

TEST_CASE("a local shadows a const") {
	const std::string source = R"(
const VALUE = 1

func test():
	var VALUE = 9
	return VALUE
)";

	REQUIRE(call_int(compile_to_ir(source), "test") == 9);
	REQUIRE(call_int(compile_to_ir(source, /*optimize=*/true), "test") == 9);
}

// -= When a jump table is built, and when it is not =-

// Sixteen opcodes addressed by consts: the target case.
static const char *OPCODE_MACHINE = R"(
const OP_HALT  = 0
const OP_LOADI = 1
const OP_MOV   = 2
const OP_ADD   = 3
const OP_SUB   = 4
const OP_MUL   = 5
const OP_AND   = 6
const OP_OR    = 7
const OP_XOR   = 8
const OP_SHL   = 9
const OP_SHR   = 10
const OP_LT    = 11
const OP_JMP   = 12
const OP_JZ    = 13
const OP_JNZ   = 14
const OP_OUT   = 15

func step(op : int, a : int, b : int) -> int:
	match op:
		OP_HALT:
			return 0
		OP_LOADI:
			return b
		OP_MOV:
			return a
		OP_ADD:
			return a + b
		OP_SUB:
			return a - b
		OP_MUL:
			return a * b
		OP_AND:
			return a & b
		OP_OR:
			return a | b
		OP_XOR:
			return a ^ b
		OP_SHL:
			return a << b
		OP_SHR:
			return a >> b
		OP_LT:
			return a < b
		OP_JMP:
			return b
		OP_JZ:
			return a
		OP_JNZ:
			return a
		OP_OUT:
			return a + 1000
		_:
			return -1

func test():
	var total = 0
	var op = 0
	while op < 18:
		total = total * 3 + step(op, 12, 3)
		op += 1
	return total
)";

TEST_CASE("dense match becomes a jump table") {
	const IRProgram ir = compile_to_ir(OPCODE_MACHINE);
	const IRFunction &step = find_function(ir, "step");
	const IRInstruction &sw = only_switch(step);

	REQUIRE(sw.operands.at(1).immediate() == 0); // base
	REQUIRE(sw.operands.at(2).immediate() == 16); // one entry per opcode
	REQUIRE(sw.operands.size() == 3 + 16);

	// The subject is declared `int`, so the table decides the whole match and no
	// compare chain follows it.
	REQUIRE(sw.type_hint == Variant::INT);
	REQUIRE(count_opcode(step, IROpcode::CMP_EQ) == 0);

	// Sixteen distinct arms, all reachable, none the wildcard.
	std::set<uint32_t> targets;
	for (int64_t op = 0; op < 16; op++) {
		targets.insert(switch_target(sw, op));
	}
	REQUIRE(targets.size() == 16);
}

TEST_CASE("the machine still computes the same answers") {
	const int64_t plain = call_int(compile_to_ir(OPCODE_MACHINE), "test");
	const int64_t optimized = call_int(compile_to_ir(OPCODE_MACHINE, /*optimize=*/true), "test");
	REQUIRE(plain == optimized);

	// Spot-check individual arms: an off-by-one in the table still sums to
	// something plausible.
	const IRProgram ir = compile_to_ir(OPCODE_MACHINE, /*optimize=*/true);
	REQUIRE(call_int(ir, "step", { int64_t(0), int64_t(12), int64_t(3) }) == 0); // HALT
	REQUIRE(call_int(ir, "step", { int64_t(3), int64_t(12), int64_t(3) }) == 15); // ADD
	REQUIRE(call_int(ir, "step", { int64_t(4), int64_t(12), int64_t(3) }) == 9); // SUB
	REQUIRE(call_int(ir, "step", { int64_t(5), int64_t(12), int64_t(3) }) == 36); // MUL
	REQUIRE(call_int(ir, "step", { int64_t(9), int64_t(12), int64_t(3) }) == 96); // SHL
	REQUIRE(call_int(ir, "step", { int64_t(10), int64_t(12), int64_t(3) }) == 1); // SHR
	REQUIRE(call_int(ir, "step", { int64_t(15), int64_t(12), int64_t(3) }) == 1012); // OUT
	REQUIRE(call_int(ir, "step", { int64_t(16), int64_t(12), int64_t(3) }) == -1); // past the table
	REQUIRE(call_int(ir, "step", { int64_t(-1), int64_t(12), int64_t(3) }) == -1); // below the table

	verify_through_the_pipeline(OPCODE_MACHINE);
}

TEST_CASE("a short match keeps the compares") {
	// Three arms: the table setup costs about what the compares do, and the
	// compares cost no code size.
	const std::string source = R"(
func pick(n : int) -> int:
	match n:
		0:
			return 10
		1:
			return 20
		2:
			return 30
		_:
			return 40

func test():
	return pick(0) + pick(1) + pick(2) + pick(3)
)";

	const IRProgram ir = compile_to_ir(source);
	const IRFunction &pick = find_function(ir, "pick");
	REQUIRE(count_opcode(pick, IROpcode::SWITCH) == 0);
	REQUIRE(count_opcode(pick, IROpcode::CMP_EQ) == 3);
	REQUIRE(call_int(ir, "test") == 100);
}

TEST_CASE("a sparse match keeps the compares") {
	// Five values spread over ten thousand: the table would be nearly all holes.
	const std::string source = R"(
func pick(n : int) -> int:
	match n:
		0:
			return 1
		10:
			return 2
		100:
			return 3
		1000:
			return 4
		10000:
			return 5
		_:
			return 0

func test():
	return pick(0) + pick(10) + pick(100) + pick(1000) + pick(10000) + pick(7)
)";

	const IRProgram ir = compile_to_ir(source);
	REQUIRE(count_opcode(find_function(ir, "pick"), IROpcode::SWITCH) == 0);
	REQUIRE(call_int(ir, "test") == 15);
}

TEST_CASE("a non constant pattern forbids the table") {
	// `limit` may hold 2, in which case GDScript takes the first arm. A table
	// jumping straight to the `2:` body would run the wrong one, so one
	// unevaluable pattern disqualifies the whole match.
	const std::string source = R"(
func pick(n : int, limit : int) -> int:
	match n:
		limit:
			return 1
		0:
			return 2
		1:
			return 3
		2:
			return 4
		3:
			return 5
		_:
			return 6

func test():
	return pick(2, 2) * 10 + pick(2, 9)
)";

	const IRProgram ir = compile_to_ir(source);
	REQUIRE(count_opcode(find_function(ir, "pick"), IROpcode::SWITCH) == 0);
	REQUIRE(call_int(ir, "test") == 14); // first arm for n == limit, then the 2: arm
}

// -= What the table must not decide by itself =-

TEST_CASE("an untyped subject keeps the chain behind the table") {
	// `match 3.0` must reach the `3:` arm, and `match true` the `1:` arm. The
	// table handles only integers, so unless the subject is known to be one the
	// compare chain stays behind it and catches every fall-through.
	const std::string source = R"(
func pick(n):
	match n:
		0:
			return 10
		1:
			return 11
		2:
			return 12
		3:
			return 13
		4:
			return 14
		_:
			return -1

func test():
	return pick(3) * 1000000 + pick(3.0) * 10000 + pick(true) * 100 + pick(2.5)
)";

	const IRProgram ir = compile_to_ir(source);
	const IRFunction &pick = find_function(ir, "pick");

	const IRInstruction &sw = only_switch(pick);
	REQUIRE(sw.type_hint == IRInstruction::TypeHint_NONE); // so the backend tests the type
	REQUIRE(count_opcode(pick, IROpcode::CMP_EQ) == 5); // and the chain is still there

	// 13 via the table, 13 via the chain for the float, 11 for the bool, and the
	// wildcard for a float equal to nothing.
	REQUIRE(call_int(ir, "test") == 13131100 - 1);

	verify_through_the_pipeline(source);
}

TEST_CASE("holes and duplicates") {
	const std::string source = R"(
const LOWEST = -4

func classify(v : int) -> int:
	var out = 0
	match v:
		LOWEST:
			out = 1
		-2, -1:
			out = 2
		1:
			out = 3
		3:
			out = 4
		LOWEST:
			out = 999
	return out

func test():
	return 0
)";

	const IRProgram ir = compile_to_ir(source);
	const IRInstruction &sw = only_switch(find_function(ir, "classify"));

	REQUIRE(sw.operands.at(1).immediate() == -4);
	REQUIRE(sw.operands.at(2).immediate() == 8); // -4 .. 3

	// -2 and -1 share an arm; -3, 0 and 2 are holes and go where falling out of
	// the match goes; the second LOWEST is unreachable, as in the compare chain.
	REQUIRE(switch_target(sw, -2) == switch_target(sw, -1));
	REQUIRE(switch_target(sw, -3) == switch_target(sw, 0));
	REQUIRE(switch_target(sw, 0) == switch_target(sw, 2));
	REQUIRE(switch_target(sw, -4) != switch_target(sw, -3));
	REQUIRE(switch_target(sw, -4) != switch_target(sw, 3));
	REQUIRE(switch_target(sw, 4) == IRStringTable::INVALID_ID); // past the end of the table

	IRProgram optimized = compile_to_ir(source, /*optimize=*/true);
	for (int64_t v = -6; v < 6; v++) {
		const int64_t expected =
				(v == -4) ? 1 : (v == -2 || v == -1) ? 2
				: (v == 1)							 ? 3
				: (v == 3)							 ? 4
													 : 0;
		// A match with no wildcard and no arm for `v` falls out and runs what
		// follows, leaving `out` unchanged.
		REQUIRE(call_int(optimized, "classify", { v }) == expected);
	}
}

TEST_CASE("a wildcard is not a table entry") {
	const std::string source = R"(
func pick(n : int) -> int:
	match n:
		0:
			return 1
		1:
			return 2
		3:
			return 3
		4:
			return 4
		_:
			return 99

func test():
	return 0
)";

	const IRProgram ir = compile_to_ir(source);
	const IRInstruction &sw = only_switch(find_function(ir, "pick"));
	REQUIRE(sw.operands.at(2).immediate() == 5); // 0 .. 4

	// The hole at 2 targets the wildcard's body, not an arm of its own.
	const uint32_t hole = switch_target(sw, 2);
	REQUIRE(hole != IRStringTable::INVALID_ID);
	REQUIRE(hole != switch_target(sw, 0));
	REQUIRE(hole != switch_target(sw, 4));

	const IRProgram optimized = compile_to_ir(source, /*optimize=*/true);
	REQUIRE(call_int(optimized, "pick", { int64_t(2) }) == 99);
	REQUIRE(call_int(optimized, "pick", { int64_t(9) }) == 99);
	REQUIRE(call_int(optimized, "pick", { int64_t(-1) }) == 99);
}

TEST_CASE("break and return inside an arm") {
	// The arms are emitted after the table rather than between the compares, so
	// `break` and `continue` in an arm must still refer to the enclosing loop.
	const std::string source = R"(
func test():
	var total = 0
	var i = 0
	while i < 20:
		i += 1
		match i:
			1:
				total += 1
			2:
				continue
			3:
				total += 100
			4:
				total += 1000
			5:
				break
			_:
				total += 10000
	return total * 10 + i
)";

	const IRProgram ir = compile_to_ir(source, /*optimize=*/true);
	REQUIRE(count_opcode(find_function(ir, "test"), IROpcode::SWITCH) == 1);
	REQUIRE(call_int(ir, "test") == 11015);

	verify_through_the_pipeline(source);
}

// -= The backend =-

TEST_CASE("the table reaches riscv") {
	IRProgram ir = compile_to_ir(OPCODE_MACHINE, /*optimize=*/true);
	RISCVCodeGen codegen;
	const std::vector<uint8_t> code = codegen.generate(ir);
	REQUIRE(!code.empty());

	// The table is `count` consecutive jumps in the instruction stream, so the
	// dispatch must end in an indirect jump followed by sixteen JALs. Checking
	// the encoding here catches a wrong stride, which every higher-level test
	// would still pass.
	size_t jalr_at = 0;
	bool found = false;
	for (size_t offset = 0; offset + 4 <= code.size(); offset += 4) {
		uint32_t instr = 0;
		std::memcpy(&instr, &code[offset], 4);
		if ((instr & 0x7F) != 0x67) {
			continue; // not a JALR
		}
		if (((instr >> 7) & 0x1F) != 0) {
			continue; // not `jr`: a JALR that keeps a return address is a call
		}
		size_t jumps = 0;
		for (size_t entry = offset + 4; entry + 4 <= code.size(); entry += 4) {
			uint32_t word = 0;
			std::memcpy(&word, &code[entry], 4);
			if ((word & 0x7F) != 0x6F) {
				break;
			}
			jumps++;
		}
		if (jumps >= 16) {
			jalr_at = offset;
			found = true;
			break;
		}
	}
	REQUIRE((found && "no jump table in the generated code"));

	// Immediately before the indirect jump: sh2add, scaling the index by the 4
	// bytes per table entry. A wrong scale lands in the middle of the table.
	uint32_t sh2add = 0;
	std::memcpy(&sh2add, &code[jalr_at - 4], 4);
	REQUIRE((sh2add & 0x7F) == 0x33); // OP
	REQUIRE(((sh2add >> 12) & 0x7) == 4); // funct3
	REQUIRE(((sh2add >> 25) & 0x7F) == 0x10); // funct7: Zba
}

TEST_CASE("a huge range is refused") {
	// Six arms hundreds of thousands apart: a table covering them would be most
	// of a megabyte of jumps for six destinations.
	const std::string source = R"(
func pick(n : int) -> int:
	match n:
		0:
			return 1
		100000:
			return 2
		200000:
			return 3
		300000:
			return 4
		400000:
			return 5
		500000:
			return 6
		_:
			return 0

func test():
	return pick(0) + pick(300000) + pick(7)
)";

	const IRProgram ir = compile_to_ir(source);
	REQUIRE(count_opcode(find_function(ir, "pick"), IROpcode::SWITCH) == 0);
	REQUIRE(call_int(ir, "test") == 5);
}

// -= switch =-
//
// Mandatory jump table: compiles to SWITCH or fails. Everything switch
// rejects, match still accepts.

// Returns CompilerException message, or "" on success.
static std::string compile_error(const std::string &source) {
	try {
		compile_to_ir(source);
	} catch (const CompilerException &e) {
		return e.what();
	}
	return "";
}

static bool mentions(const std::string &haystack, const std::string &needle) {
	return haystack.find(needle) != std::string::npos;
}

// 16-arm int dispatch, parameterised by keyword.
static std::string dispatch_source(const std::string &keyword) {
	std::string source = "func pick(op : int) -> int:\n\t" + keyword + " op:\n";
	for (int i = 0; i < 16; i++) {
		source += "\t\t" + std::to_string(i) + ":\n\t\t\treturn " +
				std::to_string(100 + i) + "\n";
	}
	source += "\t\t_:\n\t\t\treturn -1\n";
	source += "\nfunc test():\n\treturn pick(0) + pick(15) + pick(99)\n";
	return source;
}

TEST_CASE("switch is dispatch and nothing else") {
	const std::string source = dispatch_source("switch");
	const IRProgram ir = compile_to_ir(source);
	const IRFunction &pick = find_function(ir, "pick");

	const IRInstruction &sw = only_switch(pick);
	REQUIRE(sw.type_hint == Variant::INT); // no type test emitted
	// No CMP_EQ: every arm is a table entry, none becomes a VEVAL.
	REQUIRE(count_opcode(pick, IROpcode::CMP_EQ) == 0);

	REQUIRE(switch_target(sw, 0) != switch_target(sw, 15));
	REQUIRE(switch_target(sw, 16) == IRStringTable::INVALID_ID); // out-of-range -> wildcard

	REQUIRE(call_int(ir, "test") == 100 + 115 - 1);
	verify_through_the_pipeline(source);
}

TEST_CASE("switch and a typed match are the same machine code") {
	// Typed match already emits SWITCH; switch adds only the compile-time
	// guarantee. Byte-identical ELF confirms zero overhead.
	auto machine_code = [](const std::string &keyword) {
		IRProgram ir = compile_to_ir(dispatch_source(keyword), /*optimize=*/true);
		RISCVCodeGen codegen;
		return codegen.generate(ir);
	};

	const std::vector<uint8_t> from_switch = machine_code("switch");
	const std::vector<uint8_t> from_match = machine_code("match");
	REQUIRE(!from_switch.empty());
	REQUIRE(from_switch == from_match);
}

TEST_CASE("switch takes the table below the match floor") {
	// MIN_SWITCH_CASES blocks match below the threshold; switch bypasses the floor.
	const std::string arms = R"( op:
		0:
			return 10
		1:
			return 20
		_:
			return -1

func test():
	return pick(0) * 100 + pick(1) * 10 + pick(9)
)";

	const IRProgram from_switch = compile_to_ir("func pick(op : int) -> int:\n\tswitch" + arms);
	const IRProgram from_match = compile_to_ir("func pick(op : int) -> int:\n\tmatch" + arms);

	REQUIRE(count_opcode(find_function(from_switch, "pick"), IROpcode::SWITCH) == 1);
	REQUIRE(count_opcode(find_function(from_match, "pick"), IROpcode::SWITCH) == 0);

	// Same result; only dispatch shape differs.
	REQUIRE(call_int(from_switch, "test") == 1200 - 1);
	REQUIRE(call_int(from_match, "test") == 1200 - 1);
}

TEST_CASE("switch refuses what a table cannot do") {
	// Each case: a switch the table rejects, with expected diagnostic substring.
	// Same source under match must still compile.
	const struct {
		const char *what;
		const char *body;
		const char *expected;
	} cases[] = {
		{ "an untyped subject",
		  "func pick(op):\n\tKEYWORD op:\n\t\t0:\n\t\t\treturn 1\n\t\t1:\n\t\t\treturn 2\n",
		  "has to be a known integer" },
		{ "a float subject",
		  "func pick(op : float):\n\tKEYWORD op:\n\t\t0:\n\t\t\treturn 1\n\t\t1:\n\t\t\treturn 2\n",
		  "but this is a FLOAT" },
		{ "a guard",
		  "func pick(op : int):\n\tKEYWORD op:\n\t\t0 when op > 1:\n\t\t\treturn 1\n\t\t1:\n\t\t\treturn 2\n",
		  "cannot have a 'when' guard" },
		{ "a binding",
		  "func pick(op : int):\n\tKEYWORD op:\n\t\t0:\n\t\t\treturn 1\n\t\tvar v:\n\t\t\treturn v\n",
		  "this is a binding" },
		{ "an array pattern",
		  "func pick(op : int):\n\tKEYWORD op:\n\t\t0:\n\t\t\treturn 1\n\t\t[1, 2]:\n\t\t\treturn 2\n",
		  "this is an array pattern" },
		{ "a dictionary pattern",
		  "func pick(op : int):\n\tKEYWORD op:\n\t\t0:\n\t\t\treturn 1\n\t\t{\"k\": 1}:\n\t\t\treturn 2\n",
		  "this is a dictionary pattern" },
		{ "a wildcard sharing an arm",
		  "func pick(op : int):\n\tKEYWORD op:\n\t\t0, _:\n\t\t\treturn 1\n",
		  "this is a wildcard" },
		{ "a non-integer constant",
		  "func pick(op : int):\n\tKEYWORD op:\n\t\t0:\n\t\t\treturn 1\n\t\t\"x\":\n\t\t\treturn 2\n",
		  "has to be an integer constant" },
		{ "a pattern that does not fold",
		  "func pick(op : int):\n\tKEYWORD op:\n\t\t0:\n\t\t\treturn 1\n\t\top:\n\t\t\treturn 2\n",
		  "the compiler can fold" },
		{ "no integer pattern at all",
		  "func pick(op : int):\n\tKEYWORD op:\n\t\t_:\n\t\t\treturn 1\n",
		  "needs at least one integer pattern" },
		{ "a duplicate",
		  "func pick(op : int):\n\tKEYWORD op:\n\t\t0:\n\t\t\treturn 1\n\t\t1, 0:\n\t\t\treturn 2\n",
		  "Duplicate 'switch' pattern 0" },
		{ "a spread too wide to index",
		  "func pick(op : int):\n\tKEYWORD op:\n\t\t0:\n\t\t\treturn 1\n\t\t100000:\n\t\t\treturn 2\n",
		  "too sparse for a jump table" },
	};

	for (const auto &entry : cases) {
		std::string source = entry.body;
		const std::string with_switch =
				source.replace(source.find("KEYWORD"), 7, "switch");
		const std::string error = compile_error(with_switch);
		REQUIRE((!error.empty() && "a switch that cannot be a table has to be refused"));
		if (!mentions(error, entry.expected)) {
			std::cerr << "  " << entry.what << ": expected \"" << entry.expected
					  << "\" in:\n    " << error << std::endl;
			FAIL("wrong diagnostic");
		}

		// Same source under match must compile.
		source = entry.body;
		const std::string with_match =
				source.replace(source.find("KEYWORD"), 7, "match");
		REQUIRE((compile_error(with_match).empty() &&
				 "switch must not make anything illegal that match accepts"));
	}
}

TEST_CASE("switch dispatches a real loop") {
	// Inferred int subject from bitwise extraction; table still holds.
	const std::string source = R"(
func step(word : int, acc : int) -> int:
	var op = (word >> 4) & 7
	switch op:
		0:
			return acc + (word & 15)
		1:
			return acc - (word & 15)
		2:
			return acc * (word & 15)
		3:
			return acc << (word & 7)
		4:
			return acc >> (word & 7)
		5:
			return acc ^ (word & 15)
		_:
			return acc

func test():
	var acc = 0
	acc = step(3, acc)
	acc = step(19, acc)
	acc = step(34, acc)
	acc = step(51, acc)
	acc = step(112, acc)
	return acc
)";

	const IRProgram ir = compile_to_ir(source);
	const IRFunction &step = find_function(ir, "step");
	REQUIRE(only_switch(step).type_hint == Variant::INT);
	REQUIRE(count_opcode(step, IROpcode::CMP_EQ) == 0);

	// (((0+3)-3)*2)<<3 == 0; unmapped opcode is identity.
	REQUIRE(call_int(ir, "test") == call_int(compile_to_ir(source, true), "test"));

	verify_through_the_pipeline(source);
}
