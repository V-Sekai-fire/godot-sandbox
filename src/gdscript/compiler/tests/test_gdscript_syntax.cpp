// GDScript that Godot accepts and the compiler has to as well: nested classes,
// expression statements, and compound assignment into call results.
#include "../codegen.h"
#include "../compiler.h"
#include "../compiler_exception.h"
#include "../ir_interpreter.h"
#include "../ir_optimizer.h"
#include "../ir_verifier.h"
#include "../lexer.h"
#include "../parser.h"
#include "../source_model.h"
#include "witness/doctest.h"
#include <string>

using namespace gdscript;

static Program parse(const std::string &source) {
	Lexer lexer(source);
	lexer.set_extensions(false);
	Parser parser(lexer.tokenize());
	parser.set_extensions(false);
	return parser.parse();
}

static IRProgram compile_to_ir(const std::string &source, bool extensions = false) {
	Program program = parse(source);
	CodeGenerator codegen;
	codegen.set_extensions(extensions);
	IRProgram ir = codegen.generate(program);
	ir_verify(ir);
	return ir;
}

static int count_calls(const IRProgram &ir, const std::string &function, const std::string &callee) {
	int calls = 0;
	for (const IRFunction &func : ir.functions) {
		if (func.name != function) {
			continue;
		}
		for (const IRInstruction &instr : func.instructions) {
			if (instr.opcode == IROpcode::CALL && ir.strings[instr.operands[0].string_id] == callee) {
				calls++;
			}
		}
	}
	return calls;
}

TEST_CASE("a class nested in a class is hoisted to file scope") {
	const Program program = parse(
			"class Outer:\n"
			"\tclass Inner:\n"
			"\t\tvar n = 1\n"
			"\tvar inner = Inner.new()\n");
	bool outer = false;
	bool inner = false;
	for (const StructDecl &decl : program.structs) {
		outer = outer || decl.name == "Outer";
		inner = inner || decl.name == "Inner";
	}
	CHECK(outer);
	CHECK(inner);
}

TEST_CASE("an expression statement parses whole and changes nothing") {
	IRProgram ir = compile_to_ir(
			"func test():\n"
			"\tvar c = 3\n"
			"\tc ++ 1\n"
			"\tc + 4\n"
			"\treturn c\n");
	IRInterpreter interp(ir);
	CHECK(std::get<int64_t>(interp.call("test")) == 3);
}

TEST_CASE("compound assignment into a call result evaluates the call once") {
	const IRProgram ir = compile_to_ir(
			"func g(a):\n"
			"\treturn a\n"
			"func test(a):\n"
			"\tg(a)[0] += 5\n"
			"\tg(a).size += 1\n"
			"\treturn a\n");
	CHECK(count_calls(ir, "test", "g") == 2);
}

TEST_CASE("compound assignment through a negative index") {
	CHECK_NOTHROW(compile_to_ir(
			"func test(a):\n"
			"\ta[-1] -= 1\n"
			"\treturn a\n"));
}

TEST_CASE("a call on its own is still not an assignment target") {
	CHECK_THROWS_AS(compile_to_ir(
							"func g():\n"
							"\treturn 1\n"
							"func test():\n"
							"\tg() += 1\n"),
					CompilerException);
}

static const Expr *returned(const Program &program) {
	const ReturnStmt *ret = dynamic_cast<const ReturnStmt *>(program.functions.front().body.back().get());
	REQUIRE(ret != nullptr);
	return ret->value.get();
}

TEST_CASE("operators after a cast take the cast as their left operand") {
	const Program compared = parse("func f(n):\n\treturn n as Node3D != null\n");
	const BinaryExpr *ne = dynamic_cast<const BinaryExpr *>(returned(compared));
	REQUIRE(ne != nullptr);
	CHECK(dynamic_cast<const CastExpr *>(ne->left.get()) != nullptr);

	const Program joined = parse("func f(n, c):\n\treturn n as Node3D == null and c\n");
	const BinaryExpr *both = dynamic_cast<const BinaryExpr *>(returned(joined));
	REQUIRE(both != nullptr);
	const BinaryExpr *eq = dynamic_cast<const BinaryExpr *>(both->left.get());
	REQUIRE(eq != nullptr);
	CHECK(dynamic_cast<const CastExpr *>(eq->left.get()) != nullptr);

	CHECK_NOTHROW(compile_to_ir("func f(n, c):\n\treturn n as Node3D if c else null\n"));
	CHECK_NOTHROW(compile_to_ir("func f(n):\n\treturn n as int - 1\n"));

	// `not in` after a cast, beside the prefix `not` a tighter operator may take.
	const Program excluded = parse("func f(x):\n\treturn x as int not in [1, 2]\n");
	const UnaryExpr *negated = dynamic_cast<const UnaryExpr *>(returned(excluded));
	REQUIRE(negated != nullptr);
	const BinaryExpr *in = dynamic_cast<const BinaryExpr *>(negated->operand.get());
	REQUIRE(in != nullptr);
	CHECK(dynamic_cast<const CastExpr *>(in->left.get()) != nullptr);
	CHECK_NOTHROW(compile_to_ir("func f(x):\n\treturn x as int not in [1, 2]\n"));
}

