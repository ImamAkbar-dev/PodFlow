#!/bin/bash
# load.sh N [file] [server-binary]
# Run from the repo root. Starts ./server, launches N parallel `./client get`,
# then checks md5, wall time and the server stats CSV. Not committed (Member 1 scratch).
set -u
N=${1:?usage: load.sh N [file] [server-binary]}
FILE=${2:-sample_30s.wav}
SRV=${3:-./server}
PORT=8888
OUT=$(mktemp -d /tmp/podflow_load.XXXXXX)
ulimit -n 65535 2>/dev/null || ulimit -n "$(ulimit -Hn)"

$SRV $PORT --stats-csv "$OUT/stats.csv" > "$OUT/server.log" 2> "$OUT/server.err" &
SP=$!
sleep 0.7

PIDS=()
START=$(date +%s.%N)
for i in $(seq 1 "$N"); do
  ./client get 127.0.0.1 $PORT "$FILE" "$OUT/o$i.wav" > "$OUT/c$i.txt" 2>&1 &
  PIDS+=($!)
done
for p in "${PIDS[@]}"; do wait "$p"; done
END=$(date +%s.%N)

REF=$(md5sum "media/$FILE" | cut -d' ' -f1)
echo "N                 : $N"
echo "wall time         : $(echo "$END - $START" | bc) s"
echo "md5 identical     : $(md5sum "$OUT"/o*.wav | grep -c "$REF") / $N"
echo "client failures   : $(grep -L 'STREAM COMPLETED' "$OUT"/c*.txt | wc -l)"

sleep 1.2
kill -INT $SP; wait $SP 2>/dev/null
awk -F, 'NR>1 { if ($2>c) c=$2; if ($3>s) s=$3; if ($7>m) m=$7; ch=$4; u=$5; l=$6 }
         END  { printf "peak clients      : %d\npeak streaming    : %d\nchunks_sent       : %d\nunderruns         : %d\nlate_resets       : %d\nmax_lateness_us   : %d (worst 1-second window)\n", c,s,ch,u,l,m }' "$OUT/stats.csv"
grep -E "ThreadSanitizer|AddressSanitizer|runtime error" "$OUT/server.err" | sort | uniq -c
echo "files kept in     : $OUT"