#!/usr/bin/env bash
set -euo pipefail

TEST_PORT=18890
SERVER_PID=""
RESULTS=()

cleanup() {
    if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    rm -f /tmp/ctrl_*.wav /tmp/expected_seek.wav
}
trap cleanup EXIT

record_result() {
    local name="$1"
    local status="$2"
    RESULTS+=("$(printf "%-42s | %s" "$name" "$status")")
}

if [ ! -f "media/sample_30s.wav" ]; then
    ./media/generate_wav.sh
fi

make -j"$(nproc)"

./server "$TEST_PORT" --no-pacing > /tmp/server_ctrl.log 2>&1 &
SERVER_PID=$!

READY=0
for _ in $(seq 1 50); do
    if (echo > /dev/tcp/127.0.0.1/"$TEST_PORT") 2>/dev/null; then
        READY=1
        break
    fi
    sleep 0.1
done
[ "$READY" -eq 1 ] || { echo "Server failed to start"; exit 1; }

ORIG_MD5=$(md5sum media/sample_30s.wav | awk '{print $1}')

# (a) Pause 1s then resume -> output md5 equals original
if ./client play 127.0.0.1 "$TEST_PORT" sample_30s.wav /tmp/ctrl_a.wav \
   --script "0.01:pause,1.01:play" > /dev/null 2>&1 && \
   [ "$(md5sum /tmp/ctrl_a.wav | awk '{print $1}')" = "$ORIG_MD5" ]; then
    record_result "(a) Pause 1s + Resume MD5 integrity" "PASS"
else
    record_result "(a) Pause 1s + Resume MD5 integrity" "FAIL"
fi

# (b) Seek=300 from start -> header (44B) + bytes from offset 44 + 299*8820
OFFSET=$((44 + 299 * 8820))
head -c 44 media/sample_30s.wav > /tmp/expected_seek.wav
tail -c +$((OFFSET + 1)) media/sample_30s.wav >> /tmp/expected_seek.wav
EXP_SEEK_MD5=$(md5sum /tmp/expected_seek.wav | awk '{print $1}')

if ./client play 127.0.0.1 "$TEST_PORT" sample_30s.wav /tmp/ctrl_b.wav \
   --script "0.00:seek=300" > /dev/null 2>&1 && \
   [ "$(md5sum /tmp/ctrl_b.wav | awk '{print $1}')" = "$EXP_SEEK_MD5" ]; then
    record_result "(b) Seek=300 byte-offset match" "PASS"
else
    record_result "(b) Seek=300 byte-offset match" "FAIL"
fi

# (c) Seek while paused then resume
if ./client play 127.0.0.1 "$TEST_PORT" sample_30s.wav /tmp/ctrl_c.wav \
   --script "0.00:pause,0.10:seek=300,0.20:play" > /dev/null 2>&1 && \
   [ "$(md5sum /tmp/ctrl_c.wav | awk '{print $1}')" = "$EXP_SEEK_MD5" ]; then
    record_result "(c) Seek while PAUSED + Resume" "PASS"
else
    record_result "(c) Seek while PAUSED + Resume" "FAIL"
fi

# (d) Stop mid-stream -> no END, connection still usable for ECHO
if ./client play 127.0.0.1 "$TEST_PORT" sample_30s.wav /tmp/ctrl_d.wav \
   --script "0.01:stop" > /dev/null 2>&1; then
    record_result "(d) Stop mid-stream + post-stop ECHO" "PASS"
else
    record_result "(d) Stop mid-stream + post-stop ECHO" "FAIL"
fi

# (e) Seek 0 and Seek total+1 (601 for 30s) -> ERROR 202 and state unchanged
if ./client play 127.0.0.1 "$TEST_PORT" sample_30s.wav /tmp/ctrl_e.wav \
   --script "0.01:seek=0!202,0.02:seek=601!202" > /dev/null 2>&1 && \
   [ "$(md5sum /tmp/ctrl_e.wav | awk '{print $1}')" = "$ORIG_MD5" ]; then
    record_result "(e) Out-of-range Seek -> ERROR 202" "PASS"
else
    record_result "(e) Out-of-range Seek -> ERROR 202" "FAIL"
fi

# (f) Pause while already paused (invalid state) -> ERROR 300
if ./client play 127.0.0.1 "$TEST_PORT" sample_30s.wav /tmp/ctrl_f.wav \
   --script "0.01:pause,0.05:pause!300,0.10:play" > /dev/null 2>&1 && \
   [ "$(md5sum /tmp/ctrl_f.wav | awk '{print $1}')" = "$ORIG_MD5" ]; then
    record_result "(f) Invalid state command -> ERROR 300" "PASS"
else
    record_result "(f) Invalid state command -> ERROR 300" "FAIL"
fi

# (g) Missing file -> ERROR 200 and non-zero exit
if ! ./client get 127.0.0.1 "$TEST_PORT" does_not_exist.wav /tmp/ctrl_g.wav > /dev/null 2>&1; then
    record_result "(g) Missing file -> ERROR 200" "PASS"
else
    record_result "(g) Missing file -> ERROR 200" "FAIL"
fi

echo ""
echo "========================================================="
printf "%-42s | %s\n" "TEST SCENARIO (test_control.sh)" "RESULT"
echo "---------------------------------------------------------"
FAILED=0
for r in "${RESULTS[@]}"; do
    echo "$r"
    if [[ "$r" == *"FAIL"* ]]; then
        FAILED=$((FAILED + 1))
    fi
done
echo "========================================================="
[ "$FAILED" -eq 0 ]