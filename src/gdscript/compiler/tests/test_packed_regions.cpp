// Packed array regions (codegen.cpp, "Packed array regions").
//
// The planted test first: `v[i]` on a packed array lowers to ECALL_VARIANT_GET
// and `out[i] = x` to ECALL_VARIANT_SET, so without regions a loop over N
// elements makes N host calls. With fast arrays the loop copies the array into
// guest memory once and its body makes none. The same assertion without fast
// arrays is the control: it still fails (doctest::should_fail), which shows the
// counter sees the per-element path whenever it is there.
//
// The rest pins what makes a region and what does not. What a region computes
// is checked against interpreted GDScript in Godot, where the host side exists
// (the differential runs listed in UPSTREAM.md).
#include "../codegen.h"
#include "../compiler.h"
#include "../ir_optimizer.h"
#include "../ir_verifier.h"
#include "../lexer.h"
#include "../parser.h"
#include "../syscall_numbers.h"
#include "witness/doctest.h"
#include <algorithm>
#include <string>

using namespace gdscript;

namespace {

// Plain GDScript, as the station's Slug kernels are written.
const char *SOURCE = R"(
func sum(v: PackedFloat32Array) -> float:
	var s := 0.0
	var n: int = v.size()
	for i in n:
		s += v[i]
	return s

func fill(n: int) -> PackedFloat32Array:
	var out := PackedFloat32Array()
	out.resize(n)
	for i in n:
		out[i] = float(i)
	return out

func walk(v: PackedInt32Array) -> int:
	var s := 0
	for x in v:
		s += x
	return s

func rows(v: PackedVector2Array, c: PackedColorArray) -> float:
	var s := 0.0
	var i := 0
	while i < v.size():
		s += v[i].x * c[i % c.size()].r
		i += 1
	return s

func scalar(n: int) -> int:
	var s := 0
	for i in n:
		s += (i * i) % 7
	return s
)";

IRProgram compile_to_ir(const std::string &source, bool fast_arrays, bool optimize = false) {
	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();
	CodeGenerator codegen;
	codegen.set_fast_arrays(fast_arrays);
	IRProgram ir = codegen.generate(program);
	ir_verify(ir);
	if (optimize) {
		IROptimizer().optimize(ir);
		ir_verify(ir);
	}
	return ir;
}

const IRFunction &function_named(const IRProgram &ir, const std::string &name) {
	for (const IRFunction &func : ir.functions) {
		if (func.name == name) {
			return func;
		}
	}
	FAIL("no function " << name);
	return ir.functions.front();
}

// Opcodes the RISC-V backend always lowers to an ecall into the host.
bool is_host_call(IROpcode opcode) {
	static const IROpcode host_calls[] = {
		IROpcode::CALL_SYSCALL,
		IROpcode::VCALL,
		IROpcode::VGET,
		IROpcode::VSET,
		IROpcode::VARIANT_SET,
		IROpcode::ARRAY_GET,
		IROpcode::ARRAY_SET,
		IROpcode::DICT_SET,
		IROpcode::DICT_GET_CONST,
		IROpcode::DICT_SET_CONST,
		IROpcode::DICT_SET_CONST_STR,
		IROpcode::DICT_HAS_CONST,
		IROpcode::CONSTRUCT,
	};
	return std::find(std::begin(host_calls), std::end(host_calls), opcode) != std::end(host_calls);
}

bool references_label(const IRInstruction &instr, uint32_t label) {
	for (const IRValue &operand : instr.operands) {
		if (operand.type == IRValue::Type::LABEL && operand.string_id == label) {
			return true;
		}
	}
	return false;
}

// Host calls in one pass through the body of the first loop of `function`:
// from its head label to the last instruction that goes back to it. With fast
// arrays the first loop is the region's copy loop; the original loop, kept as
// the fallback, comes after it.
int host_calls_per_element(const IRProgram &ir, const std::string &function) {
	const IRFunction &func = function_named(ir, function);
	size_t head = func.instructions.size();
	for (size_t i = 0; i < func.instructions.size(); i++) {
		const IRInstruction &instr = func.instructions[i];
		if (instr.opcode != IROpcode::LABEL) {
			continue;
		}
		const std::string &name = ir.strings[instr.operands[0].string_id];
		if (name.rfind("for_loop_", 0) == 0 || name.rfind("loop_", 0) == 0 ||
			name.rfind("packed_walk_", 0) == 0) {
			head = i;
			break;
		}
	}
	REQUIRE_MESSAGE(head < func.instructions.size(), function << " has no loop");
	const uint32_t loop = func.instructions[head].operands[0].string_id;
	size_t back = head;
	for (size_t i = head + 1; i < func.instructions.size(); i++) {
		if (references_label(func.instructions[i], loop)) {
			back = i;
		}
	}
	REQUIRE_MESSAGE(back > head, function << "'s loop has no back edge");
	int calls = 0;
	for (size_t i = head; i <= back; i++) {
		calls += is_host_call(func.instructions[i].opcode) ? 1 : 0;
	}
	return calls;
}

