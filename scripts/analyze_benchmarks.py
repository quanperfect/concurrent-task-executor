#!/usr/bin/env python3
"""Validate raw benchmark batches and summarize them with paired speedup ratios."""

from __future__ import annotations

import argparse
import csv
import json
import statistics
from collections import defaultdict
from pathlib import Path


def distribution(values: list[float]) -> dict[str, float]:
    ordered = sorted(values)
    if not ordered:
        raise ValueError("empty distribution")
    # Linear interpolation at (n - 1) * p, including small smoke fixtures.
    def quantile(p: float) -> float:
        index = (len(ordered) - 1) * p
        low = int(index)
        high = min(low + 1, len(ordered) - 1)
        return ordered[low] + (ordered[high] - ordered[low]) * (index - low)

    return {"median": statistics.median(ordered), "q1": quantile(0.25),
            "q3": quantile(0.75), "min": ordered[0], "max": ordered[-1]}


def analyze(path: Path) -> dict:
    with path.open(newline="") as stream:
        reader = csv.DictReader(stream)
        if reader.fieldnames and len(reader.fieldnames) != len(set(reader.fieldnames)):
            raise ValueError("duplicate CSV column")
        rows = list(reader)
    if not rows:
        raise ValueError("no measurements")
    identity_keys = ("session_id", "revision", "dirty_tree", "workload", "tasks", "rounds",
                     "bytes_per_task", "seed", "build_profile", "compiler_id", "stdlib_id",
                     "warmups", "repetitions", "order_seed", "checksum")
    required = set(identity_keys) | {"schema_version", "case_id", "block_id", "order_index",
                                    "mode", "workers", "producers", "elapsed_ns",
                                    "completed_tasks", "validation_ok", "status", "instrumented"}
    if not required.issubset(rows[0]) or any(None in row or any(row.get(k) is None for k in required)
                                          for row in rows):
        raise ValueError("invalid CSV schema")
    identity = {key: rows[0][key] for key in identity_keys}
    blocks: dict[int, dict[str, dict]] = defaultdict(dict)
    cases: dict[str, list[dict]] = defaultdict(list)
    order_indices = set()
    expected_repetitions = int(identity["repetitions"])
    if expected_repetitions <= 0 or int(identity["tasks"]) <= 0:
        raise ValueError("invalid task/repetition count")
    for row in rows:
        if any(row[key] != identity[key] for key in identity_keys):
            raise ValueError("mixed workload, build, result, or session identity")
        if (row["schema_version"] != "1" or row["validation_ok"] != "true" or
                row["status"] != "ok" or row["instrumented"] != "false" or
                row["build_profile"] != "Release"):
            raise ValueError("invalid, instrumented, or non-Release measurement")
        if int(row["completed_tasks"]) != int(row["tasks"]) or int(row["elapsed_ns"]) <= 0:
            raise ValueError("incomplete batch or nonpositive duration")
        mode = row["mode"]
        workers, producers = int(row["workers"]), int(row["producers"])
        if mode not in {"sequential", "pool", "cold_batch", "producer_pool"}:
            raise ValueError("unknown mode")
        if producers < 1 or producers > int(row["tasks"]):
            raise ValueError("invalid producer count")
        if mode == "sequential":
            if workers != 0 or producers != 1:
                raise ValueError("invalid sequential baseline")
        elif workers < 1 or (mode != "producer_pool" and producers != 1):
            raise ValueError("invalid pool scenario")
        case = f"{mode}_w{workers}_p{producers}"
        if row["case_id"] != case:
            raise ValueError("case identity does not match scenario")
        block, order = int(row["block_id"]), int(row["order_index"])
        if block < 0 or case in blocks[block] or order in order_indices:
            raise ValueError("duplicate or invalid case/block/order")
        blocks[block][case] = row
        cases[case].append(row)
        order_indices.add(order)
    if set(blocks) != set(range(expected_repetitions)):
        raise ValueError("missing repetition block")
    if order_indices != set(range(len(rows))):
        raise ValueError("missing order index")
    expected_cases = set(cases)
    if any(set(block) != expected_cases for block in blocks.values()):
        raise ValueError("incomplete comparison block")
    if any(row["mode"] == "pool" for row in rows):
        if not {"sequential_w0_p1", "pool_w1_p1"}.issubset(expected_cases):
            raise ValueError("pool scaling requires paired direct and one-worker baselines")

    summaries = []
    for case, measurements in sorted(cases.items(),
                                     key=lambda item: (item[1][0]["mode"],
                                                       int(item[1][0]["producers"]),
                                                       int(item[1][0]["workers"]))):
        first = measurements[0]
        summary = {"case_id": case, "mode": first["mode"], "workers": int(first["workers"]),
                   "producers": int(first["producers"]), "repetitions": len(measurements),
                   "elapsed_ms": distribution([int(r["elapsed_ns"]) / 1e6 for r in measurements]),
                   "throughput_tasks_s": distribution([int(r["tasks"]) * 1e9 / int(r["elapsed_ns"])
                                                        for r in measurements])}
        if first["mode"] == "pool":
            direct, pool, overhead = [], [], []
            for row in measurements:
                pair = blocks[int(row["block_id"])]
                ns = int(row["elapsed_ns"])
                seq_ns = int(pair["sequential_w0_p1"]["elapsed_ns"])
                one_ns = int(pair["pool_w1_p1"]["elapsed_ns"])
                direct.append(seq_ns / ns)
                pool.append(one_ns / ns)
                overhead.append((one_ns - seq_ns) / int(row["tasks"]))
            summary["speedup_direct"] = distribution(direct)
            summary["speedup_pool"] = distribution(pool)
            summary["efficiency_pool"] = distribution([x / summary["workers"] for x in pool])
            if summary["workers"] == 1:
                summary["net_executor_ns_per_task"] = distribution(overhead)
        summaries.append(summary)
    return {"schema_version": 1, "raw_file": str(path), "identity": identity,
            "ratios": "median of paired-block ratios; quartiles use linear interpolation",
            "cases": summaries}


