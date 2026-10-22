#!/usr/bin/env python3
"""Render an exported scaling figure from a validated analysis JSON file."""

import argparse
import json
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("summary", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        parser.error("figure already exists")
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    report = json.loads(args.summary.read_text())
    cases = sorted((c for c in report["cases"] if c["mode"] == "pool"), key=lambda c: c["workers"])
    if not cases:
        parser.error("no pool scaling data")
    fig, axes = plt.subplots(1, 2, figsize=(11, 4.5), layout="constrained")
    workers = [c["workers"] for c in cases]
    for axis, metrics in zip(axes, [("throughput_tasks_s",), ("speedup_direct", "speedup_pool")]):
        for metric in metrics:
            median = [c[metric]["median"] for c in cases]
            lower = [c[metric]["q1"] for c in cases]
            upper = [c[metric]["q3"] for c in cases]
            label = {"throughput_tasks_s": "Completed tasks/s", "speedup_direct": "vs direct sequential",
                     "speedup_pool": "vs one-worker pool"}[metric]
            axis.plot(workers, median, marker="o", markersize=3, label=label)
            axis.fill_between(workers, lower, upper, alpha=0.16)
        axis.set_xlabel("Worker threads")
        axis.grid(alpha=0.2)
        axis.legend()
    axes[0].set_ylabel("Tasks/s")
    axes[1].set_ylabel("Speedup (paired median; shading = IQR)")
    identity = report["identity"]
    fig.suptitle(f"{identity['workload']}: {int(identity['tasks']):,} tasks, "
                 f"{identity['repetitions']} repetition blocks")
    fig.savefig(args.output, dpi=180)
    plt.close(fig)
    print(args.output)


if __name__ == "__main__":
    main()
