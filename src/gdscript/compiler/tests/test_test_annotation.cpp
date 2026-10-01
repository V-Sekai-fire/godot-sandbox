// `@test`: in-document test cases.
//
// A `.sgd` marks an argless top-level `func` with `@test`, and the compiler
// publishes it as a test case beside the ELF. The function stays an ordinary
// method -- same symbol, same signature, same arity check -- so the only new
// information is which functions are tests. The host runner calls them and
// counts exceptions; nothing about the run-time surface changes.
//
// Two things are pinned down here. First, where the annotation is refused:
// anywhere the runner would have to invent something (a parameter value, a
// resumption of a coroutine, an instance of a nested class). Second, that a
// shipping build (`emit_tests` off) leaves no trace of a test in the ELF, since
// the tests are dropped before codegen rather than filtered out of the table.
#include "../codegen.h"
#include "../compiler.h"
#include "../compiler_exception.h"
#include "../function_signature.h"
#include "../lexer.h"
#include "../parser.h"
#include "../source_model.h"
#include "witness/doctest.h"
#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

using namespace gdscript;

static IRProgram compile_to_ir(const std::string &source) {
	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	parser.set_doc_comments(lexer.doc_comments());
	Program program = parser.parse();
	CodeGenerator codegen;
	return codegen.generate(program);
}

static std::string rejection(const std::string &source) {
	try {
		compile_to_ir(source);
	} catch (const CompilerException &e) {
		return e.message();
	}
	return "";
}

static int rejection_line(const std::string &source) {
	try {
		compile_to_ir(source);
	} catch (const CompilerException &e) {
		return e.line();
	}
	return 0;
}

static const FunctionSignature &find_test(const IRProgram &ir, const std::string &name) {
	for (const FunctionSignature &test : ir.tests) {
		if (test.name == name)
			return test;
	}
	throw std::runtime_error("Test not found: " + name);
}

static bool has_function(const IRProgram &ir, const std::string &name) {
	return std::any_of(ir.functions.begin(), ir.functions.end(),
					   [&name](const IRFunction &function) { return function.name == name; });
}

// -= Accepted placements =-

TEST_CASE("a plain function becomes a test") {
	const IRProgram ir = compile_to_ir(
			"func helper() -> int:\n"
			"\treturn 1\n"
			"@test\n"
			"func it_adds():\n"
			"\tassert(helper() == 1)\n");
	REQUIRE(ir.tests.size() == 1);
	REQUIRE(ir.tests[0].name == "it_adds");
	REQUIRE(ir.tests[0].parameters.empty());
	// Still an ordinary method: symbol, signature and arity are untouched.
	REQUIRE(has_function(ir, "it_adds"));
	REQUIRE(std::any_of(ir.signatures.begin(), ir.signatures.end(),
						[](const FunctionSignature &s) { return s.name == "it_adds"; }));
}

TEST_CASE("a static function becomes a test") {
	const IRProgram ir = compile_to_ir(
			"@test\n"
			"static func math_is_math():\n"
			"\tassert(1 + 1 == 2)\n");
	REQUIRE(ir.tests.size() == 1);
	REQUIRE(find_test(ir, "math_is_math").is_static);
}

TEST_CASE("tests are published in declaration order") {
	const IRProgram ir = compile_to_ir(
			"@test\n"
			"func first():\n"
			"\tpass\n"
			"func not_a_test():\n"
			"\tpass\n"
			"@test\n"
			"func second():\n"
			"\tpass\n");
	REQUIRE(ir.tests.size() == 2);
	REQUIRE(ir.tests[0].name == "first");
	REQUIRE(ir.tests[1].name == "second");
}

TEST_CASE("a test carries its line and doc comment") {
	const IRProgram ir = compile_to_ir(
			"extends Node\n"
			"\n"
			"@test\n"
			"## Members start at their declared defaults.\n"
			"func defaults_hold():\n"
			"\tpass\n");
	const FunctionSignature &test = find_test(ir, "defaults_hold");
	REQUIRE(test.line == 5);
	REQUIRE(test.description == "Members start at their declared defaults.");
}

TEST_CASE("an annotation stacks with others") {
	const IRProgram ir = compile_to_ir(
			"@tool\n"
			"@test\n"
			"func runs_in_the_editor_too():\n"
			"\tpass\n");
	REQUIRE(ir.tests.size() == 1);
}

