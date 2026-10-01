// Function signatures, as the host has to see them.
//
// A call arriving from Godot lands on the exported symbol directly. The Sandbox
// ABI hands the guest one pointer per argument and no count, so a caller that
// leaves an argument out leaves a null pointer in its register and the guest
// faults reading a Variant out of it. The host is therefore the only place that
// can reject the call, and it can only do so with the arity in hand -- which is
// what IRProgram::signatures carries out of the compiler.
//
// Defaults are part of the same problem: the callee cannot fill one in, since
// it cannot tell whether it was given the argument. A default that folds to a
// constant is handed to the host to pass; one that does not leaves the
// parameter required for a host call, which is refused rather than guessed at.
#include "../codegen.h"
#include "../compiler.h"
#include "../compiler_exception.h"
#include "../function_signature.h"
#include "../lexer.h"
#include "../parser.h"
#include "witness/doctest.h"
#include <iostream>
#include <string>

using namespace gdscript;

static IRProgram compile_to_ir(const std::string &source) {
	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	// Comments are not tokens; Compiler::compile() passes them separately.
	// Omitting this leaves every doc comment empty.
	parser.set_doc_comments(lexer.doc_comments());
	Program program = parser.parse();
	CodeGenerator codegen;
	return codegen.generate(program);
}

static std::string compile_error(const std::string &source) {
	try {
		compile_to_ir(source);
	} catch (const CompilerException &e) {
		return e.what();
	}
	return "";
}

static const FunctionSignature &find_signature(const IRProgram &ir, const std::string &name) {
	for (const auto &sig : ir.signatures) {
		if (sig.name == name) {
			return sig;
		}
	}
	throw std::runtime_error("Signature not found: " + name);
}

// -= Tests =-

TEST_CASE("one signature per function") {
	const IRProgram ir = compile_to_ir(
			"func a():\n"
			"\treturn 1\n"
			"func b(x):\n"
			"\treturn x\n");

	// Same order as the functions, so the two can be walked together.
	REQUIRE(ir.signatures.size() == ir.functions.size());
	for (size_t i = 0; i < ir.signatures.size(); i++) {
		REQUIRE(ir.signatures[i].name == ir.functions[i].name);
	}
}

TEST_CASE("arity of a plain function") {
	const IRProgram ir = compile_to_ir(
			"func other(f: float):\n"
			"\treturn f\n");

	const FunctionSignature &sig = find_signature(ir, "other");
	REQUIRE(sig.parameters.size() == 1);
	REQUIRE(sig.required_arguments == 1);
	REQUIRE(sig.parameters[0].name == "f");
	REQUIRE(sig.parameters[0].type == Variant::FLOAT);
	REQUIRE(!sig.parameters[0].optional());
}

TEST_CASE("untyped parameter is any variant") {
	const IRProgram ir = compile_to_ir(
			"func f(a, b: int):\n"
			"\treturn b\n");

	const FunctionSignature &sig = find_signature(ir, "f");
	REQUIRE(sig.parameters[0].type == FunctionParameter::ANY_TYPE);
	REQUIRE(sig.parameters[1].type == Variant::INT);
	REQUIRE(sig.required_arguments == 2);
}

TEST_CASE("constant defaults are carried") {
	const IRProgram ir = compile_to_ir(
			"func f(a, b = 5, c = 1.5, d = \"hi\", e = true, g = null):\n"
			"\treturn a\n");

	const FunctionSignature &sig = find_signature(ir, "f");
	REQUIRE(sig.parameters.size() == 6);
	// Only 'a' has to be supplied; the host can produce the rest itself.
	REQUIRE(sig.required_arguments == 1);

	REQUIRE(!sig.parameters[0].optional());
	REQUIRE(sig.parameters[1].optional());
	REQUIRE(sig.parameters[1].default_kind == FunctionParameter::DefaultKind::INT);
	REQUIRE(std::get<int64_t>(sig.parameters[1].default_value) == 5);
	REQUIRE(sig.parameters[2].default_kind == FunctionParameter::DefaultKind::FLOAT);
	REQUIRE(std::get<double>(sig.parameters[2].default_value) == 1.5);
	REQUIRE(sig.parameters[3].default_kind == FunctionParameter::DefaultKind::STRING);
	REQUIRE(std::get<std::string>(sig.parameters[3].default_value) == "hi");
	REQUIRE(sig.parameters[4].default_kind == FunctionParameter::DefaultKind::BOOL);
	REQUIRE(std::get<bool>(sig.parameters[4].default_value) == true);
	REQUIRE(sig.parameters[5].default_kind == FunctionParameter::DefaultKind::NIL);
}

TEST_CASE("negated and const defaults fold") {
	// '-1' is a unary minus over a literal, and a global const is a name; both
	// fold, so neither makes the parameter required.
	const IRProgram ir = compile_to_ir(
			"const LIMIT = 42\n"
			"func f(a = -1, b = LIMIT):\n"
			"\treturn a\n");

	const FunctionSignature &sig = find_signature(ir, "f");
	REQUIRE(sig.required_arguments == 0);
	REQUIRE(std::get<int64_t>(sig.parameters[0].default_value) == -1);
	REQUIRE(std::get<int64_t>(sig.parameters[1].default_value) == 42);
}

