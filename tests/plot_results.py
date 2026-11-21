#!/usr/bin/env python3
"""
PodFlow Benchmark Plot Generator
Generates publication-ready PNG plots from results/ using matplotlib and numpy only.
"""

import argparse
import csv
import glob
import json
import os
import sys
import numpy as np
import matplotlib

matplotlib.use("Agg")  # Headless backend for WSL2/Linux without X11
import matplotlib.pyplot as plt  # noqa: E402


def load_scenarios(results_dir):
    """Load all n_<N>/summary.json files sorted by N ascending."""
    summaries = []
    for path in glob.glob(os.path.join(results_dir, "n_*", "summary.json")):
        with open(path, "r") as f:
            data = json.load(f)
            data["_dir"] = os.path.dirname(path)
            summaries.append(data)
    summaries.sort(key=lambda x: x["n_clients"])
    return summaries


def load_raw_d_ms(scenario_dir):
    raw_path = os.path.join(scenario_dir, "raw_d_ms.csv")
    vals = []
    if not os.path.isfile(raw_path):
        return np.array([], dtype=np.float64)
    with open(raw_path, "r", newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            try:
                vals.append(float(row["d_ms"]))
            except (ValueError, KeyError):
                continue
    return np.array(vals, dtype=np.float64)


def plot_jitter_cdf(scenarios, out_dir):
    plt.figure(figsize=(8, 5))
    for sc in scenarios:
        n = sc["n_clients"]
        d = load_raw_d_ms(sc["_dir"])
        if len(d) == 0:
            continue
        d_sorted = np.sort(d)
        cdf = np.arange(1, len(d_sorted) + 1) / float(len(d_sorted))
        plt.plot(d_sorted, cdf, linewidth=1.8, label=f"N = {n} clients")

    plt.title("Empirical CDF of Inter-Chunk Jitter Deviation ($D_i$)")
    plt.xlabel(r"Jitter Deviation $D_i = |\Delta_i - 50\,\mathrm{ms}|$ (ms)")
    plt.ylabel("Cumulative Probability (CDF)")
    plt.grid(True, linestyle="--", alpha=0.5)
    plt.legend(loc="lower right")
    plt.tight_layout()
    plt.savefig(os.path.join(out_dir, "jitter_cdf.png"), dpi=150)
    plt.close()


def plot_jitter_histogram(scenarios, out_dir):
    plt.figure(figsize=(8, 5))
    selected = [scenarios[0]] if len(scenarios) == 1 else [scenarios[0], scenarios[-1]]
    for sc in selected:
        n = sc["n_clients"]
        d = load_raw_d_ms(sc["_dir"])
        if len(d) == 0:
            continue
        upper = max(1.0, float(np.percentile(d, 99.5)))
        plt.hist(
            d[d <= upper],
            bins=40,
            alpha=0.6,
            density=True,
            label=f"N = {n} (max={np.max(d):.2f} ms)",
        )

    plt.title("Distribution Histogram of Jitter Deviation ($D_i$)")
    plt.xlabel("Jitter Deviation $D_i$ (ms)")
    plt.ylabel("Probability Density")
    plt.grid(True, linestyle="--", alpha=0.5)
    plt.legend()
    plt.tight_layout()
    plt.savefig(os.path.join(out_dir, "jitter_hist.png"), dpi=150)
    plt.close()


def plot_p95_jitter_vs_n(scenarios, out_dir):
    ns = [sc["n_clients"] for sc in scenarios]
    p50s = [sc["jitter_ms"]["p50"] for sc in scenarios]
    p95s = [sc["jitter_ms"]["p95"] for sc in scenarios]
    p99s = [sc["jitter_ms"]["p99"] for sc in scenarios]

    plt.figure(figsize=(8, 5))
    plt.plot(ns, p50s, marker="o", linewidth=2, label="p50 (Median) Jitter")
    plt.plot(ns, p95s, marker="s", linewidth=2, label="p95 Jitter")
    plt.plot(ns, p99s, marker="^", linewidth=2, label="p99 Jitter")

    plt.title("PodFlow Jitter Percentiles vs. Concurrent Clients ($N$)")
    plt.xlabel("Concurrent Clients ($N$)")
    plt.ylabel("Jitter Deviation $D_i$ (ms)")
    plt.grid(True, linestyle="--", alpha=0.5)
    plt.legend()
    plt.tight_layout()
    plt.savefig(os.path.join(out_dir, "p95_jitter_vs_n.png"), dpi=150)
    plt.close()


def plot_cpu_rss_vs_n(scenarios, out_dir):
    ns = [sc["n_clients"] for sc in scenarios]
    cpu_max = [sc["server_resources"]["cpu_max_pct"] for sc in scenarios]
    cpu_avg = [sc["server_resources"]["cpu_avg_pct"] for sc in scenarios]
    rss_max = [sc["server_resources"]["rss_max_mb"] for sc in scenarios]

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(12, 4.8))

    ax1.plot(ns, cpu_max, marker="o", color="tab:red", linewidth=2, label="Peak CPU %")
    ax1.plot(ns, cpu_avg, marker="x", linestyle="--", color="tab:orange", linewidth=1.8, label="Avg CPU %")
    ax1.set_title("Server CPU Utilization vs. Clients ($N$)")
    ax1.set_xlabel("Concurrent Clients ($N$)")
    ax1.set_ylabel("CPU Utilization (%)")
    ax1.grid(True, linestyle="--", alpha=0.5)
    ax1.legend()

    ax2.plot(ns, rss_max, marker="s", color="tab:blue", linewidth=2, label="Peak RSS Memory")
    ax2.set_title("Server Resident Memory (RSS) vs. Clients ($N$)")
    ax2.set_xlabel("Concurrent Clients ($N$)")
    ax2.set_ylabel("Resident Set Size (MB)")
    ax2.grid(True, linestyle="--", alpha=0.5)
    ax2.legend()

    plt.tight_layout()
    plt.savefig(os.path.join(out_dir, "server_cpu_rss_vs_n.png"), dpi=150)
    plt.close()


