#!/usr/bin/env python3
"""
PodFlow Automated Multi-Client Load, Resource & Back-Pressure Test Suite.
Reads Linux /proc directly for server CPU% and RSS memory without external dependencies.
"""

import argparse
import csv
import hashlib
import json
import os
import resource
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import jitter_analyzer  # noqa: E402

CLK_TCK = os.sysconf(os.sysconf_names["SC_CLK_TCK"])


def md5_file(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def wait_port_ready(host, port, timeout_s=5.0):
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        try:
            with socket.create_connection((host, port), timeout=0.2):
                return True
        except OSError:
            time.sleep(0.05)
    return False


def read_proc_cpu_ticks(pid):
    """Read utime + stime (in clock ticks) from /proc/<pid>/stat."""
    try:
        with open(f"/proc/{pid}/stat", "r") as f:
            data = f.read()
        rparen = data.rfind(")")
        fields = data[rparen + 2 :].split()
        utime = int(fields[11])
        stime = int(fields[12])
        return utime + stime
    except OSError:
        return None


def read_proc_rss_kb(pid):
    """Read VmRSS (in KiB) from /proc/<pid>/status."""
    try:
        with open(f"/proc/{pid}/status", "r") as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1])
    except OSError:
        pass
    return 0


class ResourceMonitor(threading.Thread):
    """Samples server CPU% and RSS memory every 1.0 second into server_resource.csv."""

    def __init__(self, pid, out_csv_path):
        super().__init__(daemon=True)
        self.pid = pid
        self.out_csv_path = out_csv_path
        self.stop_event = threading.Event()
        self.samples = []

    def run(self):
        os.makedirs(os.path.dirname(os.path.abspath(self.out_csv_path)), exist_ok=True)
        prev_ticks = read_proc_cpu_ticks(self.pid)
        prev_time = time.monotonic()
        t0 = prev_time

        with open(self.out_csv_path, "w", newline="") as f:
            writer = csv.writer(f)
            writer.writerow(["t_s", "cpu_pct", "rss_mb"])

            while not self.stop_event.wait(1.0):
                now = time.monotonic()
                ticks = read_proc_cpu_ticks(self.pid)
                rss_kb = read_proc_rss_kb(self.pid)
                if ticks is None:
                    break

                dt = now - prev_time
                dticks = ticks - (prev_ticks if prev_ticks is not None else ticks)
                cpu_pct = ((dticks / CLK_TCK) / dt) * 100.0 if dt > 0 else 0.0
                rss_mb = rss_kb / 1024.0
                elapsed = round(now - t0, 2)

                row = {"t_s": elapsed, "cpu_pct": round(cpu_pct, 2), "rss_mb": round(rss_mb, 2)}
                self.samples.append(row)
                writer.writerow([row["t_s"], row["cpu_pct"], row["rss_mb"]])
                f.flush()

                prev_ticks = ticks
                prev_time = now

    def stop(self):
        self.stop_event.set()
        self.join(timeout=2.0)


