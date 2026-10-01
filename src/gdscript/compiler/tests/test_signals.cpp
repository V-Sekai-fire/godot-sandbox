// Signal declaration publishing and name resolution.
#include "../codegen.h"
#include "../compiler.h"
#include "../compiler_exception.h"
#include "../function_signature.h"
#include "../ir_optimizer.h"
#include "../ir_verifier.h"
#include "../lexer.h"
#include "../parser.h"
#include "../riscv_codegen.h"
#include "witness/doctest.h"
#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

using namespace gdscript;

// -= Helpers =-

static IRProgram compile_to_ir(const std::string &source) {
	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	parser.set_doc_comments(lexer.doc_comments());
	Program program = parser.parse();
	CodeGenerator codegen;
	return codegen.generate(program);
}

static void compile_to_machine_code(const std::string &source) {
	IRProgram ir = compile_to_ir(source);
	IROptimizer optimizer;
	optimizer.optimize(ir);
	ir_verify(ir, "the optimizer");
	RISCVCodeGen backend;
	const std::vector<uint8_t> code = backend.generate(ir);
	REQUIRE(!code.empty());
}

static CompilerException compile_failure(const std::string &source) {
	try {
		compile_to_ir(source);
	} catch (const CompilerException &e) {
		return e;
	}
	FAIL("expected this source to be refused");
	throw std::runtime_error("unreachable");
}

static const FunctionSignature &find_signal(const IRProgram &ir, const std::string &name) {
	for (const auto &sig : ir.signals) {
		if (sig.name == name) {
			return sig;
		}
	}
	throw std::runtime_error("Signal not published: " + name);
}

static const IRFunction &find_function(const IRProgram &ir, const std::string &name) {
	for (const auto &func : ir.functions) {
		if (func.name == name) {
			return func;
		}
	}
	throw std::runtime_error("Function not found: " + name);
}

static std::string function_text(const IRProgram &ir, const std::string &name) {
	std::string text;
	for (const auto &instr : find_function(ir, name).instructions) {
		// With the table, a string operand prints its text rather than the
		// interned id these assertions are written against.
		text += instr.to_string(&ir.strings);
		text += '\n';
	}
	return text;
}

static int count_opcode(const IRFunction &func, IROpcode opcode) {
	int count = 0;
	for (const auto &instr : func.instructions) {
		if (instr.opcode == opcode) {
			count++;
		}
	}
	return count;
}

// -= What is published =-

TEST_CASE("a signal is published with its parameters") {
	const IRProgram ir = compile_to_ir(
			"signal health_changed(new_value: int, cause)\n"
			"func f():\n"
			"\treturn 1\n");

	const FunctionSignature &sig = find_signal(ir, "health_changed");
	REQUIRE(sig.parameters.size() == 2);
	REQUIRE(sig.parameters[0].name == "new_value");
	REQUIRE(sig.parameters[0].type == Variant::INT);
	REQUIRE(sig.parameters[1].name == "cause");
	REQUIRE(sig.parameters[1].type == FunctionParameter::ANY_TYPE);

	REQUIRE(sig.required_arguments == sig.parameters.size());
	REQUIRE(!sig.parameters[0].optional());
	REQUIRE(sig.line == 1);
}

TEST_CASE("a struct parameter is a dictionary") {
	const IRProgram ir = compile_to_ir(
			"struct Hit:\n"
			"\tvar damage = 0\n"
			"signal landed(hit: Hit)\n"
			"func f():\n"
			"\treturn 1\n");

	REQUIRE(find_signal(ir, "landed").parameters[0].type == Variant::DICTIONARY);
}

TEST_CASE("a signal generates no code") {
	const IRProgram ir = compile_to_ir(
			"signal done\n"
			"func f():\n"
			"\treturn 1\n");

	REQUIRE(ir.functions.size() == 1);
	REQUIRE(ir.signatures.size() == 1);
	REQUIRE(ir.signatures[0].name == "f");
	REQUIRE(ir.signals.size() == 1);
}

TEST_CASE("empty parameter list is no parameter list") {
	const IRProgram bare = compile_to_ir("signal done\nfunc f():\n\treturn 1\n");
	const IRProgram parens = compile_to_ir("signal done()\nfunc f():\n\treturn 1\n");

	REQUIRE(find_signal(bare, "done").parameters.empty());
	REQUIRE(find_signal(parens, "done").parameters.empty());
}

TEST_CASE("doc comment is published") {
	const IRProgram ir = compile_to_ir(
			"## Fired when the health changes.\n"
			"## The new value comes with it.\n"
			"signal health_changed(new_value)\n"
			"func f():\n"
			"\treturn 1\n");

	const FunctionSignature &sig = find_signal(ir, "health_changed");
	REQUIRE(sig.description == "Fired when the health changes.\nThe new value comes with it.");
	REQUIRE(sig.line == 3);
}

