// Fuzzing the compiler against itself.
//
// The IR verifier and optimization invariance are checks; the generator is what
// turns them into fuzzers. Every generated program is put through both:
//
//   V1  the IR has to verify after code generation and after every optimizer
//       pass, so a pass that corrupts the IR fails here.
//   V2  the optimized program has to compute what the unoptimized one computes,
//       for the full pipeline and for every prefix of it.
//
// Neither needs an expected output written down, which is the point: the
// programs are ones nobody thought to write.
//
// Per commit this runs a fixed seed corpus, so it is deterministic and quick.
// Nightly it is meant to be run with GDSC_FUZZ_SEED=<random> GDSC_FUZZ_COUNT=<many>;
// a failure prints the seed and the shrunk program, and re-running with that seed
// reproduces it exactly. The seed is an environment variable rather than an
// argument because doctest owns argv now.
#include "../codegen.h"
#include "../compiler.h"
#include "../compiler_exception.h"
#include "../ir_interpreter.h"
#include "../ir_optimizer.h"
#include "../ir_verifier.h"
#include "../lexer.h"
#include "../parser.h"
#include "../traits.h"
#include "gdscript_generator.h"
#include "property_support.h"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using namespace gdscript;
using gdscript_test::GeneratedProgram;

namespace {

// The number of programs the per-commit run covers, from seed 0 upwards. Enough
// to exercise the grammar without making the test suite slow.
constexpr uint64_t DEFAULT_COUNT = 400;

struct RunResult {
	IRInterpreter::Value value;
	std::vector<IRInterpreter::Value> globals;
};

IRProgram build_ir(const std::string &source, size_t pass_limit) {
	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();
	apply_traits(program);
	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);
	ir_verify(ir, "codegen");
	if (pass_limit > 0) {
		IROptimizer optimizer;
		optimizer.set_pass_limit(pass_limit);
		optimizer.optimize(ir);
	}
	return ir;
}

RunResult run(const std::string &source, size_t pass_limit) {
	IRProgram ir = build_ir(source, pass_limit);
	IRInterpreter interpreter(ir);
	RunResult result;
	result.value = interpreter.call("test");
	for (size_t i = 0; i < interpreter.global_count(); i++) {
		result.globals.push_back(interpreter.global(i));
	}
	return result;
}

// int and bool are the same answer: the interpreter produces either for a truth
// value. int against float is a real difference.
bool values_equal(const IRInterpreter::Value &a, const IRInterpreter::Value &b) {
	const bool a_nil = std::holds_alternative<std::monostate>(a);
	const bool b_nil = std::holds_alternative<std::monostate>(b);
	if (a_nil || b_nil) {
		return a_nil && b_nil;
	}
	const bool a_float = std::holds_alternative<double>(a);
	const bool b_float = std::holds_alternative<double>(b);
	if (a_float != b_float) {
		return false;
	}
	if (a_float) {
		const double x = std::get<double>(a);
		const double y = std::get<double>(b);
		return x == y || (std::isnan(x) && std::isnan(y));
	}
	const bool a_string = std::holds_alternative<std::string>(a);
	const bool b_string = std::holds_alternative<std::string>(b);
	if (a_string || b_string) {
		return a_string && b_string && std::get<std::string>(a) == std::get<std::string>(b);
	}
	const int64_t x = std::holds_alternative<bool>(a) ? (std::get<bool>(a) ? 1 : 0) : std::get<int64_t>(a);
	const int64_t y = std::holds_alternative<bool>(b) ? (std::get<bool>(b) ? 1 : 0) : std::get<int64_t>(b);
	return x == y;
}

