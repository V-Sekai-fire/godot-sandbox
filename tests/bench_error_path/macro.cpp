// compiled with -fno-exceptions (as the engine is): macro design
#include "common.h"
#if defined(__cpp_exceptions)
#error "macro.cpp must build without exceptions"
#endif
// SB_FAIL records the error, stops the machine, and returns the failure value.
// V0: inline record. V1: record in a cold out-of-line function.
[[gnu::noinline, gnu::cold]] static void sb_fail_cold(Machine &m, uint32_t code, uint64_t arg) {
	m.err_code = code; m.err_arg = arg; m.stop();
}
#if SB_VARIANT == 0
#define SB_FAIL(m, code, arg, retval) do { (m).err_code = (code); (m).err_arg = (arg); (m).stop(); return retval; } while (0)
#else
#define SB_FAIL(m, code, arg, retval) do { sb_fail_cold((m), (code), (arg)); return retval; } while (0)
#endif
// Propagate a helper's failure without unwinding: helper returned null, error is already in m.
#define SB_TRY(var, expr, retval) auto *var = (expr); if (SB_UNLIKELY(!var)) return retval
[[gnu::noinline]] static Obj *get_object(Machine &m, uint64_t addr, int depth) {
	if (depth == 0) {
		if (SB_UNLIKELY(addr == 0)) SB_FAIL(m, E_NOT_SCOPED, addr, nullptr);
		return &g_objs[addr & 63];
	}
	SB_TRY(r, get_object(m, addr, depth - 1), nullptr);
	asm volatile("" ::: "memory");
	return r;
}
[[gnu::noinline]] static void handler(Machine &m, uint64_t addr, int depth) {
	SB_TRY(o, get_object(m, addr, depth), );
	g_sink = g_sink + o->v;
}
#if SB_VARIANT == 0
Handler get_macro_handler0() { return handler; }
bool call_macro0(Handler h, Machine &m, uint64_t a, int d) {
	h(m, a, d);
	if (SB_UNLIKELY(m.stopped)) { format_error(m); return true; }
	return false;
}
#else
Handler get_macro_handler1() { return handler; }
bool call_macro1(Handler h, Machine &m, uint64_t a, int d) {
	h(m, a, d);
	if (SB_UNLIKELY(m.stopped)) { format_error(m); return true; }
	return false;
}
#endif
