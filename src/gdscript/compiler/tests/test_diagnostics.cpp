// Diagnostics: what a failed compile can tell an editor.
//
// The .sgd script language extension underlines errors in the Godot editor, and
// to do that it needs the line and column as numbers rather than as prose
// inside the formatted message. Compiler::get_error_info() is that channel, so
// these tests pin down that every phase's errors arrive with their location
// intact, that a successful compile clears it, and that the formatted message
// still quotes the offending source line.
#include "../compiler.h"
#include "../compiler_exception.h"
#include "witness/doctest.h"
#include <iostream>
#include <string>

using namespace gdscript;

// -= Helpers =-

// Compile source that is expected to fail, and hand back what the compiler knew
// about the failure.
static CompilerError failing_compile(const std::string &source) {
	Compiler compiler;
	CompilerOptions options;
	options.extensions = true;
	options.output_elf = true;
	const std::vector<uint8_t> elf = compiler.compile(source, options);
	REQUIRE((elf.empty() && "expected this program to fail to compile"));
	const CompilerError &error = compiler.get_error_info();
	REQUIRE(error.has_error);
	return error;
}

static bool contains(const std::string &haystack, const std::string &needle) {
	return haystack.find(needle) != std::string::npos;
}

// -= Tests =-

TEST_CASE("success leaves no error") {
	Compiler compiler;
	const std::vector<uint8_t> elf = compiler.compile("func f():\n\treturn 1\n");
	REQUIRE(!elf.empty());
	REQUIRE(!compiler.get_error_info().has_error);
	REQUIRE(compiler.get_error().empty());
}

TEST_CASE("error is cleared by a later success") {
	// The same Compiler reused: a stale error would make the editor underline a
	// line in a file that now compiles.
	Compiler compiler;
	REQUIRE(compiler.compile("func f(:\n\treturn 1\n").empty());
	REQUIRE(compiler.get_error_info().has_error);

	REQUIRE(!compiler.compile("func f():\n\treturn 1\n").empty());
	REQUIRE(!compiler.get_error_info().has_error);
	REQUIRE(compiler.get_error_info().line == 0);
	REQUIRE(compiler.get_error_info().message.empty());
}

TEST_CASE("lexer error has a location") {
	const CompilerError error = failing_compile("func f():\n\tvar s = \"unterminated\n");
	REQUIRE(error.type == ErrorType::LEXER_ERROR);
	REQUIRE(error.line == 2);
}

TEST_CASE("parser error has a location") {
	const CompilerError error = failing_compile("func f():\n\treturn 1\n\nfunc g)\n");
	REQUIRE(error.type == ErrorType::PARSER_ERROR);
	REQUIRE(error.line == 4);
	REQUIRE(error.column > 0);
}

TEST_CASE("codegen error names the function") {
	// An unknown struct field is caught while lowering to IR, the one place the
	// compiler knows a location, the enclosing function and a hint all at once.
	const CompilerError error = failing_compile(
			"struct Point:\n"
			"\tvar x = 0\n"
			"\n"
			"func f():\n"
			"\tvar p = Point.new()\n"
			"\treturn p.z\n");
	REQUIRE(error.type == ErrorType::CODEGEN_ERROR);
	REQUIRE(error.line == 6);
	REQUIRE(error.column > 0);
	REQUIRE(error.function == "f");
	REQUIRE(contains(error.message, "z"));
	REQUIRE(contains(error.hint, "x"));
}

TEST_CASE("message has no type prefix") {
	// get_error() formats "[TYPE] message (line N)" for a terminal. The editor
	// puts the message in its own error field and the line in its own gutter,
	// so message must be the message alone.
	const CompilerError error = failing_compile("func f():\n\treturn 1\n\nfunc g)\n");
	REQUIRE(!error.message.empty());
	REQUIRE(error.message.front() != '[');
	REQUIRE(!contains(error.message, "line "));
}

TEST_CASE("formatted message quotes the source line") {
	// Only compile() has the source text, so it is the one place that can put
	// the offending line under the message for a terminal user.
	Compiler compiler;
	REQUIRE(compiler.compile("func f():\n\treturn 1\n\nfunc g)\n").empty());
	const std::string message = compiler.get_error();
	REQUIRE(contains(message, "[Parser Error]"));
	REQUIRE(contains(message, "func g)"));
	REQUIRE(contains(message, "^"));
}

TEST_CASE("unclosed bracket points at the bracket") {
	// An unclosed '(' swallows every newline after it, so EOF is the symptom,
	// not the cause: report the bracket's position.
	const CompilerError error = failing_compile("func f():\n\treturn 1\n\nfunc g(\n");
	REQUIRE(error.type == ErrorType::LEXER_ERROR);
	REQUIRE(error.line == 4);
	REQUIRE(error.column == 7);
	REQUIRE(contains(error.message, "Unclosed '('"));
}

TEST_CASE("source line survives crlf") {
	// A file saved on Windows must not put a carriage return in the snippet,
	// which would land the caret on its own line.
	Compiler compiler;
	REQUIRE(compiler.compile("func f():\r\n\treturn 1\r\n\r\nfunc g(\r\n").empty());
	const std::string message = compiler.get_error();
	REQUIRE(!contains(message, "\r"));
}

// The boxed ABI has sixteen total slots: seven pointers in a1-a7 and nine on
// the entry stack. A seventeenth parameter must still fail at its declaration.
TEST_CASE("too many parameters is refused") {
	const CompilerError error = failing_compile(
			"func f(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q):\n\treturn q\n");
	REQUIRE(error.type == ErrorType::CODEGEN_ERROR);
	REQUIRE(error.line == 1);
	REQUIRE(contains(error.message, "at most 16"));
}

// Cross-file script class: its bare value is the Script resource.
TEST_CASE("a script class outside the program") {
	Compiler compiler;
	CompilerOptions options;
	options.extensions = true;
	options.global_script_classes.emplace_back("Other", "res://other.gd");

	REQUIRE(!compiler.compile("func f():\n\treturn Other.helper()\n", options).empty());
	REQUIRE(!compiler.compile("func f():\n\treturn Other.SCALE\n", options).empty());
	REQUIRE(!compiler.compile("func f():\n\treturn Other.Shape.BOX\n", options).empty());
	REQUIRE(!compiler.compile("func f():\n\treturn Other.new()\n", options).empty());

	REQUIRE(!compiler.compile("func f():\n\treturn Other\n", options).empty());

	// Constants and statics are read-only through the class resource.
	REQUIRE(compiler.compile("func f():\n\tOther.SCALE = 2\n", options).empty());
	const CompilerError &assignment = compiler.get_error_info();
	REQUIRE(assignment.has_error);
	REQUIRE(assignment.type == ErrorType::CODEGEN_ERROR);
	REQUIRE(assignment.line == 2);
	REQUIRE(contains(assignment.message, "Cannot assign"));

	std::cout << "  \u2713 a script class in another file is its Script resource"
			  << std::endl;
}
