#include "../lexer.h"
#include "../parser.h"
#include "witness/doctest.h"
#include <iostream>

using namespace gdscript;

TEST_CASE("simple function") {
	std::string source = R"(func add(a, b):
	return a + b
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	REQUIRE(program.functions.size() == 1);
	REQUIRE(program.functions[0].name == "add");
	REQUIRE(program.functions[0].parameters.size() == 2);
	REQUIRE(program.functions[0].parameters[0].name == "a");
	REQUIRE(program.functions[0].parameters[1].name == "b");
	REQUIRE(program.functions[0].body.size() == 1);

	// Check return statement
	auto *ret_stmt = dynamic_cast<ReturnStmt *>(program.functions[0].body[0].get());
	REQUIRE(ret_stmt != nullptr);
	REQUIRE(ret_stmt->value != nullptr);
}

TEST_CASE("variable declaration") {
	std::string source = R"(func test():
	var x = 10
	var y
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	REQUIRE(program.functions[0].body.size() == 2);

	auto *var1 = dynamic_cast<VarDeclStmt *>(program.functions[0].body[0].get());
	REQUIRE(var1 != nullptr);
	REQUIRE(var1->name == "x");
	REQUIRE(var1->initializer != nullptr);

	auto *var2 = dynamic_cast<VarDeclStmt *>(program.functions[0].body[1].get());
	REQUIRE(var2 != nullptr);
	REQUIRE(var2->name == "y");
	REQUIRE(var2->initializer == nullptr);
}

TEST_CASE("if statement") {
	std::string source = R"(func test(x):
	if x > 0:
		return 1
	else:
		return -1
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	REQUIRE(program.functions[0].body.size() == 1);

	auto *if_stmt = dynamic_cast<IfStmt *>(program.functions[0].body[0].get());
	REQUIRE(if_stmt != nullptr);
	REQUIRE(if_stmt->condition != nullptr);
	REQUIRE(if_stmt->then_branch.size() == 1);
	REQUIRE(if_stmt->else_branch.size() == 1);
}

TEST_CASE("if var binding") {
	Lexer lexer(
			"func inferred(x):\n"
			"\tif var value := x:\n"
			"\t\treturn value\n"
			"func typed(x):\n"
			"\tif var value: int? = x:\n"
			"\t\treturn value\n"
			"\telse:\n"
			"\t\treturn 0\n");
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	REQUIRE(program.functions.size() == 2);
	auto *inferred = dynamic_cast<IfStmt *>(program.functions[0].body[0].get());
	REQUIRE(inferred != nullptr);
	REQUIRE(inferred->condition == nullptr);
	REQUIRE(inferred->binding != nullptr);
	REQUIRE(inferred->binding->name == "value");
	REQUIRE(inferred->binding->type_hint.empty());
	REQUIRE(inferred->binding->initializer != nullptr);

	auto *typed = dynamic_cast<IfStmt *>(program.functions[1].body[0].get());
	REQUIRE((typed != nullptr && typed->binding != nullptr));
	REQUIRE(typed->binding->type_hint.to_string() == "int?");
	REQUIRE(typed->else_branch.size() == 1);

	bool missing_initializer = false;
	try {
		Lexer bad_lexer("func f():\n\tif var value:\n\t\tpass\n");
		Parser bad_parser(bad_lexer.tokenize());
		bad_parser.parse();
	} catch (const std::exception &) {
		missing_initializer = true;
	}
	REQUIRE((missing_initializer && "if-var must require an initializer"));
}

TEST_CASE("while loop") {
	std::string source = R"(func test():
	var i = 0
	while i < 10:
		i = i + 1
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	REQUIRE(program.functions[0].body.size() == 2);

	auto *while_stmt = dynamic_cast<WhileStmt *>(program.functions[0].body[1].get());
	REQUIRE(while_stmt != nullptr);
	REQUIRE(while_stmt->condition != nullptr);
	REQUIRE(while_stmt->body.size() == 1);
}

TEST_CASE("expressions") {
	std::string source = R"(func test():
	var a = 1 + 2 * 3
	var b = (1 + 2) * 3
	var c = x and y or z
	var d = not x
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	REQUIRE(program.functions[0].body.size() == 4);

	// Check that all are variable declarations with expressions
	for (int i = 0; i < 4; i++) {
		auto *var_decl = dynamic_cast<VarDeclStmt *>(program.functions[0].body[i].get());
		REQUIRE(var_decl != nullptr);
		REQUIRE(var_decl->initializer != nullptr);
	}
}

