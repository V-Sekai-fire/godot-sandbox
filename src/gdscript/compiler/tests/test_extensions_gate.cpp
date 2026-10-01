#include "../compiler.h"
#include "../source_model.h"
#include "witness/doctest.h"
#include <string>
#include <vector>

using namespace gdscript;

namespace {

bool compiles(const std::string &source, bool extensions) {
	Compiler compiler;
	CompilerOptions options;
	options.output_elf = false;
	options.extensions = extensions;
	compiler.compile(source, options);
	return !compiler.get_error_info().has_error;
}

bool has_error(const std::string &source, uint32_t extra_flags) {
	const SourceModel model = analyze_source(source, "gate.gd", ANALYZE_DIAGNOSTICS | extra_flags);
	for (const SourceDiagnostic &d : model.diagnostics) {
		if (d.severity == DiagnosticSeverity::ERROR)
			return true;
	}
	return false;
}

const std::vector<std::string> extension_sources = {
	"struct Point:\n\tvar x: int\n",
	"func f(a: int?) -> int:\n\treturn 0\n",
	"func f(a: int | String) -> int:\n\treturn 0\n",
	"func f(a):\n\treturn a ?? 1\n",
	"func f(a):\n\treturn a?.size()\n",
	"@test\nfunc t():\n\tpass\n",
	"@requires_host_hook\nfunc f() -> int:\n\treturn 1\n",
};

} // namespace

TEST_CASE("extensions are refused unless opted in") {
	for (const std::string &source : extension_sources) {
		CAPTURE(source);
		CHECK_FALSE(compiles(source, false));
		CHECK(compiles(source, true));
		CHECK(has_error(source, 0));
		CHECK_FALSE(has_error(source, ANALYZE_EXTENSIONS));
	}
}

TEST_CASE("extension keywords are plain identifiers in GDScript") {
	const std::string source =
			"func f() -> int:\n\tvar struct := 1\n\tvar trait := 2\n\tvar switch := 3\n\tvar uses := 4\n"
			"\treturn struct + trait + switch + uses\n";
	CHECK(compiles(source, false));
	CHECK_FALSE(has_error(source, 0));
}

TEST_CASE("plain GDScript compiles either way") {
	const std::string source = "func f(a: int) -> int:\n\treturn a | 1\n";
	CHECK(compiles(source, false));
	CHECK(compiles(source, true));
}

TEST_CASE("a script base a native class imports is gated the same way") {
	for (bool extensions : { false, true }) {
		CAPTURE(extensions);
		Compiler compiler;
		CompilerOptions options;
		options.output_elf = false;
		options.native_classes = true;
		options.extensions = extensions;
		options.load_class_source = [](const std::string &path, const std::string &) {
			CompilerOptions::BaseSource base;
			base.path = path;
			base.source = "extends RefCounted\nfunc f(a: int?) -> int:\n\treturn 0\n";
			return base;
		};
		compiler.compile("const Base = preload(\"res://base.gd\")\n"
						 "class Inner extends Base:\n"
						 "\tfunc g():\n"
						 "\t\treturn 1\n",
						 options);
		CHECK(compiler.get_error_info().has_error == !extensions);
	}
}
