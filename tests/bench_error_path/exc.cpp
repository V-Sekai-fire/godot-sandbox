// compiled with -fexceptions: today's design (throw in helper, catch at vmcall)
#include "common.h"
#include <stdexcept>
Obj g_objs[64]; volatile uint64_t g_sink;
[[gnu::noinline]] static Obj *get_object(const Machine &m, uint64_t addr, int depth) {
	if (depth == 0) {
		if (SB_UNLIKELY(addr == 0)) {
			char buf[256];
			snprintf(buf, sizeof buf, "Object is not scoped: %lu", (unsigned long)addr);
			throw std::runtime_error(buf);
		}
		return &g_objs[addr & 63];
	}
	Obj *r = get_object(m, addr, depth - 1);
	asm volatile("" ::: "memory");
	return r;
}
[[gnu::noinline]] static void handler(Machine &m, uint64_t addr, int depth) {
	Obj *o = get_object(m, addr, depth);
	g_sink = g_sink + o->v;
}
Handler get_exc_handler() { return handler; }
bool call_exc(Handler h, Machine &m, uint64_t addr, int depth) {
	try { h(m, addr, depth); }
	catch (const std::exception &e) {
		snprintf(m.msg, sizeof m.msg, "%s", e.what());
		return true;
	}
	return false;
}
