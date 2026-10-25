#!/usr/bin/env python3
"""plot_hist.py — render the latency histogram from bench.cpp's CSV.

Section 9.3: x = latency bucket, y = count on a LOG scale (most ops are fast,
the tail is rare but is the story). Also prints the percentile table so the
image and the numbers agree.

Usage:
    python tools/plot_hist.py [docs/bench.csv] [docs/latency_hist.png]

Depends only on matplotlib (+ the stdlib). No pandas/numpy required.
"""
import csv
import sys
from collections import defaultdict

import matplotlib
matplotlib.use("Agg")  # headless: write a file, never open a window
import matplotlib.pyplot as plt


def load(path):
    by_op = defaultdict(list)
    allv = []
    with open(path, newline="") as f:
        r = csv.DictReader(f)
        for row in r:
            ns = int(row["ns"])
            by_op[row["op"]].append(ns)
            allv.append(ns)
    return allv, by_op


def pct(sorted_v, p):
    if not sorted_v:
        return 0
    return sorted_v[min(len(sorted_v) - 1, int(p * (len(sorted_v) - 1)))]


def main():
    csv_path = sys.argv[1] if len(sys.argv) > 1 else "docs/bench.csv"
    png_path = sys.argv[2] if len(sys.argv) > 2 else "docs/latency_hist.png"

    allv, by_op = load(csv_path)
    if not allv:
        print("no data in", csv_path)
        return 1

    s = sorted(allv)
    print(f"samples: {len(s)}")
    for label, p in [("p50", .50), ("p90", .90), ("p99", .99),
                     ("p99.9", .999), ("max", 1.0)]:
        print(f"  {label:>6}: {pct(s, p):>8} ns")

    # Clip the extreme tail for a readable x-axis, but keep p99.9 visible.
    hi = pct(s, 0.999) * 2 + 1

    fig, ax = plt.subplots(figsize=(9, 5))
    ax.hist([min(v, hi) for v in allv], bins=120, color="#3b7dd8",
            edgecolor="none")
    ax.set_yscale("log")
    ax.set_xlabel("per-operation latency (ns)")
    ax.set_ylabel("count (log scale)")
    ax.set_title("Matching engine — per-operation latency distribution")

    p50, p99, p999 = pct(s, .50), pct(s, .99), pct(s, .999)
    for x, lab, col in [(p50, "p50", "#2ca02c"),
                        (p99, "p99", "#ff7f0e"),
                        (p999, "p99.9", "#d62728")]:
        if x <= hi:
            ax.axvline(x, color=col, linestyle="--", linewidth=1)
            ax.text(x, ax.get_ylim()[1] * 0.5, f" {lab}={x}ns",
                    color=col, rotation=90, va="top", fontsize=8)

    fig.tight_layout()
    fig.savefig(png_path, dpi=130)
    print("wrote", png_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
