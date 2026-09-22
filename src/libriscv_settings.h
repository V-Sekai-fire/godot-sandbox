#ifndef LIBRISCV_SETTINGS_H
#define LIBRISCV_SETTINGS_H

/*
 * These values are automatically set according to their cmake variables.
 */
/* #undef RISCV_DEBUG */
#define RISCV_EXT_A
#define RISCV_EXT_C
#define RISCV_EXT_V
/* #undef RISCV_32I */
#define RISCV_64I
/* #undef RISCV_128I */
/* #undef RISCV_FCSR */
/* #undef RISCV_EXPERIMENTAL */
/* #undef RISCV_MEMORY_TRAPS */
/* #undef RISCV_MULTIPROCESS */
#define RISCV_BINARY_TRANSLATION
/* On. The arena is one contiguous buffer, so every span the host views is
   sequential and memarray never has to fail. Snapshots no longer require
   turning it off: serialize_to emits the arena's pages like any other, and
   deserialize_from already rebuilds them as non-owning views into it. */
#define RISCV_FLAT_RW_ARENA
#define RISCV_VIRTUAL_PAGING
/* #undef RISCV_ENCOMPASSING_ARENA */
#define RISCV_THREADED
/* #undef RISCV_TAILCALL_DISPATCH */
/* #undef RISCV_LIBTCC */

/*
 * Version information.
 */
#define RISCV_VERSION_MAJOR 1
#define RISCV_VERSION_MINOR 11

#endif /* LIBRISCV_SETTINGS_H */
