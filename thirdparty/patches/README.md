# thirdparty/patches

Patches against upstream commits for vendored libraries in `modules/sandbox/thirdparty/`.

## libriscv

Upstream: `https://github.com/libriscv/libriscv` (`fwsGonzo/libriscv`, branch `master`)
Base commit: `6770061e9bbd14a4579a8fc728f63a13d582f36f` (recorded in `libriscv/.gitrepo`)

### Applying

```sh
cd modules/sandbox/thirdparty/libriscv
for p in ../patches/libriscv__*.patch; do patch -p4 < "$p"; done
```

(`-p4` strips `a/modules/sandbox/thirdparty/libriscv/` from the diff header.)

### Files patched

| Patch | Upstream path |
|-------|---------------|
| `libriscv__lib_libriscv_debug.cpp.patch` | `lib/libriscv/debug.cpp` |
| `libriscv__lib_libriscv_decoder_cache.cpp.patch` | `lib/libriscv/decoder_cache.cpp` |
| `libriscv__lib_libriscv_guest_datatypes.hpp.patch` | `lib/libriscv/guest_datatypes.hpp` |
| `libriscv__lib_libriscv_linux_syscalls_mman.cpp.patch` | `lib/libriscv/linux/syscalls_mman.cpp` |
| `libriscv__lib_libriscv_linux_system_calls.cpp.patch` | `lib/libriscv/linux/system_calls.cpp` |
| `libriscv__lib_libriscv_machine.cpp.patch` | `lib/libriscv/machine.cpp` |
| `libriscv__lib_libriscv_memory.cpp.patch` | `lib/libriscv/memory.cpp` |
| `libriscv__lib_libriscv_memory_helpers_paging.hpp.patch` | `lib/libriscv/memory_helpers_paging.hpp` |
| `libriscv__lib_libriscv_native_heap.hpp.patch` | `lib/libriscv/native_heap.hpp` |
| `libriscv__lib_libriscv_rva_instr.cpp.patch` | `lib/libriscv/rva_instr.cpp` |
| `libriscv__lib_libriscv_serialize.cpp.patch` | `lib/libriscv/serialize.cpp` |
| `libriscv__lib_libriscv_threads.hpp.patch` | `lib/libriscv/threads.hpp` |
| `libriscv__lib_libriscv_tr_emit.cpp.patch` | `lib/libriscv/tr_emit.cpp` |
| `libriscv__lib_libriscv_tr_translate.cpp.patch` | `lib/libriscv/tr_translate.cpp` |
| `libriscv__lib_libriscv_util_threadpool.h.patch` | `lib/libriscv/util/threadpool.h` |
| `libriscv__lib_libriscv_win32_dlfcn.cpp.patch` | `lib/libriscv/win32/dlfcn.cpp` |
| `libriscv__lib_libriscv_win32_dlfcn.h.patch` | `lib/libriscv/win32/dlfcn.h` |
| `libriscv__lib_libriscv_win32_epoll.cpp.patch` | `lib/libriscv/win32/epoll.cpp` |
| `libriscv__lib_libriscv_win32_system_calls.cpp.patch` | `lib/libriscv/win32/system_calls.cpp` |

### Notes

`decoded_exec_segment.hpp` was patched against `33e283e1` (zero-length allocation guard)
but upstream `6770061e` superseded it with sandbox hardening (`exlen + 8` allocation +
`if (exlen > 0)` guard). No local patch needed.
