#!/usr/bin/env bash
set -euo pipefail

TEST_PORT=18889
SERVER_PID=""
RESULTS=()

cleanup() {
    if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    rm -f /tmp/md5_out_*.wav
}
trap cleanup EXIT

record_result() {
    RESULTS+=("$(printf "%-42s | %s" "$1" "$2")")
}

if [ ! -f "media/sample_30s.wav" ]; then
    ./media/generate_wav.sh
fi

make -j"$(nproc)"
./server "$TEST_PORT" --no-pacing > /tmp/server_md5.log 2>&1 &
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

run_concurrent_md5() {
    local fname="$1"
    local orig_md5
    orig_md5=$(md5sum "media/$fname" | awk '{print $1}')
    local pids=()

    for i in 1 2 3 4 5; do
        ./client get 127.0.0.1 "$TEST_PORT" "$fname" "/tmp/md5_out_${fname}_${i}.wav" > /dev/null 2>&1 &
        pids+=($!)
    done

    for pid in "${pids[@]}"; do
        wait "$pid" || return 1
    done

    for i in 1 2 3 4 5; do
        local got_md5
        got_md5=$(md5sum "/tmp/md5_out_${fname}_${i}.wav" | awk '{print $1}')
        [ "$got_md5" = "$orig_md5" ] || return 1
    done
    return 0
}

for sample in sample_30s.wav sample_tail.wav sample_5m.wav; do
    if run_concurrent_md5 "$sample"; then
        record_result "5x concurrent get $sample" "PASS"
    else
        record_result "5x concurrent get $sample" "FAIL"
    fi
done

if ! ./client get 127.0.0.1 "$TEST_PORT" missing_audio.wav /tmp/md5_out_err.wav > /dev/null 2>&1; then
    record_result "Missing file returns non-zero & ERROR" "PASS"
else
    record_result "Missing file returns non-zero & ERROR" "FAIL"
fi

echo ""
echo "========================================================="
printf "%-42s | %s\n" "TEST SCENARIO (test_md5.sh)" "RESULT"
echo "---------------------------------------------------------"
FAILED=0
for r in "${RESULTS[@]}"; do
    echo "$r"
    [[ "$r" == *"FAIL"* ]] && FAILED=$((FAILED + 1))
done
echo "========================================================="
[ "$FAILED" -eq 0 ]