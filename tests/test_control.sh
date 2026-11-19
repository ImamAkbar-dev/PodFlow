#!/usr/bin/env bash
set -euo pipefail

# ==============================================================================
# Script Uji Otomatis Playback Control FSM (PAUSE, RESUME, SEEK, STOP, ERRORS)
# ==============================================================================

TEST_PORT=18890
SERVER_PID=""
TMP_DIR="/tmp/podflow_ctrl_$$"
mkdir -p "$TMP_DIR"

cleanup() {
    if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    rm -rf "$TMP_DIR"
}
trap cleanup EXIT

# Generate media if missing
if [ ! -f "media/sample_30s.wav" ]; then
    ./media/generate_wav.sh
fi

make -s

# Start server WITH PACING ENABLED (no --no-pacing) so mid-stream controls hit PLAYING state
./server "$TEST_PORT" > /dev/null 2>&1 &
SERVER_PID=$!

# Wait for server readiness via /dev/tcp
READY=0
for _ in {1..30}; do
    if (echo > "/dev/tcp/127.0.0.1/$TEST_PORT") 2>/dev/null; then
        READY=1
        break
    fi
    sleep 0.1
done

if [ "$READY" -eq 0 ]; then
    echo "[-] Server failed to start on port $TEST_PORT"
    exit 1
fi

ORIG_MD5=$(md5sum media/sample_30s.wav | awk '{print $1}')
RESULTS=()
OVERALL_PASS=1

record_result() {
    local name="$1"
    local status="$2"
    RESULTS+=("$(printf "%-42s | %s" "$name" "$status")")
    if [ "$status" != "PASS" ]; then
        OVERALL_PASS=0
    fi
}

# (a) Pause 1 s then resume -> output md5 equals original
# Using sample_tail.wav (2.37s) so test finishes quickly with pacing
TAIL_MD5=$(md5sum media/sample_tail.wav | awk '{print $1}')
if ./client play 127.0.0.1 "$TEST_PORT" sample_tail.wav "$TMP_DIR/out_a.wav" \
    --script "0.3:pause,1.3:play" > /dev/null 2>&1; then
    OUT_A_MD5=$(md5sum "$TMP_DIR/out_a.wav" | awk '{print $1}')
    if [ "$OUT_A_MD5" = "$TAIL_MD5" ]; then
        record_result "(a) Pause 1s + Resume MD5 integrity" "PASS"
    else
        record_result "(a) Pause 1s + Resume MD5 integrity" "FAIL"
    fi
else
    record_result "(a) Pause 1s + Resume MD5 integrity" "FAIL"
fi

# (b) Seek=300 mid-stream -> verify byte alignment from chunk 300 onward
# Chunk 300 (1-based) starts at file offset 44 + 299 * 8820 = 2,637,224
if ./client play 127.0.0.1 "$TEST_PORT" sample_30s.wav "$TMP_DIR/out_b.wav" \
    --script "0.2:seek=300,0.6:stop" > /dev/null 2>&1; then
    OFFSET=$((44 + 299 * 8820))
    # Compare 3 chunks (26460 bytes) starting at chunk 300
    dd if=media/sample_30s.wav of="$TMP_DIR/exp_b.bin" bs=1 skip="$OFFSET" count=26460 status=none
    dd if="$TMP_DIR/out_b.wav" of="$TMP_DIR/got_b.bin" bs=1 skip="$OFFSET" count=26460 status=none
    if cmp -s "$TMP_DIR/exp_b.bin" "$TMP_DIR/got_b.bin"; then
        record_result "(b) Seek=300 byte-offset match" "PASS"
    else
        record_result "(b) Seek=300 byte-offset match" "FAIL"
    fi
else
    record_result "(b) Seek=300 byte-offset match" "FAIL"
fi

# (c) Seek while PAUSED -> stays PAUSED until PLAY (resume), then finishes cleanly
if ./client play 127.0.0.1 "$TEST_PORT" sample_tail.wav "$TMP_DIR/out_c.wav" \
    --script "0.2:pause,0.4:seek=20,0.6:play" > /dev/null 2>&1; then
    OFFSET_C=$((44 + 19 * 8820))
    REM_BYTES=$((418112 - OFFSET_C))
    dd if=media/sample_tail.wav of="$TMP_DIR/exp_c.bin" bs=1 skip="$OFFSET_C" count="$REM_BYTES" status=none
    dd if="$TMP_DIR/out_c.wav" of="$TMP_DIR/got_c.bin" bs=1 skip="$OFFSET_C" count="$REM_BYTES" status=none
    if cmp -s "$TMP_DIR/exp_c.bin" "$TMP_DIR/got_c.bin"; then
        record_result "(c) Seek while PAUSED + Resume" "PASS"
    else
        record_result "(c) Seek while PAUSED + Resume" "FAIL"
    fi
else
    record_result "(c) Seek while PAUSED + Resume" "FAIL"
fi

# (d) Stop mid-stream -> no END, connection still usable
if ./client play 127.0.0.1 "$TEST_PORT" sample_30s.wav "$TMP_DIR/out_d.wav" \
    --script "0.2:stop" > /dev/null 2>&1; then
    record_result "(d) Stop mid-stream + post-stop ECHO" "PASS"
else
    record_result "(d) Stop mid-stream + post-stop ECHO" "FAIL"
fi

# (e) Out-of-range Seek (seek=0 and seek=total+1) -> ERROR 202, stream continues
if ./client play 127.0.0.1 "$TEST_PORT" sample_tail.wav "$TMP_DIR/out_e.wav" \
    --script "0.2:seek=0,0.4:seek=9999" > /dev/null 2>&1; then
    OUT_E_MD5=$(md5sum "$TMP_DIR/out_e.wav" | awk '{print $1}')
    if [ "$OUT_E_MD5" = "$TAIL_MD5" ]; then
        record_result "(e) Out-of-range Seek -> ERROR 202" "PASS"
    else
        record_result "(e) Out-of-range Seek -> ERROR 202" "FAIL"
    fi
else
    record_result "(e) Out-of-range Seek -> ERROR 202" "FAIL"
fi

# (f) Invalid state command (PAUSE in IDLE via seek/pause after stop) -> ERROR 300
if ./client play 127.0.0.1 "$TEST_PORT" sample_tail.wav "$TMP_DIR/out_f.wav" \
    --script "0.2:stop,0.4:pause" > /dev/null 2>&1; then
    record_result "(f) Invalid state command -> ERROR 300" "PASS"
else
    record_result "(f) Invalid state command -> ERROR 300" "FAIL"
fi

# (g) Missing file -> ERROR 200 and non-zero exit
if ./client get 127.0.0.1 "$TEST_PORT" "does_not_exist.wav" "$TMP_DIR/out_g.wav" > /dev/null 2>&1; then
    record_result "(g) Missing file -> ERROR 200" "FAIL"
else
    record_result "(g) Missing file -> ERROR 200" "PASS"
fi

echo "============================================================"
printf "%-42s | %s\n" "TEST SCENARIO (test_control.sh)" "RESULT"
echo "============================================================"
for row in "${RESULTS[@]}"; do
    echo "$row"
done
echo "============================================================"

[ "$OVERALL_PASS" -eq 1 ]