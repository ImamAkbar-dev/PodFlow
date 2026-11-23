#!/usr/bin/env bash
# =============================================================================
# run_all.sh - run EVERY PodFlow test suite with one command and print one
# summary table (owner: Member 1).
#
#   make check          (or: bash tests/run_all.sh)
#   make check-quick    (or: bash tests/run_all.sh --quick)   about 10 s
#
# Default run : build, unit tests (protocol, ring buffer, session), shell tests
#               (framing, md5, control), jitter analyzer self-test,
#               tests/test_server.py (only if the file exists) and a SMALL load
#               test (1, 10, 50 clients, results written to a temp dir).
# --quick     : build, unit tests, framing test, jitter self-test.
#
# Every step runs even if an earlier one fails; the exit code is 1 if any step
# failed. Environment: VERBOSE=1 streams the output of every step,
# STEP_TIMEOUT=<seconds> changes the per-step limit (default 300).
# The FULL benchmark (200 clients + back-pressure + plots) is `make bench`.
# =============================================================================
set -u
cd "$(dirname "${BASH_SOURCE[0]}")/.." || exit 2

QUICK=0
case "${1:-}" in
    --quick) QUICK=1 ;;
    "")      ;;
    *)       echo "usage: bash tests/run_all.sh [--quick]" >&2; exit 2 ;;
esac

STEP_TIMEOUT=${STEP_TIMEOUT:-300}
LOGDIR=$(mktemp -d /tmp/podflow_check.XXXXXX)
ulimit -n 65535 2>/dev/null || ulimit -n "$(ulimit -Hn)" 2>/dev/null || true

if [ -t 1 ]; then
    BOLD=$'\033[1m'; GRN=$'\033[32m'; RED=$'\033[31m'; YEL=$'\033[33m'; RST=$'\033[0m'
else
    BOLD=""; GRN=""; RED=""; YEL=""; RST=""
fi

NAMES=(); STATUS=(); TIMES=()
START_ALL=$SECONDS

record() { NAMES+=("$1"); STATUS+=("$2"); TIMES+=("$3"); }