TEST_CASE("a class getter on an untyped field, below comment lines") {
	CHECK_NOTHROW(compile_to_ir(
			"class A:\n"
			"\tvar keys = {}\n"
			"\tvar coefficients:\n"
			"\t\tget:\n"
			"\t\t\treturn keys.get(\"c\", [])\n"
			"\tvar flag: bool:\n"
			"\t\t# a note\n"
			"\t\tget:\n"
			"\t\t\treturn true\n"));
}

TEST_CASE("built-in type enums are integers") {
	IRProgram ir = compile_to_ir("func test():\n\treturn Vector3.AXIS_Z + Vector2i.AXIS_Y * 10\n");
	IRInterpreter interp(ir);
	CHECK(std::get<int64_t>(interp.call("test")) == 12);
}

TEST_CASE("a class or engine typed local starts null and takes an instance") {
	CHECK_NOTHROW(compile_to_ir(
			"class Def:\n"
			"\tvar n = 1\n"
			"func f(c):\n"
			"\tvar d: Def = null\n"
			"\tif c:\n"
			"\t\td = Def.new()\n"
			"\tvar s: Node3D = null\n"
			"\ts = Node3D.new()\n"
			"\treturn [d, s]\n"));
}

TEST_CASE("an Array literal fills a packed array slot") {
	const IRProgram ir = compile_to_ir(
			"var lines: PackedStringArray = []\n"
			"func f():\n"
			"\tvar local: PackedStringArray = [\"a\"]\n"
			"\treturn [lines, local]\n");
	int conversions = 0;
	for (const IRFunction &func : ir.functions) {
		for (const IRInstruction &instr : func.instructions) {
			conversions += instr.opcode == IROpcode::MAKE_PACKED_STRING_ARRAY;
		}
	}
	for (const IRInstruction &instr : ir.member_init.instructions) {
		conversions += instr.opcode == IROpcode::MAKE_PACKED_STRING_ARRAY;
	}
	CHECK(conversions == 2);
	// Assigned later, not only declared with it: upstream v0.60 converts the initializer only.
	CHECK_NOTHROW(compile_to_ir("func f():\n\tvar p: PackedInt32Array\n\tp = [4, 5]\n\treturn p\n"));
}

TEST_CASE("a bare string at file level is a comment") {
	CHECK_NOTHROW(parse("func f():\n\tpass\n'''\nnot code: (\n'''\n"));
}

static int count_code(const SourceModel &model, const std::string &code) {
	int n = 0;
	for (const SourceDiagnostic &d : model.diagnostics) {
		n += d.code == code;
	}
	return n;
}

TEST_CASE("a bare call in a class names the class's method before the file's function") {
	const SourceModel model = analyze_source(
			"func run(a, b):\n"
			"\treturn a\n"
			"class Handler:\n"
			"\tfunc run(a, b, c, d = 0):\n"
			"\t\treturn a\n"
			"\tfunc go():\n"
			"\t\treturn run(1, 2, 3)\n",
			"arity.gd", ANALYZE_DIAGNOSTICS | ANALYZE_DECLARATIONS);
	CHECK(count_code(model, "TOO_MANY_ARGUMENTS") == 0);

	const SourceModel outside = analyze_source(
			"func run(a, b):\n"
			"\treturn a\n"
			"func go():\n"
			"\treturn run(1, 2, 3)\n",
			"arity.gd", ANALYZE_DIAGNOSTICS | ANALYZE_DECLARATIONS);
	CHECK(count_code(outside, "TOO_MANY_ARGUMENTS") == 1);
}

TEST_CASE("an expression statement is reported once") {
	const SourceModel model = analyze_source("func f():\n\tvar c = 1\n\tc ++ 1\n\treturn c\n", "once.gd",
											 ANALYZE_DIAGNOSTICS | ANALYZE_DECLARATIONS);
	CHECK(count_code(model, "STANDALONE_EXPRESSION") == 1);
}

TEST_CASE("an untyped var is a Variant in GDScript and fixed in SafeGDScript") {
	const std::string changes = "func f():\n\tvar x = 0\n\tx = 1.5\n\tx = \"s\"\n\treturn x\n";
	CHECK_NOTHROW(compile_to_ir(changes));
	CHECK_THROWS(compile_to_ir(changes, true));

	const std::string inferred = "func f():\n\tvar x := 0\n\tx = \"s\"\n\treturn x\n";
	CHECK_THROWS(compile_to_ir(inferred));
}

TEST_CASE("a float stored in an int variable is truncated in GDScript") {
	const std::string narrowing = "func f(v: float) -> int:\n\tvar n: int = 0\n\tn = v\n\treturn n\n";
	CHECK_NOTHROW(compile_to_ir(narrowing));
	CHECK_THROWS(compile_to_ir(narrowing, true));
}