TEST_CASE("unfoldable default stays required") {
	// The host cannot build a two-element array, and the callee cannot tell it
	// was left out. Requiring it is refused at the boundary rather than
	// silently passing something else.
	const IRProgram ir = compile_to_ir(
			"func f(a = [1, 2]):\n"
			"\treturn a\n");

	const FunctionSignature &sig = find_signature(ir, "f");
	REQUIRE(sig.parameters.size() == 1);
	REQUIRE(!sig.parameters[0].optional());
	REQUIRE(sig.required_arguments == 1);

	// An empty container does fold: the backend writes it directly.
	const IRProgram empty = compile_to_ir(
			"func f(a = [], b = {}):\n"
			"\treturn a\n");
	const FunctionSignature &esig = find_signature(empty, "f");
	REQUIRE(esig.required_arguments == 0);
	REQUIRE(esig.parameters[0].default_kind == FunctionParameter::DefaultKind::EMPTY_ARRAY);
	REQUIRE(esig.parameters[1].default_kind == FunctionParameter::DefaultKind::EMPTY_DICT);
}

TEST_CASE("struct parameter is a dictionary") {
	// A struct instance is an ordinary Dictionary, so that is what the host is
	// told to pass and what it gets back.
	const IRProgram ir = compile_to_ir(
			"struct BankAccount:\n"
			"\tvar balance = 0\n"
			"\n"
			"func f(acct: BankAccount) -> BankAccount:\n"
			"\treturn acct\n");

	const FunctionSignature &sig = find_signature(ir, "f");
	REQUIRE(sig.parameters[0].type == Variant::DICTIONARY);
	REQUIRE(sig.return_type == Variant::DICTIONARY);
}

TEST_CASE("return type") {
	const IRProgram ir = compile_to_ir(
			"func f() -> int:\n"
			"\treturn 1\n"
			"func g():\n"
			"\treturn 1\n");

	REQUIRE(find_signature(ir, "f").return_type == Variant::INT);
	REQUIRE(find_signature(ir, "g").return_type == FunctionParameter::ANY_TYPE);
}

TEST_CASE("compiler publishes signatures") {
	// The .sgd script language reads these off the Compiler right after a
	// compile, so they have to survive the whole pipeline.
	CompilerOptions options;
	options.output_elf = true;

	Compiler compiler;
	const auto elf = compiler.compile("func other(f: float):\n\treturn f\n", options);
	REQUIRE(!elf.empty());
	REQUIRE(compiler.get_function_signatures().size() == 1);
	REQUIRE(compiler.get_function_signatures()[0].name == "other");
	REQUIRE(compiler.get_function_signatures()[0].required_arguments == 1);

	// A compile that fails before code generation publishes nothing, rather
	// than the previous compile's answer.
	Compiler failing;
	REQUIRE(failing.compile("func f(:\n", options).empty());
	REQUIRE(failing.get_function_signatures().empty());
}

// -= Editor metadata =-
//
// line and description are not call information: they back jump-to-definition
// and hover. The ELF carries neither -- its symbol table maps names to code
// addresses, not to source lines.

TEST_CASE("declaration line") {
	const IRProgram ir = compile_to_ir(
			"\n"
			"func first():\n"
			"\treturn 1\n"
			"\n"
			"func second(a, b):\n"
			"\treturn a\n");

	// 1-based, and the 'func' line, not the body's.
	REQUIRE(find_signature(ir, "first").line == 2);
	REQUIRE(find_signature(ir, "second").line == 5);
}

TEST_CASE("doc comment") {
	const IRProgram ir = compile_to_ir(
			"## Adds two things.\n"
			"##\n"
			"## The second is optional.\n"
			"func documented(a, b = 1):\n"
			"\treturn a\n"
			"\n"
			"# An ordinary comment is not documentation.\n"
			"func plain():\n"
			"\treturn 2\n"
			"\n"
			"## Detached by a blank line, so it documents nothing.\n"
			"\n"
			"func detached():\n"
			"\treturn 3\n");

	// Marker and one following space stripped; block in source order.
	REQUIRE(find_signature(ir, "documented").description ==
			"Adds two things.\n\nThe second is optional.");
	// One '#' is a plain comment.
	REQUIRE(find_signature(ir, "plain").description.empty());
	// A blank line ends the block.
	REQUIRE(find_signature(ir, "detached").description.empty());
}