# step "name" command args...   (runs the command, records PASS/FAIL)
step() {
    local name=$1; shift
    local log="$LOGDIR/$(printf '%02d' "${#NAMES[@]}").log"
    local t0=$SECONDS rc

    printf '%s>>> %s%s\n' "$BOLD" "$name" "$RST"
    if [ "${VERBOSE:-0}" = 1 ]; then
        timeout "$STEP_TIMEOUT" "$@" 2>&1 | tee "$log"
        rc=${PIPESTATUS[0]}
    else
        timeout "$STEP_TIMEOUT" "$@" > "$log" 2>&1
        rc=$?
    fi

    if [ "$rc" -eq 0 ]; then
        printf '    %sPASS%s (%ds)\n' "$GRN" "$RST" $((SECONDS - t0))
        record "$name" PASS $((SECONDS - t0))
    else
        [ "$rc" -eq 124 ] && echo "    (timed out after ${STEP_TIMEOUT}s)"
        printf '    %sFAIL%s (exit %d, %ds) - last lines of %s:\n' \
               "$RED" "$RST" "$rc" $((SECONDS - t0)) "$log"
        [ "${VERBOSE:-0}" = 1 ] || tail -n 20 "$log" | sed 's/^/    | /'
        record "$name" FAIL $((SECONDS - t0))
    fi
}

skip() {
    printf '%s>>> %s%s\n    %sSKIP%s (%s)\n' "$BOLD" "$1" "$RST" "$YEL" "$RST" "$2"
    record "$1" SKIP 0
}

# --------------------------------------------------------------------- preflight
for tool in gcc make python3; do
    command -v "$tool" >/dev/null 2>&1 || { echo "error: '$tool' not found" >&2; exit 2; }
done
HAVE_NUMPY=1
python3 -c 'import numpy' 2>/dev/null || HAVE_NUMPY=0

PORTS="18888 18889 18890 19000 19001 19002 19003"
for p in $PORTS; do
    if (echo > "/dev/tcp/127.0.0.1/$p") 2>/dev/null; then
        echo "error: port $p is already in use (a leftover ./server?). Try: pkill -f './server'" >&2
        exit 2
    fi
done

if [ ! -f media/sample_30s.wav ] || [ ! -f media/sample_tail.wav ] || [ ! -f media/sample_5m.wav ]; then
    echo "generating media files (media/generate_wav.sh)..."
    bash media/generate_wav.sh > "$LOGDIR/media.log" 2>&1 || { cat "$LOGDIR/media.log"; exit 2; }
fi

# ----------------------------------------------------------------------- steps
step "build (make)"                      make -s all
step "unit tests (protocol, ring, session)" make -s test
step "shell: framing / echo"             bash tests/test_framing.sh

if [ "$HAVE_NUMPY" -eq 1 ]; then
    step "python: jitter analyzer self-test" python3 tests/jitter_analyzer.py --self-test
else
    skip "python: jitter analyzer self-test" "numpy missing: sudo apt install python3-numpy"
fi

if [ "$QUICK" -eq 0 ]; then
    step "shell: md5 integrity (multi-client)" bash tests/test_md5.sh
    step "shell: playback control (pause/seek/stop)" bash tests/test_control.sh

    if [ -f tests/test_server.py ]; then
        step "python: server features (503, errors, shutdown)" python3 tests/test_server.py
    fi

    if [ "$HAVE_NUMPY" -eq 1 ]; then
        LOAD_DIR="$LOGDIR/load_results"
        step "load test (1, 10, 50 clients)" \
             python3 tests/load_test.py --counts 1,10,50 --skip-backpressure \
                     --results-dir "$LOAD_DIR"
        step "load test verdict (md5 + no failed clients)" python3 - "$LOAD_DIR/all_scenarios.json" <<'PY'
import json, sys
bad = 0
for s in json.load(open(sys.argv[1])):
    ok = s.get("md5_all_match") and s.get("failed_clients", 1) == 0
    print(f"{s['scenario']:8s} md5_all_match={s.get('md5_all_match')} "
          f"failed_clients={s.get('failed_clients')} -> {'OK' if ok else 'BAD'}")
    bad += 0 if ok else 1
sys.exit(1 if bad else 0)
PY
    else
        skip "load test (1, 10, 50 clients)" "numpy missing: sudo apt install python3-numpy"
    fi
fi

# --------------------------------------------------------------------- summary
PASSED=0; FAILED=0; SKIPPED=0
echo
echo "${BOLD}==================== PODFLOW TEST SUMMARY ====================${RST}"
for i in "${!NAMES[@]}"; do
    case "${STATUS[$i]}" in
        PASS) PASSED=$((PASSED + 1)); col=$GRN ;;
        FAIL) FAILED=$((FAILED + 1)); col=$RED ;;
        *)    SKIPPED=$((SKIPPED + 1)); col=$YEL ;;
    esac
    printf ' %s%-5s%s %-52s %4ss\n' "$col" "${STATUS[$i]}" "$RST" "${NAMES[$i]}" "${TIMES[$i]}"
done
echo "--------------------------------------------------------------"
printf ' %d passed, %d failed, %d skipped   (total %ds)\n' \
       "$PASSED" "$FAILED" "$SKIPPED" $((SECONDS - START_ALL))

if [ "$FAILED" -gt 0 ]; then
    echo " ${RED}RESULT: FAILED${RST}   logs kept in $LOGDIR"
    exit 1
fi
rm -rf "$LOGDIR"
if [ "$SKIPPED" -gt 0 ]; then
    echo " ${YEL}RESULT: PASSED (but $SKIPPED step(s) were skipped, see above)${RST}"
else
    echo " ${GRN}RESULT: ALL TESTS PASSED${RST}"
fi
exit 0