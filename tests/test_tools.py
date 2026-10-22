"""Synthetic analyzer fixtures and real benchmark CLI integration checks."""

import csv
import importlib.util
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("analysis", ROOT / "scripts/analyze_benchmarks.py")
analysis = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(analysis)
LOCK_SPEC = importlib.util.spec_from_file_location("locks", ROOT / "scripts/analyze_locks.py")
locks = importlib.util.module_from_spec(LOCK_SPEC)
LOCK_SPEC.loader.exec_module(locks)
BINARY = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else None
PROBE = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else None


def synthetic_rows():
    rows = []
    for block, durations in enumerate(((100, 160, 50), (120, 180, 80))):
        for (mode, workers), duration in zip((("sequential", 0), ("pool", 1), ("pool", 2)), durations):
            rows.append(dict(schema_version="1", session_id="SYNTHETIC-TEST-ONLY",
                             case_id=f"{mode}_w{workers}_p1", block_id=str(block),
                             order_index=str(len(rows)), revision="a" * 40, dirty_tree="false",
                             mode=mode, workload="minimal", workers=str(workers), producers="1",
                             tasks="10", rounds="", bytes_per_task="", seed="1",
                             elapsed_ns=str(duration), completed_tasks="10", checksum="123",
                             validation_ok="true", status="ok", build_profile="Release",
                             compiler_id="synthetic", stdlib_id="synthetic", instrumented="false",
                             warmups="1", repetitions="2", order_seed="42"))
    return rows


class AnalyzerTests(unittest.TestCase):
    def analyze(self, rows):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "synthetic.csv"
            with path.open("w", newline="") as stream:
                writer = csv.DictWriter(stream, fieldnames=list(synthetic_rows()[0]))
                writer.writeheader()
                writer.writerows(rows)
            return analysis.analyze(path)

    def test_paired_ratios_and_units(self):
        result = self.analyze(synthetic_rows())
        pool = next(case for case in result["cases"] if case["workers"] == 2)
        self.assertAlmostEqual(pool["speedup_direct"]["median"], 1.75)
        self.assertAlmostEqual(pool["speedup_pool"]["median"], 2.725)
        self.assertAlmostEqual(pool["throughput_tasks_s"]["median"], 162_500_000)
        one = next(case for case in result["cases"] if case["workers"] == 1)
        self.assertAlmostEqual(one["net_executor_ns_per_task"]["median"], 6)

    def test_invalid_measurements_are_rejected(self):
        for key, value in [("validation_ok", "false"), ("instrumented", "true"),
                           ("status", "failed"), ("build_profile", "Debug"),
                           ("completed_tasks", "9"), ("elapsed_ns", "0"),
                           ("checksum", "999"), ("compiler_id", "different")]:
            with self.subTest(key=key):
                rows = synthetic_rows()
                rows[0][key] = value
                with self.assertRaises(ValueError):
                    self.analyze(rows)

    def test_missing_duplicate_and_mismatched_cases_are_rejected(self):
        cases = [synthetic_rows()[:-1], synthetic_rows() + [synthetic_rows()[0]],
                 [r for r in synthetic_rows() if r["workers"] != "1"]]
        rows = synthetic_rows()
        rows[0]["case_id"] = "pool_w99_p1"
        cases.append(rows)
        for rows in cases:
            with self.assertRaises(ValueError):
                self.analyze(rows)

    def test_block_completeness_cannot_be_hidden_by_renumbering(self):
        rows = synthetic_rows()[:-1]
        for i, row in enumerate(rows):
            row["order_index"] = str(i)
        with self.assertRaisesRegex(ValueError, "incomplete comparison"):
            self.analyze(rows)

    def test_lock_report_rejects_missing_blocks_on_both_sides(self):
        # Both files lose the same block, so matching key sets alone is insufficient.
        release = [r for r in synthetic_rows() if r["block_id"] == "0"]
        diagnostic = [dict(r, instrumented="true") for r in release]
        with tempfile.TemporaryDirectory() as directory:
            paths = [Path(directory) / "release.csv", Path(directory) / "probe.csv"]
            for path, values in zip(paths, (release, diagnostic)):
                with path.open("w", newline="") as stream:
                    writer = csv.DictWriter(stream, fieldnames=list(values[0]))
                    writer.writeheader()
                    writer.writerows(values)
            with self.assertRaisesRegex(ValueError, "incomplete diagnostic repetition"):
                locks.summarize(*paths)

    def test_lock_report_rejects_malformed_matching_batches(self):
        # Corrupt both sides identically: matching files still need valid scenarios.
        for field, value in (("workers", "-1"), ("producers", "0"), ("tasks", "0"),
                             ("mode", "unknown"), ("case_id", "wrong"), ("order_index", "0")):
            with self.subTest(field=field), tempfile.TemporaryDirectory() as directory:
                values = synthetic_rows()
                for row in values:
                    row[field] = value
                paths = [Path(directory) / "release.csv", Path(directory) / "probe.csv"]
                for path, flag in zip(paths, ("false", "true")):
                    with path.open("w", newline="") as stream:
                        writer = csv.DictWriter(stream, fieldnames=list(values[0]))
                        writer.writeheader()
                        writer.writerows(dict(row, instrumented=flag) for row in values)
                with self.assertRaises(ValueError):
                    locks.summarize(*paths)

    def test_lock_csv_rejects_duplicate_columns_and_truncated_rows(self):
        for content in ("a,a\n1,2\n", "a,b\n1\n", "a,b\n1,2,3\n"):
            with self.subTest(content=content), tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / "invalid.csv"
                path.write_text(content)
                with self.assertRaises(ValueError):
                    locks.rows(path)


