#!/usr/bin/env bash
# Nyalakan server C (dengan --stats-csv) dan gateway sekaligus; Ctrl+C mematikan keduanya.
#
#   bash ui/scripts/run_demo.sh
#   MAX_CLIENTS=2 bash ui/scripts/run_demo.sh      # uji 503 dengan 3 tab
#
# Variabel lingkungan (semua opsional):
#   PORT=8080  SERVER_PORT=8888  MAX_CLIENTS=<N>  NO_PACING=1
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
UI="$ROOT/ui"
PORT="${PORT:-8080}"
SERVER_PORT="${SERVER_PORT:-8888}"
STATS_CSV="$(mktemp /tmp/podflow_stats.XXXXXX.csv)"

# Python: pakai venv ui/.venv bila ada (WSL Ubuntu 24 memblokir pip global).
PY="$UI/.venv/bin/python"
[ -x "$PY" ] || PY=python3
"$PY" -c "import aiohttp" 2>/dev/null || {
    echo "aiohttp belum terpasang. Jalankan:  make -C ui deps" >&2; exit 1; }

port_busy() { (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null; }
for p in "$PORT" "$SERVER_PORT"; do
    if port_busy "$p"; then echo "port $p sudah dipakai proses lain" >&2; exit 1; fi
done

cd "$ROOT"                       # server membaca ./media dari cwd
[ -x ./server ] || make
ls media/*.wav >/dev/null 2>&1 || bash media/generate_wav.sh

SERVER_PID="" GW_PID=""
cleanup() {
    trap - EXIT INT TERM
    echo; echo "[demo] mematikan gateway dan server..."
    [ -n "$GW_PID" ]     && kill "$GW_PID"     2>/dev/null || true
    [ -n "$SERVER_PID" ] && kill "$SERVER_PID" 2>/dev/null || true   # SIGTERM = shutdown rapi
    wait 2>/dev/null || true
    rm -f "$STATS_CSV"
}
trap cleanup EXIT INT TERM

ARGS=("$SERVER_PORT" --stats-csv "$STATS_CSV")
[ -n "${MAX_CLIENTS:-}" ] && ARGS+=(--max-clients "$MAX_CLIENTS")
[ -n "${NO_PACING:-}" ]   && ARGS+=(--no-pacing)

./server "${ARGS[@]}" &
SERVER_PID=$!

for _ in $(seq 1 50); do                      # tunggu server siap (maks 5 dtk)
    port_busy "$SERVER_PORT" && break
    kill -0 "$SERVER_PID" 2>/dev/null || { echo "server C berhenti mendadak" >&2; exit 1; }
    sleep 0.1
done
port_busy "$SERVER_PORT" || { echo "server C tidak kunjung listen" >&2; exit 1; }

"$PY" "$UI/gateway/gateway.py" --port "$PORT" \
    --server-host 127.0.0.1 --server-port "$SERVER_PORT" \
    --stats-csv "$STATS_CSV" --server-pid "$SERVER_PID" &
GW_PID=$!

echo "[demo] server pid=$SERVER_PID (port $SERVER_PORT) | gateway pid=$GW_PID"
echo "[demo] buka http://localhost:$PORT   (Ctrl+C untuk berhenti)"
wait -n "$SERVER_PID" "$GW_PID" || true       # salah satu mati -> bersihkan semuanya
