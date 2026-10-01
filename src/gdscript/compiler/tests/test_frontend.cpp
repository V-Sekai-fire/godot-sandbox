#include "../compiler.h"
#include "../ir_interpreter.h"
#include "../ir_verifier.h"
#include "witness/doctest.h"
#include <algorithm>
#include <stdexcept>

using namespace gdscript;

namespace {

const char *const EXAMPLE =
		"@tool\nclass_name FrontendExample\n"
		"var count: int = 7\n"
		"func answer(value: int) -> int:\n\treturn value * 2 + 2\n";

} // namespace

TEST_CASE("the frontend means the same thing optimized or not") {
	Compiler compiler;
	for (bool optimize : { false, true }) {
		CompilerOptions options;
		options.extensions = true;
		options.optimize = optimize;
		auto ir = compiler.compile_to_ir(EXAMPLE, options);
		REQUIRE_MESSAGE(ir.has_value(), compiler.get_error());
		ir_verify(*ir);
		IRInterpreter interpreter(*ir);
		CHECK_MESSAGE(std::get<int64_t>(interpreter.call("answer", { int64_t(20) })) == 42,
					  "optimized and unoptimized frontend semantics differ");
		CHECK_MESSAGE(compiler.is_tool(), "lost script metadata");
		CHECK(compiler.get_class_name() == "FrontendExample");
		CHECK_MESSAGE(compiler.get_property_signatures().size() == 1, "lost property metadata");
		CHECK(ir->properties.size() == 1);
		CHECK_MESSAGE(!compiler.get_function_signatures().empty(), "lost signatures");
		CHECK_MESSAGE(compiler.get_line_table().entries.empty(),
					  "the frontend emitted machine addresses");
	}
}

TEST_CASE("a failed parse reports diagnostics and clears the metadata") {
	Compiler compiler;
	auto invalid = compiler.compile_to_ir("func broken(:\n\tpass\n");
	CHECK_MESSAGE(!invalid, "a broken source compiled");
	CHECK(compiler.get_error_info().has_error);
	CHECK(!compiler.get_error().empty());
	CHECK_MESSAGE(!compiler.is_tool(), "stale script metadata after a failure");
	CHECK(compiler.get_class_name().empty());
}

TEST_CASE("an empty source succeeds and says nothing went wrong") {
	Compiler compiler;
	auto empty = compiler.compile_to_ir("");
	REQUIRE(empty.has_value());
	CHECK_MESSAGE(!compiler.get_error_info().has_error,
				  "an empty success must differ from a failure");
	CHECK(compiler.get_error().empty());
}

TEST_CASE("a shipping build drops the tests") {
	Compiler compiler;
	CompilerOptions options;
	options.extensions = true;
	options.emit_tests = false;
	auto shipping = compiler.compile_to_ir(
			"@test\nfunc check_answer():\n\tpass\nfunc answer():\n\treturn 42\n", options);
	REQUIRE_MESSAGE(shipping.has_value(), compiler.get_error());
	CHECK(shipping->tests.empty());
	CHECK_MESSAGE(std::none_of(shipping->functions.begin(), shipping->functions.end(),
							   [](const IRFunction &function) { return function.name == "check_answer"; }),
				  "the frontend retained a test in a shipping build");
}

TEST_CASE("a base script is merged, and a restricted build refuses one") {
	Compiler compiler;
	CompilerOptions options;
	options.extensions = true;
	options.base_sources.push_back({ "Base", "res://base.sgd",
									 "func inherited() -> int:\n\treturn 42\n", false });
	auto derived = compiler.compile_to_ir(
			"extends Base\nfunc answer() -> int:\n\treturn inherited()\n", options);
	REQUIRE_MESSAGE(derived.has_value(), compiler.get_error());
	ir_verify(*derived);
	IRInterpreter inherited(*derived);
	CHECK_MESSAGE(std::get<int64_t>(inherited.call("answer")) == 42,
				  "the frontend skipped base merging");

	options.restricted = true;
	CHECK_MESSAGE(!compiler.compile_to_ir("extends Base\n", options),
				  "the frontend skipped the restricted policy");
}

TEST_CASE("a native class build keeps packed members, nested signals and preloads") {
	Compiler compiler;
	for (bool optimize : { false, true }) {
		CompilerOptions editor;
		editor.native_classes = true;
		editor.extensions = true;
		editor.optimize = optimize;
		auto compiled = compiler.compile_to_ir(
				"extends RefCounted\n"
				"var names: PackedStringArray = []\n"
				"var optional: PackedStringArray? = []\n"
				"class Row extends RefCounted:\n"
				"\tconst Data = preload(\"res://data.ugd\")\n"
				"\tsignal clicked(index: int)\n"
				"\tfunc trigger(index: int):\n\t\tclicked.emit(index)\n"
				"func axis():\n\treturn Vector3.AXIS_Z + Vector4i.AXIS_W\n",
				editor);
		REQUIRE_MESSAGE(compiled.has_value(), compiler.get_error());
		ir_verify(*compiled);
		auto axis_ir = compiler.compile_to_ir("func axis():\n\treturn Vector3.AXIS_Z + Vector4i.AXIS_W\n", editor);
		REQUIRE_MESSAGE(axis_ir.has_value(), compiler.get_error());
		IRInterpreter axes(*axis_ir);
		CHECK_MESSAGE(std::get<int64_t>(axes.call("axis")) == 5, "builtin integer constants changed values");
		CHECK_MESSAGE((compiled->has_member_init && compiled->has_global_init),
					  "packed members and nested preloads need their respective initializers");
		CHECK_MESSAGE((compiled->class_signatures.size() == 1 &&
					   compiled->class_signatures[0].signals.size() == 1),
					  "nested class signal metadata missing");
		auto encoded = encode_class_signatures(compiled->class_signatures);
		std::vector<ClassSignature> decoded;
		CHECK_MESSAGE(decode_class_signatures(encoded.data(), encoded.size(), decoded), "class metadata round trip");
		CHECK_MESSAGE((decoded.size() == 1 && decoded[0].signals.size() == 1 &&
					   decoded[0].signals[0].name == "clicked" &&
					   decoded[0].signals[0].parameters[0].type == Variant::INT),
					  "class metadata lost its signal signature");
		CHECK_MESSAGE((!decode_class_signatures(encoded.data(), encoded.size() - 1, decoded) && decoded.empty()),
					  "truncated class signal metadata accepted");
		CHECK_MESSAGE(!compiler.compile_to_ir("class Row:\n\tsignal hit\n\tvar hit = 1\n", editor),
					  "nested signal and field collision accepted");
		CHECK_MESSAGE(!compiler.compile_to_ir("class Row:\n\tsignal hit\n\tsignal hit\n", editor),
					  "duplicate nested signal accepted");
	}
}
