#!/usr/bin/env bash
# Compile packed_cases.gd, rewrite_cases.gd and bench.gd in the 2x2 matrix
# (rewrite x fast arrays) next to this script, and emit both rewritten GDScript
# texts (with and without fast arrays) for the interpreter runs. Then, from a
# Godot project that has the godot_sandbox addon (one with ECALL_PACKED_ACQUIRE)
# and these files:
#   godot --headless --path . --script res://run_packed.gd -- cases=packed \
#       ref=res://packed_cases.gd gd:gd_rw_fa=res://packed_cases_rw_fa.gd \
#       gd:gd_rw_nofa=res://packed_cases_rw_nofa.gd rw.fa=packed_cases.rw.fa.elf ...
# and the same with cases=rewrite and rewrite_cases*. Every column must say same.
# check_rewritten.gd runs bench.gd's rewritten texts as GDScript; run_bench.gd
# times one bench ELF against bench.gd.
#   build_matrix.sh <gdscript_to_riscv> [compiler flags, e.g. --double-precision]
set -eu
G=$1; shift
cd "$(dirname "$0")"
for src in packed_cases rewrite_cases bench; do
	for rw in rewrite no-rewrite; do
		for fa in fast-arrays no-fast-arrays; do
			tag=$([ $rw = rewrite ] && echo rw || echo norw).$([ $fa = fast-arrays ] && echo fa || echo nofa)
			"$G" --$rw --$fa "$@" -o "$src.$tag.elf" "$src.gd"
		done
	done
	"$G" --rewrite --fast-arrays --emit-rewritten "${src}_rw_fa.gd" "$@" -o /dev/null "$src.gd" 2>/dev/null
	"$G" --rewrite --no-fast-arrays --emit-rewritten "${src}_rw_nofa.gd" "$@" -o /dev/null "$src.gd" 2>/dev/null
done
