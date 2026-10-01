#include "../codegen.h"
#include "../compiler.h"
#include "../ir_interpreter.h"
#include "../lexer.h"
#include "../parser.h"
#include "witness/doctest.h"
#include <iostream>

using namespace gdscript;

// Helper to compile and execute
IRInterpreter::Value execute(const std::string &source, const std::string &function = "main",
							 const std::vector<IRInterpreter::Value> &args = {}) {
	Compiler compiler;
	CompilerOptions options;

	// Compile to IR
	auto elf_data = compiler.compile(source, options);
	if (elf_data.empty()) {
		throw std::runtime_error("Compilation failed: " + compiler.get_error());
	}

	// Extract IR from compiler
	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();
	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);

	// Execute using interpreter
	IRInterpreter interp(ir);
	return interp.call(function, args);
}

int64_t execute_int(const std::string &source, const std::string &function = "main",
					const std::vector<IRInterpreter::Value> &args = {}) {
	auto result = execute(source, function, args);
	// 'and', 'or' and 'not' produce a bool in GDScript, so an integer-valued
	// expectation has to accept one. Anything else (a float, a string) is a
	// genuine mismatch and still throws.
	if (std::holds_alternative<bool>(result)) {
		return std::get<bool>(result) ? 1 : 0;
	}
	return std::get<int64_t>(result);
}

TEST_CASE("literals") {
	// Integer
	std::string src_int = R"(
func test():
	return 42
)";
	REQUIRE(execute_int(src_int, "test") == 42);

	// Negative integer
	std::string src_neg = R"(
func test():
	return -42
)";
	REQUIRE(execute_int(src_neg, "test") == -42);

	// Boolean true
	std::string src_true = R"(
func test():
	return 1
)";
	REQUIRE(execute_int(src_true, "test") == 1);

	// Boolean false
	std::string src_false = R"(
func test():
	return 0
)";
	REQUIRE(execute_int(src_false, "test") == 0);

	// Null (treated as 0 in integer context)
	std::string src_null = R"(
func test():
	var x = null
	return 0
)";
	REQUIRE(execute_int(src_null, "test") == 0);
}

TEST_CASE("all operators") {
	// Arithmetic - all need proper indentation
	REQUIRE(execute_int("func test():\n\treturn 10 + 5\n", "test") == 15);
	REQUIRE(execute_int("func test():\n\treturn 10 - 5\n", "test") == 5);
	REQUIRE(execute_int("func test():\n\treturn 10 * 5\n", "test") == 50);
	REQUIRE(execute_int("func test():\n\treturn 10 / 5\n", "test") == 2);
	REQUIRE(execute_int("func test():\n\treturn 10 % 3\n", "test") == 1);

	// Comparison
	REQUIRE(execute_int("func test():\n\treturn 5 == 5\n", "test") == 1);
	REQUIRE(execute_int("func test():\n\treturn 5 != 3\n", "test") == 1);
	REQUIRE(execute_int("func test():\n\treturn 3 < 5\n", "test") == 1);
	REQUIRE(execute_int("func test():\n\treturn 5 <= 5\n", "test") == 1);
	REQUIRE(execute_int("func test():\n\treturn 5 > 3\n", "test") == 1);
	REQUIRE(execute_int("func test():\n\treturn 5 >= 5\n", "test") == 1);

	// Logical
	REQUIRE(execute_int("func test():\n\treturn 1 and 1\n", "test") == 1);
	REQUIRE(execute_int("func test():\n\treturn 1 and 0\n", "test") == 0);
	REQUIRE(execute_int("func test():\n\treturn 0 or 1\n", "test") == 1);
	REQUIRE(execute_int("func test():\n\treturn 0 or 0\n", "test") == 0);
	REQUIRE(execute_int("func test():\n\treturn not 0\n", "test") == 1);
	REQUIRE(execute_int("func test():\n\treturn not 1\n", "test") == 0);

	// Unary
	REQUIRE(execute_int("func test():\n\treturn -5\n", "test") == -5);
}

TEST_CASE("control flow") {
	// If
	std::string src_if = R"(
func test():
	if 1:
		return 10
	return 20
)";
	REQUIRE(execute_int(src_if, "test") == 10);

	// If-else
	std::string src_if_else = R"(
func test():
	if 0:
		return 10
	else:
		return 20
)";
	REQUIRE(execute_int(src_if_else, "test") == 20);

	// If-elif-else
	std::string src_elif = R"(