def markdown(report: dict) -> str:
    def ratio(value):
        if value is None:
            return "—"
        return f"{value:.3g}×" if value < .01 else f"{value:.2f}×"

    identity = report["identity"]
    lines = ["# Benchmark summary", "", f"Source: `{report['raw_file']}`", "",
             f"Workload: **{identity['workload']}**, {identity['tasks']} tasks; "
             f"rounds={identity['rounds'] or 'n/a'}, bytes/task={identity['bytes_per_task'] or 'n/a'}.",
             f"Revision: `{identity['revision']}`; dirty tree: {identity['dirty_tree']}.", "",
             "Medians with interquartile ranges; ratios are paired within repetition blocks.", "",
             "| Mode | W | P | Batch ms [Q1, Q3] | Tasks/s | Direct speedup | Pool speedup |",
             "| --- | ---: | ---: | ---: | ---: | ---: | ---: |"]
    for case in report["cases"]:
        timing = case["elapsed_ms"]
        direct = case.get("speedup_direct", {}).get("median")
        pool = case.get("speedup_pool", {}).get("median")
        lines.append(f"| {case['mode']} | {case['workers']} | {case['producers']} | "
                     f"{timing['median']:.3f} [{timing['q1']:.3f}, {timing['q3']:.3f}] | "
                     f"{case['throughput_tasks_s']['median']:,.0f} | "
                     f"{ratio(direct)} | {ratio(pool)} |")
    lines.append("")
    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("raw", type=Path)
    parser.add_argument("--output", type=Path, required=True, help="new JSON summary path")
    args = parser.parse_args()
    report = analyze(args.raw)
    md_path = args.output.with_suffix(".md")
    if args.output.exists() or md_path.exists():
        parser.error("summary output already exists")
    with args.output.open("x") as stream:
        json.dump(report, stream, indent=2)
        stream.write("\n")
    with md_path.open("x") as stream:
        stream.write(markdown(report))
    print(md_path)


if __name__ == "__main__":
    main()
