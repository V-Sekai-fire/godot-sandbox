#include "../compiler_exception.h"
#include "../lexer.h"
#include "witness/doctest.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace gdscript;

// Tokenize source that is expected to be rejected, and hand back the error.
static CompilerException lex_failure(const std::string &source) {
	try {
		Lexer lexer(source);
		lexer.tokenize();
	} catch (const CompilerException &e) {
		return e;
	}
	FAIL("expected this source to fail to tokenize");
	throw std::runtime_error("unreachable");
}

TEST_CASE("basic tokens") {
	Lexer lexer("func main():\n\tpass");
	auto tokens = lexer.tokenize();

	// Find specific tokens we care about (don't rely on exact count)
	bool found_func = false, found_main = false, found_lparen = false;
	bool found_rparen = false, found_colon = false, found_pass = false;

	for (const auto &tok : tokens) {
		if (tok.type == TokenType::FUNC)
			found_func = true;
		if (tok.type == TokenType::IDENTIFIER && tok.lexeme == "main")
			found_main = true;
		if (tok.type == TokenType::LPAREN)
			found_lparen = true;
		if (tok.type == TokenType::RPAREN)
			found_rparen = true;
		if (tok.type == TokenType::COLON)
			found_colon = true;
		if (tok.type == TokenType::PASS)
			found_pass = true;
	}

	REQUIRE(found_func);
	REQUIRE(found_main);
	REQUIRE(found_lparen);
	REQUIRE(found_rparen);
	REQUIRE(found_colon);
	REQUIRE(found_pass);
}

TEST_CASE("contextual uses keyword") {
	Lexer lexer(
			"uses Damageable\n"
			"class Enemy uses Damageable:\n\tpass\n"
			"var uses = 3\n"
			"func f():\n\tuses(1)\n");
	const std::vector<Token> tokens = lexer.tokenize();
	int keyword_count = 0;
	int identifier_count = 0;
	for (const Token &token : tokens) {
		if (token.lexeme != "uses")
			continue;
		if (token.type == TokenType::USES)
			keyword_count++;
		if (token.type == TokenType::IDENTIFIER)
			identifier_count++;
	}
	REQUIRE(keyword_count == 2);
	REQUIRE(identifier_count == 2);

	Lexer old_words("var interface = 1\nvar implements = 2\n");
	for (const Token &token : old_words.tokenize()) {
		if (token.lexeme == "interface" || token.lexeme == "implements")
			REQUIRE(token.type == TokenType::IDENTIFIER);
	}
}

TEST_CASE("indentation") {
	std::string source = R"(func test():
	var x = 1
	if x > 0:
		return x
)";

	Lexer lexer(source);
	auto tokens = lexer.tokenize();

	// Check for INDENT and DEDENT tokens
	int indent_count = 0;
	int dedent_count = 0;
	for (const auto &tok : tokens) {
		if (tok.type == TokenType::INDENT)
			indent_count++;
		if (tok.type == TokenType::DEDENT)
			dedent_count++;
	}

	REQUIRE(indent_count == 2); // After function def, after if
	REQUIRE(dedent_count == 2); // Matching dedents
}

TEST_CASE("operators") {
	Lexer lexer("x = a + b * c - d / e % f");
	auto tokens = lexer.tokenize();

	REQUIRE(tokens[1].type == TokenType::ASSIGN);
	REQUIRE(tokens[3].type == TokenType::PLUS);
	REQUIRE(tokens[5].type == TokenType::MULTIPLY);
	REQUIRE(tokens[7].type == TokenType::MINUS);
	REQUIRE(tokens[9].type == TokenType::DIVIDE);
	REQUIRE(tokens[11].type == TokenType::MODULO);
}

TEST_CASE("comparison operators") {
	Lexer lexer("a == b != c < d <= e > f >= g");
	auto tokens = lexer.tokenize();

	REQUIRE(tokens[1].type == TokenType::EQUAL);
	REQUIRE(tokens[3].type == TokenType::NOT_EQUAL);
	REQUIRE(tokens[5].type == TokenType::LESS);
	REQUIRE(tokens[7].type == TokenType::LESS_EQUAL);
	REQUIRE(tokens[9].type == TokenType::GREATER);
	REQUIRE(tokens[11].type == TokenType::GREATER_EQUAL);
}