TEST_CASE("a class constant that does not fold is built where it is read in GDScript") {
	const std::string source =
			"class Importer:\n"
			"\tconst MODES: Dictionary = {0: 30, 1: 120}\n"
			"\tconst OFFSET: Vector3 = Vector3(300, 100, 0)\n"
			"\tfunc fps(mode):\n"
			"\t\treturn MODES[mode]\n"
			"func offset():\n"
			"\treturn Importer.OFFSET\n";
	const IRProgram ir = compile_to_ir(source);
	int dictionaries = 0;
	for (const IRFunction &func : ir.functions) {
		if (func.name == "@Importer.fps") {
			for (const IRInstruction &instr : func.instructions) {
				dictionaries += instr.opcode == IROpcode::MAKE_DICTIONARY;
			}
		}
	}
	CHECK(dictionaries == 1);
	CHECK_THROWS(compile_to_ir(source, true));
}

TEST_CASE("continue after a nested typed-array loop reaches the outer loop") {
	Compiler compiler;
	CompilerOptions options;
	const std::vector<uint8_t> elf = compiler.compile(
			"func f(outer: Array):\n"
			"\tvar n = 0\n"
			"\tfor a in outer:\n"
			"\t\tvar inner: Array = a\n"
			"\t\tfor b in inner:\n"
			"\t\t\tn += 1\n"
			"\t\tif n > 3:\n"
			"\t\t\tcontinue\n"
			"\t\tn += 10\n"
			"\treturn n\n",
			options);
	CAPTURE(compiler.get_error());
	CHECK_FALSE(elf.empty());
}

// Three shapes the rx census (contract-zone-backend#92) found in scripts Godot accepts.

TEST_CASE("a keyword after a dot is a member name") {
	const Program program = parse(
			"func ok(s: String) -> bool:\n"
			"\treturn s.match(\"a*\")\n");
	const MemberCallExpr *call = dynamic_cast<const MemberCallExpr *>(returned(program));
	REQUIRE(call != nullptr);
	CHECK(call->member_name == "match");
	CHECK(call->is_method_call);
	CHECK_NOTHROW(compile_to_ir(
			"extends Node\n"
			"func ok(s: String) -> bool:\n"
			"\treturn s.match(\"a*\")\n"));
}

TEST_CASE("control: a keyword still cannot start an expression") {
	CHECK_THROWS(parse(
			"extends Node\n"
			"func bad():\n"
			"\tvar x = match\n"));
}

TEST_CASE("pass is a class-body statement") {
	const Program program = parse(
			"extends Node\n"
			"\n"
			"pass\n"
			"\n"
			"func f() -> int:\n"
			"\treturn 1\n");
	CHECK(program.functions.size() == 1);
}

TEST_CASE("a lambda in a member initializer is lifted") {
	const IRProgram ir = compile_to_ir(
			"extends Node\n"
			"var x = (func(): return 1).call()\n");
	bool lifted = false;
	for (const IRFunction &func : ir.functions) {
		lifted = lifted || func.name == "@lambda_0";
	}
	CHECK(lifted);
}

TEST_CASE("a lambda in a function body is lifted under its own label, after the initializer's") {
	const IRProgram ir = compile_to_ir(
			"extends Node\n"
			"var x = (func(): return 1).call()\n"
			"func f() -> int:\n"
			"\treturn (func(): return 2).call()\n");
	int lifted = 0;
	for (const IRFunction &func : ir.functions) {
		lifted += func.name.rfind("@lambda_", 0) == 0 ? 1 : 0;
	}
	CHECK(lifted == 2);
}

TEST_CASE("a method of a nested class by bare name is a Callable bound to the instance") {
	const IRProgram ir = compile_to_ir(
			"class Inner:\n"
			"\tvar n: int = 0\n"
			"\tfunc go(sig: Signal):\n"
			"\t\tsig.connect(init)\n"
			"\tfunc init():\n"
			"\t\tn += 1\n");
	bool bound = false;
	for (const IRFunction &func : ir.functions) {
		for (const IRInstruction &instr : func.instructions) {
			if (instr.opcode == IROpcode::MAKE_CALLABLE && ir.strings[instr.operands[1].string_id] == "@Inner.init") {
				bound = true;
			}
		}
	}
	CHECK(bound);
}

TEST_CASE("control: a name that is neither a variable nor a method is still undefined") {
	CHECK_THROWS_AS(compile_to_ir(
							"class Inner:\n"
							"\tfunc go(sig: Signal):\n"
							"\t\tsig.connect(nothing)\n"),
					CompilerException);
}

TEST_CASE("a member read before its declaration holds its type's default, as GDScript runs initializers in order") {
	const IRProgram ir = compile_to_ir(
			"var a: float = b\n"
			"var b: float = 1.5\n"
			"var c: int = d\n"
			"var d: int = 7\n"
			"func test_a() -> float:\n"
			"\treturn a\n"
			"func test_b() -> float:\n"
			"\treturn b\n"
			"func test_c() -> int:\n"
			"\treturn c\n");
	IRInterpreter interpreter(ir);
	CHECK(std::get<double>(interpreter.call("test_a", {})) == 0.0);
	CHECK(std::get<double>(interpreter.call("test_b", {})) == 1.5);
	CHECK(std::get<int64_t>(interpreter.call("test_c", {})) == 0);
}
