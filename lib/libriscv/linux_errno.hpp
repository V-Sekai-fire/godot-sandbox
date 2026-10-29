#pragma once
#include <cerrno>

namespace riscv {
// Guests read Linux errno numbers. The Windows C runtime numbers its errno
// values like Linux up to ERANGE (34) and differently after it; ENOSYS is 40
// there and 38 on Linux, so a guest told "not implemented" saw ELOOP, and
// glibc aborts a thread whose rseq registration fails with anything but
// ENOSYS. Every host errno handed to a guest goes through this.
inline int linux_errno(int e) {
#ifdef _WIN32
	switch (e) {
	case EDEADLK: return 35;
	case ENAMETOOLONG: return 36;
	case ENOLCK: return 37;
	case ENOSYS: return 38;
	case ENOTEMPTY: return 39;
	case EILSEQ: return 84;
	default: return e;
	}
#else
	return e;
#endif
}
} // namespace riscv
