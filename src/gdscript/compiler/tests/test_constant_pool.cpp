#include "../codegen.h"
#include "../elf_builder.h"
#include "../lexer.h"
#include "../parser.h"
#include "../riscv_codegen.h"
#include "witness/doctest.h"
#include <iostream>

using namespace gdscript;

TEST_CASE("64bit constant pool") {
	std::string source = R"(
func test():
	var x = 1311768467463790320
	return x
)";

	Lexer lexer(source);
	auto tokens = lexer.tokenize();

	Parser parser(tokens);
	Program program = parser.parse();

	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);

	RISCVCodeGen riscv;
	auto code = riscv.generate(ir);
	auto const_pool = riscv.get_constant_pool();

	// Verify constant pool contains our large constant
	REQUIRE(const_pool.size() == 1);
	REQUIRE(const_pool[0] == 1311768467463790320LL);

	// Build ELF and verify it includes the constant pool
	ElfBuilder elf;
	auto elf_data = elf.build(ir);

	// ELF should be larger than just the code (includes headers + constant pool)
	REQUIRE(elf_data.size() > code.size());

	std::cout << "    - Code size: " << code.size() << " bytes" << std::endl;
	std::cout << "    - Constant pool: " << const_pool.size() << " constants" << std::endl;
	std::cout << "    - ELF size: " << elf_data.size() << " bytes" << std::endl;
}

TEST_CASE("multiple constants") {
	std::string source = R"(
func test():
	var a = 1311768467463790320
	var b = 5876543210123456789
	var c = 1234567890123456789
	return a + b + c
)";

	Lexer lexer(source);
	auto tokens = lexer.tokenize();

	Parser parser(tokens);
	Program program = parser.parse();

	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);

	RISCVCodeGen riscv;
	auto code = riscv.generate(ir);
	auto const_pool = riscv.get_constant_pool();

	// Should have 3 constants
	REQUIRE(const_pool.size() == 3);
	REQUIRE(const_pool[0] == 1311768467463790320LL);
	REQUIRE(const_pool[1] == 5876543210123456789LL);
	REQUIRE(const_pool[2] == 1234567890123456789LL);

	std::cout << "    - Constant pool: " << const_pool.size() << " constants" << std::endl;
}

TEST_CASE("constant deduplication") {
	std::string source = R"(
func test():
	var a = 1311768467463790320
	var b = 1311768467463790320
	var c = 1311768467463790320
	return a + b + c
)";

	Lexer lexer(source);
	auto tokens = lexer.tokenize();

	Parser parser(tokens);
	Program program = parser.parse();

	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);

	RISCVCodeGen riscv;
	auto code = riscv.generate(ir);
	auto const_pool = riscv.get_constant_pool();

	// Should only have 1 constant (deduplicated)
	REQUIRE(const_pool.size() == 1);
	REQUIRE(const_pool[0] == 1311768467463790320LL);

	std::cout << "    - Deduplicated 3 references to 1 constant" << std::endl;
}

TEST_CASE("small constants not pooled") {
	std::string source = R"(
func test():
	var a = 42
	var b = 1000
	var c = -500
	return a + b + c
)";

	Lexer lexer(source);
	auto tokens = lexer.tokenize();

	Parser parser(tokens);
	Program program = parser.parse();

	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);

	RISCVCodeGen riscv;
	auto code = riscv.generate(ir);
	auto const_pool = riscv.get_constant_pool();

	// Small constants should not be in pool
	REQUIRE(const_pool.size() == 0);
}

TEST_CASE("float constants") {
	std::string source = R"(
func test():
	var a = 3.14159
	var b = 2.71828
	return a + b
)";

	Lexer lexer(source);
	auto tokens = lexer.tokenize();

	Parser parser(tokens);
	Program program = parser.parse();

	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);

	RISCVCodeGen riscv;
	auto code = riscv.generate(ir);
	auto const_pool = riscv.get_constant_pool();

	// Float constants are stored as int64 bit patterns
	// They should be in the constant pool
	REQUIRE(const_pool.size() == 2);

	// Verify the bit patterns match the float values
	union {
		int64_t i;
		double d;
	} conv;
	conv.i = const_pool[0];
	REQUIRE((conv.d > 3.14 && conv.d < 3.15)); // Approximately 3.14159
	conv.i = const_pool[1];
	REQUIRE((conv.d > 2.71 && conv.d < 2.72)); // Approximately 2.71828

	std::cout << "    - Float constants stored as int64 bit patterns" << std::endl;
}