TEST_CASE("literals") {
	Lexer lexer(R"(42 3.14 "hello" 'world' true false null)");
	auto tokens = lexer.tokenize();

	REQUIRE(tokens[0].type == TokenType::INTEGER);
	REQUIRE(std::get<int64_t>(tokens[0].value) == 42);

	REQUIRE(tokens[1].type == TokenType::FLOAT);
	REQUIRE(std::get<double>(tokens[1].value) == 3.14);

	REQUIRE(tokens[2].type == TokenType::STRING);
	REQUIRE(std::get<std::string>(tokens[2].value) == "hello");

	REQUIRE(tokens[3].type == TokenType::STRING);
	REQUIRE(std::get<std::string>(tokens[3].value) == "world");

	REQUIRE(tokens[4].type == TokenType::TRUE);
	REQUIRE(tokens[5].type == TokenType::FALSE);
	REQUIRE(tokens[6].type == TokenType::NULL_VAL);
}

TEST_CASE("keywords") {
	Lexer lexer("func var return if else elif while for break continue pass and or not");
	auto tokens = lexer.tokenize();

	REQUIRE(tokens[0].type == TokenType::FUNC);
	REQUIRE(tokens[1].type == TokenType::VAR);
	REQUIRE(tokens[2].type == TokenType::RETURN);
	REQUIRE(tokens[3].type == TokenType::IF);
	REQUIRE(tokens[4].type == TokenType::ELSE);
	REQUIRE(tokens[5].type == TokenType::ELIF);
	REQUIRE(tokens[6].type == TokenType::WHILE);
	REQUIRE(tokens[7].type == TokenType::FOR);
	REQUIRE(tokens[8].type == TokenType::BREAK);
	REQUIRE(tokens[9].type == TokenType::CONTINUE);
	REQUIRE(tokens[10].type == TokenType::PASS);
	REQUIRE(tokens[11].type == TokenType::AND);
	REQUIRE(tokens[12].type == TokenType::OR);
	REQUIRE(tokens[13].type == TokenType::NOT);
}

TEST_CASE("string escapes") {
	Lexer lexer(R"("hello\nworld\t\"test\"")");
	auto tokens = lexer.tokenize();

	REQUIRE(tokens[0].type == TokenType::STRING);
	std::string expected = "hello\nworld\t\"test\"";
	REQUIRE(std::get<std::string>(tokens[0].value) == expected);
}

static std::string lex_string(const std::string &source) {
	Lexer lexer(source);
	auto tokens = lexer.tokenize();
	REQUIRE(tokens[0].type == TokenType::STRING);
	return std::get<std::string>(tokens[0].value);
}

TEST_CASE("control escapes") {
	REQUIRE(lex_string(R"("\a")") == "\a");
	REQUIRE(lex_string(R"("\b")") == "\b");
	REQUIRE(lex_string(R"("\f")") == "\f");
	REQUIRE(lex_string(R"("\v")") == "\v");
	REQUIRE(lex_string(R"("\r")") == "\r");

	// Unknown escapes are rejected.
	REQUIRE(lex_failure(R"("\x41")").error_type() == ErrorType::LEXER_ERROR);
	lex_failure(R"("\0")");
	lex_failure(R"("\q")");

	// r"..." keeps the backslash it is written with, escape or not.
	REQUIRE(lex_string(R"(r"\x41")") == "\\x41");
}

TEST_CASE("line continuation in string") {
	// Backslash-newline joins lines; indentation of the continuation is kept.
	REQUIRE(lex_string("\"a\\\n\tb\"") == "a\tb");
	REQUIRE(lex_string("\"a\\\r\n b\"") == "a b");
	// A lone CR is not a continuation.
	lex_failure("\"a\\\rb\"");
}

TEST_CASE("unicode escapes") {
	REQUIRE(lex_string(R"("é")") == "\xc3\xa9");
	REQUIRE(lex_string(R"("\U0000e9")") == "\xc3\xa9");
	REQUIRE(lex_string(R"("A")") == "A");
	REQUIRE(lex_string(R"("\U01F600")") == "\xf0\x9f\x98\x80");

	// Surrogate pair via two \u escapes.
	REQUIRE(lex_string(R"("😀")") == "\xf0\x9f\x98\x80");

	// Incomplete/misordered surrogates are rejected.
	lex_failure(R"("\ud83d")");
	lex_failure(R"("\ud83dx")");
	lex_failure(R"("\ud83dA")");
	lex_failure(R"("\ude00")");

	// Exactly 4 / 6 hex digits consumed.
	REQUIRE(lex_string(R"("\u00e941")") == "\xc3\xa9"
										   "41");
	REQUIRE(lex_string(R"("\U0000e941")") == "\xc3\xa9"
											 "41");
	lex_failure(R"("\u00e")");
	lex_failure(R"("\U00e9")");

	// Out of range.
	lex_failure(R"("\U110000")");

	REQUIRE(lex_string(R"("aéb")") == "a\xc3\xa9"
									  "b");
}

