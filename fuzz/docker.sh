#!/bin/bash
# Build and run a libriscv ELF loader fuzzer inside Docker, in normal
# libFuzzer mode (single process with live coverage output to the terminal).
#
# Usage:
#   ./docker.sh [elffuzzer32|elffuzzer64] [-- extra libfuzzer args]
#
# Examples:
#   ./docker.sh                          # elffuzzer64
#   ./docker.sh elffuzzer32              # 32-bit ELF loader
#   ./docker.sh elffuzzer64 -- -max_len=8192
#
# The corpus and any crash artifacts are persisted on the host under
# fuzz/corpus-<target>/ so progress survives container restarts.
set -e

TARGET="${1:-elffuzzer64}"

# Drop the target arg; anything after `--` is forwarded to libFuzzer.
shift $(( $# < 1 ? $# : 1 )) || true
EXTRA=()
if [ "${1:-}" = "--" ]; then shift; EXTRA=("$@"); fi

case "$TARGET" in
	elffuzzer32) SEEDS=(/app/fuzz/testcases   /app/fuzz/seeds32) ;;
	elffuzzer64) SEEDS=(/app/fuzz/testcases64 /app/fuzz/seeds64) ;;
	*) echo "Usage: $0 [elffuzzer32|elffuzzer64] [-- extra libfuzzer args]"; exit 1 ;;
esac

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
CORPUS="$HERE/corpus-$TARGET"
IMAGE=libriscv-fuzz

mkdir -p "$CORPUS"

echo ">>> Building $IMAGE image..."
docker build -t "$IMAGE" -f "$HERE/Dockerfile" "$ROOT"

echo ">>> Fuzzing $TARGET (corpus: $CORPUS)"
# handle_segv=0/handle_sigfpe=0: let libFuzzer own the signal handlers.
docker run --rm -it \
	-v "$CORPUS:/corpus" \
	-e ASAN_OPTIONS=disable_coredump=0:unmap_shadow_on_exit=1:handle_segv=0:handle_sigfpe=0 \
	"$IMAGE" \
	./"$TARGET" \
		-handle_fpe=0 \
		-max_len=131072 \
		-artifact_prefix=/corpus/ \
		"${EXTRA[@]}" \
		/corpus "${SEEDS[@]}"
