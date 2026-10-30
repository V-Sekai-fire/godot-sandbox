#include <stdio.h>
#include "../../tests/unit/include/native_libc.h"
static inline void halt(int code) {
	register int a0 asm("a0") = code;
	// SYSTEM with 0x7FF imm
	asm volatile (".insn i SYSTEM, 0, x0, x0, 0x7ff" : : "r"(a0) : "memory");
}
void fast_exit(int code) {
	halt(code);
}

int main()
{
	printf("Hello, World!\n");
	fast_exit(42);
}

int test(int val, const char* str)
{
	printf("Test: val=%d, str=%s\n", val, str);
	fflush(stdout);
	return 12345;
}

int do_nothing()
{
	return 0;
}