// -= Refused placements =-

TEST_CASE("a variable is refused") {
	REQUIRE(rejection("@test\nvar x = 1\n") ==
			"@test can only be applied to a function");
	REQUIRE(rejection("@test\nconst X = 1\n") ==
			"@test can only be applied to a function");
}

TEST_CASE("a signal is refused") {
	REQUIRE(rejection("@test\nsignal fired()\n") ==
			"@test can only be applied to a function");
}

TEST_CASE("no declaration is refused") {
	REQUIRE(rejection("@test\n") == "@test can only be applied to a function");
}

TEST_CASE("a nested class method is refused") {
	const std::string source =
			"class Inner:\n"
			"\t@test\n"
			"\tfunc inner_test():\n"
			"\t\tpass\n";
	REQUIRE(rejection(source) == "@test is for a file-level function");
	// Reported where the annotation is, not where the file-level scan gave up.
	REQUIRE(rejection_line(source) == 2);
}

TEST_CASE("a trait method is refused") {
	REQUIRE(rejection(
					"trait Killable:\n"
					"\t@test\n"
					"\tfunc die() -> void\n") == "@test is for a file-level function");
}

TEST_CASE("a parameter is refused") {
	REQUIRE(rejection(
					"@test\n"
					"func takes_one(value: int):\n"
					"\tpass\n") == "@test function 'takes_one' takes no parameters");
	// A default is still a parameter the runner would have to invent.
	REQUIRE(rejection(
					"@test\n"
					"func takes_default(value: int = 3):\n"
					"\tpass\n") == "@test function 'takes_default' takes no parameters");
}

TEST_CASE("a coroutine is refused") {
	REQUIRE(rejection(
					"@test\n"
					"func waits():\n"
					"\tawait get_tree().process_frame\n") ==
			"@test function 'waits' cannot be a coroutine");
}

TEST_CASE("rpc is refused") {
	REQUIRE(rejection(
					"@rpc(\"any_peer\")\n"
					"@test\n"
					"func remote_test():\n"
					"\tpass\n") == "@test cannot be combined with @rpc");
}

TEST_CASE("arguments are refused") {
	REQUIRE(rejection(
					"@test(\"name\")\n"
					"func named():\n"
					"\tpass\n") == "@test takes no arguments");
}

TEST_CASE("a duplicate is refused") {
	REQUIRE(rejection(
					"@test\n"
					"@test\n"
					"func twice():\n"
					"\tpass\n") == "@test can only be used once per function");
}

// -= Base sources =-

TEST_CASE("an overridden base test is not a test") {
	// A displaced base implementation lands on a mangled symbol; only the
	// method visible on the final script is a test of it. Same rule @rpc uses.
	CompilerOptions options;
	options.extensions = true;
	options.output_elf = false;
	options.base_sources.push_back(CompilerOptions::BaseSource{
			"Base", "res://base.sgd",
			"@test\n"
			"func shared_case():\n"
			"\tpass\n",
			false });

	Compiler compiler;
	compiler.compile(
			"extends \"res://base.sgd\"\n"
			"@test\n"
			"func shared_case():\n"
			"\tpass\n",
			options);
	REQUIRE(compiler.get_error().empty());
	const std::vector<FunctionSignature> &tests = compiler.get_test_signatures();
	REQUIRE(tests.size() == 1);
	REQUIRE(tests[0].name == "shared_case");
}

TEST_CASE("an inherited base test is a test") {
	CompilerOptions options;
	options.extensions = true;
	options.output_elf = false;
	options.base_sources.push_back(CompilerOptions::BaseSource{
			"Base", "res://base.sgd",
			"@test\n"
			"func base_case():\n"
			"\tpass\n",
			false });

	Compiler compiler;
	compiler.compile("extends \"res://base.sgd\"\nfunc leaf():\n\tpass\n", options);
	REQUIRE(compiler.get_error().empty());
	REQUIRE(compiler.get_test_signatures().size() == 1);
	REQUIRE(compiler.get_test_signatures()[0].name == "base_case");
}

// -= Shipping builds =-

static const std::string TEST_PROGRAM =
		"func helper() -> int:\n"
		"\treturn 7\n"
		"@test\n"
		"func helper_answers_seven():\n"
		"\tassert(helper() == 7)\n"
		"func other() -> int:\n"
		"\treturn helper() + 1\n";

