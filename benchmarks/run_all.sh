#!/usr/bin/env bash
# Runs every experiment from benchmarks/README.md, then summarizes them.
# Build first:
#   cmake --preset gcc-release && cmake --build --preset gcc-release
# Usage: benchmarks/run_all.sh [output-dir]   (default: benchmarks/results/raw)
set -euo pipefail
cd "$(dirname "$0")/.."

bench=build/gcc-release/benchmarks/cte_bench
probe=build/gcc-release/benchmarks/cte_bench_probe
out=${1:-benchmarks/results/raw}
summary=build/benchmark-summaries
rm -rf "$summary"
mkdir -p "$out" "$summary"

cpus=$(nproc)
all_workers=$(seq -s, 1 "$cpus")
some_workers=$(printf '%s\n' 1 2 4 8 16 "$cpus" | sort -nu | paste -sd,)

run() { # run NAME BINARY ARGS... (cte_bench refuses to overwrite an existing file)
    local name=$1 binary=$2
    shift 2
    echo "== $name"
    "$binary" --output "$out/$name.csv" --repetitions 9 "$@"
}

run scaling-compute "$bench" --workload compute --tasks 10000 --rounds 30000 --workers "$all_workers"
run granularity-minimal "$bench" --workload minimal --tasks 10000 --workers "$some_workers"
for rounds in 100 1000 5000; do
    run "granularity-r$rounds" "$bench" --workload compute --tasks 10000 --rounds "$rounds" \
        --workers "$some_workers"
done
run mixed-compute "$bench" --workload mixed_compute --tasks 10000 --rounds 5000 --workers "$some_workers"
run buffer-checksums "$bench" --workload buffer_checksum --tasks 10000 --bytes-per-task 4096 \
    --workers "$some_workers"
run cold-single-task "$bench" --mode cold_batch --workload minimal --tasks 1 --workers "1,4,$cpus"
run contention-release "$bench" --mode producer_pool --workload minimal --tasks 20000 --workers 4,16 \
    --producers 1,2,4,8
run contention-probe "$probe" --mode producer_pool --workload minimal --tasks 20000 --workers 4,16 \
    --producers 1,2,4,8

for csv in "$out"/*.csv; do
    name=$(basename "$csv" .csv)
    case $name in
    contention-probe | *.locks) ;; # instrumented; summarized together with the release run below
    *) python3 scripts/analyze_benchmarks.py "$csv" --output "$summary/$name.json" ;;
    esac
done
python3 scripts/analyze_locks.py --release "$out/contention-release.csv" \
    --diagnostic "$out/contention-probe.csv" --output "$summary/contention-locks.json"
echo "Summaries are in $summary. To redraw the chart (needs matplotlib, see scripts/requirements.txt):"
echo "  python3 scripts/plot_benchmarks.py $summary/scaling-compute.json --output benchmarks/results/scaling.png"