TEST_CASE("wire format round trip") {
	Compiler compiler;
	const std::vector<uint8_t> elf = compiler.compile(
			"## Doc.\n"
			"signal hit(damage: int, source)\n"
			"signal done\n"
			"func f():\n"
			"\treturn 1\n");
	REQUIRE(!elf.empty());
	REQUIRE(compiler.get_signal_signatures().size() == 2);
	REQUIRE(compiler.get_function_signatures().size() == 1);

	const std::vector<uint8_t> blob = encode_function_signatures(compiler.get_signal_signatures());
	std::vector<FunctionSignature> decoded;
	REQUIRE(decode_function_signatures(blob.data(), blob.size(), decoded));
	REQUIRE(decoded.size() == 2);
	REQUIRE(decoded[0].name == "hit");
	REQUIRE(decoded[0].description == "Doc.");
	REQUIRE(decoded[0].parameters.size() == 2);
	REQUIRE(decoded[0].parameters[0].type == Variant::INT);
	REQUIRE(decoded[0].parameters[1].type == FunctionParameter::ANY_TYPE);
	REQUIRE(decoded[0].required_arguments == 2);
	REQUIRE(decoded[1].name == "done");
	REQUIRE(decoded[1].parameters.empty());
}

// -= What a use of the name lowers to =-

TEST_CASE("a signal name is the property read") {
	const char *uses[] = {
		"\tsig.emit(1)\n",
		"\tsig.connect(c)\n",
		"\treturn sig\n",
		"\treturn sig.is_connected(c)\n",
	};
	for (const char *use : uses) {
		const std::string bare = std::string("signal sig(v)\nfunc test(c):\n") + use;
		std::string qualified = std::string("signal sig(v)\nfunc test(c):\n") + use;
		qualified.replace(qualified.find("sig", qualified.find("func")), 3, "self.sig");

		const std::string bare_text = function_text(compile_to_ir(bare), "test");
		const std::string qualified_text = function_text(compile_to_ir(qualified), "test");
		if (bare_text != qualified_text) {
			std::cerr << "FAIL: " << use << "--- bare ---\n"
					  << bare_text << "--- qualified ---\n"
					  << qualified_text;
			FAIL("a signal name must lower to the property read");
		}
	}

	const IRProgram ir = compile_to_ir("signal sig(v)\nfunc test():\n\treturn sig\n");
	const IRFunction &test = find_function(ir, "test");
	REQUIRE(count_opcode(test, IROpcode::GET_NODE) == 1);
	REQUIRE(count_opcode(test, IROpcode::VGET) == 1);
}

// -= What emit/connect lower to =-

TEST_CASE("a signal method call goes to the owner") {
	struct {
		const char *call;
		const char *owner_method;
	} cases[] = {
		{ "sig.emit(1)", "emit_signal" },
		{ "sig.connect(c)", "connect" },
		{ "sig.disconnect(c)", "disconnect" },
		{ "sig.is_connected(c)", "is_connected" },
		{ "self.sig.emit(1)", "emit_signal" },
	};
	for (const auto &[call, owner_method] : cases) {
		const IRProgram ir = compile_to_ir(
				std::string("signal sig(v)\nfunc test(c):\n\t") + call + "\n");
		const IRFunction &test = find_function(ir, "test");

		if (count_opcode(test, IROpcode::VGET) != 0) {
			std::cerr << "FAIL: " << call << " still reads the signal as a property\n"
					  << function_text(ir, "test");
			FAIL("a signal method call must not build the Signal");
		}
		REQUIRE(count_opcode(test, IROpcode::VCALL) == 1);

		const std::string text = function_text(ir, "test");
		if (text.find(std::string("\"") + owner_method + "\"") == std::string::npos) {
			std::cerr << "FAIL: " << call << " does not call " << owner_method << "\n"
					  << text;
			FAIL("expected the owner method");
		}
		REQUIRE(count_opcode(test, IROpcode::LOAD_STRING) == 1);
		REQUIRE(std::find(ir.string_constants.begin(), ir.string_constants.end(), "sig") != ir.string_constants.end());
	}

	std::cout << "  ✓ emit/connect/disconnect/is_connected go to the owner Object"
			  << std::endl;
}