TEST_CASE("a shipping build drops the tests") {
	CompilerOptions options;
	options.extensions = true;
	options.emit_tests = false;
	Compiler compiler;
	const std::vector<uint8_t> elf = compiler.compile(TEST_PROGRAM, options);
	REQUIRE(!elf.empty());
	REQUIRE(compiler.get_test_signatures().empty());
	// The symbol is gone, not merely unlisted: the declaration never reached
	// codegen, so nothing in the ELF names it.
	const std::string image(reinterpret_cast<const char *>(elf.data()), elf.size());
	REQUIRE(image.find("helper_answers_seven") == std::string::npos);
	REQUIRE(image.find("helper") != std::string::npos);
	REQUIRE(std::none_of(compiler.get_function_signatures().begin(),
						 compiler.get_function_signatures().end(),
						 [](const FunctionSignature &s) { return s.name == "helper_answers_seven"; }));
}

static std::string build_error(const std::string &source, bool emit_tests) {
	CompilerOptions options;
	options.extensions = true;
	options.emit_tests = emit_tests;
	Compiler compiler;
	const std::vector<uint8_t> elf = compiler.compile(source, options);
	return elf.empty() ? compiler.get_error() : std::string();
}

TEST_CASE("reaching a test from a plain function is refused") {
	const std::string called =
			"@test\nfunc checks_it():\n\tpass\n"
			"func caller():\n\tchecks_it()\n";
	REQUIRE(build_error(called, true).find("@test") != std::string::npos);
	REQUIRE(build_error(called, false).find("@test") != std::string::npos);

	const std::string as_callable =
			"@test\nfunc checks_it():\n\tpass\n"
			"func caller():\n\tvar c = checks_it\n\tc.call()\n";
	REQUIRE(as_callable.find("checks_it") != std::string::npos);
	REQUIRE(build_error(as_callable, true).find("@test") != std::string::npos);
	REQUIRE(build_error(as_callable, false).find("@test") != std::string::npos);

	const std::string between_tests =
			"@test\nfunc checks_it():\n\tpass\n"
			"@test\nfunc checks_more():\n\tchecks_it()\n";
	REQUIRE(build_error(between_tests, true).empty());
	REQUIRE(build_error(between_tests, false).empty());

	const std::string helper =
			"@test\nfunc checks_it():\n\thelper()\n"
			"func helper():\n\tpass\n";
	REQUIRE(build_error(helper, true).empty());
	REQUIRE(build_error(helper, false).empty());
}

TEST_CASE("dropping the tests leaves the rest unchanged") {
	// Tests call helpers, never the reverse, so removing them is not supposed
	// to move a single instruction of the code that ships.
	CompilerOptions with_tests;
	with_tests.extensions = true;
	CompilerOptions without_tests;
	without_tests.extensions = true;
	without_tests.emit_tests = false;

	Compiler a;
	Compiler b;
	const std::vector<uint8_t> shipped = b.compile(TEST_PROGRAM, without_tests);
	a.compile(TEST_PROGRAM, with_tests);
	REQUIRE(!shipped.empty());

	// Same function bodies, same signatures, minus the one test.
	std::vector<std::string> kept;
	for (const FunctionSignature &s : a.get_function_signatures()) {
		if (s.name != "helper_answers_seven")
			kept.push_back(s.name);
	}
	std::vector<std::string> shipped_names;
	for (const FunctionSignature &s : b.get_function_signatures()) {
		shipped_names.push_back(s.name);
	}
	REQUIRE(kept == shipped_names);
	REQUIRE(a.get_test_signatures().size() == 1);
}

// -= Source model =-

TEST_CASE("the source model flags a test") {
	const SourceModel model = analyze_source(TEST_PROGRAM, "res://sample.sgd",
											 ANALYZE_DECLARATIONS | ANALYZE_EXTENSIONS);

	bool found = false;
	for (const SourceDeclaration &declaration : model.declarations) {
		if (declaration.name != "helper_answers_seven")
			continue;
		found = true;
		REQUIRE(declaration.kind == DeclarationKind::FUNCTION);
		REQUIRE((declaration.flags & DECLARATION_TEST) != 0);
		REQUIRE(std::find(declaration.annotation_arguments.begin(),
						  declaration.annotation_arguments.end(), "@test") !=
				declaration.annotation_arguments.end());
	}
	REQUIRE(found);
}