TEST_CASE("comments") {
	Lexer lexer("# This is a comment\nvar x = 10  # inline comment\n");
	auto tokens = lexer.tokenize();

	// Should skip comments entirely
	REQUIRE((tokens[0].type == TokenType::NEWLINE || tokens[0].type == TokenType::VAR));
}

TEST_CASE("bitwise operators") {
	Lexer lexer("a & b | c ^ ~d << e >> f\n");
	auto tokens = lexer.tokenize();

	REQUIRE(tokens[1].type == TokenType::BIT_AND);
	REQUIRE(tokens[3].type == TokenType::BIT_OR);
	REQUIRE(tokens[5].type == TokenType::BIT_XOR);
	REQUIRE(tokens[6].type == TokenType::BIT_NOT);
	REQUIRE(tokens[8].type == TokenType::SHIFT_LEFT);
	REQUIRE(tokens[10].type == TokenType::SHIFT_RIGHT);
}

TEST_CASE("bitwise compound assignment") {
	Lexer lexer("a &= 1\nb |= 2\nc ^= 3\nd <<= 4\ne >>= 5\n");
	auto tokens = lexer.tokenize();

	REQUIRE(tokens[1].type == TokenType::BIT_AND_ASSIGN);
	REQUIRE(tokens[5].type == TokenType::BIT_OR_ASSIGN);
	REQUIRE(tokens[9].type == TokenType::BIT_XOR_ASSIGN);
	REQUIRE(tokens[13].type == TokenType::SHIFT_LEFT_ASSIGN);
	REQUIRE(tokens[17].type == TokenType::SHIFT_RIGHT_ASSIGN);
}

TEST_CASE("logical operator aliases") {
	// '&&', '||' and '!' are accepted as aliases for 'and', 'or' and 'not'
	Lexer lexer("a && b || !c != d\n");
	auto tokens = lexer.tokenize();

	REQUIRE(tokens[1].type == TokenType::AND);
	REQUIRE(tokens[3].type == TokenType::OR);
	REQUIRE(tokens[4].type == TokenType::NOT);
	REQUIRE(tokens[6].type == TokenType::NOT_EQUAL);
}

TEST_CASE("radix literals") {
	Lexer lexer("0xFF 0Xdead_beef 0b1011 0B11\n");
	auto tokens = lexer.tokenize();

	REQUIRE(tokens[0].type == TokenType::INTEGER);
	REQUIRE(std::get<int64_t>(tokens[0].value) == 255);
	REQUIRE(std::get<int64_t>(tokens[1].value) == 0xdeadbeefLL);
	REQUIRE(std::get<int64_t>(tokens[2].value) == 11);
	REQUIRE(std::get<int64_t>(tokens[3].value) == 3);
}

TEST_CASE("numeric separators and exponents") {
	Lexer lexer("1_000_000 1.5e3 2.5E-3 7e2\n");
	auto tokens = lexer.tokenize();

	REQUIRE(tokens[0].type == TokenType::INTEGER);
	REQUIRE(std::get<int64_t>(tokens[0].value) == 1000000);
	REQUIRE(tokens[1].type == TokenType::FLOAT);
	REQUIRE(std::get<double>(tokens[1].value) == 1500.0);
	REQUIRE(tokens[2].type == TokenType::FLOAT);
	REQUIRE(std::abs(std::get<double>(tokens[2].value) - 0.0025) < 1e-12);
	REQUIRE(tokens[3].type == TokenType::FLOAT);
	REQUIRE(std::get<double>(tokens[3].value) == 700.0);
}

TEST_CASE("match keyword") {
	Lexer lexer("match x:\n");
	auto tokens = lexer.tokenize();

	REQUIRE(tokens[0].type == TokenType::MATCH);
}

TEST_CASE("unicode identifiers") {
	Lexer lexer("var caf\xc3\xa9 = 4\n");
	auto tokens = lexer.tokenize();

	REQUIRE(tokens[0].type == TokenType::VAR);
	REQUIRE(tokens[1].type == TokenType::IDENTIFIER);
	REQUIRE(tokens[1].lexeme == "caf\xc3\xa9");
	REQUIRE(tokens[2].type == TokenType::ASSIGN);

	Lexer leading("\xc3\xa9t\xc3\xa9 = 1\n");
	auto led = leading.tokenize();
	REQUIRE(led[0].type == TokenType::IDENTIFIER);
	REQUIRE(led[0].lexeme == "\xc3\xa9t\xc3\xa9");
}

