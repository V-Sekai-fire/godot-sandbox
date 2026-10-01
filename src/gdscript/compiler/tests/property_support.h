// Shared scaffolding for the property and falsifiability tests.
//
// Three kinds of check live in this suite:
//
//   unit           a named case with a written-down input and expected output
//   property       PROP_HOLDS: a statement that has to hold for every program
//                  witness-cpp's ladder can generate
//   falsifiability PROP_FALSIFIABLE: a statement that is deliberately false,
//                  which the same generator and ladder have to catch
//
// The third is what keeps the second honest. A property over a generator that
// only ever produces one trivial program passes while testing nothing; the
// paired falsifiability case fails in that situation, because a generator that
// cannot reach a counterexample cannot find the planted one either.
#pragma once

#include "witness/doctest.h"

#include "../compiler.h"
#include "../ir_interpreter.h"

#include <cstdint>
#include <string>
#include <variant>

namespace gdscript_property {

// Compiling a program per trial costs milliseconds, so the compile-backed
// properties walk a shorter ladder than witness-cpp's default 3000 trials.
// Two rungs, 120 programs: enough for a generator to reach the interesting
// shapes, quick enough to run on every commit.
inline constexpr witness::Level COMPILE_LADDER[2] = {
	{ 0, 64, 256, 40 },
	{ 1, 512, 1024, 80 },
};

// Compile a source and call one of its functions through the IR interpreter.
// Returns false when the source does not compile, which a property states
// explicitly rather than silently passing.
inline bool call_int(const std::string &source, const std::string &function,
					 const std::vector<gdscript::IRInterpreter::Value> &args, int64_t &out) {
	gdscript::Compiler compiler;
	gdscript::CompilerOptions options;
	options.output_elf = false;
	auto ir = compiler.compile_to_ir(source, options);
	if (!ir.has_value()) {
		return false;
	}
	gdscript::IRInterpreter interpreter(*ir);
	const gdscript::IRInterpreter::Value result = interpreter.call(function, args);
	if (!std::holds_alternative<int64_t>(result)) {
		return false;
	}
	out = std::get<int64_t>(result);
	return true;
}

inline bool compiles(const std::string &source) {
	gdscript::Compiler compiler;
	gdscript::CompilerOptions options;
	options.output_elf = false;
	return compiler.compile_to_ir(source, options).has_value();
}

} // namespace gdscript_property

// A property over the short compile ladder. Fails if a counterexample turns up.
#define PROP_HOLDS(m_T, m_query, m_gen, m_pred)                                     \
	do {                                                                            \
		::witness::Generator<m_T> _gen = m_gen;                                     \
		::std::function<bool(const m_T &)> _pred = m_pred;                          \
		::witness::Shrinker<m_T> _shrink = &::witness::no_shrink<m_T>;              \
		::std::function<void(::std::ostream &, const m_T &)> _print;                \
		::witness::Trial _trial = ::witness::resolve_with_ladder<m_T>(              \
				m_query, ::gdscript_property::COMPILE_LADDER, _gen, _pred, _shrink, \
				_print);                                                            \
		INFO(_trial.message);                                                       \
		CHECK(_trial.outcome != ::witness::Outcome::FOUND);                         \
	} while (false)

// A statement that is false on purpose. The same generator and ladder have to
// produce a counterexample, or the property paired with it proves nothing.
#define PROP_FALSIFIABLE(m_T, m_query, m_gen, m_pred)                                       \
	do {                                                                                    \
		::witness::Generator<m_T> _gen = m_gen;                                             \
		::std::function<bool(const m_T &)> _pred = m_pred;                                  \
		::witness::Shrinker<m_T> _shrink = &::witness::no_shrink<m_T>;                      \
		::std::function<void(::std::ostream &, const m_T &)> _print;                        \
		::witness::Trial _trial = ::witness::resolve_with_ladder<m_T>(                      \
				m_query, ::gdscript_property::COMPILE_LADDER, _gen, _pred, _shrink,         \
				_print);                                                                    \
		CHECK_MESSAGE(_trial.outcome == ::witness::Outcome::FOUND,                          \
					  "nothing falsified a statement that is false: the generator cannot "  \
					  "reach a counterexample, so the property beside it holds vacuously"); \
	} while (false)