TEST_CASE("function call") {
	std::string source = R"(func test():
	var result = add(1, 2)
	print("hello")
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	REQUIRE(program.functions[0].body.size() == 2);

	// First statement: var result = add(1, 2)
	auto *var_decl = dynamic_cast<VarDeclStmt *>(program.functions[0].body[0].get());
	REQUIRE(var_decl != nullptr);

	auto *call_expr = dynamic_cast<CallExpr *>(var_decl->initializer.get());
	REQUIRE(call_expr != nullptr);
	REQUIRE(call_expr->function_name == "add");
	REQUIRE(call_expr->arguments.size() == 2);
}

TEST_CASE("method call") {
	std::string source = R"(func test():
	var node = get_node("/root")
	node.set_position(Vector2(0, 0))
	var pos = node.get_position()
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	REQUIRE(program.functions[0].body.size() == 3);

	// Second statement: node.set_position(...)
	auto *expr_stmt = dynamic_cast<ExprStmt *>(program.functions[0].body[1].get());
	REQUIRE(expr_stmt != nullptr);

	auto *member_call = dynamic_cast<MemberCallExpr *>(expr_stmt->expression.get());
	REQUIRE(member_call != nullptr);
	REQUIRE(member_call->member_name == "set_position");
	REQUIRE(member_call->arguments.size() == 1);
}

TEST_CASE("nested control flow") {
	std::string source = R"(func test(x):
	if x > 0:
		while x > 0:
			x = x - 1
			if x == 5:
				break
	else:
		return -1
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	auto *if_stmt = dynamic_cast<IfStmt *>(program.functions[0].body[0].get());
	REQUIRE(if_stmt != nullptr);
	REQUIRE(if_stmt->then_branch.size() == 1);

	auto *while_stmt = dynamic_cast<WhileStmt *>(if_stmt->then_branch[0].get());
	REQUIRE(while_stmt != nullptr);
	REQUIRE(while_stmt->body.size() == 2);
}

TEST_CASE("multiple functions") {
	std::string source = R"(func add(a, b):
	return a + b

func multiply(a, b):
	return a * b

func main():
	var x = add(10, 20)
	var y = multiply(x, 2)
	return y
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	REQUIRE(program.functions.size() == 3);
	REQUIRE(program.functions[0].name == "add");
	REQUIRE(program.functions[1].name == "multiply");
	REQUIRE(program.functions[2].name == "main");
}

TEST_CASE("parameter type hints") {
	std::string source = R"(func add(a: int, b: int):
	return a + b
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	REQUIRE(program.functions.size() == 1);
	REQUIRE(program.functions[0].parameters.size() == 2);
	REQUIRE(program.functions[0].parameters[0].name == "a");
	REQUIRE(program.functions[0].parameters[0].type_hint == "int");
	REQUIRE(program.functions[0].parameters[1].name == "b");
	REQUIRE(program.functions[0].parameters[1].type_hint == "int");
}

TEST_CASE("function return type") {
	std::string source = R"(func add(a: int, b: int) -> int:
	return a + b
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	REQUIRE(program.functions.size() == 1);
	REQUIRE(program.functions[0].return_type == "int");
}

TEST_CASE("variable type hints") {
	std::string source = R"(func test():
	var x: int = 10
	var y: float = 3.14
	var name: String = "hello"
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	REQUIRE(program.functions[0].body.size() == 3);

	auto *var1 = dynamic_cast<VarDeclStmt *>(program.functions[0].body[0].get());
	REQUIRE(var1 != nullptr);
	REQUIRE(var1->name == "x");
	REQUIRE(var1->type_hint == "int");

	auto *var2 = dynamic_cast<VarDeclStmt *>(program.functions[0].body[1].get());
	REQUIRE(var2 != nullptr);
	REQUIRE(var2->name == "y");
	REQUIRE(var2->type_hint == "float");

	auto *var3 = dynamic_cast<VarDeclStmt *>(program.functions[0].body[2].get());
	REQUIRE(var3 != nullptr);
	REQUIRE(var3->name == "name");
	REQUIRE(var3->type_hint == "String");
}

