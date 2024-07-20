#include "syscalls.h"
#include <cstddef>
#include <string_view>
#include <cstdio>
#include "variant.hpp"
#include <span>

template <typename T>
using remove_cvref = std::remove_cv_t<std::remove_reference_t<T>>;

/// --> Fucking ugly side <-- ///

extern "C" __attribute__((used, retain, noreturn)) void fast_exit()
{
	asm (".insn i SYSTEM, 0, x0, x0, 0x7ff");
	__builtin_unreachable();
}

MAKE_SYSCALL(ECALL_PRINT, void, sys_print, const Variant*, size_t);

/// --> Nice side <-- ///

struct UtilityFunctions
{
	template <typename... Args>
	static void print(Args&&... vars)
	{
		std::array<Variant, sizeof...(Args)> vptrs;
		int idx = 0;
		([&] {
			if constexpr (std::is_same_v<Variant, remove_cvref<Args>>)
				vptrs[idx++] = vars;
			else
				vptrs[idx++] = Variant(vars);
		}(), ...);
		sys_print(vptrs.data(), vptrs.size());
	}
};

#include "variant.hpp"

int main()
{
	UtilityFunctions::print("Hello, ", 55, " world!\n");

	// do shit here...

	fast_exit();
}

extern "C"
void my_function(std::span<Variant> args)
{
	UtilityFunctions::print("Hello, ", 124.5, " world!\n");
}

extern "C"
void function3(std::span<Variant> args)
{
	UtilityFunctions::print("x = ", args[0], " y = ", args[1], " text = ", args[2]);
	//UtilityFunctions::print("x = ", args[0], " y = ", args[1]);
}

extern "C"
void final_function(std::span<Variant> args)
{
	UtilityFunctions::print("The function was called!!\n");
}
extern "C"
void trampoline_function(std::span<Variant> args)
{
	UtilityFunctions::print("Trampoline is calling first argument...\n");
	args[0].call("Hello World!");
	UtilityFunctions::print("First argument called!\n");
}