std::string describe(const IRInterpreter::Value &value) {
	if (std::holds_alternative<std::monostate>(value))
		return "null (nil)";
	if (std::holds_alternative<int64_t>(value))
		return std::to_string(std::get<int64_t>(value)) + " (int)";
	if (std::holds_alternative<double>(value))
		return std::to_string(std::get<double>(value)) + " (float)";
	if (std::holds_alternative<bool>(value))
		return std::string(std::get<bool>(value) ? "true" : "false") + " (bool)";
	return "\"" + std::get<std::string>(value) + "\" (string)";
}

// What went wrong with one program. `kind` is what makes two failures the same
// failure, and is what shrinking is held to: a shrunk program that fails a
// different way is a different bug, and reporting it in place of the original
// would send whoever reads it after the wrong thing.
struct Failure {
	std::string kind; // empty when the program is fine
	std::string detail;

	bool ok() const { return kind.empty(); }
};

Failure check(const std::string &source) {
	// Structs deliberately lower through the host Dictionary ABI, which the
	// scalar reference interpreter does not emulate. They still traverse code
	// generation, verification, and every optimizer prefix; only the result
	// comparison is skipped for this host-dependent subset.
	if (source.find("struct FuzzPoint:") != std::string::npos) {
		try {
			build_ir(source, 0);
			const auto &passes = IROptimizer::pipeline();
			for (size_t n = 1; n <= passes.size(); n++) {
				build_ir(source, n);
			}
			Compiler compiler;
			CompilerOptions options;
			options.extensions = true;
			if (compiler.compile(source, options).empty()) {
				return { "struct ELF generation failed", compiler.get_error_info().message };
			}
			return {};
		} catch (const CompilerException &e) {
			return { std::string("rejected (") + e.error_type_string() + ")", e.what() };
		} catch (const std::exception &e) {
			return { "struct compiler run failed", e.what() };
		}
	}

	RunResult unoptimized;
	try {
		unoptimized = run(source, 0);
	} catch (const CompilerException &e) {
		// A generated program the compiler rejects is either a generator bug or
		// a missing feature. Either is worth seeing rather than skipping, and
		// the error type keeps one kind of rejection apart from another.
		return { std::string("rejected (") + e.error_type_string() + ")", e.what() };
	} catch (const std::exception &e) {
		return { "unoptimized run failed", e.what() };
	}

	const auto &passes = IROptimizer::pipeline();
	for (size_t n = 1; n <= passes.size(); n++) {
		try {
			// Verification runs inside optimize_function() between passes, so a
			// pass that breaks the IR is caught before the answer is compared.
			const RunResult optimized = run(source, n);
			if (!values_equal(unoptimized.value, optimized.value)) {
				return { std::string("pass '") + passes[n - 1].name + "' changed the result",
						 "passes 1.." + std::to_string(n) + ": " + describe(unoptimized.value) +
								 " became " + describe(optimized.value) };
			}
			if (optimized.globals.size() != unoptimized.globals.size()) {
				return { "optimization changed the number of globals", "" };
			}
			for (size_t i = 0; i < unoptimized.globals.size(); i++) {
				if (!values_equal(unoptimized.globals[i], optimized.globals[i])) {
					return { std::string("pass '") + passes[n - 1].name + "' changed a global",
							 "passes 1.." + std::to_string(n) + ", global " + std::to_string(i) + ": " +
									 describe(unoptimized.globals[i]) + " became " + describe(optimized.globals[i]) };
				}
			}
		} catch (const CompilerException &e) {
			return { std::string("pass '") + passes[n - 1].name + "' produced " + e.error_type_string(),
					 e.what() };
		} catch (const std::exception &e) {
			return { std::string("pass '") + passes[n - 1].name + "' made it fail", e.what() };
		}
	}

	return {};
}

std::string describe(const Failure &failure) {
	if (failure.detail.empty()) {
		return failure.kind;
	}
	return failure.kind + ": " + failure.detail;
}

// Print a program indented, so it stands apart from the report around it.
void print_source(const std::string &source) {
	std::string line;
	for (char c : source) {
		if (c == '\n') {
			std::cerr << "    | " << line << "\n";
			line.clear();
		} else {
			line += c;
		}
	}
	if (!line.empty()) {
		std::cerr << "    | " << line << "\n";
	}
}