TEST_CASE("triple quoted strings") {
	Lexer lexer("\"\"\"one\ntwo\"\"\"\n");
	auto tokens = lexer.tokenize();

	REQUIRE(tokens[0].type == TokenType::STRING);
	REQUIRE(std::get<std::string>(tokens[0].value) == "one\ntwo");

	// The lines it spans still count, so an error below it is reported on the
	// line the user is looking at.
	Lexer counting("\"\"\"one\ntwo\"\"\"\nx\n");
	auto counted = counting.tokenize();
	bool found_x = false;
	for (const auto &tok : counted) {
		if (tok.type == TokenType::IDENTIFIER && tok.lexeme == "x") {
			REQUIRE(tok.line == 3);
			found_x = true;
		}
	}
	REQUIRE(found_x);

	// Single quotes open one just the same, and an empty one is still a string.
	Lexer single("'''a'''");
	REQUIRE(std::get<std::string>(single.tokenize()[0].value) == "a");
	Lexer empty("\"\"\"\"\"\"");
	REQUIRE(std::get<std::string>(empty.tokenize()[0].value).empty());
}

TEST_CASE("plain strings span lines") {
	Lexer lexer("\"one\ntwo\"\n");
	auto tokens = lexer.tokenize();
	REQUIRE(tokens[0].type == TokenType::STRING);
	REQUIRE(std::get<std::string>(tokens[0].value) == "one\ntwo");

	Lexer counting("'one\ntwo'\nx\n");
	auto counted = counting.tokenize();
	bool found_x = false;
	for (const auto &tok : counted) {
		if (tok.type == TokenType::IDENTIFIER && tok.lexeme == "x") {
			REQUIRE(tok.line == 3);
			found_x = true;
		}
	}
	REQUIRE(found_x);
}

TEST_CASE("unterminated string stops at the line") {
	const CompilerException error = lex_failure("func f():\n\tvar s = \"oops\n\treturn s\n");
	REQUIRE(error.error_type() == ErrorType::LEXER_ERROR);
	REQUIRE(error.line() == 2);
	REQUIRE(error.column() == 10);

	// An unterminated triple-quoted string is reported where it opened too.
	const CompilerException triple = lex_failure("\n\"\"\"oops\nand more\n");
	REQUIRE(triple.line() == 2);
	REQUIRE(triple.column() == 1);
}

// ---------------------------------------------------------------------------
// Properties, and the falsifiability check that keeps them honest.
// ---------------------------------------------------------------------------

#include "property_support.h"

namespace {

// Identifiers the lexer is supposed to accept: a letter or underscore, then
// letters, digits and underscores.
std::string generate_identifier(witness::RNG &rng, const witness::Level &level) {
	static const std::string HEAD = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_";
	static const std::string TAIL = HEAD + "0123456789";
	const uint32_t extra = rng.uint_range(0, std::max<uint32_t>(1, uint32_t(level.fin_bound) / 16));
	std::string name(1, HEAD[rng.uint_range(0, uint32_t(HEAD.size()) - 1)]);
	for (uint32_t i = 0; i < extra; i++) {
		name.push_back(TAIL[rng.uint_range(0, uint32_t(TAIL.size()) - 1)]);
	}
	return name;
}

// The lexeme the lexer hands back for `var <name> = 1`, or an empty string when
// it produced no identifier at all.
std::string lexed_identifier(const std::string &name) {
	Lexer lexer("var " + name + " = 1\n");
	for (const Token &token : lexer.tokenize()) {
		if (token.type == TokenType::IDENTIFIER) {
			return token.lexeme;
		}
	}
	return {};
}

} // namespace

TEST_CASE("property: an identifier lexes back to itself") {
	PROP_HOLDS(std::string, "the lexeme is the identifier",
			   &generate_identifier,
			   [](const std::string &name) { return lexed_identifier(name) == name; });
}

TEST_CASE("falsifiability: the identifier generator reaches long names") {
	// False on purpose: the generator produces names past three characters, and
	// the ladder has to find one. If it cannot, the property above is vacuous.
	PROP_FALSIFIABLE(std::string, "every identifier is at most three characters",
					 &generate_identifier,
					 [](const std::string &name) { return name.size() <= 3; });
}
