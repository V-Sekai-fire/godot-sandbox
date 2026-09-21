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
/* Undefined so a guest can be snapshotted: Memory::serialize_to throws
   FEATURE_DISABLED under a flat read-write arena. The arena is what gives
   zero-copy host-guest calls, so this trades that for save_state(). Measure
   the ecall round-trip before assuming the trade is worth it. */
/* #undef RISCV_FLAT_RW_ARENA */
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