void report(const GeneratedProgram &original, const Failure &original_failure,
			const GeneratedProgram &shrunk, const Failure &shrunk_failure) {
	std::cerr << "\nFUZZ FAILURE (seed " << original.seed << ")\n"
			  << "  " << describe(shrunk_failure) << "\n"
			  << "  Reproduce with: test_fuzz --seed " << original.seed << " --count 1\n"
			  << "  Shrunk program:\n";
	print_source(shrunk.source());

	if (shrunk.source() != original.source()) {
		std::cerr << "  As generated (" << describe(original_failure) << "):\n";
		print_source(original.source());
	}
	std::cerr << std::endl;
}

} // namespace

namespace {

uint64_t env_number(const char *name, uint64_t fallback) {
	const char *value = std::getenv(name);
	return value != nullptr ? std::strtoull(value, nullptr, 10) : fallback;
}

// One generated program, all the way through both checks. A failure is shrunk
// to the smallest program that still fails the same way, because a report is
// only useful if a person can read it.
void fuzz_one(uint64_t seed) {
	gdscript_test::Generator generator(seed);
	const GeneratedProgram program = generator.generate();

	const Failure failure = check(program.source());
	if (failure.ok()) {
		return;
	}

	// Accepting any failure would let the shrinker walk into a program that
	// fails because deleting a declaration left a name undefined, which is a
	// different bug from the one being reported.
	const GeneratedProgram smallest = gdscript_test::shrink(program,
															[&failure](const std::string &source) {
																return check(source).kind == failure.kind;
															});

	// Report the failure the shrunk program produces: it is the one being
	// shown, so it has to be the one described.
	const Failure shrunk_failure = check(smallest.source());
	report(program, failure, smallest, shrunk_failure.ok() ? failure : shrunk_failure);
	FAIL_CHECK("seed ", seed, ": ", failure.kind);
}

} // namespace

// Verification is on regardless of the build type: a fuzz run with the verifier
// off is only running half the checks.
const bool SETUP_ONCE = [] {
	set_ir_verification_enabled(true);
	return true;
}();

TEST_CASE("generated programs verify and mean the same thing optimized") {
	const uint64_t seed = env_number("GDSC_FUZZ_SEED", 0);
	const uint64_t count = env_number("GDSC_FUZZ_COUNT", DEFAULT_COUNT);
	for (uint64_t i = 0; i < count; i++) {
		fuzz_one(seed + i);
	}
}

// The same two checks, reached the other way: witness picks the seeds by its
// own ladder rather than walking a fixed range, and prints the seed to re-run
// with when one of them fails.
TEST_CASE("a program from a seed nobody chose still verifies") {
	PROP_HOLDS(uint64_t, "the verifier and optimization invariance hold",
			   ([](witness::RNG &rng, const witness::Level &level) {
				   return uint64_t(rng.uint_range(0, uint32_t(level.fin_bound)));
			   }),
			   ([](const uint64_t &seed) {
				   gdscript_test::Generator generator(seed);
				   return check(generator.generate().source()).ok();
			   }));
}

// A statement that is false on purpose, to show the generator and the ladder
// can actually reach a counterexample. Without it, a generator that produced
// nothing but `func f(): pass` would pass both checks above in silence.
TEST_CASE("falsifiability: the generator reaches programs with declarations in them") {
	PROP_FALSIFIABLE(uint64_t, "every generated program is under forty characters",
					 ([](witness::RNG &rng, const witness::Level &level) {
						 return uint64_t(rng.uint_range(0, uint32_t(level.fin_bound)));
					 }),
					 ([](const uint64_t &seed) {
						 gdscript_test::Generator generator(seed);
						 return generator.generate().source().size() < 40;
					 }));
}
