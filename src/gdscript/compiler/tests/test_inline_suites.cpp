// One-line suites: `if x: return`, `func f(): return 1`, one-line match arms.
//
// The body of a ':' is the same grammar whether it is written indented on the
// following lines or on the line of the ':' itself, so the property worth
// pinning down is that the two spellings compile to the same IR -- not that the
// one-line form happens to produce something that works. Every construct that
// takes a body is checked against its block twin, then run.
//
// The two places a one-line body can be misread are where the lexer's layout
// tokens do the deciding:
//
//   - a one-line body leaves its NEWLINE behind, so an `elif`/`else` written on
//     the next line still belongs to it, while a DEDENT before that `else` puts
//     it out of reach and back on the enclosing `if`;
//   - `;` continues the body it is written in, so `if false: a = 1; a = 2`
//     runs neither statement.
#include "../codegen.h"
#include "../compiler_exception.h"
#include "../ir_interpreter.h"
#include "../ir_optimizer.h"
#include "../ir_verifier.h"
#include "../lexer.h"
#include "../parser.h"
#include "witness/doctest.h"
#include <iostream>
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

// Instruction text carries no source line, so two spellings of the same body
// compare equal exactly when they mean the same thing.
static std::string function_text(const IRProgram &ir, const std::string &name) {
	std::string text;
	for (const auto &instr : find_function(ir, name).instructions) {
		text += instr.to_string();
		text += '\n';
	}
	return text;
}

// The one-line spelling of a body must compile to what the indented spelling
// compiles to. Both sources declare `test`.
static void assert_same_ir(const std::string &what, const std::string &one_line, const std::string &block) {
	const IRProgram inline_ir = compile_to_ir(one_line);
	const IRProgram block_ir = compile_to_ir(block);
	const std::string inline_text = function_text(inline_ir, "test");
	const std::string block_text = function_text(block_ir, "test");
	if (inline_text != block_text) {
		std::cerr << "FAIL: " << what << ": one-line and block spellings differ\n"
				  << "--- one-line ---\n"
				  << inline_text
				  << "--- block ---\n"
				  << block_text;
		FAIL("one-line suite must compile to the block form");
	}
}

static int64_t run_int(const std::string &source, const std::string &function = "test",
					   const std::vector<IRInterpreter::Value> &args = {}) {
	IRProgram ir = compile_to_ir(source, true);
	ir_verify(ir);
	IRInterpreter interp(ir);
	return std::get<int64_t>(interp.call(function, args));
}

static bool rejects(const std::string &source) {
	try {
		compile_to_ir(source);
	} catch (const CompilerException &) {
		return true;
	} catch (const std::exception &) {
		return true;
	}
	return false;
}

// -= Tests =-

// Every construct that takes a body, one-line against indented.
TEST_CASE("spellings agree") {
	std::cout << "Test: one-line and indented spellings agree\n";

	assert_same_ir("if",
				   "func test(n):\n"
				   "\tif n > 0: return 1\n"
				   "\treturn 0\n",
				   "func test(n):\n"
				   "\tif n > 0:\n"
				   "\t\treturn 1\n"
				   "\treturn 0\n");

	assert_same_ir("if/elif/else",
				   "func test(n):\n"
				   "\tif n < 0: return -1\n"
				   "\telif n == 0: return 0\n"
				   "\telse: return 1\n",
				   "func test(n):\n"
				   "\tif n < 0:\n"
				   "\t\treturn -1\n"
				   "\telif n == 0:\n"
				   "\t\treturn 0\n"
				   "\telse:\n"
				   "\t\treturn 1\n");

	assert_same_ir("while",
				   "func test(n):\n"
				   "\twhile n > 0: n -= 1\n"
				   "\treturn n\n",
				   "func test(n):\n"
				   "\twhile n > 0:\n"
				   "\t\tn -= 1\n"
				   "\treturn n\n");

	assert_same_ir("for",
				   "func test(n):\n"
				   "\tvar total = 0\n"
				   "\tfor i in n: total += i\n"
				   "\treturn total\n",
				   "func test(n):\n"
				   "\tvar total = 0\n"
				   "\tfor i in n:\n"
				   "\t\ttotal += i\n"
				   "\treturn total\n");

	assert_same_ir("match arms",
				   "func test(n):\n"
				   "\tmatch n:\n"
				   "\t\t0: return 10\n"
				   "\t\t1, 2: return 20\n"
				   "\t\tvar v when v > 9: return 30\n"
				   "\t\t_: return 40\n",
				   "func test(n):\n"
				   "\tmatch n:\n"
				   "\t\t0:\n"
				   "\t\t\treturn 10\n"
				   "\t\t1, 2:\n"
				   "\t\t\treturn 20\n"
				   "\t\tvar v when v > 9:\n"
				   "\t\t\treturn 30\n"
				   "\t\t_:\n"
				   "\t\t\treturn 40\n");

	assert_same_ir("func",
				   "func test(n): return n + 1\n",
				   "func test(n):\n"
				   "\treturn n + 1\n");

	// A `;` list is one body, not a statement that escaped it.
	assert_same_ir("semicolon-separated body",
				   "func test(n):\n"
				   "\tif n > 0: n += 1; n += 2\n"
				   "\treturn n\n",
				   "func test(n):\n"
				   "\tif n > 0:\n"
				   "\t\tn += 1\n"
				   "\t\tn += 2\n"
				   "\treturn n\n");

	std::cout << "  All spellings agree\n";
}

