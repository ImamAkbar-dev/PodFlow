#!/usr/bin/env bash
# Gate inti sebelum/sesudah pekerjaan UI (Tugas.md butir Rafly).
#   bash ui/scripts/core_gate.sh before     # simpan baseline
#   bash ui/scripts/core_gate.sh after      # simpan lalu bandingkan dengan baseline
# Yang dibandingkan hanya baris hasil (PASS/FAIL/ok), bukan waktu.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="$ROOT/ui/.gate"; mkdir -p "$OUT"
LABEL="${1:?pakai: before | after}"
cd "$ROOT" || exit 2
{
  echo "== git diff inti terhadap develop (harus kosong untuk 'after') =="
  git diff --stat develop -- src include tests Makefile 2>&1 || true
  echo "== make =="; make 2>&1 | tail -n 5
  echo "== make test =="; make test 2>&1
  echo "== make check-quick =="; bash tests/run_all.sh --quick 2>&1
} > "$OUT/$LABEL.full.txt"
grep -Ei 'pass|fail|ok|error|warning' "$OUT/$LABEL.full.txt" \
  | sed -E 's/[0-9]+(\.[0-9]+)?\s*(ms|s)\b//g' > "$OUT/$LABEL.txt"
echo "disimpan: ui/.gate/$LABEL.txt"
if [ "$LABEL" = after ] && [ -f "$OUT/before.txt" ]; then
  if diff -u "$OUT/before.txt" "$OUT/after.txt"; then echo "GATE IDENTIK"; else echo "GATE BERBEDA"; exit 1; fi
fi
