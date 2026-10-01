// Structs: sugar for a Dictionary with a fixed set of keys.
//
// Nothing about a struct survives into the IR, so what these tests pin down is
// the lowering: that an instance is a MAKE_DICTIONARY holding every declared
// field, that a field access is a Dictionary get or set rather than the
// property syscall (which reaches an Object and nothing else), and that a field
// name the struct does not declare is a compile error instead of a new key.
#include "../codegen.h"
#include "../compiler.h"
#include "../compiler_exception.h"
#include "../function_signature.h"
#include "../ir_optimizer.h"
#include "../lexer.h"
#include "../parser.h"
#include "../riscv_codegen.h"
#include "../variant_layout.h"
#include "witness/doctest.h"
#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

using namespace gdscript;

// -= Helpers =-

static Program parse(const std::string &source) {
	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	return parser.parse();
}

static IRProgram compile_to_ir(const std::string &source, bool optimize = false) {
	Program program = parse(source);
	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);
	if (optimize) {
		IROptimizer optimizer;
		optimizer.optimize(ir);
	}
	return ir;
}

// Comments are not tokens: Compiler::compile() hands them to the parser
// separately, and without that every doc comment is empty.
static IRProgram compile_with_doc_comments(const std::string &source) {
	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	parser.set_doc_comments(lexer.doc_comments());
	Program program = parser.parse();
	CodeGenerator codegen;
	return codegen.generate(program);
}

