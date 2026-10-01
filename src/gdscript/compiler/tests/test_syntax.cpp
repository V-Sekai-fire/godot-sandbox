// Syntax the compiler previously rejected: node-path sugar, raw/typed string
// literals, signal, static var, attributes, `for i in <int>`, REQUIRE(), null.
// A parse failure takes the whole file down.
#include "../codegen.h"
#include "../compiler_exception.h"
#include "../ir_optimizer.h"
#include "../ir_verifier.h"
#include "../lexer.h"
#include "../parser.h"
#include "../riscv_codegen.h"
#include "../syscall_numbers.h"
#include "witness/doctest.h"
#include <iostream>
#include <string>

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

static bool refuses(const std::string &source) {
	try {
		compile_to_ir(source);
	} catch (const CompilerException &) {
		return true;
	}
	return false;
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

static const IRInstruction &only(const IRFunction &func, IROpcode opcode) {
	const IRInstruction *found = nullptr;
	for (const auto &instr : func.instructions) {
		if (instr.opcode == opcode) {
			REQUIRE((found == nullptr && "expected exactly one"));
			found = &instr;
		}
	}
	REQUIRE(found != nullptr);
	return *found;
}

static IRProgram compile_with_project(const std::string &source,
									  std::vector<std::string> autoloads,
									  std::vector<std::pair<std::string, std::string>> classes) {
	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();
	CodeGenerator codegen;
	codegen.set_autoloads(std::move(autoloads));
	codegen.set_global_script_classes(std::move(classes));
	return codegen.generate(program);
}

static bool machine_code_builds(const std::string &source) {
	IRProgram ir = compile_to_ir(source, true);
	RISCVCodeGen backend;
	return !backend.generate(ir).empty();
}

// Single string constant from the program.
static std::string only_string(const IRProgram &ir) {
	REQUIRE(ir.string_constants.size() == 1);
	return ir.string_constants[0];
}

// -= $Node and %Unique =-

TEST_CASE("node path sugar") {
	// All four spellings -> ECALL_GET_NODE.
	static const struct {
		const char *source;
		const char *path;
	} cases[] = {
		{ "$Sprite2D", "Sprite2D" },
		{ "$Path/To/Node", "Path/To/Node" },
		{ "$\"Path/To/Node\"", "Path/To/Node" },
		{ "%Unique", "%Unique" },
		{ "%Unique/Child", "%Unique/Child" },
		{ "$Parent/%Unique", "Parent/%Unique" },
	};

	for (const auto &one : cases) {
		const std::string source = std::string("func test():\n\treturn ") + one.source + "\n";
		const IRProgram ir = compile_to_ir(source);
		const IRFunction &test = find_function(ir, "test");
		REQUIRE(count_opcode(test, IROpcode::GET_NODE) == 1);
		REQUIRE(count_opcode(test, IROpcode::CALL) == 0);
		// Path embedded in instruction; ECALL_GET_NODE reads raw characters.
		for (const auto &instr : test.instructions) {
			if (instr.opcode == IROpcode::GET_NODE) {
				const std::string &got = ir.strings[instr.operands[1].string_id];
				if (got != one.path) {
					std::cerr << "  " << one.source << " -> \"" << got << "\"" << std::endl;
					REQUIRE(false);
				}
			}
		}
		REQUIRE(machine_code_builds(source));
	}

	// Run-time path -> VCALL on node (syscall takes raw characters only).
	const IRProgram computed = compile_to_ir("func test(p):\n\treturn get_node(p)\n");
	const IRFunction &computed_fn = find_function(computed, "test");
	REQUIRE(count_opcode(computed_fn, IROpcode::VCALL) == 1);
	REQUIRE(count_opcode(computed_fn, IROpcode::GET_NODE) == 1);

	// `%` is modulo in expression context.
	const IRProgram modulo = compile_to_ir("func test(a, b):\n\treturn a % b\n");
	REQUIRE(count_opcode(find_function(modulo, "test"), IROpcode::MOD) == 1);
	const IRProgram assign = compile_to_ir("func test(a):\n\ta %= 3\n\treturn a\n");
	REQUIRE(count_opcode(find_function(assign, "test"), IROpcode::MOD) == 1);

	// Missing path segment -> error.
	REQUIRE(refuses("func test():\n\treturn $\n"));
	REQUIRE(refuses("func test():\n\treturn $A/\n"));
}

// -= String literal spellings =-

TEST_CASE("string literals") {
	// Raw string: backslash is literal but still escapes the quote.
	const IRProgram raw = compile_to_ir("func test():\n\treturn r\"a\\nb\"\n");
	REQUIRE(only_string(raw) == "a\\nb");
	const IRProgram raw_quote = compile_to_ir("func test():\n\treturn r\"a\\\"b\"\n");
	REQUIRE(only_string(raw_quote) == "a\\\"b");
	// Normal string escapes.
	const IRProgram escaped = compile_to_ir("func test():\n\treturn \"a\\nb\"\n");
	REQUIRE(only_string(escaped) == "a\nb");
	// Bare `r` is an identifier.
	REQUIRE(refuses("func test():\n\treturn r\n"));
	const IRProgram identifier = compile_to_ir("func test():\n\tvar r = 1\n\treturn r\n");
	REQUIRE(count_opcode(find_function(identifier, "test"), IROpcode::LOAD_STRING) == 0);

	// &"x" -> STRING_NAME, ^"a/b" -> NODE_PATH via LOAD_STRING_AS.
	const IRProgram string_name = compile_to_ir("func test():\n\treturn &\"speed\"\n");
	const IRFunction &string_name_fn = find_function(string_name, "test");
	REQUIRE(count_opcode(string_name_fn, IROpcode::LOAD_STRING_AS) == 1);
	REQUIRE(count_opcode(string_name_fn, IROpcode::LOAD_STRING) == 0);
	for (const auto &instr : string_name_fn.instructions) {
		if (instr.opcode == IROpcode::LOAD_STRING_AS) {
			REQUIRE(instr.operands[2].immediate() == Variant::STRING_NAME);
		}
	}
	const IRProgram node_path = compile_to_ir("func test():\n\treturn ^\"a/b\"\n");
	for (const auto &instr : find_function(node_path, "test").instructions) {
		if (instr.opcode == IROpcode::LOAD_STRING_AS) {
			REQUIRE(instr.operands[2].immediate() == Variant::NODE_PATH);
		}
	}
	REQUIRE(machine_code_builds("func test():\n\treturn &\"speed\"\n"));
	REQUIRE(machine_code_builds("func test():\n\treturn ^\"a/b\"\n"));

	// & and ^ remain bitwise operators without a string literal.
	const IRProgram bitwise = compile_to_ir("func test(a, b):\n\treturn (a & b) ^ 1\n");
	REQUIRE(count_opcode(find_function(bitwise, "test"), IROpcode::BIT_AND) == 1);
	REQUIRE(count_opcode(find_function(bitwise, "test"), IROpcode::BIT_XOR) == 1);
}

TEST_CASE("tool annotation") {
	const IRProgram tool = compile_to_ir("@tool\nfunc test():\n\treturn 1\n");
	REQUIRE(tool.is_tool);
	const IRProgram late = compile_to_ir("func test():\n\treturn 1\n@tool\n");
	REQUIRE(late.is_tool);
	const IRProgram plain = compile_to_ir("func test():\n\treturn 1\n");
	REQUIRE(!plain.is_tool);

	std::cout << "  \u2713 @tool reaches the host and nothing else" << std::endl;
}

TEST_CASE("engine class new") {
	const IRProgram ir = compile_to_ir("func test():\n\treturn Timer.new()\n");
	const IRFunction &test = find_function(ir, "test");
	int creates = 0;
	for (const auto &instr : test.instructions) {
		if (instr.opcode != IROpcode::CALL_SYSCALL) {
			continue;
		}
		if (instr.operands[1].immediate() == ECALL_NODE_CREATE) {
			creates++;
		}
	}
	REQUIRE(creates == 1);

	const IRProgram shadowed = compile_to_ir(
			"struct Timer:\n\tvar a = 1\n"
			"func test():\n\treturn Timer.new()\n");
	const IRFunction &shadow_test = find_function(shadowed, "test");
	for (const auto &instr : shadow_test.instructions) {
		REQUIRE((instr.opcode != IROpcode::CALL_SYSCALL ||
				 instr.operands[1].immediate() != ECALL_NODE_CREATE));
	}

	REQUIRE(refuses("func test():\n\treturn Timer.new(1)\n"));

	std::cout << "  \u2713 Type.new() reaches ClassDB through the class allowlist" << std::endl;
}

TEST_CASE("leading dot float") {
	REQUIRE(machine_code_builds("func test():\n\tvar a := .5\n\treturn a\n"));
	REQUIRE(machine_code_builds("func test():\n\treturn .5e2\n"));
	REQUIRE(machine_code_builds("func test():\n\tfor i in range(1, 5):\n\t\tprint(i)\n"));

	std::cout << "  \u2713 a float literal may start at the point" << std::endl;
}

TEST_CASE("typed container cast") {
	REQUIRE(machine_code_builds("func test(a):\n\tvar b = a as Array[int]\n\treturn b\n"));
	REQUIRE(machine_code_builds("func test(a):\n\tfor x in a as Array[int]:\n\t\tprint(x)\n"));

	std::cout << "  \u2713 a cast carries element types the same way a declaration does"
			  << std::endl;
}

TEST_CASE("nested class extends on its own line") {
	REQUIRE(machine_code_builds(
			"class Foo:\n\textends Node\n\n\tvar x := 1\n"
			"func test():\n\treturn 1\n"));
	REQUIRE(machine_code_builds("class Foo extends Node:\n\tvar x := 1\nfunc test():\n\treturn 1\n"));
	REQUIRE(refuses("class Foo extends Node:\n\textends Node2D\n\tvar x := 1\n"
					"func test():\n\treturn 1\n"));

	std::cout << "  \u2713 a nested class takes its base from either line" << std::endl;
}

TEST_CASE("null initializer leaves the slot untyped") {
	REQUIRE(machine_code_builds(
			"func test():\n\tvar col = null\n\tcol = Timer.new()\n\treturn col\n"));
	REQUIRE(machine_code_builds("func test():\n\tvar x = null\n\tx = 5\n\treturn x\n"));
	REQUIRE(refuses("func test():\n\tvar x = 5\n\tx = \"hi\"\n\treturn x\n"));

	std::cout << "  \u2713 a null initializer asks for a slot that holds anything" << std::endl;
}

TEST_CASE("array literal becomes a packed array") {
	const IRProgram ir = compile_to_ir(
			"func test():\n\tvar p: PackedVector2Array = []\n\treturn p\n");
	REQUIRE(count_opcode(find_function(ir, "test"), IROpcode::MAKE_PACKED_VECTOR2_ARRAY) == 1);
	REQUIRE(machine_code_builds(
			"func test() -> PackedVector3Array:\n\treturn [Vector3(1, 2, 3)]\n"));
	REQUIRE(refuses("func test():\n\tvar p: PackedVector2Array = 5\n\treturn p\n"));

	std::cout << "  \u2713 an Array literal converts to the packed type declared" << std::endl;
}

TEST_CASE("autoload resolves to a named object") {
	const IRProgram ir = compile_with_project(
			"func test():\n\treturn Global.SPEED\n", { "Global" }, {});
	const IRFunction &test = find_function(ir, "test");
	int gets = 0;
	for (const auto &instr : test.instructions) {
		if (instr.opcode == IROpcode::CALL_SYSCALL &&
			instr.operands[1].immediate() == ECALL_GET_OBJ) {
			gets++;
		}
	}
	REQUIRE(gets == 1);

	const IRProgram unregistered = compile_to_ir("func test():\n\treturn Global.SPEED\n");
	const IRFunction &plain = find_function(unregistered, "test");
	const IRInstruction &lookup = only(plain, IROpcode::VCALL);
	REQUIRE(unregistered.strings[lookup.operands[2].string_id] == "class_get_integer_constant");

	std::cout << "  \u2713 an autoload resolves to the node the project registered" << std::endl;
}

TEST_CASE("script class new takes arguments") {
	const IRProgram ir = compile_with_project(
			"func test(s):\n\treturn Runner.new(s, true)\n", {},
			{ { "Runner", "res://runner.gd" } });
	const IRFunction &test = find_function(ir, "test");
	REQUIRE(count_opcode(test, IROpcode::LOAD_RESOURCE) == 1);
	const IRInstruction &call = only(test, IROpcode::VCALL);
	REQUIRE(ir.strings[call.operands[2].string_id] == "new");
	REQUIRE(call.operands[3].immediate() == 2);
	for (const auto &instr : test.instructions) {
		REQUIRE((instr.opcode != IROpcode::CALL_SYSCALL ||
				 instr.operands[1].immediate() != ECALL_NODE_CREATE));
	}

	REQUIRE(refuses("func test():\n\treturn Timer.new(1)\n"));

	std::cout << "  \u2713 a script class constructs through its own script" << std::endl;
}

TEST_CASE("script class value is the script resource") {
	const std::vector<std::pair<std::string, std::string>> classes = {
		{ "Runner", "res://runner.gd" }
	};
	const IRProgram bare = compile_with_project(
			"func test():\n\treturn Runner\n", {}, classes);
	const IRFunction &bare_fn = find_function(bare, "test");
	REQUIRE(count_opcode(bare_fn, IROpcode::LOAD_RESOURCE) == 1);
	REQUIRE(count_opcode(bare_fn, IROpcode::VCALL) == 0);

	const IRProgram static_call = compile_with_project(
			"func test():\n\treturn Runner.helper(1)\n", {}, classes);
	const IRFunction &static_fn = find_function(static_call, "test");
	REQUIRE(count_opcode(static_fn, IROpcode::LOAD_RESOURCE) == 1);
	const IRInstruction &helper = only(static_fn, IROpcode::VCALL);
	REQUIRE(static_call.strings[helper.operands[2].string_id] == "helper");
	REQUIRE(static_call.strings[helper.operands[2].string_id] != "new");

	const IRProgram constant = compile_with_project(
			"func test():\n\treturn Runner.SPEED\n", {}, classes);
	const IRFunction &constant_fn = find_function(constant, "test");
	REQUIRE(count_opcode(constant_fn, IROpcode::LOAD_RESOURCE) == 1);
	REQUIRE(count_opcode(constant_fn, IROpcode::VGET) == 1);

	std::cout << "  \u2713 bare classes, constants and statics use the Script resource"
			  << std::endl;
}

TEST_CASE("engine class constant") {
	const IRProgram ir = compile_to_ir(
			"func test():\n\treturn ScrollContainer.SCROLL_MODE_DISABLED\n");
	const IRFunction &test = find_function(ir, "test");
	const IRInstruction &call = only(test, IROpcode::VCALL);
	REQUIRE(ir.strings[call.operands[2].string_id] == "class_get_integer_constant");

	const IRProgram folded = compile_to_ir("func test():\n\treturn Vector2.ZERO\n");
	REQUIRE(count_opcode(find_function(folded, "test"), IROpcode::VCALL) == 0);

	std::cout << "  \u2713 an engine class constant is read from ClassDB" << std::endl;
}

TEST_CASE("engine singleton method is an object call") {
	// Platform-specific singletons can be checked without running that platform:
	// they must load the named object, never dispatch a static call via ClassDB.
	for (const char *singleton : {
				 "RenderingServer",
				 "TranslationServer",
				 "CameraServer",
				 "JavaScriptBridge",
				 "JavaClassWrapper",
				 "AccessibilityServer",
				 "NavigationServer2DManager",
				 "NavigationServer3DManager",
				 "GDScriptLanguageProtocol",
				 "OpenXRFutureExtension",
				 "OpenXRRenderModelExtension",
				 "OpenXRSpatialEntityExtension",
				 "OpenXRSpatialAnchorCapability",
				 "OpenXRSpatialPlaneTrackingCapability",
				 "OpenXRSpatialMarkerTrackingCapability",
				 "OpenXRFrameSynthesisExtension",
				 "OpenXRAndroidThreadSettingsExtension",
		 }) {
		const IRProgram ir = compile_to_ir(std::string("func test():\n\treturn ") +
										   singleton + ".get_class()\n");
		const IRFunction &test = find_function(ir, "test");
		const IRInstruction &call = only(test, IROpcode::VCALL);
		REQUIRE(ir.strings[call.operands[2].string_id] == "get_class");
		const IRInstruction &lookup = only(test, IROpcode::CALL_SYSCALL);
		REQUIRE(lookup.operands[1].immediate() == ECALL_GET_OBJ);
		REQUIRE(ir.string_constants[lookup.operands[2].immediate()] == singleton);
		REQUIRE(call.operands[1].reg_value == lookup.operands[0].reg_value);
	}
}

// -= Declarations that used to take the whole file down =-

TEST_CASE("declarations") {
	const std::string script =
			"@tool\n"
			"extends Node\n"
			"class_name Enemy\n"
			"signal died(who)\n"
			"signal healed\n"
			"@export_range(0, 100) var hp = 100\n"
			"@export var speed : float = 1.5\n"
			"static var counter = 0\n"
			"@warning_ignore(\"unused\")\n"
			"static func bump() -> int:\n"
			"\tcounter += 1\n"
			"\treturn counter\n"
			"func test():\n"
			"\treturn hp + bump()\n";

	const IRProgram ir = compile_to_ir(script);
	REQUIRE(ir.globals.size() == 3);
	// @export_range: property is published, hint dropped.
	REQUIRE((ir.globals[0].name == "hp" && ir.globals[0].is_property));
	REQUIRE((ir.globals[1].name == "speed" && ir.globals[1].is_property));
	// `static var` -> global (no instance to differ from).
	REQUIRE((ir.globals[2].name == "counter" && !ir.globals[2].is_property));
	find_function(ir, "bump");
	find_function(ir, "test");
	REQUIRE(machine_code_builds(script));

	const IRProgram onready = compile_to_ir(
			"@onready var n = 1\nfunc test():\n\treturn n\n");
	REQUIRE((onready.globals.size() == 1 && onready.globals[0].name == "n"));
	const IRFunction &ready = find_function(onready, "_ready");
	REQUIRE(count_opcode(ready, IROpcode::STORE_GLOBAL) == 1);
	REQUIRE(refuses("func test():\n\t@onready var n = 1\n\treturn n\n"));
	REQUIRE(refuses("@onready const K = 1\nfunc test():\n\treturn K\n"));
	// Unknown attribute -> error.
	REQUIRE(refuses("@bogus var n = 1\nfunc test():\n\treturn n\n"));
	// Unterminated argument list -> error.
	REQUIRE(refuses("@export_range(0, 100 var hp = 1\nfunc test():\n\treturn hp\n"));
	// @export names a property; a function is not one, and dropping the
	// annotation silently would publish nothing.
	REQUIRE(refuses("@export func test():\n\treturn 1\n"));
	// A file-level annotation before a function is still fine.
	compile_to_ir("@tool\nfunc test():\n\treturn 1\n");

	// Grouping annotations stand alone (not attached to a declaration).
	for (const char *grouping : { "export_group", "export_subgroup", "export_category" }) {
		compile_to_ir(std::string("@") + grouping + "(\"Stats\")\nfunc test():\n\treturn 1\n");
		const IRProgram grouped = compile_to_ir(std::string("@") + grouping +
												"(\"Stats\")\n@export var hp = 1\nfunc test():\n\treturn hp\n");
		REQUIRE(grouped.globals.size() == 1);
		REQUIRE(grouped.globals[0].is_property);
	}
	compile_to_ir("@export_category(\"A\")\n@export_subgroup(\"B\")\n"
				  "@export var hp = 1\nfunc test():\n\treturn hp\n");
}

TEST_CASE("statement annotations") {
	const IRProgram ir = compile_to_ir(
			"func test():\n"
			"\t@warning_ignore(\"unused_variable\")\n"
			"\tvar unused = 1\n"
			"\treturn 2\n");
	find_function(ir, "test");

	compile_to_ir("func test():\n\t@warning_ignore(\"a\")\n\t@warning_ignore(\"b\")\n\treturn 1\n");
	compile_to_ir("func test():\n\tif true:\n\t\t@warning_ignore(\"a\")\n\t\treturn 1\n\treturn 0\n");
	compile_to_ir("func test():\n\t@warning_ignore_start(\"a\")\n\tvar x = 1\n"
				  "\t@warning_ignore_restore(\"a\")\n\treturn x\n");

	REQUIRE(refuses("func test():\n\t@export var x = 1\n\treturn x\n"));
	REQUIRE(refuses("func test():\n\t@bogus var x = 1\n\treturn x\n"));
}

TEST_CASE("rpc annotations") {
	const IRProgram ir = compile_to_ir(
			"@rpc\n"
			"func authority_default():\n\treturn 1\n"
			"@rpc(\"unreliable_ordered\", \"any_peer\", \"call_local\", 7)\n"
			"func customized(value):\n\treturn value\n");
	REQUIRE(ir.rpc_configs.size() == 2);
	REQUIRE(ir.rpc_configs[0].name == "authority_default");
	REQUIRE(ir.rpc_configs[0].rpc_mode == 2);
	REQUIRE(ir.rpc_configs[0].transfer_mode == 2);
	REQUIRE(!ir.rpc_configs[0].call_local);
	REQUIRE(ir.rpc_configs[0].channel == 0);
	REQUIRE(ir.rpc_configs[1].name == "customized");
	REQUIRE(ir.rpc_configs[1].rpc_mode == 1);
	REQUIRE(ir.rpc_configs[1].transfer_mode == 1);
	REQUIRE(ir.rpc_configs[1].call_local);
	REQUIRE(ir.rpc_configs[1].channel == 7);

	REQUIRE(refuses("@rpc(\"unknown\")\nfunc f():\n\tpass\n"));
	REQUIRE(refuses("@rpc(any_peer)\nfunc f():\n\tpass\n"));
	REQUIRE(refuses("@rpc(\"authority\", \"any_peer\")\nfunc f():\n\tpass\n"));
	REQUIRE(refuses("@rpc(\"authority\", \"call_remote\", \"reliable\", -1)\n"
					"func f():\n\tpass\n"));
	REQUIRE(refuses("@rpc\nvar value = 1\n"));
	REQUIRE(refuses("func f():\n\t@rpc\n\treturn 1\n"));
}

// Qualified type names (`A.B`) parse and drop; only the engine can resolve them.
TEST_CASE("qualified type names") {
	// Parameter, return type, variable, and inside a container's element type.
	compile_to_ir("func test(a : Node.Inner):\n\treturn a\n");
	compile_to_ir("func test() -> Node.Inner:\n\treturn null\n");
	compile_to_ir("var a : Node.Inner = null\nfunc test():\n\treturn a\n");
	compile_to_ir("func test(a : Array[Node.Inner]):\n\treturn a\n");
	compile_to_ir("func test(a : A.B.C):\n\treturn a\n");
	compile_to_ir("func test(index):\n\treturn index as Viewport.MSAA\n");

	// Dropped entirely, not misread as the first segment.
	const IRProgram ir = compile_to_ir(
			"var qualified : Node.Inner = null\n"
			"var plain : Array = []\n");
	REQUIRE(ir.globals[0].type_hint == IRInstruction::TypeHint_NONE);
	REQUIRE(ir.globals[1].type_hint == Variant::ARRAY);

	// Unresolvable qualified type leaves the local untyped.
	const IRProgram local = compile_to_ir(
			"func test():\n"
			"\tvar a : Node.Inner = null\n"
			"\treturn a\n");
	find_function(local, "test");

	// `extends` with dotted name and path.
	compile_to_ir("extends Node.Inner\nfunc test():\n\treturn 1\n");
	compile_to_ir("extends \"res://other.gd\"\nfunc test():\n\treturn 1\n");
	compile_to_ir("extends Node\nfunc test():\n\treturn 1\n");

	// A dangling '.' is still a syntax error.
	REQUIRE(refuses("func test(a : Node.):\n\treturn a\n"));
	REQUIRE(refuses("extends Node.\nfunc test():\n\treturn 1\n"));

	REQUIRE(machine_code_builds("func test(a : Node.Inner) -> Node.Inner:\n\treturn a\n"));
}

// -= for i in <int> =-

TEST_CASE("for over an integer") {
	// The counted loop, not the container walk: ECALL_ARRAY_SIZE on an
	// integer throws in the host.
	const IRProgram literal = compile_to_ir(
			"func test():\n\tvar t = 0\n\tfor i in 10:\n\t\tt += i\n\treturn t\n", true);
	const IRFunction &literal_fn = find_function(literal, "test");
	REQUIRE(count_opcode(literal_fn, IROpcode::CALL_SYSCALL) == 0);
	REQUIRE(count_opcode(literal_fn, IROpcode::VCALL) == 0);

	// Same for a bound whose type is declared rather than literal.
	const IRProgram declared = compile_to_ir(
			"func test(n : int):\n\tvar t = 0\n\tfor i in n:\n\t\tt += i\n\treturn t\n", true);
	REQUIRE(count_opcode(find_function(declared, "test"), IROpcode::CALL_SYSCALL) == 0);

	// An untyped bound cannot be told from a container, so it stays a walk.
	const IRProgram untyped = compile_to_ir(
			"func test(n):\n\tfor i in n:\n\t\tpass\n");
	REQUIRE(count_opcode(find_function(untyped, "test"), IROpcode::CALL_SYSCALL) > 0);

	// Float bound: counted loop, no syscall.
	for (const char *source : { "func test():\n\tfor i in 2.5:\n\t\tpass\n",
								"func test():\n\tvar f : float = 3.0\n\tfor i in f:\n\t\tpass\n" }) {
		const IRProgram floated = compile_to_ir(source);
		REQUIRE(count_opcode(find_function(floated, "test"), IROpcode::CALL_SYSCALL) == 0);
	}

	// A bool or null is not iterable in the engine either.
	REQUIRE(refuses("func test():\n\tfor i in true:\n\t\tpass\n"));
	REQUIRE(refuses("func test():\n\tfor i in null:\n\t\tpass\n"));

	// `for i: int in ...` parses; the type is the loop's, not the hint's.
	compile_to_ir("func test():\n\tfor i: int in range(3):\n\t\tpass\n");
	compile_to_ir("func test(a):\n\tfor v: Vector2 in a:\n\t\tpass\n");
}

// -= REQUIRE() =-

TEST_CASE("assert") {
	// A branch over a THROW. A dropped assert is worse than no assert, so
	// this must not be a self-call the engine silently ignores.
	const IRProgram with_message = compile_to_ir(
			"func test(x):\n\tassert(x > 0, \"x must be positive\")\n\treturn x\n");
	const IRFunction &with_message_fn = find_function(with_message, "test");
	REQUIRE(count_opcode(with_message_fn, IROpcode::THROW) == 1);
	REQUIRE(count_opcode(with_message_fn, IROpcode::VCALL) == 0);
	for (const auto &instr : with_message_fn.instructions) {
		if (instr.opcode == IROpcode::THROW) {
			REQUIRE(with_message.strings[instr.operands[0].string_id] == "assert");
			REQUIRE(with_message.strings[instr.operands[1].string_id] == "x must be positive");
		}
	}

	// One argument gets a default message.
	const IRProgram bare = compile_to_ir("func test(x):\n\tassert(x)\n\treturn 1\n");
	REQUIRE(count_opcode(find_function(bare, "test"), IROpcode::THROW) == 1);

	// The optimizer must not delete it, and the backend must emit it.
	const IRProgram optimized = compile_to_ir(
			"func test(x):\n\tassert(x)\n\treturn 1\n", true);
	REQUIRE(count_opcode(find_function(optimized, "test"), IROpcode::THROW) == 1);
	REQUIRE(machine_code_builds("func test(x):\n\tassert(x, \"no\")\n\treturn 1\n"));

	const IRProgram literal_msg = compile_to_ir("func test(x):\n\tassert(x, \"no\")\n");
	const IRInstruction &baked = only(find_function(literal_msg, "test"), IROpcode::THROW);
	REQUIRE(baked.operands.size() == 3);
	REQUIRE(baked.operands[2].immediate() == 0);

	const IRProgram computed_msg = compile_to_ir("func test(x):\n\tassert(x, x)\n");
	const IRInstruction &computed = only(find_function(computed_msg, "test"), IROpcode::THROW);
	REQUIRE(computed.operands.size() == 4);
	REQUIRE(computed.operands[2].immediate() == 1);
	REQUIRE(machine_code_builds("func test(x, n):\n\tassert(x, \"[%s]\" % n)\n"));

	REQUIRE(refuses("func test(x):\n\tassert()\n"));
	REQUIRE(refuses("func test(x):\n\tassert(x, \"a\", \"b\")\n"));

	// A local function of that name still wins.
	const IRProgram shadowed = compile_to_ir(
			"func REQUIRE(x):\n\treturn x\nfunc test():\n\treturn REQUIRE(1)\n");
	REQUIRE(count_opcode(find_function(shadowed, "test"), IROpcode::THROW) == 0);
}

// -= null =-

TEST_CASE("null is not zero") {
	// LOAD_IMM 0 is an INT Variant holding zero, which Godot does not treat
	// as null: `x == null` would compare against the integer.
	const IRProgram returned = compile_to_ir("func test():\n\treturn null\n");
	const IRFunction &returned_fn = find_function(returned, "test");
	REQUIRE(count_opcode(returned_fn, IROpcode::LOAD_NIL) == 1);
	REQUIRE(count_opcode(returned_fn, IROpcode::LOAD_IMM) == 0);

	// A variable with no initializer is null too, unless its declared type
	// has a default of its own.
	const IRProgram untyped = compile_to_ir("func test():\n\tvar x\n\treturn x\n");
	REQUIRE(count_opcode(find_function(untyped, "test"), IROpcode::LOAD_NIL) == 1);
	const IRProgram object = compile_to_ir("func test():\n\tvar n : Node\n\treturn n\n");
	REQUIRE(count_opcode(find_function(object, "test"), IROpcode::LOAD_NIL) == 1);

	// A declared type that the guest can build gets that type's default, not
	// an integer zero wearing the type's label.
	const IRProgram string_var = compile_to_ir("func test():\n\tvar s : String\n\treturn s\n");
	REQUIRE(count_opcode(find_function(string_var, "test"), IROpcode::LOAD_STRING) == 1);
	const IRProgram array_var = compile_to_ir("func test():\n\tvar a : Array\n\treturn a\n");
	REQUIRE(count_opcode(find_function(array_var, "test"), IROpcode::MAKE_ARRAY) == 1);

	REQUIRE(machine_code_builds("func test():\n\treturn null\n"));
}

// -= @GlobalScope constants =-

TEST_CASE("global constants") {
	// Compile-time numbers: an immediate, not a global load or a syscall.
	const IRProgram pi = compile_to_ir("func test():\n\treturn PI\n");
	const IRFunction &pi_fn = find_function(pi, "test");
	REQUIRE(count_opcode(pi_fn, IROpcode::LOAD_FLOAT_IMM) == 1);
	REQUIRE(count_opcode(pi_fn, IROpcode::LOAD_GLOBAL) == 0);

	// The type tags pair with typeof(), and both sides fold to integers, so
	// the compare stays off the VEVAL path.
	const IRProgram tags = compile_to_ir("func test(x):\n\treturn typeof(x) == TYPE_INT\n");
	const IRFunction &tags_fn = find_function(tags, "test");
	REQUIRE(count_opcode(tags_fn, IROpcode::TYPE_OF) == 1);
	for (const auto &instr : tags_fn.instructions) {
		if (instr.opcode == IROpcode::LOAD_IMM) {
			REQUIRE(instr.operands[1].immediate() == Variant::INT);
		}
	}

	compile_to_ir("func test():\n\treturn TAU\n");
	compile_to_ir("func test():\n\treturn INF\n");
	compile_to_ir("func test():\n\treturn NAN\n");
	compile_to_ir("func test():\n\treturn OK\n");
	compile_to_ir("func test():\n\treturn FAILED\n");
	compile_to_ir("func test():\n\treturn ERR_FILE_NOT_FOUND\n");
	compile_to_ir("func test():\n\treturn KEY_ESCAPE\n");
	compile_to_ir("func test():\n\treturn MOUSE_BUTTON_LEFT\n");
	compile_to_ir("func test():\n\treturn JOY_AXIS_LEFT_X\n");
	compile_to_ir("func test():\n\treturn TYPE_MAX\n");

	// A declaration of the same name still shadows it.
	const IRProgram shadowed = compile_to_ir(
			"func test():\n\tvar PI = 3\n\treturn PI\n");
	REQUIRE(count_opcode(find_function(shadowed, "test"), IROpcode::LOAD_FLOAT_IMM) == 0);
	const IRProgram global = compile_to_ir("var OK = 7\nfunc test():\n\treturn OK\n");
	REQUIRE(count_opcode(find_function(global, "test"), IROpcode::LOAD_GLOBAL) == 1);

	// A name that is not one is still undefined.
	REQUIRE(refuses("func test():\n\treturn TYPE_NOT_A_TYPE\n"));
}
