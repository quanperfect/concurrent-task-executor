#!/usr/bin/env python3
"""Summarize producer-lock probes separately from normal throughput measurements."""

import argparse
import csv
import json
import statistics
from collections import defaultdict
from pathlib import Path


def rows(path):
    with Path(path).open(newline="") as stream:
        reader = csv.DictReader(stream)
        if not reader.fieldnames or len(reader.fieldnames) != len(set(reader.fieldnames)):
            raise ValueError("missing or duplicate CSV columns")
        result = list(reader)
        if any(None in row or None in row.values() for row in result):
            raise ValueError("invalid CSV row")
        return result


def percentile(values, fraction):
    ordered = sorted(values)
    if not ordered:
        raise ValueError("no lock samples")
    position = (len(ordered) - 1) * fraction
    low = int(position)
    high = min(low + 1, len(ordered) - 1)
    return ordered[low] + (ordered[high] - ordered[low]) * (position - low)


def summarize(release_path, diagnostic_path):
    release = rows(release_path)
    diagnostic = rows(diagnostic_path)
    if not release or not diagnostic:
        raise ValueError("missing batch measurements")

    def index(measurements, instrumented):
        result = {}
        orders = set()
        identity = ("session_id", "revision", "dirty_tree", "workload", "tasks", "rounds",
                    "bytes_per_task", "seed", "compiler_id", "stdlib_id", "warmups",
                    "repetitions", "order_seed", "checksum")
        first = measurements[0]
        for row in measurements:
            required = set(identity) | {"schema_version", "case_id", "block_id", "order_index",
                                       "mode", "workers", "producers", "elapsed_ns", "completed_tasks",
                                       "status", "validation_ok", "instrumented", "build_profile"}
            if not required.issubset(row):
                raise ValueError("missing diagnostic batch columns")
            if any(row[field] != first[field] for field in identity):
                raise ValueError("mixed diagnostic batch identity")
            key = (row["case_id"], int(row["block_id"]))
            if key in result:
                raise ValueError("duplicate batch")
            if (row["schema_version"] != "1" or row["status"] != "ok" or row["validation_ok"] != "true" or
                    row["instrumented"] != instrumented or row["build_profile"] != "Release" or
                    int(row["elapsed_ns"]) <= 0 or row["completed_tasks"] != row["tasks"]):
                raise ValueError("invalid batch measurement")
            result[key] = row
            mode = row["mode"]
            workers, producers, tasks = (int(row[field]) for field in ("workers", "producers", "tasks"))
            if mode not in {"sequential", "pool", "cold_batch", "producer_pool"} or tasks <= 0:
                raise ValueError("invalid diagnostic scenario")
            if not 1 <= producers <= tasks or (mode != "producer_pool" and producers != 1):
                raise ValueError("invalid diagnostic producers")
            if (mode == "sequential" and workers != 0) or (mode != "sequential" and workers <= 0):
                raise ValueError("invalid diagnostic workers")
            if row["case_id"] != f"{mode}_w{workers}_p{producers}":
                raise ValueError("diagnostic case identity mismatch")
            order = int(row["order_index"])
            if order in orders:
                raise ValueError("duplicate diagnostic order index")
            orders.add(order)
        repetitions = int(first["repetitions"])
        case_ids = {key[0] for key in result}
        expected = {(case, block) for case in case_ids for block in range(repetitions)}
        if repetitions <= 0 or set(result) != expected:
            raise ValueError("incomplete diagnostic repetition blocks")
        if orders != set(range(len(measurements))):
            raise ValueError("incomplete diagnostic order indices")
        return result

    baseline, probes = index(release, "false"), index(diagnostic, "true")
    if set(baseline) != set(probes):
        raise ValueError("diagnostic and release cases/blocks differ")
    same = ("revision", "mode", "workers", "producers", "tasks", "workload", "rounds", "bytes_per_task",
            "seed", "compiler_id", "stdlib_id", "warmups", "repetitions", "order_seed", "checksum")
    for key, probe in probes.items():
        if any(probe[k] != baseline[key][k] for k in same):
            raise ValueError("diagnostic/release workload or build mismatch")
    samples = defaultdict(list)
    indices = defaultdict(set)
    strides = set()
    for sample in rows(str(diagnostic_path) + ".locks.csv"):
        if not {"session_id", "case_id", "block_id", "producer", "sample_index",
                "sample_stride", "wait_ns", "hold_ns"}.issubset(sample):
            raise ValueError("missing lock sample columns")
        key = (sample["case_id"], int(sample["block_id"]))
        if key not in probes or sample["session_id"] != probes[key]["session_id"]:
            raise ValueError("sample has no matching diagnostic batch")
        if probes[key]["mode"] == "sequential":
            raise ValueError("sequential execution has no queue-lock samples")
        producer, index_value = int(sample["producer"]), int(sample["sample_index"])
        stride, wait, hold = int(sample["sample_stride"]), int(sample["wait_ns"]), int(sample["hold_ns"])
        if not 0 <= producer < int(probes[key]["producers"]) or stride < 1 or min(wait, hold) < 0:
            raise ValueError("invalid lock sample")
        if index_value in indices[(key, producer)]:
            raise ValueError("duplicate lock sample")
        indices[(key, producer)].add(index_value)
        strides.add(stride)
        samples[key[0]].append((wait, hold))
    if len(strides) != 1:
        raise ValueError("missing or mixed sampling stride")
    stride = next(iter(strides))
    for key, batch in probes.items():
        if batch["mode"] == "sequential":
            continue
        tasks, producers = int(batch["tasks"]), int(batch["producers"])
        for p in range(producers):
            task_count = tasks * (p + 1) // producers - tasks * p // producers
            expected = (task_count + stride - 1) // stride
            if indices[(key, p)] != set(range(expected)):
                raise ValueError("missing lock samples")
    summaries = []
    for case, values in sorted(samples.items()):
        waits, holds = zip(*values)
        batch_keys = [key for key in probes if key[0] == case]
        ratios = [int(probes[key]["elapsed_ns"]) / int(baseline[key]["elapsed_ns"])
                  for key in batch_keys]
        summaries.append({"case_id": case, "samples": len(values), "stride": stride,
                          "wait_p50_ns": statistics.median(waits), "wait_p95_ns": percentile(waits, .95),
                          "hold_p50_ns": statistics.median(holds), "hold_p95_ns": percentile(holds, .95),
                          "probe_to_release_batch_ratio": statistics.median(ratios)})
    return {"release_raw": str(release_path), "diagnostic_raw": str(diagnostic_path),
            "caveat": "Instrumented producer-side samples include timer cost and OS scheduling. "
                      "Batch ratios compare separate sessions and include environmental noise; "
                      "they are not a causal estimate of intrinsic instrumentation cost.",
            "cases": summaries}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--release", type=Path, required=True)
    parser.add_argument("--diagnostic", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    result = summarize(args.release, args.diagnostic)
    md = args.output.with_suffix(".md")
    if args.output.exists() or md.exists():
        parser.error("output already exists")
    lines = ["# Queue-lock diagnostic summary", "", result["caveat"], "",
             "| Case | Samples | Wait p50 / p95 ns | Hold p50 / p95 ns | Probe/release batch ratio |",
             "| --- | ---: | ---: | ---: | ---: |"]
    for case in result["cases"]:
        lines.append(f"| {case['case_id']} | {case['samples']} | "
                     f"{case['wait_p50_ns']:.0f} / {case['wait_p95_ns']:.0f} | "
                     f"{case['hold_p50_ns']:.0f} / {case['hold_p95_ns']:.0f} | "
                     f"{case['probe_to_release_batch_ratio']:.2f}× |")
    with args.output.open("x") as stream:
        json.dump(result, stream, indent=2)
        stream.write("\n")
    with md.open("x") as stream:
        stream.write("\n".join(lines) + "\n")
    print(md)


if __name__ == "__main__":
    main()
