#pragma once
#include <cstdint>
#include <cstdio>
#define SB_UNLIKELY(x) __builtin_expect(!!(x), 0)
struct Obj { uint64_t v; };
extern Obj g_objs[64];
extern volatile uint64_t g_sink;
struct Machine { // stands in for riscv::Machine + Sandbox error slot
	bool stopped = false;
	uint32_t err_code = 0; uint64_t err_arg = 0;
	char msg[256];
	void stop() noexcept { stopped = true; }
};
enum { E_NULL = 1, E_NOT_SCOPED = 2 };
// Deferred, shared formatting: both designs pay it once per reported fault.
inline void format_error(Machine &m) {
	switch (m.err_code) {
		case E_NULL: snprintf(m.msg, sizeof m.msg, "Object is Null"); break;
		default: snprintf(m.msg, sizeof m.msg, "Object is not scoped: %lu", (unsigned long)m.err_arg);
	}
}
using Handler = void (*)(Machine &, uint64_t addr, int depth);
// boundary = what Sandbox::vmcall does around a syscall: returns true on fault
bool call_exc(Handler h, Machine &m, uint64_t addr, int depth);
bool call_macro(Handler h, Machine &m, uint64_t addr, int depth);
Handler get_exc_handler();
bool call_exc(Handler h, Machine &m, uint64_t addr, int depth);
