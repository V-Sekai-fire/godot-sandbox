#!/usr/bin/env bash
# Build the RISC-V guest and the host HTTP server, then start the server.
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ── 1. Cross-compile the RISC-V guest ─────────────────────────────
echo "=== Building RISC-V guest ==="
pushd "$SCRIPT_DIR/guest"
chmod +x build.sh
./build.sh
popd

# ── 2. Build the host server with CMake ───────────────────────────
echo "=== Building host server ==="
mkdir -p "$SCRIPT_DIR/.build"
pushd "$SCRIPT_DIR/.build"
cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DRISCV_FLAT_RW_ARENA=OFF \
    -DRISCV_32I=OFF
make -j"$(nproc)"
popd

# ── 3. Start the server ────────────────────────────────────────────
echo "=== Starting HTTP server ==="
echo "Open http://127.0.0.1:8080 in your browser, or:"
echo "  curl http://127.0.0.1:8080/"
echo "  curl http://127.0.0.1:8080/info"
echo "  curl http://127.0.0.1:8080/echo"
echo ""
"$SCRIPT_DIR/.build/http_server" "$SCRIPT_DIR/guest/request_handler"
