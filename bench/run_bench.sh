#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 4 ]]; then
  echo "Usage: $0 <host> <port> <username> <password> [result_dir]"
  echo "Example: $0 127.0.0.1 8080 testuser testpass"
  exit 1
fi

HOST="$1"
PORT="$2"
USER="$3"
PASS="$4"
OUT_DIR="${5:-results/$(date -u +%Y%m%dT%H%M%SZ)}"

mkdir -p "$OUT_DIR"

fail() {
  echo "[ERROR] $1" >&2
  exit 1
}

echo "[precheck] Checking server health..."
if ! curl -fsS --max-time 3 "http://$HOST:$PORT/api/health" >/dev/null; then
  fail "health check failed: http://$HOST:$PORT/api/health is unreachable"
fi

echo "[1/4] Building benchmark binaries..."
cmake -S . -B build >/dev/null
cmake --build build -j"$(nproc)" >/dev/null

echo "[2/4] Running bench_login..."
if ! ./build/bench_login "$HOST" "$PORT" 16 1000 "$USER" "$PASS" \
  --csv-out "$OUT_DIR/login.csv" \
  >"$OUT_DIR/login.txt"; then
  fail "bench_login failed. See: $OUT_DIR/login.txt"
fi

echo "[3/4] Running bench_db..."
if ! ./build/bench_db "$HOST" "$PORT" "$USER" "$PASS" \
  --duration 20 \
  --stages 10,50,100,200 \
  --csv-out "$OUT_DIR/db.csv" \
  >"$OUT_DIR/db_stdout.csv" 2>"$OUT_DIR/db_stderr.log"; then
  echo "[ERROR] bench_db failed. stderr:" >&2
  tail -n 50 "$OUT_DIR/db_stderr.log" >&2 || true
  fail "See logs: $OUT_DIR/db_stderr.log and $OUT_DIR/db_stdout.csv"
fi

echo "[4/4] Running bench_sse..."
if ! ./build/bench_sse "$HOST" "$PORT" 100 \
  --stages 100,300,500 \
  --duration 30 \
  --model claude-sonnet-4-6 \
  --csv-out "$OUT_DIR/sse.csv" \
  --json-out "$OUT_DIR/sse.json" \
  >"$OUT_DIR/sse.txt"; then
  fail "bench_sse failed. See: $OUT_DIR/sse.txt"
fi

echo "Done. Results saved to: $OUT_DIR"