int count_syscalls(const IRFunction &func, int64_t number) {
	int count = 0;
	for (const IRInstruction &instr : func.instructions) {
		if (instr.opcode == IROpcode::CALL_SYSCALL && instr.operands.size() >= 2 &&
			instr.operands[1].type == IRValue::Type::IMMEDIATE &&
			instr.operands[1].immediate() == number) {
			count++;
		}
	}
	return count;
}

int count_opcode(const IRFunction &func, IROpcode opcode) {
	return int(std::count_if(func.instructions.begin(), func.instructions.end(),
							 [&](const IRInstruction &instr) { return instr.opcode == opcode; }));
}

bool has_region(const std::string &source, const std::string &function) {
	const IRProgram ir = compile_to_ir(source, true);
	return count_syscalls(function_named(ir, function), ECALL_PACKED_ACQUIRE) > 0;
}

} // namespace

// The control: the same counter finds nothing in a loop that stays in the guest, so a
// failure below is about the element access, not about how loops are counted.
TEST_CASE("a scalar loop makes no host call per element") {
	CHECK(host_calls_per_element(compile_to_ir(SOURCE, true), "scalar") == 0);
	CHECK(host_calls_per_element(compile_to_ir(SOURCE, false), "scalar") == 0);
}

TEST_CASE("with fast arrays a packed-array loop makes no host call per element") {
	for (const bool optimize : { false, true }) {
		const IRProgram ir = compile_to_ir(SOURCE, true, optimize);
		for (const std::string function : { "sum", "fill", "walk", "rows" }) {
			CAPTURE(function);
			CAPTURE(optimize);
			CHECK(host_calls_per_element(ir, function) == 0);
		}
	}
}

// Planted before regions existed; now the control. Without fast arrays the
// element access is still a host call, and this check still fails.
TEST_CASE("control: without fast arrays a packed-array loop makes a host call per element" *
		  doctest::should_fail()) {
	const IRProgram ir = compile_to_ir(SOURCE, false);
	constexpr int elements = 1000;
	for (const std::string function : { "sum", "fill" }) {
		CAPTURE(function);
		const int per_element = host_calls_per_element(ir, function);
		const int per_loop = per_element * elements;
		CHECK_MESSAGE(per_loop <= 1, function << " makes " << per_element << " host call(s) per element, " << per_loop << " for a " << elements << "-element loop");
	}
}

TEST_CASE("without fast arrays nothing changes") {
	const IRProgram ir = compile_to_ir(SOURCE, false);
	for (const IRFunction &func : ir.functions) {
		CAPTURE(func.name);
		CHECK(count_syscalls(func, ECALL_PACKED_ACQUIRE) == 0);
		CHECK(count_opcode(func, IROpcode::PACKED_GET) == 0);
		CHECK(count_opcode(func, IROpcode::PACKED_SET) == 0);
	}
}

TEST_CASE("the original loop stays as the fallback, and every exit releases") {
	const IRProgram ir = compile_to_ir(R"(
func copy(src: PackedFloat32Array, dst: PackedFloat32Array, stop: int) -> int:
	for i in src.size():
		dst[i] = src[i]
		if i == stop:
			return i
	return -1
)",
									   true);
	const IRFunction &func = function_named(ir, "copy");
	CHECK(count_syscalls(func, ECALL_PACKED_ACQUIRE) == 2);
	CHECK(count_opcode(func, IROpcode::PACKED_GET) == 1);
	CHECK(count_opcode(func, IROpcode::PACKED_SET) == 1);
	// The fallback: the element accesses as they were, plus the out-of-range
	// exits, which repeat the access through the host.
	CHECK(count_syscalls(func, ECALL_VARIANT_GET) == 2);
	CHECK(count_opcode(func, IROpcode::VARIANT_SET) == 2);
	// Releases: the normal exit, the return inside the loop, and the bail chain.
	CHECK(count_syscalls(func, ECALL_PACKED_RELEASE) == 2 + 2 + 2);
}

