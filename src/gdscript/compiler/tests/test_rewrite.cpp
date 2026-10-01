// The GDScript rewrite of loops over packed arrays (rewrite.h): which loops are
// rewritten, into what text, and that the rest keep the authored text. Whether
// the rewritten text computes what the authored one does is checked in Godot,
// where both run as GDScript (the differential runs listed in UPSTREAM.md).
#include "../compiler.h"
#include "../ir.h"
#include "../lexer.h"
#include "../parser.h"
#include "../rewrite.h"
#include "witness/doctest.h"
#include <string>

using namespace gdscript;

namespace {

RewriteResult rewrite(const std::string &source, bool fast_arrays = true) {
	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	const Program program = parser.parse();
	RewriteOptions options;
	options.fast_arrays = fast_arrays;
	return rewrite_packed_loops(source, program, options);
}

bool contains(const std::string &text, const std::string &part) {
	return text.find(part) != std::string::npos;
}

} // namespace

TEST_CASE("with fast arrays a typed walk is left to the region") {
	const std::string source =
			"func f(v: PackedFloat32Array) -> float:\n"
			"\tvar t := 0.0\n"
			"\tfor x in v:\n"
			"\t\tt += x\n"
			"\treturn t\n";
	const RewriteResult result = rewrite(source);
	CHECK(result.applied == 0);
	CHECK(result.source == source);
}

TEST_CASE("without fast arrays a read-only walk reads an Array copy") {
	const RewriteResult result = rewrite(
			"func f(v: PackedInt32Array) -> int:\n"
			"\tvar t := 0\n"
			"\tfor x in v:\n"
			"\t\tt += x\n"
			"\treturn t\n",
			false);
	REQUIRE(result.applied == 1);
	CHECK(contains(result.source, "for _sgd_e0 in Array(v):\n\t\tvar x: int = _sgd_e0\n"));
	// One that writes the array keeps reading the array itself.
	const RewriteResult written = rewrite(
			"func f(v: PackedInt32Array) -> int:\n"
			"\tvar i := 0\n"
			"\tfor x in v:\n"
			"\t\tv[i] = x * 2\n"
			"\t\ti += 1\n"
			"\treturn i\n",
			false);
	REQUIRE(written.applied == 1);
	CHECK(contains(written.source, "\tfor _sgd_i0 in v.size():\n\t\tvar x: int = v[_sgd_i0]\n\t\tv[i] = x * 2\n"));
	CHECK_FALSE(contains(written.source, "for x in v"));
}

TEST_CASE("an untyped walk is guarded, with the authored loop as the fallback") {
	const RewriteResult result = rewrite(
			"func f(values) -> float:\n"
			"\tvar t := 0.0\n"
			"\tfor x in values:\n"
			"\t\tt += float(x)\n"
			"\treturn t\n");
	REQUIRE(result.applied == 1);
	CHECK(contains(result.source,
				   "\tif typeof(values) >= TYPE_PACKED_BYTE_ARRAY and typeof(values) <= TYPE_PACKED_VECTOR4_ARRAY:\n"
				   "\t\tfor _sgd_i0 in values.size():\n"
				   "\t\t\tvar x = values[_sgd_i0]\n"
				   "\t\t\tt += float(x)\n"
				   "\telse:\n"
				   "\t\tfor x in values:\n"
				   "\t\t\tt += float(x)\n"));
}

TEST_CASE("without fast arrays a while over size() reads it once") {
	const std::string source =
			"func f(v: PackedInt64Array) -> int:\n"
			"\tvar i := 0\n"
			"\twhile i < v.size() and v[i] < 10:\n"
			"\t\ti += 1\n"
			"\treturn i\n";
	const RewriteResult result = rewrite(source, false);
	REQUIRE(result.applied == 1);
	CHECK(contains(result.source, "\tvar _sgd_n0 := v.size()\n\twhile i < _sgd_n0 and v[i] < 10:\n"));
	// With them the region reads the size from its copy.
	CHECK(rewrite(source).applied == 0);
}

