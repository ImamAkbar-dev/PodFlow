#!/usr/bin/env bash
set -euo pipefail

TEST_PORT=18888
SERVER_PID=""
RESULTS=()

cleanup() {
    if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

record_result() {
    RESULTS+=("$(printf "%-42s | %s" "$1" "$2")")
}

make -j"$(nproc)"
./server "$TEST_PORT" --no-pacing > /tmp/server_framing.log 2>&1 &
SERVER_PID=$!

READY=0
for _ in $(seq 1 50); do
    if (echo > /dev/tcp/127.0.0.1/"$TEST_PORT") 2>/dev/null; then
        READY=1
        break
    fi
    sleep 0.1
done
[ "$READY" -eq 1 ] || exit 1

if ./client echo 127.0.0.1 "$TEST_PORT" 1000 > /dev/null 2>&1; then
    record_result "Single client 1000 ECHO frames" "PASS"
else
    record_result "Single client 1000 ECHO frames" "FAIL"
fi

PIDS=()
for _ in 1 2 3 4 5; do
    ./client echo 127.0.0.1 "$TEST_PORT" 1000 > /dev/null 2>&1 &
    PIDS+=($!)
done

CONC_OK=1
for pid in "${PIDS[@]}"; do
    wait "$pid" || CONC_OK=0
done

if [ "$CONC_OK" -eq 1 ]; then
    record_result "5x concurrent clients 1000 ECHO frames" "PASS"
else
    record_result "5x concurrent clients 1000 ECHO frames" "FAIL"
fi

echo ""
echo "========================================================="
printf "%-42s | %s\n" "TEST SCENARIO (test_framing.sh)" "RESULT"
echo "---------------------------------------------------------"
FAILED=0
for r in "${RESULTS[@]}"; do
    echo "$r"
    [[ "$r" == *"FAIL"* ]] && FAILED=$((FAILED + 1))
done
echo "========================================================="
[ "$FAILED" -eq 0 ]