def plot_underruns_resets_vs_n(scenarios, out_dir):
    ns = [sc["n_clients"] for sc in scenarios]
    underruns = [sc["server_telemetry"]["underruns"] for sc in scenarios]
    late_resets = [sc["server_telemetry"]["late_resets"] for sc in scenarios]

    x = np.arange(len(ns))
    width = 0.35

    plt.figure(figsize=(8, 5))
    plt.bar(x - width / 2, underruns, width, label="Ring Buffer Underruns")
    plt.bar(x + width / 2, late_resets, width, label="Pacer Late Resets (>5 intervals)")

    plt.xticks(x, [str(n) for n in ns])
    plt.title("Server Underruns & Pacer Late Resets vs. Clients ($N$)")
    plt.xlabel("Concurrent Clients ($N$)")
    plt.ylabel("Event Count")
    plt.grid(True, axis="y", linestyle="--", alpha=0.5)
    plt.legend()
    plt.tight_layout()
    plt.savefig(os.path.join(out_dir, "underruns_resets_vs_n.png"), dpi=150)
    plt.close()


def plot_sync_skew_vs_n(scenarios, out_dir):
    multi = [sc for sc in scenarios if sc["n_clients"] > 1]
    if not multi:
        return

    ns = [sc["n_clients"] for sc in multi]
    p50_skew = [sc["sync_skew_ms"]["p50"] for sc in multi]
    p95_skew = [sc["sync_skew_ms"]["p95"] for sc in multi]
    max_skew = [sc["sync_skew_ms"]["max"] for sc in multi]

    plt.figure(figsize=(8, 5))
    plt.plot(ns, p50_skew, marker="o", linewidth=2, label="p50 Sync Skew")
    plt.plot(ns, p95_skew, marker="s", linewidth=2, label="p95 Sync Skew")
    plt.plot(ns, max_skew, marker="^", linestyle="--", linewidth=1.8, label="Max Sync Skew")

    plt.title("Inter-Client Playback Sync Skew vs. Concurrent Clients ($N$)")
    plt.xlabel("Concurrent Clients ($N$)")
    plt.ylabel("Chunk Arrival Spread max(t) - min(t) (ms)")
    plt.grid(True, linestyle="--", alpha=0.5)
    plt.legend()
    plt.tight_layout()
    plt.savefig(os.path.join(out_dir, "sync_skew_vs_n.png"), dpi=150)
    plt.close()


def main():
    parser = argparse.ArgumentParser(description="Generate PodFlow benchmark PNG charts.")
    parser.add_argument("--results-dir", default="results", help="Directory containing n_* scenario folders")
    args = parser.parse_args()

    scenarios = load_scenarios(args.results_dir)
    if not scenarios:
        print(f"[-] No scenario summaries found in {args.results_dir}/n_*/summary.json", file=sys.stderr)
        sys.exit(1)

    plot_jitter_cdf(scenarios, args.results_dir)
    plot_jitter_histogram(scenarios, args.results_dir)
    plot_p95_jitter_vs_n(scenarios, args.results_dir)
    plot_cpu_rss_vs_n(scenarios, args.results_dir)
    plot_underruns_resets_vs_n(scenarios, args.results_dir)
    plot_sync_skew_vs_n(scenarios, args.results_dir)

    print(f"[+] Successfully generated 6 benchmark plots in ./{args.results_dir}/:")
    for fname in [
        "jitter_cdf.png",
        "jitter_hist.png",
        "p95_jitter_vs_n.png",
        "server_cpu_rss_vs_n.png",
        "underruns_resets_vs_n.png",
        "sync_skew_vs_n.png",
    ]:
        print(f"  - {os.path.join(args.results_dir, fname)}")


if __name__ == "__main__":
    main()