TEST_CASE("emitting in a loop builds no signals") {
	// A host call, so nothing hoists it out: a Signal per pass would abort the guest
	// at Sandbox::MAX_REFS.
	const IRProgram ir = compile_to_ir(
			"signal sig(v)\n"
			"func test():\n"
			"\tfor i in range(200):\n"
			"\t\tsig.emit(i)\n");

	REQUIRE(count_opcode(find_function(ir, "test"), IROpcode::VGET) == 0);
}

TEST_CASE("emit answers null") {
	// Signal.emit() is void; Object.emit_signal() answers an error code.
	const IRProgram ir = compile_to_ir(
			"signal sig(v)\nfunc test():\n\treturn sig.emit(1)\n");

	REQUIRE(count_opcode(find_function(ir, "test"), IROpcode::LOAD_NIL) == 1);
}

TEST_CASE("a local signal method is not rerouted") {
	const IRProgram ir = compile_to_ir(
			"signal sig(v)\n"
			"func test(c):\n"
			"\tvar sig = c\n"
			"\treturn sig.is_connected(c)\n");

	const IRFunction &test = find_function(ir, "test");
	REQUIRE(count_opcode(test, IROpcode::GET_NODE) == 0);
	REQUIRE(count_opcode(test, IROpcode::VGET) == 0);
}

TEST_CASE("a local shadows a signal") {
	const IRProgram ir = compile_to_ir(
			"signal sig(v)\n"
			"func test():\n"
			"\tvar sig = 5\n"
			"\treturn sig\n");

	const IRFunction &test = find_function(ir, "test");
	REQUIRE(count_opcode(test, IROpcode::GET_NODE) == 0);
	REQUIRE(count_opcode(test, IROpcode::VGET) == 0);
}

TEST_CASE("a signal is visible before its declaration") {
	const IRProgram ir = compile_to_ir(
			"func test():\n"
			"\tlate.emit()\n"
			"signal late\n");

	REQUIRE(count_opcode(find_function(ir, "test"), IROpcode::VCALL) == 1);
	REQUIRE(find_signal(ir, "late").parameters.empty());
}

TEST_CASE("a signal reaches a lambda") {
	const IRProgram ir = compile_to_ir(
			"signal sig(v)\n"
			"func test():\n"
			"\tvar f = func(): sig.emit(1)\n"
			"\treturn f\n");

	const IRFunction &lifted = find_function(ir, "@lambda_0");
	REQUIRE(count_opcode(lifted, IROpcode::GET_NODE) == 1);
	REQUIRE(count_opcode(lifted, IROpcode::VCALL) == 1);
	REQUIRE(lifted.parameters.empty());
}

TEST_CASE("the whole pipeline") {
	compile_to_machine_code(
			"signal hit(damage: int)\n"
			"signal done\n"
			"func fire(n):\n"
			"\thit.emit(n)\n"
			"\tdone.emit()\n"
			"func hook(c):\n"
			"\thit.connect(c)\n"
			"\treturn hit.is_connected(c)\n");
}

// -= What is refused =-

TEST_CASE("a default on a signal parameter is refused") {
	const CompilerException error = compile_failure("signal sig(v = 1)\n");
	REQUIRE(std::string(error.message()).find("default value") != std::string::npos);

	// At the parameter, not at the ')' the check runs after.
	const CompilerException across_lines = compile_failure("signal sig(\n\ta = 1\n)\n");
	REQUIRE(across_lines.line() == 2);
	REQUIRE(across_lines.column() == 3); // the tab, then the name
}

TEST_CASE("a name taken by a signal is refused") {
	const char *collisions[] = {
		"signal sig(v)\nvar sig = 1\n",
		"signal sig(v)\nfunc sig():\n\treturn 1\n",
		"signal sig(v)\nenum { sig }\n",
		"signal sig(v)\nstruct sig:\n\tvar a = 1\n",
		"signal sig(v)\nsignal sig(v)\n",
	};
	for (const char *source : collisions) {
		const CompilerException error = compile_failure(source);
		const std::string message = error.message();
		REQUIRE(message.find("sig") != std::string::npos);
		REQUIRE((message.find("signal") != std::string::npos ||
				 message.find("Signal") != std::string::npos));
	}
}

TEST_CASE("assigning to a signal is refused") {
	const CompilerException error = compile_failure(
			"signal sig(v)\n"
			"func test():\n"
			"\tsig = 1\n");
	REQUIRE(std::string(error.message()).find("sig") != std::string::npos);
}

TEST_CASE("calling a signal is refused") {
	const CompilerException error = compile_failure(
			"signal sig(v)\n"
			"func test():\n"
			"\tsig(1)\n");
	const std::string message = error.message();
	REQUIRE(message.find("sig") != std::string::npos);
	REQUIRE(std::string(error.hint()).find("emit") != std::string::npos);
}
