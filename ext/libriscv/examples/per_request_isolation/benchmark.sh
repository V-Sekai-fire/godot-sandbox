#!/usr/bin/env bash
# Benchmark the per_request_isolation HTTP server.
#
# Starts the server, warms up, fires 1000 sequential requests from a
# Python HTTP client (no subprocess-per-request overhead), then prints:
#   - client-side end-to-end average (incl. TCP round-trip)
#   - server-side fork+run+destroy average parsed from the server log

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SERVER="$SCRIPT_DIR/.build/http_server"
GUEST="$SCRIPT_DIR/guest/request_handler"
PORT=8080
WARMUP=100
REQUESTS=2000
URL="http://127.0.0.1:$PORT/"

# ── sanity checks ─────────────────────────────────────────────────
if [[ ! -x "$SERVER" ]]; then
    echo "ERROR: $SERVER not found. Run ./build_and_run.sh first." >&2
    exit 1
fi
if [[ ! -f "$GUEST" ]]; then
    echo "ERROR: $GUEST not found. Run ./build_and_run.sh first." >&2
    exit 1
fi
if ! command -v python3 &>/dev/null; then
    echo "ERROR: python3 is required for timing." >&2
    exit 1
fi

# ── start server ──────────────────────────────────────────────────
LOG=$(mktemp /tmp/libriscv_bench.XXXXXX)
cleanup() {
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    rm -f "$LOG"
}
trap cleanup EXIT

# stdbuf -oL forces line-buffered stdout so timing lines reach the log
# file before we parse it, even though stdout is redirected.
stdbuf -oL "$SERVER" "$GUEST" >"$LOG" 2>&1 &
SERVER_PID=$!

# Wait up to 3 s for the server to accept connections.
READY=0
for _ in $(seq 30); do
    if curl -sf "$URL" >/dev/null 2>&1; then
        READY=1
        break
    fi
    sleep 0.1
done
if [[ $READY -eq 0 ]]; then
    echo "ERROR: server did not become ready in 3 s." >&2
    cat "$LOG" >&2
    exit 1
fi

# ── warmup ────────────────────────────────────────────────────────
printf "Warming up (%d requests)...\n" "$WARMUP"
for _ in $(seq "$WARMUP"); do
    curl -sf "$URL" >/dev/null
done

# ── benchmark ─────────────────────────────────────────────────────
printf "Benchmarking (%d sequential requests)...\n\n" "$REQUESTS"

python3 - "$PORT" "$REQUESTS" "$LOG" "$WARMUP" <<'PYEOF'
import http.client, time, sys, re

port     = int(sys.argv[1])
n        = int(sys.argv[2])
logfile  = sys.argv[3]
warmup   = int(sys.argv[4])

def do_request(conn):
    conn.request("GET", "/")
    r = conn.getresponse()
    r.read()

# measure n sequential requests — one new TCP connection each
# (Connection: close is set by the server)
t0 = time.perf_counter_ns()
for _ in range(n):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
    do_request(conn)
    conn.close()
t1 = time.perf_counter_ns()

total_ns  = t1 - t0
avg_us    = total_ns / n / 1_000
total_ms  = total_ns / 1_000_000
throughput = n / (total_ns / 1_000_000_000)

print("─" * 44)
print("  Client-side (end-to-end, incl. TCP)")
print("─" * 44)
print(f"  Requests          {n}")
print(f"  Total time        {total_ms:.1f} ms")
print(f"  Avg per request   {avg_us:.1f} µs")
print(f"  Throughput        {throughput:.0f} req/s")

# ── parse server-side fork+run+destroy lines ──────────────────────
with open(logfile) as f:
    lines = f.read()

# pattern: "fork+run+destroy: 59 µs  (run=52 µs)"
total_times = [int(m) for m in re.findall(r'fork\+run\+destroy: (\d+)', lines)]
run_times   = [int(m) for m in re.findall(r'run=(\d+)',               lines)]

# skip warmup samples
bench_total = total_times[warmup:]
bench_run   = run_times[warmup:]

if bench_total:
    s = sorted(bench_total)
    avg_t = sum(s) / len(s)
    p50_t = s[len(s) // 2]
    p99_t = s[max(0, int(len(s) * 0.99) - 1)]

    avg_r = sum(bench_run) / len(bench_run) if bench_run else 0

    print()
    print("─" * 44)
    print("  Server-side (fork + run + destroy)")
    print("─" * 44)
    print(f"  Samples           {len(bench_total)}")
    print(f"  Avg total         {avg_t:.1f} µs")
    print(f"  Avg run-only      {avg_r:.1f} µs")
    print(f"  Min / p50 / p99   {s[0]} / {p50_t} / {p99_t} µs")
    print(f"  Max               {s[-1]} µs")
else:
    print("\n  (no server-side timing data — expected ~%d lines)" % n)
print("─" * 44)
PYEOF