// The same programs, executed.
TEST_CASE("execution") {
	std::cout << "Test: one-line suites run\n";

	const std::string classify =
			"func test(n):\n"
			"\tif n < 0: return -1\n"
			"\telif n == 0: return 0\n"
			"\telse: return 1\n";
	REQUIRE(run_int(classify, "test", { int64_t(-5) }) == -1);
	REQUIRE(run_int(classify, "test", { int64_t(0) }) == 0);
	REQUIRE(run_int(classify, "test", { int64_t(5) }) == 1);

	REQUIRE(run_int(
					"func test():\n"
					"\tvar n = 4\n"
					"\twhile n > 0: n -= 1\n"
					"\treturn n\n") == 0);

	REQUIRE(run_int(
					"func test():\n"
					"\tvar total = 0\n"
					"\tfor i in 5: total += i\n"
					"\treturn total\n") == 10);

	// A one-line func, called from another one-line func.
	REQUIRE(run_int(
					"func double(n): return n * 2\n"
					"func test(): return double(21)\n") == 42);

	const std::string arms =
			"func test(n):\n"
			"\tmatch n:\n"
			"\t\t0: return 10\n"
			"\t\t1, 2: return 20\n"
			"\t\tvar v when v > 9: return 30\n"
			"\t\t_: return 40\n";
	REQUIRE(run_int(arms, "test", { int64_t(0) }) == 10);
	REQUIRE(run_int(arms, "test", { int64_t(2) }) == 20);
	REQUIRE(run_int(arms, "test", { int64_t(50) }) == 30);
	REQUIRE(run_int(arms, "test", { int64_t(4) }) == 40);

	std::cout << "  One-line suites run\n";
}

// A `;` continues the body it was written in. Both statements are the if's, so
// a false condition runs neither.
TEST_CASE("semicolon stays in the body") {
	std::cout << "Test: ';' continues the one-line body\n";

	const std::string source =
			"func test(n):\n"
			"\tvar a = 0\n"
			"\tif n > 0: a = 1; a = 2\n"
			"\treturn a\n";
	REQUIRE(run_int(source, "test", { int64_t(1) }) == 2);
	REQUIRE(run_int(source, "test", { int64_t(0) }) == 0);

	std::cout << "  ';' continues the body\n";
}

// Where the `else` lands. A one-line body leaves its NEWLINE, so the `else` on
// the next line is the inline `if`'s; a DEDENT in between hands it to the
// enclosing one instead.
TEST_CASE("else binding") {
	std::cout << "Test: 'else' after a one-line body\n";

	// else belongs to the inline if.
	const std::string same_level =
			"func test(n):\n"
			"\tif n > 0: return 1\n"
			"\telse: return 2\n";
	REQUIRE(run_int(same_level, "test", { int64_t(1) }) == 1);
	REQUIRE(run_int(same_level, "test", { int64_t(0) }) == 2);

	// A DEDENT sits between the inner one-line body and the `else`, which is
	// therefore the outer if's. n == 1 falls out of the inner if having
	// returned nothing, so `test` reaches the trailing return.
	const std::string outer =
			"func test(n):\n"
			"\tif n > 0:\n"
			"\t\tif n > 5: return 1\n"
			"\telse:\n"
			"\t\treturn 2\n"
			"\treturn 3\n";
	REQUIRE(run_int(outer, "test", { int64_t(9) }) == 1);
	REQUIRE(run_int(outer, "test", { int64_t(1) }) == 3);
	REQUIRE(run_int(outer, "test", { int64_t(0) }) == 2);

	std::cout << "  'else' binds to the right 'if'\n";
}

// A one-line body is a body: it nests, and what nests inside it ends where the
// line does.
TEST_CASE("nesting") {
	std::cout << "Test: nested one-line suites\n";

	const std::string nested =
			"func test(a, b):\n"
			"\tif a > 0: if b > 0: return 3\n"
			"\treturn 0\n";
	REQUIRE(run_int(nested, "test", { int64_t(1), int64_t(1) }) == 3);
	REQUIRE(run_int(nested, "test", { int64_t(1), int64_t(0) }) == 0);
	REQUIRE(run_int(nested, "test", { int64_t(0), int64_t(1) }) == 0);

	// A one-line body inside a one-line func, and a loop inside it.
	REQUIRE(run_int(
					"func test():\n"
					"\tvar total = 0\n"
					"\tfor i in 6: if i % 2 == 0: total += i\n"
					"\treturn total\n") == 6);

	std::cout << "  Nesting works\n";
}

// A block body still ends where it did: the DEDENT, with no newline of its own.
TEST_CASE("block bodies unchanged") {
	std::cout << "Test: indented bodies still end at the DEDENT\n";

	// Nothing follows the innermost block but end of file.
	REQUIRE(run_int(
					"func test():\n"
					"\tvar n = 0\n"
					"\tfor i in 3:\n"
					"\t\tif i > 0:\n"
					"\t\t\tn += i\n"
					"\treturn n\n") == 3);

	// No trailing newline after a one-line body at end of file.
	REQUIRE(run_int("func test(): return 7") == 7);

	std::cout << "  Indented bodies unchanged\n";
}

// A ':' with nothing after it is still an error, on one line or on the next.
TEST_CASE("empty body rejected") {
	std::cout << "Test: an empty body is rejected\n";

	REQUIRE(rejects(
			"func test(n):\n"
			"\tif n > 0:\n"
			"\treturn 1\n"));
	REQUIRE(rejects("func test():\n"));
	REQUIRE(rejects(
			"func test(n):\n"
			"\twhile n > 0:\n"
			"\treturn n\n"));

	std::cout << "  Empty bodies rejected\n";
}