TEST_CASE("wire format round trip") {
	// Both sides of the sandbox boundary compile function_signature.cpp, so the
	// format need only agree with itself -- but every field has to survive the
	// round trip, not just the call information.
	const IRProgram ir = compile_to_ir(
			"## What it does.\n"
			"func f(a: int, b := 2.5, c = \"x\") -> String:\n"
			"\treturn c\n");

	const std::vector<uint8_t> blob = encode_function_signatures(ir.signatures);
	std::vector<FunctionSignature> decoded;
	REQUIRE(decode_function_signatures(blob.data(), blob.size(), decoded));
	REQUIRE(decoded.size() == 1);

	const FunctionSignature &sig = decoded[0];
	const FunctionSignature &original = find_signature(ir, "f");
	REQUIRE(sig.name == original.name);
	REQUIRE(sig.line == original.line);
	REQUIRE(sig.description == "What it does.");
	REQUIRE(sig.return_type == Variant::STRING);
	REQUIRE(sig.required_arguments == 1);
	REQUIRE(sig.parameters.size() == 3);
	REQUIRE(sig.parameters[0].name == "a");
	REQUIRE(sig.parameters[0].type == Variant::INT);
	REQUIRE(sig.parameters[1].default_kind == FunctionParameter::DefaultKind::FLOAT);
	REQUIRE(std::get<double>(sig.parameters[1].default_value) == 2.5);
	REQUIRE(sig.parameters[2].default_kind == FunctionParameter::DefaultKind::STRING);
	REQUIRE(std::get<std::string>(sig.parameters[2].default_value) == "x");

	// The blob comes from a guest program: a truncated one must fail the decode,
	// not be read past its end.
	std::vector<FunctionSignature> truncated;
	REQUIRE(!decode_function_signatures(blob.data(), blob.size() / 2, truncated));
	REQUIRE(truncated.empty());
}

TEST_CASE("rpc wire format round trip") {
	const std::vector<RPCConfig> original = {
		{ "default_rpc", 2, 2, false, 0 },
		{ "custom_rpc", 1, 0, true, 9 },
	};
	const std::vector<uint8_t> blob = encode_rpc_configs(original);
	std::vector<RPCConfig> decoded;
	REQUIRE(decode_rpc_configs(blob.data(), blob.size(), decoded));
	REQUIRE(decoded.size() == original.size());
	REQUIRE((decoded[0].name == "default_rpc" && decoded[0].rpc_mode == 2));
	REQUIRE((decoded[1].name == "custom_rpc" && decoded[1].rpc_mode == 1));
	REQUIRE((decoded[1].transfer_mode == 0 && decoded[1].call_local));
	REQUIRE(decoded[1].channel == 9);

	std::vector<RPCConfig> truncated;
	REQUIRE(!decode_rpc_configs(blob.data(), blob.size() - 1, truncated));
	REQUIRE(truncated.empty());
}

TEST_CASE("static is published") {
	const IRProgram ir = compile_to_ir(
			"static func shared(x):\n"
			"\treturn x\n"
			"@rpc\n"
			"func mine():\n"
			"\treturn 1\n"
			"@warning_ignore(\"unused\")\n"
			"static func annotated():\n"
			"\treturn 2\n");

	REQUIRE(find_signature(ir, "shared").is_static);
	REQUIRE(find_signature(ir, "annotated").is_static);
	REQUIRE(!find_signature(ir, "mine").is_static);

	const std::vector<uint8_t> blob = encode_function_signatures(ir.signatures);
	std::vector<FunctionSignature> decoded;
	REQUIRE(decode_function_signatures(blob.data(), blob.size(), decoded));
	bool seen = false;
	for (const FunctionSignature &sig : decoded) {
		if (sig.name == "shared") {
			seen = sig.is_static;
		}
		if (sig.name == "mine") {
			REQUIRE(!sig.is_static);
		}
	}
	REQUIRE(seen);

	std::cout << "  \u2713 a file-scope 'static func' is published as static" << std::endl;
}

TEST_CASE("a static function has no instance") {
	REQUIRE(compile_error("var v = 1\nstatic func f():\n\treturn v\n")
					.find("one per instance") != std::string::npos);
	REQUIRE(compile_error("var v = 1\nstatic func f():\n\tv = 2\n")
					.find("one per instance") != std::string::npos);
	REQUIRE(compile_error("var v = 1\nstatic func f():\n\tv += 2\n")
					.find("one per instance") != std::string::npos);
	REQUIRE(compile_error("static func f():\n\treturn self\n")
					.find("runs without one") != std::string::npos);
	REQUIRE(compile_error("var v = 1\nstatic func f():\n\tvar g = func(): return v\n\treturn g\n")
					.find("one per instance") != std::string::npos);
	REQUIRE(compile_error("extends Node\nstatic func f():\n\treturn position\n")
					.find("Undefined variable") != std::string::npos);

	REQUIRE(compile_error("static var v = 1\nconst K = 2\nstatic func f():\n\tv += K\n\treturn v\n").empty());
	REQUIRE(compile_error("var v = 1\nfunc f():\n\treturn v\n").empty());

	std::cout << "  \u2713 a 'static func' reaches neither a member nor self" << std::endl;
}
