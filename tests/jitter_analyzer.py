#!/usr/bin/env python3
"""
PodFlow Jitter & Inter-Client Sync Skew Analyzer
Implements RFC 3550 jitter smoothing and contract-specified gap filtering.
Uses only Python standard library + numpy.
"""

import argparse
import csv
import glob
import json
import os
import sys
import tempfile
import numpy as np

EXPECTED_INTERVAL_NS = 50_000_000.0  # 50 ms in nanoseconds


def parse_client_csv(csv_path):
    """
    Parse a single client CSV file and extract valid consecutive DATA intervals.
    Skips seq=0 (WAV header) and skips any interval interrupted by ACK/CMD/ERROR.
    Returns:
        d_ms_list: list of D_i = |delta_i - 50ms| in milliseconds
        rfc_j_final_ms: final smoothed RFC 3550 jitter in milliseconds
        arrival_by_seq: dict mapping seq (int) -> arrival t_mono_ns (int) for PCM chunks
    """
    d_ns_list = []
    rfc_j_ns = 0.0
    arrival_by_seq = {}

    prev_data_seq = None
    prev_data_ts = None

    with open(csv_path, "r", newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            kind = row["kind"].strip()
            seq = int(row["seq"])
            t_ns = int(row["t_mono_ns"])

            if kind in ("ACK", "CMD", "ERROR"):
                # Break continuity across control events (PAUSE, SEEK, etc.)
                prev_data_seq = None
                prev_data_ts = None
                continue

            if kind == "DATA":
                if seq == 0:
                    # Skip 44-byte WAV header sent immediately before pacing starts
                    prev_data_seq = None
                    prev_data_ts = None
                    continue

                arrival_by_seq[seq] = t_ns

                if prev_data_seq is not None and seq == prev_data_seq + 1:
                    delta_ns = float(t_ns - prev_data_ts)
                    d_i = abs(delta_ns - EXPECTED_INTERVAL_NS)
                    d_ns_list.append(d_i)
                    rfc_j_ns += (d_i - rfc_j_ns) / 16.0

                prev_data_seq = seq
                prev_data_ts = t_ns

    d_ms_list = [d / 1e6 for d in d_ns_list]
    return d_ms_list, (rfc_j_ns / 1e6), arrival_by_seq


def compute_stats(values_ms):
    """Return p50, p95, p99, max, and mean in milliseconds."""
    if not values_ms:
        return {"count": 0, "mean": 0.0, "p50": 0.0, "p95": 0.0, "p99": 0.0, "max": 0.0}
    arr = np.array(values_ms, dtype=np.float64)
    return {
        "count": int(len(arr)),
        "mean": float(np.mean(arr)),
        "p50": float(np.percentile(arr, 50)),
        "p95": float(np.percentile(arr, 95)),
        "p99": float(np.percentile(arr, 99)),
        "max": float(np.max(arr)),
    }


def analyze_files(csv_files, out_csv=None, out_raw_d_csv=None):
    """Analyze a list of client CSV files and return aggregate metrics."""
    all_d_ms = []
    per_client_rows = []
    seq_arrivals = {}  # seq -> list of timestamps across clients

    for idx, path in enumerate(sorted(csv_files)):
        d_ms, rfc_j_ms, arrivals = parse_client_csv(path)
        all_d_ms.extend(d_ms)
        st = compute_stats(d_ms)

        per_client_rows.append({
            "client_id": os.path.basename(path),
            "intervals": st["count"],
            "p50_ms": round(st["p50"], 4),
            "p95_ms": round(st["p95"], 4),
            "p99_ms": round(st["p99"], 4),
            "max_ms": round(st["max"], 4),
            "rfc3550_j_ms": round(rfc_j_ms, 4),
        })

        for seq, ts in arrivals.items():
            seq_arrivals.setdefault(seq, []).append(ts)

    # Compute inter-client sync skew (max - min arrival for each chunk k)
    skew_ms_list = []
    if len(csv_files) > 1:
        for seq in sorted(seq_arrivals.keys()):
            ts_list = seq_arrivals[seq]
            if len(ts_list) >= 2:
                skew_ns = max(ts_list) - min(ts_list)
                skew_ms_list.append(skew_ns / 1e6)

    agg_jitter = compute_stats(all_d_ms)
    agg_skew = compute_stats(skew_ms_list)

    if out_csv:
        os.makedirs(os.path.dirname(os.path.abspath(out_csv)), exist_ok=True)
        with open(out_csv, "w", newline="") as f:
            writer = csv.DictWriter(
                f,
                fieldnames=["client_id", "intervals", "p50_ms", "p95_ms", "p99_ms", "max_ms", "rfc3550_j_ms"],
            )
            writer.writeheader()
            writer.writerows(per_client_rows)

    if out_raw_d_csv:
        os.makedirs(os.path.dirname(os.path.abspath(out_raw_d_csv)), exist_ok=True)
        with open(out_raw_d_csv, "w", newline="") as f:
            writer = csv.writer(f)
            writer.writerow(["d_ms"])
            for val in all_d_ms:
                writer.writerow([f"{val:.6f}"])

    return {
        "num_clients": len(csv_files),
        "jitter": agg_jitter,
        "sync_skew": agg_skew,
        "per_client": per_client_rows,
    }


def run_unit_tests():
    """Verify analyzer math with synthetic CSV traces."""
    print("[+] Running jitter_analyzer.py unit tests...")
    with tempfile.TemporaryDirectory() as tmpdir:
        # Test 1: Perfect 50 ms cadence -> D = 0, J = 0
        perfect_csv = os.path.join(tmpdir, "perfect.csv")
        with open(perfect_csv, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["kind", "seq", "t_mono_ns", "len", "param"])
            w.writerow(["DATA", 0, 1_000_000_000, 44, 0])
            for seq in range(1, 21):
                w.writerow(["DATA", seq, 1_000_000_000 + seq * 50_000_000, 8820, 0])

        d_ms, rfc_j, _ = parse_client_csv(perfect_csv)
        assert len(d_ms) == 19, f"Expected 19 intervals, got {len(d_ms)}"
        assert max(d_ms) == 0.0, f"Expected max D=0, got {max(d_ms)}"
        assert rfc_j == 0.0, f"Expected J=0, got {rfc_j}"
        print("  [PASS] Test 1: Perfect 50 ms cadence yields D=0.0 ms, J=0.0 ms")

        # Test 2: Injected 30 ms delay on seq=5, plus PAUSE gap between seq=10 and seq=11
        spike_csv = os.path.join(tmpdir, "spike.csv")
        with open(spike_csv, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["kind", "seq", "t_mono_ns", "len", "param"])
            w.writerow(["DATA", 0, 1_000_000_000, 44, 0])
            t = 1_000_000_000
            for seq in range(1, 15):
                if seq == 5:
                    t += 80_000_000  # 50 ms + 30 ms injected spike
                elif seq == 11:
                    # Simulate a 1-second PAUSE interrupted by CMD and ACK
                    w.writerow(["CMD", 2, t + 10_000_000, 0, 0])
                    w.writerow(["ACK", 2, t + 12_000_000, 0, 11])
                    t += 1_050_000_000
                else:
                    t += 50_000_000
                w.writerow(["DATA", seq, t, 8820, 0])

        d_ms2, rfc_j2, _ = parse_client_csv(spike_csv)
        # Interval (10->11) must be skipped because of CMD/ACK, so 12 intervals total
        assert len(d_ms2) == 12, f"Expected 12 intervals (PAUSE skipped), got {len(d_ms2)}"
        assert abs(max(d_ms2) - 30.0) < 1e-6, f"Expected max D=30.0 ms, got {max(d_ms2)}"
        assert rfc_j2 > 0.0, "Expected non-zero RFC 3550 jitter after spike"
        print("  [PASS] Test 2: Injected 30 ms delay detected & PAUSE gap skipped properly")

        # Test 3: Inter-client sync skew (Client B lags Client A by 12 ms)
        client_a = os.path.join(tmpdir, "c_a.csv")
        client_b = os.path.join(tmpdir, "c_b.csv")
        for path, offset_ns in [(client_a, 0), (client_b, 12_000_000)]:
            with open(path, "w", newline="") as f:
                w = csv.writer(f)
                w.writerow(["kind", "seq", "t_mono_ns", "len", "param"])
                for seq in range(1, 11):
                    w.writerow(["DATA", seq, 2_000_000_000 + seq * 50_000_000 + offset_ns, 8820, 0])

        res = analyze_files([client_a, client_b])
        assert abs(res["sync_skew"]["p50"] - 12.0) < 1e-6, f"Expected skew=12.0 ms, got {res['sync_skew']}"
        print("  [PASS] Test 3: Inter-client sync skew (12.0 ms) accurately measured")

    print("[+] All jitter_analyzer unit tests PASSED!")


def main():
    parser = argparse.ArgumentParser(description="Analyze PodFlow client timing CSVs for jitter and sync skew.")
    parser.add_argument("inputs", nargs="*", help="Client CSV files or glob patterns")
    parser.add_argument("--out-csv", help="Path to write per-client jitter summary CSV")
    parser.add_argument("--out-raw-d", help="Path to write all raw D_i values (ms) for plotting")
    parser.add_argument("--json-out", help="Path to write aggregate JSON metrics")
    parser.add_argument("--self-test", action="store_true", help="Run built-in unit tests")
    args = parser.parse_args()

    if args.self_test:
        run_unit_tests()
        return

    files = []
    for pattern in args.inputs:
        matched = glob.glob(pattern)
        files.extend(matched if matched else [pattern])

    files = [f for f in files if os.path.isfile(f)]
    if not files:
        print("[-] No valid CSV files provided.", file=sys.stderr)
        sys.exit(1)

    summary = analyze_files(files, out_csv=args.out_csv, out_raw_d_csv=args.out_raw_d)

    print("============================================================")
    print(f" JITTER & SYNC ANALYSIS ({summary['num_clients']} clients)")
    print("============================================================")
    j = summary["jitter"]
    print(f" Jitter D_i (ms) : count={j['count']} | p50={j['p50']:.3f} | p95={j['p95']:.3f} | p99={j['p99']:.3f} | max={j['max']:.3f}")
    if summary["num_clients"] > 1:
        s = summary["sync_skew"]
        print(f" Sync Skew (ms)  : count={s['count']} | p50={s['p50']:.3f} | p95={s['p95']:.3f} | p99={s['p99']:.3f} | max={s['max']:.3f}")
    print("============================================================")

    if args.json_out:
        os.makedirs(os.path.dirname(os.path.abspath(args.json_out)), exist_ok=True)
        with open(args.json_out, "w") as f:
            json.dump(summary, f, indent=2)


if __name__ == "__main__":
    main()