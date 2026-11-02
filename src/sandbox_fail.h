#pragma once
// Exception-free error path for syscall helpers.
//
// The engine is built -fno-exceptions and only the sandbox sources use -fexceptions, so a
// refused call today unwinds through every helper frame up to Sandbox::vmcall. These macros
// record the error, stop the machine and return a failure value instead; the caller checks
// the return value. Measured in tests/bench_error_path: 40x to 160x faster per fault, and no
// slower on the path with no faults.
//
//   [[nodiscard]] godot::Object *get_object(Sandbox &s, uint64_t addr) {
//       if (UNLIKELY(addr == 0)) SB_FAIL(s, "Object is Null", nullptr);
//       ...
//   }
//   void api_foo(Machine &m) {
//       SB_TRY(obj, get_object(sandbox, addr), );   // returns early if get_object failed
//   }
//
// The message is built once, at the vmcall boundary, by Sandbox::take_failure().

#include <cstdint>

#ifndef SB_UNLIKELY
#define SB_UNLIKELY(x) __builtin_expect(!!(x), 0)
#endif

// `state` is anything with record_failure(const char *) and a machine() that has stop().
#define SB_FAIL(state, message, retval)       \
	do {                                      \
		(state).record_failure(message);      \
		(state).machine().stop();             \
		return retval;                        \
	} while (0)

// Propagate a callee's failure: the callee returned null and already recorded why.
#define SB_TRY(var, expr, retval) \
	auto *var = (expr);           \
	if (SB_UNLIKELY(!var)) return retval