TEST_CASE("loops that could change the array's size keep their text") {
	const char *const unchanged[] = {
		// The body resizes it.
		"func f(v: PackedInt32Array) -> int:\n"
		"\tvar i := 0\n"
		"\twhile i < v.size():\n"
		"\t\tif v[i] > 1:\n"
		"\t\t\tv.append(v[i] - 1)\n"
		"\t\ti += 1\n"
		"\treturn i\n",
		// A member read may run a getter.
		"var bias := 1.0\n"
		"func f(v: PackedFloat64Array) -> float:\n"
		"\tvar t := 0.0\n"
		"\tfor x in v:\n"
		"\t\tt += x * bias\n"
		"\treturn t\n",
		// A call into the guest may reach the array.
		"func g(x: float) -> float:\n"
		"\treturn x\n"
		"func f(v: PackedFloat32Array) -> float:\n"
		"\tvar t := 0.0\n"
		"\tfor x in v:\n"
		"\t\tt += g(x)\n"
		"\treturn t\n",
		// The array escapes as a value.
		"func f(v: PackedFloat32Array, keep: Array) -> void:\n"
		"\tfor x in v:\n"
		"\t\tkeep.append(v)\n",
		// A type GDScript fixes but this pass does not know is left alone (an
		// untyped walk over it would not even parse).
		"func f(v: PackedFloat32Array) -> int:\n"
		"\tvar n := v.size() / 2\n"
		"\tvar t := 0\n"
		"\tfor i in n:\n"
		"\t\tt += i\n"
		"\treturn t\n",
		// An untyped walk whose body calls into the guest.
		"func g(x) -> float:\n"
		"\treturn x\n"
		"func f(values) -> float:\n"
		"\tvar t := 0.0\n"
		"\tfor x in values:\n"
		"\t\tt += g(x)\n"
		"\treturn t\n",
	};
	for (const char *source : unchanged) {
		for (const bool fast_arrays : { false, true }) {
			CAPTURE(source);
			CAPTURE(fast_arrays);
			const RewriteResult result = rewrite(source, fast_arrays);
			CHECK(result.applied == 0);
			CHECK(result.source == source);
		}
	}
}

TEST_CASE("the rewritten text keeps the authored line numbers") {
	const std::string source =
			"func f(v: PackedFloat32Array) -> float:\n" // 1
			"\tvar t := 0.0\n" // 2
			"\tfor x in v:\n" // 3
			"\t\tt += x\n" // 4
			"\treturn t\n" // 5
			"\n" // 6
			"func g() -> int:\n" // 7
			"\treturn 1 + undefined_name\n"; // 8
	const RewriteResult result = rewrite(source, false);
	REQUIRE(result.applied == 1);
	REQUIRE(result.line_map.size() == 10); // [0] and nine rewritten lines
	CHECK(result.line_map[3] == 3);
	CHECK(result.line_map[4] == 3); // the inserted element declaration
	CHECK(result.line_map[5] == 4);
	CHECK(result.line_map[9] == 8);

	// An error past the rewritten loop is reported at its authored line.
	Compiler compiler;
	CompilerOptions options;
	options.rewrite = true;
	options.fast_arrays = false;
	CHECK_FALSE(compiler.compile_to_ir(source, options).has_value());
	CHECK(compiler.get_error_info().line == 8);
}

TEST_CASE("the compiler compiles the rewritten text and says so") {
	const std::string source =
			"func f(v: PackedFloat32Array) -> float:\n"
			"\tvar t := 0.0\n"
			"\tfor x in v:\n"
			"\t\tt += x\n"
			"\treturn t\n";
	Compiler compiler;
	CompilerOptions options;
	options.rewrite = true;
	options.fast_arrays = false;
	REQUIRE(compiler.compile_to_ir(source, options).has_value());
	CHECK(contains(compiler.get_rewritten_source(), "for _sgd_e0 in Array(v):"));
	REQUIRE(compiler.get_rewrite_notes().size() == 1);
	CHECK(contains(compiler.get_rewrite_notes()[0], "line 3: walk"));

	options.rewrite = false;
	REQUIRE(compiler.compile_to_ir(source, options).has_value());
	CHECK(compiler.get_rewritten_source() == source);
	CHECK(compiler.get_rewrite_notes().empty());
}