TEST_CASE("mixed type hints") {
	std::string source = R"(func test(a: int, b, c: float) -> void:
	var x: int = 10
	var y = 20
	var z: String
	return x
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	REQUIRE(program.functions.size() == 1);
	REQUIRE(program.functions[0].parameters.size() == 3);

	// Check parameter type hints
	REQUIRE(program.functions[0].parameters[0].type_hint == "int");
	REQUIRE(program.functions[0].parameters[1].type_hint == ""); // No type hint
	REQUIRE(program.functions[0].parameters[2].type_hint == "float");

	// Check return type
	REQUIRE(program.functions[0].return_type == "void");

	// Check variable type hints
	auto *var1 = dynamic_cast<VarDeclStmt *>(program.functions[0].body[0].get());
	REQUIRE(var1->type_hint == "int");

	auto *var2 = dynamic_cast<VarDeclStmt *>(program.functions[0].body[1].get());
	REQUIRE(var2->type_hint == ""); // No type hint

	auto *var3 = dynamic_cast<VarDeclStmt *>(program.functions[0].body[2].get());
	REQUIRE(var3->type_hint == "String");
}

TEST_CASE("extends keyword") {
	std::string source = R"(extends Node

func test():
	return 42
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	// extends should be parsed and ignored
	REQUIRE(program.functions.size() == 1);
	REQUIRE(program.functions[0].name == "test");
}

TEST_CASE("extends with multiple functions") {
	std::string source = R"(extends CharacterBody2D

func _ready():
	pass

func _process(delta: float):
	return delta
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	// extends should be parsed and ignored, functions should be parsed normally
	REQUIRE(program.functions.size() == 2);
	REQUIRE(program.functions[0].name == "_ready");
	REQUIRE(program.functions[1].name == "_process");
	REQUIRE(program.functions[1].parameters.size() == 1);
	REQUIRE(program.functions[1].parameters[0].type_hint == "float");
}

TEST_CASE("default parameter values") {
	Lexer lexer("func f(a, b = 5, c: int = 10):\n\treturn a\n");
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	REQUIRE(program.functions.size() == 1);
	const auto &params = program.functions[0].parameters;
	REQUIRE(params.size() == 3);
	REQUIRE(params[0].default_value == nullptr);
	REQUIRE(params[1].default_value != nullptr);
	REQUIRE(params[2].default_value != nullptr);
	REQUIRE(params[2].type_hint == "int");

	// A parameter without a default may not follow one that has a default
	bool threw = false;
	try {
		Lexer bad_lexer("func f(a = 1, b):\n\treturn a\n");
		Parser bad_parser(bad_lexer.tokenize());
		bad_parser.parse();
	} catch (const std::exception &) {
		threw = true;
	}
	REQUIRE((threw && "Expected an error for a non-default parameter after a default one"));
}

TEST_CASE("ternary and match parsing") {
	Lexer lexer("func f(a):\n\treturn 1 if a else 2\n");
	Parser parser(lexer.tokenize());
	Program program = parser.parse();
	REQUIRE(program.functions.size() == 1);
	auto *ret = dynamic_cast<const ReturnStmt *>(program.functions[0].body[0].get());
	REQUIRE(ret != nullptr);
	REQUIRE(dynamic_cast<const TernaryExpr *>(ret->value.get()) != nullptr);

	Lexer match_lexer("func f(a):\n\tmatch a:\n\t\t1, 2:\n\t\t\treturn 10\n\t\t_:\n\t\t\treturn 20\n");
	Parser match_parser(match_lexer.tokenize());
	Program match_program = match_parser.parse();
	auto *match_stmt = dynamic_cast<const MatchStmt *>(match_program.functions[0].body[0].get());
	REQUIRE(match_stmt != nullptr);
	REQUIRE(match_stmt->branches.size() == 2);
	REQUIRE(match_stmt->branches[0].patterns.size() == 2);
	REQUIRE(match_stmt->branches[0].patterns[0]->kind == MatchPattern::Kind::VALUE);
	REQUIRE(match_stmt->branches[1].patterns.size() == 1);
	REQUIRE(match_stmt->branches[1].patterns[0]->kind == MatchPattern::Kind::WILDCARD);
	REQUIRE(match_stmt->branches[1].is_catch_all());
}
