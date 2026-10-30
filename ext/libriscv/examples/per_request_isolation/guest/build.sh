#!/usr/bin/env bash
# Cross-compile the RISC-V guest program.
# Requires a riscv64-linux-gnu-gcc toolchain.
set -e

RISCV_CC="${RISCV_CC:-riscv64-linux-gnu-gcc-12}"

# Fallback: try the generic cross-compiler name.
if ! command -v "$RISCV_CC" &>/dev/null; then
    RISCV_CC="riscv64-linux-gnu-gcc"
fi

if ! command -v "$RISCV_CC" &>/dev/null; then
    echo "ERROR: RISC-V cross-compiler not found." >&2
    echo "Install with: sudo apt install gcc-12-riscv64-linux-gnu" >&2
    exit 1
fi

"$RISCV_CC" \
    -static \
    -O2 \
    -march=rv64gc \
    -mabi=lp64d \
    request_handler.c \
    -o request_handler

echo "Built: $(pwd)/request_handler"