TEST_CASE("a written array is guarded against sharing storage with another of its type") {
	const IRProgram ir = compile_to_ir(R"(
func shift(a: PackedInt32Array, b: PackedInt32Array, c: PackedFloat32Array) -> int:
	var t := 0
	for i in range(1, a.size()):
		a[i] = b[i - 1] + int(c[i])
		t += a[i]
	return t
)",
									   true);
	const IRFunction &func = function_named(ir, "shift");
	CHECK(count_syscalls(func, ECALL_PACKED_ACQUIRE) == 3);
	// a against b; c is another type and cannot share a's storage.
	CHECK(count_opcode(func, IROpcode::PACKED_IDENTITY) == 2);
}

TEST_CASE("loops the copy could not keep in step with the host are left alone") {
	// A guest call may reach the array through anything it can see.
	CHECK_FALSE(has_region(R"(
func f(v: float) -> float:
	return v * 2.0

func g(a: PackedFloat32Array) -> void:
	for i in a.size():
		a[i] = f(a[i])
)",
						   "g"));
	// The body resizes the array.
	CHECK_FALSE(has_region(R"(
func g(a: PackedInt32Array) -> void:
	for i in a.size():
		if a[i] > 0:
			a.append(a[i] - 1)
)",
						   "g"));
	// Another view of the array through a container.
	CHECK_FALSE(has_region(R"(
func g(a: PackedInt32Array, holder: Array) -> int:
	var t := 0
	for i in a.size():
		a[i] = i
		var view: PackedInt32Array = holder[0]
		t += view[i]
	return t
)",
						   "g"));
	// Another array of the same type, written through a method.
	CHECK_FALSE(has_region(R"(
func g(a: PackedInt32Array, b: PackedInt32Array) -> int:
	var t := 0
	for i in a.size():
		t += a[i]
		b.push_back(t)
	return t
)",
						   "g"));
	// The array escapes as a value.
	CHECK_FALSE(has_region(R"(
func g(a: PackedInt32Array, keep: Array) -> int:
	var t := 0
	for i in a.size():
		t += a[i]
		keep.append(a)
	return t
)",
						   "g"));
	// A coroutine.
	CHECK_FALSE(has_region(R"(
signal tick

func g(a: PackedInt32Array) -> int:
	var t := 0
	await tick
	for i in a.size():
		t += a[i]
	return t
)",
						   "g"));
}

TEST_CASE("value-type methods and arrays of other types do not stop a region") {
	CHECK(has_region(R"(
func g(a: PackedFloat32Array, out: PackedInt32Array) -> float:
	var t := 0.0
	var c := Color(0.5, 0.25, 0.125, 1.0)
	for i in a.size():
		c = c.lerp(Color(a[i], 0.0, 0.0, 1.0), 0.5)
		t += c.r
		out.append(i)
	return t
)",
					 "g"));
	// Building an error message from plain values, then returning.
	CHECK(has_region(R"(
func g(a: PackedInt32Array) -> Dictionary:
	for i in a.size():
		if a[i] < 0:
			return {"error": "element %d is %d" % [i, a[i]]}
	return {"error": ""}
)",
					 "g"));
}

TEST_CASE("a region lowers to RISC-V in both precisions") {
	const std::string source = std::string(SOURCE) + R"(
func vectors(p: PackedVector3Array, q: PackedVector4Array, c: PackedColorArray) -> PackedVector3Array:
	var out := PackedVector3Array()
	out.resize(p.size())
	for i in p.size():
		var v := q[i % q.size()]
		out[i] = p[i] * v.w + Vector3(c[0].r, c[0].g, c[0].b)
	return out

func bytes(b: PackedByteArray, l: PackedInt64Array, d: PackedFloat64Array) -> int:
	var t := 0
	for i in b.size():
		b[i] = b[i] + 1
		l[i % l.size()] = t
		d[i % d.size()] = t
		t += b[i]
	return t
)";
	for (const bool double_precision : { false, true }) {
		CAPTURE(double_precision);
		Compiler compiler;
		CompilerOptions options;
		options.double_precision = double_precision;
		options.fast_arrays = true;
		const std::vector<uint8_t> elf = compiler.compile(source, options);
		CHECK_MESSAGE(!elf.empty(), compiler.get_error());
	}
}