@unittest.skipIf(BINARY is None, "benchmark executable not supplied")
class CliTests(unittest.TestCase):
    def run_bench(self, path, *extra, binary=None):
        return subprocess.run([str(binary or BINARY), "--output", str(path), *extra],
                              capture_output=True, text=True, timeout=20)

    def test_real_sweep_validates_and_preserves_csv_strings(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "smoke.csv"
            session = 'comma, quote" and newline\n'
            result = self.run_bench(path, "--workload", "minimal", "--tasks", "73",
                                    "--workers", "1,2", "--repetitions", "2", "--session", session)
            self.assertEqual(result.returncode, 0, result.stderr)
            with path.open(newline="") as stream:
                rows = list(csv.DictReader(stream))
            self.assertEqual(len(rows), 6)
            self.assertEqual({r["session_id"] for r in rows}, {session})
            self.assertEqual({r["validation_ok"] for r in rows}, {"true"})
            self.assertEqual({r["completed_tasks"] for r in rows}, {"73"})
            self.assertEqual(len({r["checksum"] for r in rows}), 1)
            self.assertTrue(all(r["stdlib_id"].startswith(("libstdc++-", "libc++-"))
                                for r in rows))
            previous = path.read_bytes()
            result = self.run_bench(path, "--tasks", "1", "--repetitions", "1")
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(path.read_bytes(), previous)

    def test_invalid_arguments_fail(self):
        cases = [("--workers", "0"), ("--tasks", "-1"), ("--tasks", "0"),
                 ("--workers", "1,1"), ("--workers", "1,"), ("--mode", "unknown"),
                 ("--producers", "2"), ("--repetitions", "0"), ("--unknown", "1"),
                 ("--tasks", "1", "--tasks", "2"), ("--mode", "sequential", "--workers", "1")]
        for extra in cases:
            with self.subTest(extra=extra), tempfile.TemporaryDirectory() as directory:
                result = self.run_bench(Path(directory) / "bad.csv", *extra)
                self.assertNotEqual(result.returncode, 0)

    def test_probe_uses_separate_flagged_output_and_exact_sample_counts(self):
        if PROBE is None:
            self.skipTest("probe binary not supplied")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "probe.csv"
            result = self.run_bench(path, "--mode", "producer_pool", "--workload", "minimal",
                                    "--workers", "2", "--producers", "3", "--tasks", "73",
                                    "--repetitions", "1", "--probe-stride", "4", binary=PROBE)
            self.assertEqual(result.returncode, 0, result.stderr)
            with path.open(newline="") as stream:
                row = next(csv.DictReader(stream))
            self.assertEqual(row["instrumented"], "true")
            with Path(str(path) + ".locks.csv").open(newline="") as stream:
                samples = list(csv.DictReader(stream))
            self.assertEqual(len(samples), 19) # partitions 24, 24, 25; every fourth task
            self.assertTrue(all(int(s["wait_ns"]) >= 0 and int(s["hold_ns"]) >= 0 for s in samples))

    def test_lock_analyzer_detects_incomplete_samples(self):
        if PROBE is None:
            self.skipTest("probe binary not supplied")
        with tempfile.TemporaryDirectory() as directory:
            release = Path(directory) / "release.csv"
            diagnostic = Path(directory) / "diagnostic.csv"
            common = ("--mode", "producer_pool", "--workload", "minimal", "--workers", "2",
                      "--producers", "3", "--tasks", "73", "--repetitions", "1")
            normal = self.run_bench(release, *common)
            probed = self.run_bench(diagnostic, *common, "--probe-stride", "4", binary=PROBE)
            self.assertEqual(normal.returncode, 0, normal.stderr)
            self.assertEqual(probed.returncode, 0, probed.stderr)
            # Debug/sanitizer builds still exercise the runner; report extraction
            # deliberately permits only the release build's timings.
            with release.open(newline="") as stream:
                first = next(csv.DictReader(stream))
            if first["build_profile"] != "Release" or first["instrumented"] != "false":
                with self.assertRaises(ValueError):
                    locks.summarize(release, diagnostic)
                return
            summary = locks.summarize(release, diagnostic)
            self.assertEqual(summary["cases"][0]["samples"], 19)
            sample_path = Path(str(diagnostic) + ".locks.csv")
            lines = sample_path.read_text().splitlines()
            sample_path.write_text("\n".join(lines[:-1]) + "\n")
            with self.assertRaisesRegex(ValueError, "missing lock samples"):
                locks.summarize(release, diagnostic)


if __name__ == "__main__":
    unittest.main(argv=sys.argv[:1])