static const IRFunction &find_function(const IRProgram &ir, const std::string &name) {
	for (const auto &func : ir.functions) {
		if (func.name == name) {
			return func;
		}
	}
	throw std::runtime_error("Function not found: " + name);
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

// The method names of every VCALL in a function, in order.
static std::vector<std::string> vcall_methods(const IRProgram &ir, const IRFunction &func) {
	std::vector<std::string> methods;
	for (const auto &instr : func.instructions) {
		if (instr.opcode == IROpcode::VCALL) {
			methods.push_back(ir.strings[instr.operands.at(2).string_id]);
		}
	}
	return methods;
}

static int count_vcalls(const IRProgram &ir, const IRFunction &func, const std::string &method) {
	int count = 0;
	for (const auto &name : vcall_methods(ir, func)) {
		if (name == method) {
			count++;
		}
	}
	return count;
}

// Dictionary element accesses, by Dictionary_Op. A field read is
// ECALL_DICTIONARY_OPS with GET; a field write is the DICT_SET opcode.
static int count_dict_ops(const IRFunction &func, int64_t op) {
	int count = 0;
	for (const auto &instr : func.instructions) {
		if (instr.opcode == IROpcode::CALL_SYSCALL && instr.operands.size() >= 3 &&
			instr.operands[1].immediate() == 524 &&
			instr.operands[2].immediate() == op) {
			count++;
		}
	}
	return count;
}

static int count_dict_gets(const IRFunction &func) {
	return count_dict_ops(func, 0) + count_opcode(func, IROpcode::DICT_GET_CONST);
}

static int count_dict_sets(const IRFunction &func) {
	return count_opcode(func, IROpcode::DICT_SET) +
			count_opcode(func, IROpcode::DICT_SET_CONST);
}

// The string a register was last loaded with before instruction `before`.
static std::string string_in_register(const IRProgram &ir, const IRFunction &func,
									  size_t before, int reg) {
	for (size_t i = before; i-- > 0;) {
		const auto &instr = func.instructions[i];
		if (instr.opcode != IROpcode::LOAD_STRING) {
			continue;
		}
		if (instr.operands.at(0).reg_index() != reg) {
			continue;
		}
		return ir.string_constants.at(instr.operands.at(1).immediate());
	}
	throw std::runtime_error("no LOAD_STRING for register " + std::to_string(reg));
}

// The keys of the first MAKE_DICTIONARY in a function, in the order they are
// built. For a struct instance that is the order the fields were declared in.
static std::vector<std::string> dictionary_keys(const IRProgram &ir, const IRFunction &func) {
	for (size_t i = 0; i < func.instructions.size(); i++) {
		const auto &instr = func.instructions[i];
		if (instr.opcode == IROpcode::MAKE_DICTIONARY_KEYED) {
			const int64_t pairs = instr.operands.at(1).immediate();
			std::vector<std::string> keys;
			for (int64_t pair = 0; pair < pairs; pair++) {
				keys.push_back(ir.string_constants.at(
						instr.operands.at(2 + pair * 2).immediate()));
			}
			return keys;
		}
		if (instr.opcode != IROpcode::MAKE_DICTIONARY) {
			continue;
		}
		const int64_t pairs = instr.operands.at(1).immediate();
		std::vector<std::string> keys;
		for (int64_t pair = 0; pair < pairs; pair++) {
			const int key_reg = instr.operands.at(2 + pair * 2).reg_index();
			keys.push_back(string_in_register(ir, func, i, key_reg));
		}
		return keys;
	}
	throw std::runtime_error("no MAKE_DICTIONARY in function " + func.name);
}

// The immediate a register was last loaded with before instruction `before`.
static int64_t int_in_register(const IRFunction &func, size_t before, int reg) {
	for (size_t i = before; i-- > 0;) {
		const auto &instr = func.instructions[i];
		if (instr.opcode != IROpcode::LOAD_IMM) {
			continue;
		}
		if (instr.operands.at(0).reg_index() != reg) {
			continue;
		}
		return instr.operands.at(1).immediate();
	}
	throw std::runtime_error("no LOAD_IMM for register " + std::to_string(reg));
}

// The integer values of the first MAKE_DICTIONARY in a function, by field order.
static std::vector<int64_t> dictionary_int_values(const IRFunction &func) {
	for (size_t i = 0; i < func.instructions.size(); i++) {
		const auto &instr = func.instructions[i];
		if (instr.opcode != IROpcode::MAKE_DICTIONARY &&
			instr.opcode != IROpcode::MAKE_DICTIONARY_KEYED) {
			continue;
		}
		const int64_t pairs = instr.operands.at(1).immediate();
		std::vector<int64_t> values;
		for (int64_t pair = 0; pair < pairs; pair++) {
			const int value_reg = instr.operands.at(3 + pair * 2).reg_index();
			values.push_back(int_in_register(func, i, value_reg));
		}
		return values;
	}
	throw std::runtime_error("no MAKE_DICTIONARY in function " + func.name);
}

// Returns true when compiling the source throws a CompilerException.
static bool rejects(const std::string &source) {
	try {
		compile_to_ir(source);
		return false;
	} catch (const CompilerException &) {
		return true;
	}
}

// Every CALL in a function, as (target name, argument count). A struct method
// is lifted to a plain function named `@Struct.method`.
static std::vector<std::pair<std::string, int64_t>> calls(const IRProgram &ir,
														  const IRFunction &func) {
	std::vector<std::pair<std::string, int64_t>> found;
	for (const auto &instr : func.instructions) {
		if (instr.opcode != IROpcode::CALL && instr.opcode != IROpcode::CALL_HOSTED) {
			continue;
		}
		found.emplace_back(ir.strings[instr.operands.at(0).string_id],
						   instr.operands.at(2).immediate());
	}
	return found;
}

// The message of the compile error the source produces, or "" when it compiles.
static std::string rejection(const std::string &source) {
	try {
		compile_to_ir(source);
	} catch (const CompilerException &error) {
		return error.what();
	}
	return {};
}

// The declaration every test below builds on.
static const std::string BANK_ACCOUNT =
		"struct BankAccount:\n"
		"\tvar balance = 0\n"
		"\tvar loan = 0\n"
		"\n";

// -= Tests =-

TEST_CASE("struct declaration parses") {
	const Program program = parse(
			"struct BankAccount:\n"
			"\tvar balance = 0\n"
			"\tvar loan: int = 5\n"
			"\tvar owner: String\n"
			"\tvar note\n"
			"\n"
			"func test():\n"
			"\treturn 1\n");

	REQUIRE(program.structs.size() == 1);
	const StructDecl &decl = program.structs[0];
	REQUIRE(decl.name == "BankAccount");
	REQUIRE(decl.fields.size() == 4);

	REQUIRE(decl.fields[0].name == "balance");
	REQUIRE(decl.fields[0].type_hint.empty());
	REQUIRE(decl.fields[0].default_value != nullptr);

	REQUIRE(decl.fields[1].name == "loan");
	REQUIRE(decl.fields[1].type_hint == "int");
	REQUIRE(decl.fields[1].default_value != nullptr);

	// A type hint without a value, and a value without a type hint, are both
	// field declarations.
	REQUIRE(decl.fields[2].type_hint == "String");
	REQUIRE(decl.fields[2].default_value == nullptr);
	REQUIRE(decl.fields[3].type_hint.empty());
	REQUIRE(decl.fields[3].default_value == nullptr);

	REQUIRE(decl.field_index("loan") == 1);
	REQUIRE(decl.find_field("nope") == nullptr);
	REQUIRE(decl.field_list() == "balance, loan, owner, note");

	// The function after the struct is still parsed.
	REQUIRE(program.functions.size() == 1);

	// An empty struct is written with 'pass', like an empty function body.
	const Program empty = parse("struct Empty:\n\tpass\n\nfunc test():\n\treturn 1\n");
	REQUIRE(empty.structs.size() == 1);
	REQUIRE(empty.structs[0].fields.empty());
}

TEST_CASE("struct body members") {
	const IRProgram ir = compile_to_ir(
			"struct S:\n"
			"\tconst STEP = 2\n"
			"\tvar x = 1\n"
			"\tfunc advance():\n\t\tself.x += S.STEP\n\t\treturn self.x\n"
			"\tstatic func unit():\n\t\treturn S(1)\n"
			"\nfunc test():\n\tvar s: S\n\treturn s.advance() + S.unit().x\n");
	REQUIRE(find_function(ir, "@S.advance").name == "@S.advance");
	REQUIRE(find_function(ir, "@S.unit").name == "@S.unit");
	REQUIRE(count_opcode(find_function(ir, "@S.advance"), IROpcode::DICT_GET_CONST) >= 1);
	REQUIRE(rejects(
			"struct S:\n\tvar x = 1\n\tfunc value():\n\t\treturn x\n"
			"\nfunc test(v):\n\treturn v.value()\n"));
	// The same field twice is a mistake, not a redefinition.
	REQUIRE(rejects("struct S:\n\tvar x = 1\n\tvar x = 2\n"));
}

TEST_CASE("construction builds a dictionary") {
	const IRProgram ir = compile_to_ir(BANK_ACCOUNT +
									   "func test():\n"
									   "\treturn BankAccount.new()\n");
	const IRFunction &test = find_function(ir, "test");

	// One Dictionary, holding every declared field in declaration order, with
	// the declared defaults.
	REQUIRE(count_opcode(test, IROpcode::MAKE_DICTIONARY_KEYED) == 1);
	const std::vector<std::string> keys = dictionary_keys(ir, test);
	REQUIRE((keys == std::vector<std::string>{ "balance", "loan" }));
	REQUIRE((dictionary_int_values(test) == std::vector<int64_t>{ 0, 0 }));

	// A struct is not an object: nothing about it reaches the property syscalls.
	REQUIRE(count_opcode(test, IROpcode::VGET) == 0);
	REQUIRE(count_opcode(test, IROpcode::VSET) == 0);

	// An empty struct is an empty Dictionary rather than an error.
	const IRProgram empty = compile_to_ir(
			"struct Empty:\n\tpass\n\nfunc test():\n\treturn Empty.new()\n");
	REQUIRE(dictionary_keys(empty, find_function(empty, "test")).empty());
}

TEST_CASE("both constructor forms") {
	// `BankAccount.new(...)` is the Godot idiom; `BankAccount(...)` reads like
	// the built-in types. Both mean the same thing.
	const IRProgram with_new = compile_to_ir(BANK_ACCOUNT +
											 "func test():\n\treturn BankAccount.new(100, 50)\n");
	const IRProgram plain = compile_to_ir(BANK_ACCOUNT +
										  "func test():\n\treturn BankAccount(100, 50)\n");

	const IRFunction &a = find_function(with_new, "test");
	const IRFunction &b = find_function(plain, "test");
	REQUIRE(dictionary_keys(with_new, a) == dictionary_keys(plain, b));
	REQUIRE(dictionary_int_values(a) == dictionary_int_values(b));
	REQUIRE((dictionary_int_values(a) == std::vector<int64_t>{ 100, 50 }));
}

TEST_CASE("named arguments") {
	// Named arguments land on the field they name, whatever order they are
	// written in, and whichever constructor form is used.
	const IRProgram reordered = compile_to_ir(BANK_ACCOUNT +
											  "func test():\n\treturn BankAccount.new(loan = 50, balance = 100)\n");
	REQUIRE((dictionary_int_values(find_function(reordered, "test")) ==
			 std::vector<int64_t>{ 100, 50 }));

	const IRProgram plain = compile_to_ir(BANK_ACCOUNT +
										  "func test():\n\treturn BankAccount(balance = 100, loan = 50)\n");
	REQUIRE((dictionary_int_values(find_function(plain, "test")) ==
			 std::vector<int64_t>{ 100, 50 }));

	// Positional first, then named, the way Python and GDScript's own default
	// arguments read.
	const IRProgram mixed = compile_to_ir(BANK_ACCOUNT +
										  "func test():\n\treturn BankAccount.new(100, loan = 50)\n");
	REQUIRE((dictionary_int_values(find_function(mixed, "test")) ==
			 std::vector<int64_t>{ 100, 50 }));

	// A field left out keeps its declared default.
	const IRProgram partial = compile_to_ir(BANK_ACCOUNT +
											"func test():\n\treturn BankAccount.new(loan = 50)\n");
	REQUIRE((dictionary_int_values(find_function(partial, "test")) ==
			 std::vector<int64_t>{ 0, 50 }));

	// A name the struct does not declare, a field given a value twice, a
	// positional argument after a named one, and more values than fields.
	REQUIRE(rejects(BANK_ACCOUNT + "func test():\n\treturn BankAccount.new(blance = 1)\n"));
	REQUIRE(rejects(BANK_ACCOUNT + "func test():\n\treturn BankAccount.new(1, balance = 2)\n"));
	REQUIRE(rejects(BANK_ACCOUNT + "func test():\n\treturn BankAccount.new(balance = 1, 2)\n"));
	REQUIRE(rejects(BANK_ACCOUNT + "func test():\n\treturn BankAccount.new(1, 2, 3)\n"));

	// Naming an argument of anything that is not a struct constructor would
	// drop the name silently, so it is rejected instead.
	REQUIRE(rejects("func other(a):\n\treturn a\n\nfunc test():\n\treturn other(a = 1)\n"));
}

TEST_CASE("field access is dictionary access") {
	const IRProgram ir = compile_to_ir(BANK_ACCOUNT +
									   "func test():\n"
									   "\tvar account = BankAccount.new()\n"
									   "\taccount.balance = 100\n"
									   "\treturn account.balance\n");
	const IRFunction &test = find_function(ir, "test");

	// The property syscalls reach an Object's properties and nothing else, so a
	// struct field has to take the element path or it throws at run time.
	REQUIRE(count_opcode(test, IROpcode::VGET) == 0);
	REQUIRE(count_opcode(test, IROpcode::VSET) == 0);
	REQUIRE(count_dict_sets(test) == 1);
	REQUIRE(count_dict_gets(test) == 1);

	// A field of a struct held in a parameter is known too, from the type hint.
	const IRProgram parameter = compile_to_ir(BANK_ACCOUNT +
											  "func test(account: BankAccount):\n\treturn account.balance\n");
	REQUIRE(count_opcode(find_function(parameter, "test"), IROpcode::VGET) == 0);
	REQUIRE(count_dict_gets(find_function(parameter, "test")) == 1);

	// And of one returned by a function that declares it returns one.
	const IRProgram returned = compile_to_ir(BANK_ACCOUNT +
											 "func open() -> BankAccount:\n\treturn BankAccount.new()\n"
											 "\n"
											 "func test():\n\treturn open().balance\n");
	REQUIRE(count_opcode(find_function(returned, "test"), IROpcode::VGET) == 0);
	REQUIRE(count_dict_gets(find_function(returned, "test")) == 1);
}

TEST_CASE("unknown field is rejected") {
	// Catching the typo is what a struct buys over a bare Dictionary, so a
	// field the struct does not declare is an error on read and on write alike.
	REQUIRE(rejects(BANK_ACCOUNT +
					"func test():\n\tvar a = BankAccount.new()\n\treturn a.blance\n"));
	REQUIRE(rejects(BANK_ACCOUNT +
					"func test():\n\tvar a = BankAccount.new()\n\ta.blance = 1\n\treturn a\n"));
	REQUIRE(rejects(BANK_ACCOUNT +
					"func test(a: BankAccount):\n\treturn a.blance\n"));

	const IRProgram methods = compile_to_ir(BANK_ACCOUNT +
											"func test():\n\tvar a = BankAccount.new()\n\treturn a.size()\n");
	REQUIRE(count_vcalls(methods, find_function(methods, "test"), "size") == 0);
	REQUIRE(count_dict_ops(find_function(methods, "test"), 6) == 1);

	// The declared fields are still reachable through the Dictionary itself.
	const IRProgram by_key = compile_to_ir(BANK_ACCOUNT +
										   "func test():\n\tvar a = BankAccount.new()\n\treturn a[\"balance\"]\n");
	REQUIRE(count_dict_gets(find_function(by_key, "test")) == 1);
}

TEST_CASE("subscript answers for the name") {
	REQUIRE(rejects(BANK_ACCOUNT +
					"func test():\n\tvar a = BankAccount.new()\n\treturn a[\"blance\"]\n"));
	REQUIRE(rejects(BANK_ACCOUNT +
					"func test():\n\tvar a = BankAccount.new()\n\ta[\"blance\"] = 1\n\treturn a\n"));
	REQUIRE(rejects(BANK_ACCOUNT +
					"func test():\n\tvar a = BankAccount.new()\n\ta[\"blance\"] += 1\n\treturn a\n"));
	REQUIRE(rejects(BANK_ACCOUNT +
					"func test(a: BankAccount):\n\treturn a[\"blance\"]\n"));
	REQUIRE(rejects(BANK_ACCOUNT +
					"var vault: BankAccount\n\nfunc test():\n\treturn vault[\"blance\"]\n"));
	REQUIRE(rejects(BANK_ACCOUNT +
					"func make() -> BankAccount:\n\treturn BankAccount.new()\n"
					"\nfunc test():\n\treturn make()[\"blance\"]\n"));

	REQUIRE(rejects("const K = \"blance\"\n\n" + BANK_ACCOUNT +
					"func test():\n\tvar a = BankAccount.new()\n\treturn a[K]\n"));
	const IRProgram const_key = compile_to_ir("const K = \"balance\"\n\n" + BANK_ACCOUNT +
											  "func test():\n\tvar a = BankAccount.new()\n\treturn a[K]\n");
	REQUIRE(count_dict_gets(find_function(const_key, "test")) == 1);
	const IRProgram shadowed = compile_to_ir("const K = \"blance\"\n\n" + BANK_ACCOUNT +
											 "func test():\n\tvar K = \"balance\"\n\tvar a = BankAccount.new()\n\treturn a[K]\n");
	REQUIRE(count_dict_gets(find_function(shadowed, "test")) == 1);

	const IRProgram computed = compile_to_ir(BANK_ACCOUNT +
											 "func test(k):\n\tvar a = BankAccount.new()\n\ta[k] = 1\n\treturn a\n");
	REQUIRE(count_dict_sets(find_function(computed, "test")) == 1);

	const IRProgram plain = compile_to_ir(
			"func test():\n\tvar d = {}\n\td[\"anything\"] = 1\n\treturn d[\"anything\"]\n");
	REQUIRE(count_dict_gets(find_function(plain, "test")) == 1);
}

TEST_CASE("struct is a type not a value") {
	REQUIRE(rejects(BANK_ACCOUNT + "func test():\n\treturn BankAccount\n"));
	// Two structs of the same name, a struct named after a Godot singleton, a
	// struct named after a function, and a global named after a struct: each
	// would make one of the two names unreachable.
	REQUIRE(rejects("struct S:\n\tvar x = 1\n\nstruct S:\n\tvar y = 2\n"));
	REQUIRE(rejects("struct Engine:\n\tvar x = 1\n"));
	REQUIRE(rejects("struct S:\n\tvar x = 1\n\nfunc S():\n\treturn 1\n"));
	REQUIRE(rejects("struct S:\n\tvar x = 1\n\nvar S = 1\n"));
}

TEST_CASE("declared field types") {
	// A typed field with no value gets the default of its type, the way a typed
	// variable with no initializer does.
	const IRProgram defaults = compile_to_ir(
			"struct S:\n"
			"\tvar count: int\n"
			"\tvar ratio: float\n"
			"\tvar name: String\n"
			"\tvar items: Array\n"
			"\tvar flag: bool\n"
			"\tvar anything\n"
			"\n"
			"func test():\n\treturn S.new()\n");
	const IRFunction &test = find_function(defaults, "test");
	REQUIRE((dictionary_keys(defaults, test) ==
			 std::vector<std::string>{ "count", "ratio", "name", "items", "flag", "anything" }));
	REQUIRE(count_opcode(test, IROpcode::LOAD_FLOAT_IMM) == 1);
	REQUIRE(count_opcode(test, IROpcode::MAKE_ARRAY) == 1);

	// GDScript's one implicit numeric conversion applies to a field's declared
	// type, both in the declaration and at the assignment.
	const IRProgram converts = compile_to_ir(
			"struct S:\n\tvar ratio: float = 0\n"
			"\n"
			"func test():\n\tvar s = S.new()\n\ts.ratio = 3\n\treturn s.ratio\n");
	REQUIRE(count_opcode(find_function(converts, "test"), IROpcode::CONVERT) == 2);

	// Anything else that GDScript rejects is rejected here too, rather than
	// reinterpreted: the backend would read the payload as the declared type.
	REQUIRE(rejects("struct S:\n\tvar count: int = 0\n"
					"\n"
					"func test():\n\tvar s = S.new()\n\ts.count = 1.5\n\treturn s\n"));
	REQUIRE(rejects("struct S:\n\tvar count: int = 0\n"
					"\n"
					"func test():\n\treturn S.new(1.5)\n"));
}

TEST_CASE("nested structs") {
	const std::string source =
			"struct Inner:\n"
			"\tvar x = 1\n"
			"\tvar y = 2\n"
			"\n"
			"struct Outer:\n"
			"\tvar inner: Inner\n"
			"\tvar tag = 0\n"
			"\n"
			"func test():\n"
			"\tvar o = Outer.new()\n"
			"\treturn o.inner.x\n";

	const IRProgram ir = compile_to_ir(source);
	const IRFunction &test = find_function(ir, "test");

	// A field declared as another struct defaults to an instance of it, so both
	// Dictionaries are built.
	REQUIRE(count_opcode(test, IROpcode::MAKE_DICTIONARY_KEYED) == 2);
	// Reading through the nesting stays on the element path the whole way.
	REQUIRE(count_opcode(test, IROpcode::VGET) == 0);
	REQUIRE(count_dict_gets(test) == 2);
	// And the field of the nested struct is checked against Inner, not Outer.
	REQUIRE(rejects(
			"struct Inner:\n\tvar x = 1\n"
			"\n"
			"struct Outer:\n\tvar inner: Inner\n"
			"\n"
			"func test():\n\tvar o = Outer.new()\n\treturn o.inner.tag\n"));

	// A struct that holds itself by value has no finite default, so it is
	// reported instead of recursing until the compiler runs out of stack.
	REQUIRE(rejects("struct Chain:\n\tvar next: Chain\n\nfunc test():\n\treturn Chain.new()\n"));
	// Giving the field a value breaks the cycle.
	compile_to_ir("struct Chain:\n\tvar next: Chain = null\n"
				  "\n"
				  "func test():\n\treturn Chain.new()\n");
}

TEST_CASE("struct type hints") {
	// `var a: BankAccount` with no initializer is a fresh instance, the way
	// `var a: Array` is an empty Array.
	const IRProgram bare = compile_to_ir(BANK_ACCOUNT +
										 "func test():\n\tvar account: BankAccount\n\treturn account.balance\n");
	const IRFunction &test = find_function(bare, "test");
	REQUIRE(count_opcode(test, IROpcode::MAKE_DICTIONARY_KEYED) == 1);
	REQUIRE((dictionary_keys(bare, test) == std::vector<std::string>{ "balance", "loan" }));

	// The annotation is checked against what the initializer actually is.
	compile_to_ir(BANK_ACCOUNT +
				  "func test():\n\tvar account: BankAccount = BankAccount.new()\n\treturn account\n");
	REQUIRE(rejects(BANK_ACCOUNT +
					"struct Other:\n\tvar x = 1\n"
					"\n"
					"func test():\n\tvar account: BankAccount = Other.new()\n\treturn account\n"));
	REQUIRE(rejects(BANK_ACCOUNT +
					"func test():\n\tvar account: BankAccount = 5\n\treturn account\n"));
}

TEST_CASE("struct globals") {
	// A struct-typed global with no initializer is built by the init function,
	// because a Dictionary has no compile-time representation to write into the
	// globals array.
	const IRProgram typed = compile_to_ir(BANK_ACCOUNT +
										  "var account: BankAccount\n"
										  "\n"
										  "func test():\n\taccount.balance = 1\n\treturn account.balance\n");
	// A plain `var` is a member, so its initializer runs per instance.
	REQUIRE(typed.has_member_init);
	REQUIRE(count_opcode(typed.member_init, IROpcode::MAKE_DICTIONARY_KEYED) == 1);
	REQUIRE(typed.globals.at(0).type_hint == Variant::DICTIONARY);

	const IRFunction &test = find_function(typed, "test");
	REQUIRE(count_opcode(test, IROpcode::VGET) == 0);
	REQUIRE(count_opcode(test, IROpcode::VSET) == 0);
	REQUIRE(count_dict_gets(test) == 1);
	REQUIRE(count_dict_sets(test) == 1);

	// Which struct a global holds is also taken from its initializer, which is
	// how a global is usually written.
	const IRProgram inferred = compile_to_ir(BANK_ACCOUNT +
											 "var account = BankAccount.new(7)\n"
											 "\n"
											 "func test():\n\treturn account.balance\n");
	REQUIRE(count_opcode(find_function(inferred, "test"), IROpcode::VGET) == 0);
	REQUIRE(count_dict_gets(find_function(inferred, "test")) == 1);
	REQUIRE(rejects(BANK_ACCOUNT +
					"var account = BankAccount.new()\n"
					"\n"
					"func test():\n\treturn account.blance\n"));
}

TEST_CASE("compound assignment to a field") {
	// `a.balance += 5` is the natural way to write this, and rewrites to
	// `a.balance = a.balance + 5`: one read, one add, one write.
	const IRProgram ir = compile_to_ir(BANK_ACCOUNT +
									   "func test():\n"
									   "\tvar account = BankAccount.new()\n"
									   "\taccount.balance += 5\n"
									   "\treturn account.balance\n");
	const IRFunction &test = find_function(ir, "test");
	REQUIRE(count_dict_sets(test) == 1);
	REQUIRE(count_dict_gets(test) == 2);
	REQUIRE(count_opcode(test, IROpcode::ADD) == 1);

	// A target that evaluates a call goes through a temporary: open() runs once.
	const IRProgram opened = compile_to_ir(BANK_ACCOUNT +
										   "func open() -> BankAccount:\n\treturn BankAccount.new()\n"
										   "\n"
										   "func test():\n\topen().balance += 5\n\treturn 1\n");
	REQUIRE(count_opcode(find_function(opened, "test"), IROpcode::CALL) == 1);
	// An unknown field is still rejected through this path.
	REQUIRE(rejects(BANK_ACCOUNT +
					"func test():\n\tvar a = BankAccount.new()\n\ta.blance += 5\n\treturn a\n"));
}

TEST_CASE("dictionary member access") {
	// In GDScript `d.key` is `d["key"]`. The property syscall the member path
	// used to take works on an Object and throws on a Dictionary, so a value
	// the compiler knows to be a Dictionary takes the element path.
	const IRProgram ir = compile_to_ir(
			"func test():\n"
			"\tvar d = {\"a\": 1}\n"
			"\td.a = 5\n"
			"\treturn d.a\n");
	const IRFunction &test = find_function(ir, "test");
	REQUIRE(count_opcode(test, IROpcode::VGET) == 0);
	REQUIRE(count_opcode(test, IROpcode::VSET) == 0);
	REQUIRE(count_dict_gets(test) == 1);
	REQUIRE(count_dict_sets(test) == 1);

	// A Dictionary has no declared keys, so any name is allowed on one.
	compile_to_ir("func test():\n\tvar d = {}\n\td.anything = 1\n\treturn d.anything\n");

	// An object property is still a property: nothing here changes that path.
	const IRProgram object = compile_to_ir("func test():\n\treturn self.name\n");
	REQUIRE(count_opcode(find_function(object, "test"), IROpcode::VGET) == 1);
}

TEST_CASE("an untracked instance still reaches its fields") {
	// Untracked struct: tag branches to element read/write, VGET/VSET fallback for Objects.
	const IRProgram ir = compile_to_ir(BANK_ACCOUNT +
									   "func test(account):\n"
									   "\treturn account.balance\n");
	const IRFunction &test = find_function(ir, "test");
	REQUIRE(count_opcode(test, IROpcode::TYPE_TEST) == 1);
	REQUIRE(count_dict_gets(test) == 1);
	// Object fallback survives: type unknown, could be either.
	REQUIRE(count_opcode(test, IROpcode::VGET) == 1);

	// The same for a write.
	const IRProgram write = compile_to_ir(BANK_ACCOUNT +
										  "func test(account):\n"
										  "\taccount.balance = 5\n");
	const IRFunction &w = find_function(write, "test");
	REQUIRE(count_opcode(w, IROpcode::TYPE_TEST) == 1);
	REQUIRE(count_dict_sets(w) == 1);
	REQUIRE(count_opcode(w, IROpcode::VSET) == 1);

	// Out of an Array, which carries no element type.
	const IRProgram walked = compile_to_ir(BANK_ACCOUNT +
										   "func test():\n"
										   "\tvar total = 0\n"
										   "\tfor account in [BankAccount.new(100), BankAccount.new(50)]:\n"
										   "\t\ttotal += account.balance\n"
										   "\treturn total\n");
	const IRFunction &walk = find_function(walked, "test");
	REQUIRE(count_dict_gets(walk) == 1);
	REQUIRE(count_opcode(walk, IROpcode::TYPE_TEST) == 1);

	// Unknown type: no struct to validate against, same as bare Dictionary.
	compile_to_ir(BANK_ACCOUNT + "func test(account):\n\treturn account.blance\n");
}

TEST_CASE("struct shape guards") {
	const IRProgram parameter = compile_to_ir(BANK_ACCOUNT +
											  "func balance_of(account: BankAccount):\n\treturn account.balance\n");
	const IRFunction &guarded = find_function(parameter, "balance_of");
	REQUIRE(count_opcode(guarded, IROpcode::STRUCT_CHECK) == 1);
	REQUIRE(count_opcode(guarded, IROpcode::TYPE_TEST) == 1);
	REQUIRE(count_opcode(guarded, IROpcode::THROW) >= 1);

	const IRProgram proven = compile_to_ir(BANK_ACCOUNT +
										   "func test():\n\tvar account: BankAccount = BankAccount()\n\treturn account.balance\n");
	REQUIRE(count_opcode(find_function(proven, "test"), IROpcode::STRUCT_CHECK) == 0);

	const IRProgram returned = compile_to_ir(BANK_ACCOUNT +
											 "func accept(value) -> BankAccount:\n\treturn value\n");
	REQUIRE(count_opcode(find_function(returned, "accept"), IROpcode::STRUCT_CHECK) == 1);

	CodeGenerator unchecked_codegen;
	unchecked_codegen.set_struct_checks(false);
	Program parsed = parse(BANK_ACCOUNT +
						   "func balance_of(account: BankAccount):\n\treturn account.balance\n");
	const IRProgram unchecked = unchecked_codegen.generate(parsed);
	REQUIRE(count_opcode(find_function(unchecked, "balance_of"), IROpcode::STRUCT_CHECK) == 0);
}

TEST_CASE("struct identity copy and strings") {
	const IRProgram identity = compile_to_ir(BANK_ACCOUNT +
											 "func test(value):\n"
											 "\tvar account = BankAccount()\n"
											 "\tvar copied = account.copy()\n"
											 "\treturn [account == copied, value is BankAccount, value as BankAccount]\n");
	const IRFunction &test = find_function(identity, "test");
	REQUIRE(count_vcalls(identity, test, "duplicate") == 1);
	REQUIRE(count_opcode(test, IROpcode::STRUCT_CHECK) >= 2);
	REQUIRE(count_opcode(test, IROpcode::CMP_EQ) >= 1);
	REQUIRE(rejects(BANK_ACCOUNT +
					"struct Other:\n\tvar x = 1\n"
					"\nfunc test():\n\treturn BankAccount() == Other()\n"));

	const IRProgram copy_ctor = compile_to_ir(BANK_ACCOUNT +
											  "func test():\n\tvar account = BankAccount()\n\treturn BankAccount(account)\n");
	REQUIRE(count_vcalls(copy_ctor, find_function(copy_ctor, "test"), "duplicate") == 1);

	const IRProgram rendered = compile_to_ir(BANK_ACCOUNT +
											 "func test():\n\tvar account = BankAccount(1, 2)\n\treturn [str(account), \"%s\" % account]\n");
	REQUIRE(count_dict_gets(find_function(rendered, "test")) == 4);

	const IRProgram custom = compile_to_ir(
			"struct Point:\n"
			"\tvar x = 0\n"
			"\tfunc _to_string():\n\t\treturn \"point\"\n"
			"\nfunc test():\n\treturn str(Point())\n");
	REQUIRE(count_opcode(find_function(custom, "test"), IROpcode::CALL) == 1);
}

TEST_CASE("struct match patterns") {
	const std::string point =
			"struct Point:\n\tvar x: int = 0\n\tvar y: int = 0\n\n";
	const IRProgram named = compile_to_ir(point +
										  "func test(value):\n"
										  "\tmatch value:\n"
										  "\t\tPoint(x = var a, y = 0):\n\t\t\treturn a\n"
										  "\t\t_:\n\t\t\treturn -1\n");
	const IRFunction &named_test = find_function(named, "test");
	REQUIRE(count_opcode(named_test, IROpcode::STRUCT_CHECK) == 1);
	REQUIRE(count_opcode(named_test, IROpcode::DICT_GET_CONST) == 2);

	compile_to_ir(point +
				  "func test(value):\n"
				  "\tmatch value:\n"
				  "\t\tPoint(var a, _):\n\t\t\treturn a\n"
				  "\t\t_:\n\t\t\treturn -1\n");
	REQUIRE(rejects(point +
					"func test(value):\n\tmatch value:\n\t\tPoint(z = _, y = _):\n\t\t\tpass\n"));
	REQUIRE(rejects(point +
					"func test(value):\n\tmatch value:\n\t\tPoint(x = _):\n\t\t\tpass\n"));
}

TEST_CASE("typed struct containers") {
	const std::string point = "struct Point:\n\tvar x = 0\n\n";
	const IRProgram ir = compile_to_ir(point +
									   "func total(points: Array[Point]):\n"
									   "\tvar answer = 0\n"
									   "\tfor point in points:\n\t\tanswer += point.x\n"
									   "\treturn answer\n");
	const IRFunction &total = find_function(ir, "total");
	// The Array itself is checked as an Array, but each loop value is proven Point.
	REQUIRE(count_opcode(total, IROpcode::STRUCT_CHECK) == 0);
	REQUIRE(count_opcode(total, IROpcode::DICT_GET_CONST) == 1);
	REQUIRE(rejects(point +
					"func total(points: Array[Point]):\n"
					"\tfor point in points:\n\t\treturn point.missing\n"
					"\treturn 0\n"));
	REQUIRE(rejects(point +
					"struct Sprite:\n\tvar x = 0\n\n"
					"func add(points: Array[Point]):\n\tpoints.append(Sprite())\n"));
	const IRProgram cast = compile_to_ir(point +
										 "func first(value):\n\tvar points = value as Array[Point]\n\treturn points[0].x\n");
	REQUIRE(count_opcode(find_function(cast, "first"), IROpcode::DICT_GET_CONST) == 1);
	REQUIRE(rejects(point +
					"func first(value):\n\tvar points = value as Array[Point]\n\treturn points[0].missing\n"));
	const IRProgram global = compile_to_ir(point +
										   "var points: Array[Point] = []\n\n"
										   "func first():\n\tpoints.append(Point())\n\treturn points[0].x\n");
	REQUIRE(count_opcode(find_function(global, "first"), IROpcode::DICT_GET_CONST) == 1);
}

TEST_CASE("struct signatures") {
	const IRProgram ir = compile_to_ir(
			"struct Point:\n"
			"\tvar x: int = 0\n"
			"\tfunc moved(dx: int) -> Point:\n\t\treturn Point(self.x + dx)\n"
			"\nfunc use(point: Point) -> Point:\n\treturn point\n");
	REQUIRE(ir.class_signatures.size() == 1);
	const ClassSignature &cls = ir.class_signatures.front();
	REQUIRE((cls.name == "Point" && cls.is_struct && cls.native_base.empty()));
	REQUIRE((cls.fields.size() == 1 && cls.fields.front().name == "x"));
	REQUIRE((cls.methods.size() == 1 && cls.methods.front().name == "moved"));
	const auto signature = std::find_if(ir.signatures.begin(), ir.signatures.end(),
										[](const FunctionSignature &sig) { return sig.name == "use"; });
	REQUIRE(signature != ir.signatures.end());
	REQUIRE(signature->parameters.front().class_name == "Point");
	REQUIRE(signature->return_class_name == "Point");

	Compiler compiler;
	CompilerOptions options;
	options.extensions = true;
	options.output_elf = false;
	compiler.compile(
			"struct Point:\n\tvar x: int = 0\n\n"
			"@export var point: Point\n",
			options);
	REQUIRE(!compiler.get_error_info().has_error);
	const auto &properties = compiler.get_property_signatures();
	REQUIRE(properties.size() == 1);
	REQUIRE(properties.front().name == "point");
	REQUIRE(properties.front().class_name == "Point");
	REQUIRE(properties.front().hint_string == "Point");
}

TEST_CASE("struct program reaches riscv") {
	// The whole pipeline, including the optimizer -- which runs the IR verifier
	// between passes in a debug build -- and the backend.
	const std::string source = BANK_ACCOUNT +
			"var vault: BankAccount\n"
			"\n"
			"func deposit(account: BankAccount, amount):\n"
			"\taccount.balance += amount\n"
			"\treturn account.balance\n"
			"\n"
			"func test():\n"
			"\tvar account = BankAccount.new(balance = 100, loan = 50)\n"
			"\tdeposit(account, 5)\n"
			"\tvault.loan = account.loan\n"
			"\treturn account.balance + vault.loan\n";

	IRProgram ir = compile_to_ir(source, true);
	RISCVCodeGen riscv{ VariantLayout(false) };
	const std::vector<uint8_t> code = riscv.generate(ir);
	REQUIRE(!code.empty());
	REQUIRE(code.size() % 2 == 0);
}

TEST_CASE("non escaping struct is scalar replaced") {
	const IRProgram ir = compile_to_ir(
			"struct Point:\n\tvar x = 0\n\tvar y = 0\n\n"
			"func test():\n"
			"\tvar point = Point(1, 2)\n"
			"\tpoint.x = 7\n"
			"\treturn point.x + point.y\n",
			true);
	const IRFunction &test = find_function(ir, "test");
	REQUIRE(count_opcode(test, IROpcode::MAKE_DICTIONARY_KEYED) == 0);
	REQUIRE(count_opcode(test, IROpcode::DICT_GET_CONST) == 0);
	REQUIRE(count_opcode(test, IROpcode::DICT_SET_CONST) == 0);
}

TEST_CASE("struct scalar replacement kernel shapes") {
	const IRProgram ir = compile_to_ir(
			"struct Point:\n\tvar x: int = 1\n\tvar y: int = 2\n\n"
			"func read(n: int) -> int:\n"
			"\tvar p = Point()\n\tvar acc = 0\n\tvar i = 0\n"
			"\twhile i < n:\n\t\tacc += p.x + p.y\n\t\ti += 1\n"
			"\treturn acc\n\n"
			"func construct(n: int) -> int:\n"
			"\tvar acc = 0\n\tvar i = 0\n"
			"\twhile i < n:\n\t\tvar p = Point(i, i + 1)\n"
			"\t\tacc += p.x\n\t\ti += 1\n\treturn acc\n\n"
			"func escaped(n: int) -> int:\n"
			"\tvar points: Array[Point] = [Point()]\n"
			"\tvar p = points[0]\n\tvar acc = 0\n\tvar i = 0\n"
			"\twhile i < n:\n\t\tacc += p.x + p.y\n\t\ti += 1\n"
			"\treturn acc\n",
			true);

	for (const char *name : { "read", "construct" }) {
		const IRFunction &func = find_function(ir, name);
		REQUIRE(count_opcode(func, IROpcode::MAKE_DICTIONARY_KEYED) == 0);
		REQUIRE(count_opcode(func, IROpcode::DICT_GET_CONST) == 0);
	}
	const IRFunction &escaped = find_function(ir, "escaped");
	REQUIRE(count_opcode(escaped, IROpcode::MAKE_DICTIONARY_KEYED) == 1);
	REQUIRE(count_opcode(escaped, IROpcode::DICT_GET_CONST) == 2);

	std::cout << "  ✓ immutable loop structs are replaced and escaped structs stay materialized"
			  << std::endl;
}

TEST_CASE("struct method dispatch") {
	const std::string source =
			"struct S:\n"
			"\tvar x = 1\n"
			"\tfunc get_x():\n\t\treturn self.x\n"
			"\tfunc plus(a, b = 10):\n\t\treturn self.x + a + b\n"
			"\tstatic func unit(a = 7):\n\t\treturn a\n"
			"\tfunc size():\n\t\treturn 99\n"
			"\n";

	const IRProgram ir = compile_to_ir(source +
									   "func test():\n"
									   "\tvar s = S()\n"
									   "\treturn [s.get_x(), s.plus(1), S.unit(), s.unit(), s.size(), s.keys()]\n");
	const IRFunction &test = find_function(ir, "test");
	const auto found = calls(ir, test);
	// The receiver is the first argument, and a defaulted parameter is
	// materialized at the call site rather than travelling as an arity.
	REQUIRE((found == std::vector<std::pair<std::string, int64_t>>{ { "@S.get_x", 1 }, { "@S.plus", 3 },
																	// A `static func` has no receiver, whichever side of the dot it is
																	// reached from: passing one would shift every argument along.
																	{ "@S.unit", 1 },
																	{ "@S.unit", 1 },
																	// A declared method wins over the Dictionary method of the same name.
																	{ "@S.size", 1 } }));
	// `keys()` is not declared, so it stays the Dictionary's own operation.
	REQUIRE(count_dict_ops(test, 4) == 1);

	// The lifted function carries the synthetic receiver; a static one does not.
	REQUIRE((find_function(ir, "@S.get_x").parameters == std::vector<std::string>{ "self" }));
	REQUIRE((find_function(ir, "@S.plus").parameters ==
			 std::vector<std::string>{ "self", "a", "b" }));
	REQUIRE((find_function(ir, "@S.unit").parameters == std::vector<std::string>{ "a" }));

	// A method declared to answer `self` keeps the instance tracked, so a chain
	// stays a pair of direct calls and the field read needs no shape check.
	const IRProgram chained = compile_to_ir(
			"struct S:\n"
			"\tvar x = 1\n"
			"\tfunc bump() -> S:\n\t\tself.x += 1\n\t\treturn self\n"
			"\nfunc test():\n\treturn S().bump().bump().x\n");
	const IRFunction &chain = find_function(chained, "test");
	REQUIRE((calls(chained, chain) == std::vector<std::pair<std::string, int64_t>>{ { "@S.bump", 1 }, { "@S.bump", 1 } }));
	REQUIRE(count_opcode(chain, IROpcode::STRUCT_CHECK) == 0);
	REQUIRE(count_dict_gets(chain) == 1);

	// A method reached through a declared parameter or member is the same call.
	const IRProgram declared = compile_to_ir(source +
											 "var member: S\n"
											 "func through_member():\n\treturn member.get_x()\n"
											 "func through_parameter(s: S):\n\treturn s.get_x()\n");
	REQUIRE(calls(declared, find_function(declared, "through_member")).size() == 1);
	REQUIRE(calls(declared, find_function(declared, "through_parameter")).size() == 1);

	// Arity and receiver mistakes are compile errors, not run-time surprises.
	REQUIRE(rejection(source + "func test():\n\treturn S.get_x()\n")
					.find("one per instance") != std::string::npos);
	REQUIRE(rejection(source + "func test():\n\treturn S().plus()\n")
					.find("Missing argument 'a'") != std::string::npos);
	REQUIRE(rejection(source + "func test():\n\treturn S().plus(1, 2, 3)\n")
					.find("Too many arguments") != std::string::npos);
	REQUIRE(rejection(source + "func test():\n\treturn S().plus(a = 1)\n")
					.find("only supported when constructing a struct") != std::string::npos);
	REQUIRE(rejection("struct S:\n\tvar x = 1\n\tfunc a():\n\t\treturn 1\n\tfunc a():\n\t\treturn 2\n")
					.find("more than once") != std::string::npos);

	// A `static func` runs without an instance, so neither `self` nor a bare
	// field name is available inside it.
	REQUIRE(rejection("struct S:\n\tvar x = 1\n\tstatic func bad():\n\t\treturn self.x\n")
					.find("static") != std::string::npos);
	REQUIRE(rejection("struct S:\n\tvar x = 1\n\tstatic func bad():\n\t\treturn x\n")
					.find("static") != std::string::npos);
	// A field is one per instance; `static var` would have nowhere to live.
	REQUIRE(!rejection("struct S:\n\tstatic var x = 1\n").empty());
}

TEST_CASE("struct constants") {
	// A struct holds no storage of its own, so a constant folds at the use
	// site -- from a method, a field default, and from outside the struct.
	const IRProgram ir = compile_to_ir(
			"struct S:\n"
			"\tconst STEP = 3\n"
			"\tconst NAME = \"s\"\n"
			"\tvar x = S.STEP\n"
			"\tfunc advance():\n\t\tself.x += S.STEP\n\t\treturn self.x\n"
			"\nfunc test():\n\treturn S.STEP + S().x\n");
	const IRFunction &test = find_function(ir, "test");
	REQUIRE(count_dict_gets(test) == 1);
	REQUIRE((dictionary_int_values(test) == std::vector<int64_t>{ 3 }));
	// Folded: nothing loads the constant from anywhere at run time.
	REQUIRE(count_opcode(test, IROpcode::LOAD_GLOBAL) == 0);
	REQUIRE(count_opcode(find_function(ir, "@S.advance"), IROpcode::LOAD_GLOBAL) == 0);

	REQUIRE(rejection("struct S:\n\tconst STEP = 1\n\nfunc test():\n\treturn S.NOPE\n")
					.find("no constant named 'NOPE'") != std::string::npos);
	// A container constant has no compile-time value to fold to.
	REQUIRE(rejection("struct S:\n\tconst NAMES = [\"a\"]\n\tvar x = 1\n"
					  "\nfunc test():\n\treturn S.NAMES\n")
					.find("not a compile-time value") != std::string::npos);
	REQUIRE(!rejection("struct S:\n\tconst X = 1\n\tvar X = 2\n").empty());
	REQUIRE(!rejection("struct S:\n\tconst X = 1\n\tconst X = 2\n").empty());
	// Every struct member is shared, so `static` says nothing new about a const.
	REQUIRE(!rejection("struct S:\n\tstatic const X = 1\n").empty());
	REQUIRE(!rejection("struct S:\n\tconst X\n").empty());
}

// The shape check at a host boundary is a build setting: benchmarks turn it
// off, a restricted build cannot.
TEST_CASE("struct check levels") {
	const std::string source =
			"struct Point:\n"
			"\tvar x: int = 0\n"
			"\tvar name: String = \"\"\n"
			"\nfunc use(point: Point):\n\treturn point.x\n";

	auto generate = [&](bool enabled, bool deep) {
		CodeGenerator codegen;
		codegen.set_struct_checks(enabled, deep);
		Program parsed = parse(source);
		return codegen.generate(parsed);
	};

	const IRProgram off = generate(false, false);
	const IRFunction &unchecked = find_function(off, "use");
	REQUIRE(count_opcode(unchecked, IROpcode::STRUCT_CHECK) == 0);
	REQUIRE(count_opcode(unchecked, IROpcode::TYPE_TEST) == 0);
	REQUIRE(count_opcode(unchecked, IROpcode::THROW) == 0);
	// The declaration is still a promise: the field read is a direct one.
	REQUIRE(count_dict_gets(unchecked) == 1);

	const IRProgram shape = generate(true, false);
	const IRFunction &shaped = find_function(shape, "use");
	REQUIRE(count_opcode(shaped, IROpcode::STRUCT_CHECK) == 1);
	REQUIRE(count_opcode(shaped, IROpcode::TYPE_TEST) == 1);
	// The keys are checked, but nothing coerces the values behind them.
	REQUIRE(count_opcode(shaped, IROpcode::CONVERT) == 0);
	REQUIRE(count_dict_sets(shaped) == 0);

	// DEEP keeps the shape check and additionally walks the declared scalar
	// fields, reading each one and writing the coerced value back.
	const IRProgram deep = generate(true, true);
	const IRFunction &deeply = find_function(deep, "use");
	REQUIRE(count_opcode(deeply, IROpcode::STRUCT_CHECK) == 1);
	REQUIRE(count_opcode(deeply, IROpcode::TYPE_TEST) == 1);
	REQUIRE(count_dict_sets(deeply) == 2);
	REQUIRE(count_dict_gets(deeply) == 2 + 1); // the two fields, then the read
	// NOTE: the coercion itself only fires for a value whose type the compiler
	// already knows, and a Dictionary handed over by the host carries none. A
	// deep check that validates host-supplied field values would add a guard
	// here; this pins what is emitted today so that change is visible.
	REQUIRE(count_opcode(deeply, IROpcode::CONVERT) == 0);

	// Restricted source is a mod, not the project's: the host boundary keeps
	// its check whatever the build asked for.
	auto compiled = [&](bool restricted) {
		Compiler compiler;
		CompilerOptions options;
		options.extensions = true;
		options.restricted = restricted;
		options.struct_checks = CompilerOptions::StructChecks::OFF;
		const std::vector<uint8_t> elf = compiler.compile(source, options);
		REQUIRE(!compiler.get_error_info().has_error);
		const std::string bytes(elf.begin(), elf.end());
		return bytes.find("is not a Point") != std::string::npos;
	};
	REQUIRE(!compiled(false));
	REQUIRE(compiled(true));
}

TEST_CASE("typed dictionary values") {
	const std::string point = "struct Point:\n\tvar x = 0\n\n";
	// A Dictionary[K, Struct] promises the value type, so a lookup is tracked
	// the same way a typed Array element is.
	const IRProgram parameter = compile_to_ir(point +
											  "func first(points: Dictionary[String, Point]):\n\treturn points[\"a\"].x\n");
	// Two DICT_GET_CONST (key lookup + field) and no TYPE_TEST = tracked.
	auto tracked = [](const IRFunction &func) {
		return count_opcode(func, IROpcode::DICT_GET_CONST) == 2 &&
				count_opcode(func, IROpcode::TYPE_TEST) == 0 &&
				count_opcode(func, IROpcode::TYPE_TEST_MASK) == 0 &&
				count_opcode(func, IROpcode::VGET_INLINE) == 0;
	};

	const IRFunction &first = find_function(parameter, "first");
	REQUIRE(tracked(first));
	REQUIRE(count_opcode(first, IROpcode::STRUCT_CHECK) == 0);

	const IRProgram local = compile_to_ir(point +
										  "func first():\n"
										  "\tvar points: Dictionary[String, Point] = {}\n"
										  "\treturn points[\"a\"].x\n");
	REQUIRE(tracked(find_function(local, "first")));

	const IRProgram global_var = compile_to_ir(point +
											   "var points: Dictionary[String, Point] = {}\n\n"
											   "func first():\n\treturn points[\"a\"].x\n");
	REQUIRE(tracked(find_function(global_var, "first")));

	const IRProgram cast = compile_to_ir(point +
										 "func first(value):\n"
										 "\tvar points = value as Dictionary[String, Point]\n"
										 "\treturn points[\"a\"].x\n");
	REQUIRE(tracked(find_function(cast, "first")));

	// Every one of them checks field names, because the promise is the type.
	REQUIRE(rejects(point +
					"func first(points: Dictionary[String, Point]):\n\treturn points[\"a\"].missing\n"));
	REQUIRE(rejects(point +
					"func first(value):\n"
					"\tvar points = value as Dictionary[String, Point]\n"
					"\treturn points[\"a\"].missing\n"));
	// The key type is not the value type: only the second argument tracks, so a
	// struct in the key position leaves the looked-up value untracked.
	const IRProgram keyed = compile_to_ir(point +
										  "func first(points: Dictionary[Point, String]):\n\treturn points[\"a\"].x\n");
	REQUIRE(!tracked(find_function(keyed, "first")));
	// An Array element of the wrong struct is caught where it is appended.
	REQUIRE(rejects(point +
					"func fill():\n\tvar points: Array[Point] = []\n\tpoints.append(1)\n"));
}

// The host builds documentation and completion from the published tables, so
// what the source documents has to survive both codegen and the wire format.
TEST_CASE("struct documentation round trip") {
	const IRProgram ir = compile_with_doc_comments(
			"struct Tag:\n\tvar name: String = \"\"\n"
			"## A point on the grid.\n"
			"struct Point:\n"
			"\t## How far along.\n"
			"\tvar x: int = 0\n"
			"\tvar nested: Tag\n"
			"\tfunc moved(dx: int) -> Point:\n\t\treturn Point(self.x + dx)\n"
			"\tstatic func origin() -> Point:\n\t\treturn Point()\n");
	REQUIRE(ir.class_signatures.size() == 2);
	const ClassSignature &cls = ir.class_signatures.back();
	REQUIRE(cls.is_struct);
	REQUIRE(cls.description == "A point on the grid.");
	REQUIRE(cls.fields[0].description == "How far along.");
	REQUIRE(cls.fields.size() == 2);

	REQUIRE(cls.fields[0].class_name.empty());
	REQUIRE(cls.fields[1].description.empty());
	// A struct-typed field names its struct; the Variant type is still a
	// Dictionary, which is what the instance is.
	REQUIRE(cls.fields[1].class_name == "Tag");
	REQUIRE(cls.fields[1].type == int32_t(Variant::DICTIONARY));
	REQUIRE(cls.methods.size() == 2);
	REQUIRE((cls.methods[0].name == "moved" && !cls.methods[0].is_static));
	REQUIRE((cls.methods[1].name == "origin" && cls.methods[1].is_static));

	const std::vector<uint8_t> blob = encode_class_signatures(ir.class_signatures);
	std::vector<ClassSignature> decoded;
	REQUIRE(decode_class_signatures(blob.data(), blob.size(), decoded));
	REQUIRE(decoded.size() == 2);
	REQUIRE(decoded[1].is_struct == cls.is_struct);
	REQUIRE(decoded[1].description == cls.description);
	REQUIRE(decoded[1].fields.size() == cls.fields.size());
	REQUIRE(decoded[1].fields[0].description == cls.fields[0].description);
	REQUIRE(decoded[1].fields[1].class_name == cls.fields[1].class_name);
	REQUIRE(decoded[1].methods.size() == cls.methods.size());
	REQUIRE(decoded[1].methods[1].is_static);

	// The blob comes from a guest program: a truncated one has to fail the
	// decode rather than be read past its end.
	std::vector<ClassSignature> truncated;
	REQUIRE(!decode_class_signatures(blob.data(), blob.size() / 2, truncated));
	REQUIRE(truncated.empty());

	// The lifted method travels in the function table, named for its struct and
	// without the synthetic receiver, and struct-typed parameters name the
	// struct alongside the Dictionary they are.
	const IRProgram used = compile_to_ir(
			"struct Point:\n\tvar x: int = 0\n"
			"\tfunc moved(dx: int) -> Point:\n\t\treturn Point(self.x + dx)\n"
			"\nfunc use(point: Point) -> Point:\n\treturn point\n");
	const auto method = std::find_if(used.signatures.begin(), used.signatures.end(),
									 [](const FunctionSignature &sig) { return sig.name == "@Point.moved"; });
	REQUIRE(method != used.signatures.end());
	REQUIRE((method->parameters.size() == 1 && method->parameters[0].name == "dx"));
	REQUIRE(method->return_class_name == "Point");

	const std::vector<uint8_t> functions = encode_function_signatures(used.signatures);
	std::vector<FunctionSignature> decoded_functions;
	REQUIRE(decode_function_signatures(functions.data(), functions.size(), decoded_functions));
	const auto use = std::find_if(decoded_functions.begin(), decoded_functions.end(),
								  [](const FunctionSignature &sig) { return sig.name == "use"; });
	REQUIRE(use != decoded_functions.end());
	REQUIRE(use->return_class_name == "Point");
	REQUIRE((use->parameters.size() == 1 && use->parameters[0].class_name == "Point"));
	REQUIRE(use->parameters[0].type == int32_t(Variant::DICTIONARY));
}
