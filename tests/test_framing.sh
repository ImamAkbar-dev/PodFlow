#!/usr/bin/env bash
set -euo pipefail

# ==============================================================================
# Script Uji Otomatis Framing & Echo Integrity (PART 4)
# ==============================================================================

TEST_PORT=18888
SERVER_PID=""

# Fungsi untuk membersihkan server jika script selesai/batal
cleanup() {
    if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        echo "[+] Stopping test server (PID: $SERVER_PID)..."
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

echo "===================================================="
echo " RUNNING TEST: Framing & Echo Protocol Integrity"
echo "===================================================="

# 1. Kompilasi program
make clean && make

# 2. Jalankan Server di port khusus testing (18888)
./server $TEST_PORT &
SERVER_PID=$!

echo "[+] Server started on port $TEST_PORT (PID: $SERVER_PID)"

# 3. Tunggu server siap menggunakan /dev/tcp (bukan sleep buta)
echo -n "[+] Waiting for server to be ready..."
READY=0
for i in {1..30}; do
    if (echo > /dev/tcp/127.0.0.1/$TEST_PORT) 2>/dev/null; then
        READY=1
        echo " Ready!"
        break
    fi
    sleep 0.1
done

if [ $READY -eq 0 ]; then
    echo " FAILED (Timeout waiting for server)"
    exit 1
fi

# 4. Pengujian 1 Client Single Stream (1000 echo frames)
echo "[+] Test 1: Single client 1000 echo frames..."
if ./client echo 127.0.0.1 $TEST_PORT 1000; then
    echo "  [PASS] Single client echo test passed."
else
    echo "  [FAIL] Single client echo test failed!"
    exit 1
fi

# 5. Pengujian 5 Client Serentak (Concurrent Framing)
echo "[+] Test 2: 5 Concurrent clients running 500 echo frames each..."
PIDS=()
for i in {1..5}; do
    ./client echo 127.0.0.1 $TEST_PORT 500 > /dev/null 2>&1 &
    PIDS+=($!)
done

FAIL_COUNT=0
for pid in "${PIDS[@]}"; do
    if ! wait "$pid"; then
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
done

echo "===================================================="
if [ $FAIL_COUNT -eq 0 ]; then
    echo " RESULTS: test_framing.sh -> ALL PASSED [OK]"
    echo "===================================================="
    exit 0
else
    echo " RESULTS: test_framing.sh -> FAILED ($FAIL_COUNT clients error)"
    echo "===================================================="
    exit 1
fi