def parse_server_stats_csv(stats_csv_path):
    """Extract peak underruns, late_resets, and max_lateness_us from server --stats-csv."""
    max_underruns = 0
    max_late_resets = 0
    max_lateness_us = 0
    if not os.path.isfile(stats_csv_path):
        return {"underruns": 0, "late_resets": 0, "max_lateness_us": 0}

    with open(stats_csv_path, "r", newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            try:
                max_underruns = max(max_underruns, int(row.get("underruns", 0)))
                max_late_resets = max(max_late_resets, int(row.get("late_resets", 0)))
                max_lateness_us = max(max_lateness_us, int(row.get("max_lateness_us", 0)))
            except ValueError:
                continue
    return {
        "underruns": max_underruns,
        "late_resets": max_late_resets,
        "max_lateness_us": max_lateness_us,
    }


def run_n_clients_scenario(n_clients, wav_name, port, results_root):
    scenario_name = f"n_{n_clients}"
    out_dir = os.path.join(results_root, scenario_name)
    os.makedirs(out_dir, exist_ok=True)

    orig_wav = os.path.join("media", wav_name)
    expected_md5 = md5_file(orig_wav)

    server_stats_csv = os.path.join(out_dir, "server_stats.csv")
    resource_csv = os.path.join(out_dir, "server_resource.csv")
    jitter_csv = os.path.join(out_dir, "jitter.csv")
    raw_d_csv = os.path.join(out_dir, "raw_d_ms.csv")
    summary_json = os.path.join(out_dir, "summary.json")

    print(f"[*] Starting scenario N={n_clients} on port {port}...", flush=True)

    server_cmd = [
        "./server",
        str(port),
        "--max-clients",
        str(max(1000, n_clients + 50)),
        "--stats-csv",
        server_stats_csv,
    ]
    srv_proc = subprocess.Popen(server_cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    monitor = None

    try:
        if not wait_port_ready("127.0.0.1", port):
            raise RuntimeError(f"Server failed to bind port {port}")

        monitor = ResourceMonitor(srv_proc.pid, resource_csv)
        monitor.start()

        with tempfile.TemporaryDirectory(prefix=f"podflow_n{n_clients}_") as tmpdir:
            procs = []
            csv_paths = []
            wav_paths = []

            t_start = time.monotonic()
            # Launch all N clients back-to-back without SIGSTOP deadlock
            for i in range(n_clients):
                c_csv = os.path.join(tmpdir, f"client_{i:04d}.csv")
                c_wav = os.path.join(tmpdir, f"out_{i:04d}.wav")
                csv_paths.append(c_csv)
                wav_paths.append(c_wav)

                cmd = ["./client", "play", "127.0.0.1", str(port), wav_name, c_wav, "--csv", c_csv]
                p = subprocess.Popen(
                    cmd,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                )
                procs.append(p)

            failed_clients = 0
            for p in procs:
                rc = p.wait(timeout=60)
                if rc != 0:
                    failed_clients += 1

            elapsed_s = time.monotonic() - t_start

            md5_mismatches = 0
            for wpath in wav_paths:
                if not os.path.isfile(wpath) or md5_file(wpath) != expected_md5:
                    md5_mismatches += 1

            analysis = jitter_analyzer.analyze_files(
                csv_paths, out_csv=jitter_csv, out_raw_d_csv=raw_d_csv
            )

        monitor.stop()
        srv_telemetry = parse_server_stats_csv(server_stats_csv)

        cpu_vals = [s["cpu_pct"] for s in monitor.samples] or [0.0]
        rss_vals = [s["rss_mb"] for s in monitor.samples] or [0.0]

        summary = {
            "scenario": scenario_name,
            "n_clients": n_clients,
            "wav_file": wav_name,
            "elapsed_s": round(elapsed_s, 3),
            "failed_clients": failed_clients,
            "md5_mismatches": md5_mismatches,
            "md5_all_match": (failed_clients == 0 and md5_mismatches == 0),
            "jitter_ms": analysis["jitter"],
            "sync_skew_ms": analysis["sync_skew"],
            "server_resources": {
                "cpu_avg_pct": round(sum(cpu_vals) / len(cpu_vals), 2),
                "cpu_max_pct": round(max(cpu_vals), 2),
                "rss_max_mb": round(max(rss_vals), 2),
            },
            "server_telemetry": srv_telemetry,
        }

        with open(summary_json, "w") as f:
            json.dump(summary, f, indent=2)

        j = summary["jitter_ms"]
        print(
            f"[N={n_clients:3d}] MD5_OK={summary['md5_all_match']} | "
            f"Jitter p50={j['p50']:.2f}ms p95={j['p95']:.2f}ms max={j['max']:.2f}ms | "
            f"CPU_max={summary['server_resources']['cpu_max_pct']:.1f}% | "
            f"RSS_max={summary['server_resources']['rss_max_mb']:.1f}MB | "
            f"Underruns={srv_telemetry['underruns']} LateResets={srv_telemetry['late_resets']}",
            flush=True,
        )
        return summary

    finally:
        if monitor is not None:
            monitor.stop()
        srv_proc.terminate()
        try:
            srv_proc.wait(timeout=3.0)
        except subprocess.TimeoutExpired:
            srv_proc.kill()


def run_backpressure_scenario(port, results_root):
    """
    Demonstrate back-pressure & 5s SO_SNDTIMEO drop:
    1 stalled client connects, requests sample_30s.wav, and NEVER reads from socket.
    Meanwhile 5 normal clients stream sample_tail.wav concurrently without degradation.
    """
    out_dir = os.path.join(results_root, "backpressure")
    os.makedirs(out_dir, exist_ok=True)
    summary_json = os.path.join(out_dir, "summary.json")

    print(f"[*] Starting BACK-PRESSURE test on port {port}...", flush=True)

    srv_proc = subprocess.Popen(
        ["./server", str(port)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )
    try:
        if not wait_port_ready("127.0.0.1", port):
            raise RuntimeError("Server failed to start for backpressure test")

        stalled_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        stalled_sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        stalled_sock.connect(("127.0.0.1", port))

        hello_pay = b"stalled-client"
        hello_hdr = struct.pack("!HBBIII", 0x5753, 1, 0x01, 0, len(hello_pay), 0)
        play_pay = b"sample_30s.wav"
        play_hdr = struct.pack("!HBBIII", 0x5753, 1, 0x04, 1, len(play_pay), 0)
        stalled_sock.sendall(hello_hdr + hello_pay + play_hdr + play_pay)

        t_stall_start = time.monotonic()

        orig_md5 = md5_file("media/sample_tail.wav")
        healthy_ok = 0
        with tempfile.TemporaryDirectory(prefix="podflow_bp_") as tmpdir:
            procs = []
            outs = []
            for i in range(5):
                out_w = os.path.join(tmpdir, f"healthy_{i}.wav")
                outs.append(out_w)
                procs.append(
                    subprocess.Popen(
                        ["./client", "get", "127.0.0.1", str(port), "sample_tail.wav", out_w],
                        stdout=subprocess.DEVNULL,
                        stderr=subprocess.DEVNULL,
                    )
                )
            for p, out_w in zip(procs, outs):
                if p.wait(timeout=15) == 0 and md5_file(out_w) == orig_md5:
                    healthy_ok += 1

        stalled_dropped = False
        drop_elapsed_s = 0.0
        stalled_sock.settimeout(15.0)
        try:
            while True:
                data = stalled_sock.recv(65536)
                if not data:
                    stalled_dropped = True
                    drop_elapsed_s = time.monotonic() - t_stall_start
                    break
        except socket.timeout:
            stalled_dropped = False
        finally:
            stalled_sock.close()

        bp_summary = {
            "scenario": "backpressure_5s_timeout",
            "healthy_clients_total": 5,
            "healthy_clients_md5_ok": healthy_ok,
            "stalled_client_dropped_by_server": stalled_dropped,
            "stalled_drop_elapsed_s": round(drop_elapsed_s, 3),
        }
        with open(summary_json, "w") as f:
            json.dump(bp_summary, f, indent=2)

        print(
            f"[BACK-PRESSURE] Healthy OK={healthy_ok}/5 | "
            f"Stalled Dropped={stalled_dropped} in {drop_elapsed_s:.2f}s",
            flush=True,
        )
        return bp_summary

    finally:
        srv_proc.terminate()
        try:
            srv_proc.wait(timeout=3.0)
        except subprocess.TimeoutExpired:
            srv_proc.kill()


def main():
    parser = argparse.ArgumentParser(description="Run PodFlow multi-client load & backpressure benchmarks.")
    parser.add_argument(
        "--counts",
        default="1,10,50,100,200",
        help="Comma-separated client concurrency counts (default: 1,10,50,100,200)",
    )
    parser.add_argument(
        "--wav",
        default="sample_tail.wav",
        help="WAV file in media/ to stream during load test (default: sample_tail.wav)",
    )
    parser.add_argument("--base-port", type=int, default=19000, help="Starting TCP port")
    parser.add_argument("--results-dir", default="results", help="Output directory for results")
    parser.add_argument("--skip-backpressure", action="store_true", help="Skip backpressure test")
    args = parser.parse_args()

    soft_nofile, _ = resource.getrlimit(resource.RLIMIT_NOFILE)
    counts = [int(x.strip()) for x in args.counts.split(",") if x.strip()]
    max_n = max(counts)
    if soft_nofile < max_n * 4 + 64:
        print(
            f"[!] Warning: RLIMIT_NOFILE is {soft_nofile}, which may be low for N={max_n}. "
            "Run 'ulimit -n 65535' first.",
            file=sys.stderr,
            flush=True,
        )

    if not os.path.isfile("./server") or not os.path.isfile("./client"):
        subprocess.run(["make"], check=True)

    if not os.path.isfile(os.path.join("media", args.wav)):
        subprocess.run(["./media/generate_wav.sh"], check=True)

    os.makedirs(args.results_dir, exist_ok=True)
    all_summaries = []

    print("==========================================================================", flush=True)
    print(" PODFLOW MULTI-CLIENT LOAD & RESOURCE BENCHMARK", flush=True)
    print("==========================================================================", flush=True)
    for idx, n in enumerate(counts):
        res = run_n_clients_scenario(n, args.wav, args.base_port + idx, args.results_dir)
        all_summaries.append(res)

    if not args.skip_backpressure:
        bp = run_backpressure_scenario(args.base_port + len(counts) + 1, args.results_dir)
        all_summaries.append(bp)

    with open(os.path.join(args.results_dir, "all_scenarios.json"), "w") as f:
        json.dump(all_summaries, f, indent=2)

    print("==========================================================================", flush=True)
    print(f" ALL BENCHMARKS COMPLETE. Artifacts saved in ./{args.results_dir}/", flush=True)
    print("==========================================================================", flush=True)


if __name__ == "__main__":
    main()