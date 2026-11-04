#!/usr/bin/env bash
set -euo pipefail

# ==============================================================================
# Script Uji Otomatis Integritas Stream Audio MD5 Checksum (PART 4)
# ==============================================================================

TEST_PORT=18889
SERVER_PID=""

cleanup() {
    if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        echo "[+] Stopping test server (PID: $SERVER_PID)..."
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    rm -f /tmp/test_out_*.wav
}
trap cleanup EXIT

echo "===================================================="
echo " RUNNING TEST: MD5 Checksum Audio Stream Integrity"
echo "===================================================="

# Pastikan file media contoh sudah di-generate
if [ ! -f "media/sample_30s.wav" ]; then
    ./media/generate_wav.sh
fi

make clean && make

# Jalankan server
./server $TEST_PORT &
SERVER_PID=$!

# Tunggu server aktif
READY=0
for i in {1..30}; do
    if (echo > /dev/tcp/127.0.0.1/$TEST_PORT) 2>/dev/null; then
        READY=1
        break
    fi
    sleep 0.1
done

if [ $READY -eq 0 ]; then
    echo "[-] FAILED to start server on port $TEST_PORT"
    exit 1
fi

# Helper function untuk download & bandingkan MD5
test_file_md5() {
    local wav_name="$1"
    local orig_file="media/$wav_name"
    local orig_md5
    orig_md5=$(md5sum "$orig_file" | awk '{print $1}')
    
    echo "[+] Testing download of $wav_name (Original MD5: $orig_md5)..."
    
    # Run 5 concurrent clients downloading the same file
    PIDS=()
    for i in {1..5}; do
        ./client get 127.0.0.1 $TEST_PORT "$wav_name" "/tmp/test_out_${i}.wav" > /dev/null 2>&1 &
        PIDS+=($!)
    done

    for pid in "${PIDS[@]}"; do
        if ! wait "$pid"; then
            echo "  [-] Client process $pid failed during transfer!"
            return 1
        fi
    done

    # Check MD5 of all downloaded outputs
    for i in {1..5}; do
        local downloaded_md5
        downloaded_md5=$(md5sum "/tmp/test_out_${i}.wav" | awk '{print $1}')
        if [ "$orig_md5" != "$downloaded_md5" ]; then
            echo "  [-] MD5 MISMATCH Client $i! Expected $orig_md5, Got $downloaded_md5"
            return 1
        fi
    done
    echo "  [PASS] All 5 clients downloaded $wav_name with 100% MD5 match."
    return 0
}

# 1. Test sample_30s.wav
test_file_md5 "sample_30s.wav"

# 2. Test sample_tail.wav (Uji partial chunk)
test_file_md5 "sample_tail.wav"

# 3. Test File Not Found (Error 200 Verification)
echo "[+] Testing missing file request (Should receive Error frame & non-zero exit)..."
if ./client get 127.0.0.1 $TEST_PORT "non_existent_file.wav" "/tmp/dummy.wav" > /dev/null 2>&1; then
    echo "[-] FAILED: Client should have failed for non-existent file!"
    exit 1
else
    echo "  [PASS] Non-existent file request properly rejected with error."
fi

echo "===================================================="
echo " RESULTS: test_md5.sh -> ALL MD5 CHECKS PASSED [OK]"
echo "===================================================="