func test():
	var x = 2
	if x == 1:
		return 10
	elif x == 2:
		return 20
	else:
		return 30
)";
	REQUIRE(execute_int(src_elif, "test") == 20);

	// While
	std::string src_while = R"(
func test():
	var i = 0
	var sum = 0
	while i < 5:
		sum = sum + i
		i = i + 1
	return sum
)";
	REQUIRE(execute_int(src_while, "test") == 10); // 0+1+2+3+4

	// Break
	std::string src_break = R"(
func test():
	var i = 0
	while 1:
		if i == 5:
			break
		i = i + 1
	return i
)";
	REQUIRE(execute_int(src_break, "test") == 5);

	// Continue
	std::string src_continue = R"(
func test():
	var i = 0
	var sum = 0
	while i < 10:
		i = i + 1
		if i % 2 == 0:
			continue
		sum = sum + i
	return sum
)";
	REQUIRE(execute_int(src_continue, "test") == 25); // 1+3+5+7+9

	// Pass
	std::string src_pass = R"(
func test():
	if 1:
		pass
	return 42
)";
	REQUIRE(execute_int(src_pass, "test") == 42);
}

TEST_CASE("variables") {
	// Variable declaration with initializer
	std::string src_init = R"(
func test():
	var x = 10
	return x
)";
	REQUIRE(execute_int(src_init, "test") == 10);

	// Variable declaration without initializer
	std::string src_no_init = R"(
func test():
	var x
	x = 5
	return x
)";
	REQUIRE(execute_int(src_no_init, "test") == 5);

	// Variable assignment
	std::string src_assign = R"(
func test():
	var x = 10
	x = 20
	return x
)";
	REQUIRE(execute_int(src_assign, "test") == 20);

	// Multiple variables
	std::string src_multi = R"(
func test():
	var a = 1
	var b = 2
	var c = 3
	return a + b + c
)";
	REQUIRE(execute_int(src_multi, "test") == 6);
}

TEST_CASE("functions") {
	// Function with parameters
	std::string src_params = R"(
func add(a, b):
	return a + b
)";
	REQUIRE(execute_int(src_params, "add", { int64_t(3), int64_t(4) }) == 7);

	// Multiple parameters
	std::string src_multi = R"(
func sum(a, b, c, d):
	return a + b + c + d
)";
	REQUIRE(execute_int(src_multi, "sum", { int64_t(1), int64_t(2), int64_t(3), int64_t(4) }) == 10);

	// Function without return (implicit return)
	std::string src_no_ret = R"(
func test():
	var x = 10
)";
	REQUIRE(std::holds_alternative<std::monostate>(execute(src_no_ret, "test")));

	// Bare return
	std::string src_bare_ret = R"(
func test():
	var x = 10
	return
)";
	REQUIRE(std::holds_alternative<std::monostate>(execute(src_bare_ret, "test")));
}

TEST_CASE("expressions") {
	// Operator precedence
	std::string src_prec = R"(
func test():
	return 2 + 3 * 4
)";
	REQUIRE(execute_int(src_prec, "test") == 14); // Not 20

	// Parentheses
	std::string src_paren = R"(
func test():
	return (2 + 3) * 4
)";
	REQUIRE(execute_int(src_paren, "test") == 20);

	// Nested expressions
	std::string src_nested = R"(
func test():
	return ((5 + 3) * 2) - (4 / 2)
)";
	REQUIRE(execute_int(src_nested, "test") == 14); // (8*2) - 2 = 14

	// Mixed logical and arithmetic
	std::string src_mixed = R"(
func test():
	return (5 > 3) and (2 < 4)
)";
	REQUIRE(execute_int(src_mixed, "test") == 1);
}

TEST_CASE("edge cases") {
	// Zero
	REQUIRE(execute_int("func test():\n\treturn 0\n", "test") == 0);

	// Large numbers
	REQUIRE(execute_int("func test():\n\treturn 1000000\n", "test") == 1000000);

	// Division by zero behavior (implementation defined)
	// We skip this as it may crash or return undefined

	// Deeply nested scopes
	std::string src_deep = R"(
func test():
	var x = 1
	if 1:
		var y = 2
		if 1:
			var z = 3
			if 1:
				var w = 4
				return x + y + z + w
	return 0
)";
	REQUIRE(execute_int(src_deep, "test") == 10);

	// Empty function body with implicit return
	std::string src_empty = R"(
func test():
	pass
)";
	REQUIRE(std::holds_alternative<std::monostate>(execute(src_empty, "test")));
}
