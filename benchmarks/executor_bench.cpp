#include "build_info.hpp"
#include "runner.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <numeric>
#include <random>
#include <set>
#include <sstream>
#include <string>

#ifdef CTE_QUEUE_PROBES
#include "diagnostics.hpp"
#endif

namespace {
using namespace cte::bench;

std::uint64_t number(std::string_view value) {
    std::uint64_t result = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size()) {
        throw std::invalid_argument("invalid unsigned integer: " + std::string(value));
    }
    return result;
}

std::vector<std::size_t> counts(std::string_view value, std::size_t maximum) {
    std::vector<std::size_t> result;
    while (!value.empty()) {
        const auto comma = value.find(',');
        const auto parsed = number(value.substr(0, comma));
        if (parsed == 0 || parsed > maximum) {
            throw std::invalid_argument("thread count is outside the supported range");
        }
        const auto count = static_cast<std::size_t>(parsed);
        if (std::find(result.begin(), result.end(), count) != result.end()) {
            throw std::invalid_argument("duplicate thread count");
        }
        result.push_back(count);
        if (comma == std::string_view::npos) {
            break;
        }
        value.remove_prefix(comma + 1);
        if (value.empty()) {
            throw std::invalid_argument("trailing comma in thread list");
        }
    }
    if (result.empty()) {
        throw std::invalid_argument("empty thread list");
    }
    return result;
}

struct options {
    parameters workload;
    std::string mode = "sweep";
    std::vector<std::size_t> workers{1, 2, 4};
    std::vector<std::size_t> producers{1};
    unsigned repetitions = 9;
    unsigned warmups = 1;
    std::uint64_t order_seed = 9172026;
    std::string output;
    std::string session = "manual";
    std::string revision = "unrecorded";
    std::string dirty = "unknown";
    std::size_t probe_stride = 16;
};

options parse(int argc, char** argv) {
    options result;
    std::set<std::string_view> seen;
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 >= argc) {
            throw std::invalid_argument("each option needs a value; see --help");
        }
        const std::string_view key(argv[i]);
        const std::string_view value(argv[i + 1]);
        if (!seen.insert(key).second) {
            throw std::invalid_argument("duplicate option: " + std::string(key));
        }
        if (key == "--workload") {
            result.workload.kind = parse_workload(value);
        } else if (key == "--mode") {
            result.mode = value;
        } else if (key == "--workers") {
            result.workers = counts(value, 1024);
        } else if (key == "--producers") {
            result.producers = counts(value, 256);
        } else if (key == "--tasks") {
            const auto count = number(value);
            if (count == 0 || count > 1'000'000) {
                throw std::invalid_argument("tasks must be in [1,1000000]");
            }
            result.workload.tasks = static_cast<std::size_t>(count);
        } else if (key == "--rounds") {
            result.workload.rounds = number(value);
        } else if (key == "--bytes-per-task") {
            const auto bytes = number(value);
            if (bytes > 256U * 1024U * 1024U) {
                throw std::invalid_argument("bytes/task exceeds input budget");
            }
            result.workload.bytes_per_task = static_cast<std::size_t>(bytes);
        } else if (key == "--seed") {
            result.workload.seed = number(value);
        } else if (key == "--order-seed") {
            result.order_seed = number(value);
        } else if (key == "--repetitions") {
            const auto repetitions = number(value);
            if (repetitions == 0 || repetitions > 1000) {
                throw std::invalid_argument("repetitions must be in [1,1000]");
            }
            result.repetitions = static_cast<unsigned>(repetitions);
        } else if (key == "--warmups") {
            const auto passes = number(value);
            if (passes > 100) {
                throw std::invalid_argument("warmups must be in [0,100]");
            }
            result.warmups = static_cast<unsigned>(passes);
        } else if (key == "--output") {
            result.output = value;
        } else if (key == "--session") {
            result.session = value;
        } else if (key == "--revision") {
            result.revision = value;
        } else if (key == "--dirty-tree") {
            if (value != "true" && value != "false" && value != "unknown") {
                throw std::invalid_argument("dirty-tree must be true, false, or unknown");
            }
            result.dirty = value;
#ifdef CTE_QUEUE_PROBES
        } else if (key == "--probe-stride") {
            const auto stride = number(value);
            if (stride == 0 || stride > 1'000'000) {
                throw std::invalid_argument("probe-stride must be in [1,1000000]");
            }
            result.probe_stride = static_cast<std::size_t>(stride);
#endif
        } else {
            throw std::invalid_argument("unknown option: " + std::string(key));
        }
    }
    if (result.output.empty()) {
        throw std::invalid_argument("--output is required (existing files are never overwritten)");
    }
    if (result.mode != "sweep" && result.mode != "sequential" && result.mode != "pool" &&
        result.mode != "cold_batch" && result.mode != "producer_pool") {
        throw std::invalid_argument("unknown mode");
    }
    if (result.mode != "producer_pool" && result.producers != std::vector<std::size_t>{1}) {
        throw std::invalid_argument("multiple producers require --mode producer_pool");
    }
    if (result.mode == "sequential" && seen.contains("--workers")) {
        throw std::invalid_argument("sequential mode does not take --workers");
    }
    if (*std::max_element(result.producers.begin(), result.producers.end()) >
        result.workload.tasks) {
        throw std::invalid_argument("producers cannot exceed tasks");
    }
    if (result.workload.kind == workload::buffer_checksum && !seen.contains("--bytes-per-task")) {
        result.workload.bytes_per_task = 4096;
    }
    if (result.workload.kind == workload::minimal ||
        result.workload.kind == workload::buffer_checksum) {
        if (seen.contains("--rounds")) {
            throw std::invalid_argument("rounds only applies to compute workloads");
        }
        result.workload.rounds = 0;
    }
    return result;
}

std::string cell(std::string_view value) {
    std::string result = "\"";
    for (const auto character : value) {
        if (character == '"') {
            result += '"';
        }
        result += character;
    }
    result += '"';
    return result;
}

class csv_file {
  public:
    explicit csv_file(const std::string& path) : file_(std::fopen(path.c_str(), "wx")) {
        if (!file_) {
            throw std::runtime_error("cannot exclusively create output: " +
                                     std::string(std::strerror(errno)));
        }
    }
    void row(const std::vector<std::string>& values) {
        std::string line;
        for (std::size_t i = 0; i != values.size(); ++i) {
            if (i != 0) {
                line += ',';
            }
            line += cell(values[i]);
        }
        line += '\n';
        if (std::fwrite(line.data(), 1, line.size(), file_.get()) != line.size() ||
            std::fflush(file_.get()) != 0) {
            throw std::runtime_error("failed writing benchmark output");
        }
    }
    void finish() {
        if (std::fclose(file_.release()) != 0) {
            throw std::runtime_error("failed closing benchmark output");
        }
    }

  private:
    struct file_closer {
        void operator()(std::FILE* file) const noexcept {
            (void)std::fclose(file);
        }
    };
    std::unique_ptr<std::FILE, file_closer> file_;
};

std::vector<scenario> scenarios(const options& config) {
    std::vector<scenario> result;
    if (config.mode == "sweep" || config.mode == "sequential") {
        result.push_back({mode::sequential, 0, 1});
    }
    if (config.mode != "sequential") {
        for (auto workers : config.workers) {
            for (auto producers : config.producers) {
                result.push_back({config.mode == "cold_batch"      ? mode::cold_batch
                                  : config.mode == "producer_pool" ? mode::producer_pool
                                                                   : mode::pool,
                                  workers, producers});
            }
        }
    }
    return result;
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--clock-probe") {
            std::vector<std::int64_t> samples(10'000);
            for (auto& sample : samples) {
                const auto first = std::chrono::steady_clock::now();
                const auto second = std::chrono::steady_clock::now();
                sample =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(second - first).count();
            }
            std::sort(samples.begin(), samples.end());
            std::cout << "{\"samples\":10000,\"clock_pair_min_ns\":" << samples.front()
                      << ",\"clock_pair_median_ns\":" << samples[samples.size() / 2]
                      << ",\"clock_pair_p95_ns\":" << samples[samples.size() * 95 / 100] << "}\n";
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--help") {
            std::cout << "cte_bench --output PATH [--mode "
                         "sweep|sequential|pool|cold_batch|producer_pool]\n"
                         "  [--workload minimal|compute|mixed_compute|buffer_checksum]\n"
                         "  [--workers 1,2,4] [--producers 1] [--tasks 10000] [--rounds 1000]\n"
                         "  [--bytes-per-task 4096] [--repetitions 9] [--warmups 1]\n"
                         "  [--seed 20260917] [--order-seed 9172026]\n"
                         "  [--session ID] [--revision REV] [--dirty-tree true|false|unknown]\n";
#ifdef CTE_QUEUE_PROBES
            std::cout << "  [--probe-stride 16] (writes a separate .locks.csv sample file)\n";
#endif
            std::cout << "  --clock-probe measures back-to-back steady_clock reads\n";
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--version") {
            std::cout << "cte_bench " << build::version << " compiler=" << build::compiler
                      << " profile=" << build::profile << " sanitizer=" << build::sanitizer
#ifdef CTE_QUEUE_PROBES
                      << " queue_probes=true"
#endif
                      << '\n';
            return 0;
        }
        const auto config = parse(argc, argv);
        auto cases = scenarios(config);
        csv_file output(config.output);
#ifdef CTE_QUEUE_PROBES
        csv_file diagnostic_output(config.output + ".locks.csv");
        diagnostic_output.row({"session_id", "case_id", "block_id", "producer", "sample_index",
                               "sample_stride", "wait_ns", "hold_ns"});
#endif
        const auto data = make_dataset(config.workload);
        std::vector<std::uint64_t> values(data.config.tasks);
        std::mt19937_64 order(config.order_seed);
        output.row({"schema_version",  "session_id",     "case_id",       "block_id",
                    "order_index",     "revision",       "dirty_tree",    "mode",
                    "workload",        "workers",        "producers",     "tasks",
                    "rounds",          "bytes_per_task", "seed",          "elapsed_ns",
                    "completed_tasks", "checksum",       "validation_ok", "status",
                    "build_profile",   "compiler_id",    "stdlib_id",     "instrumented",
                    "warmups",         "repetitions",    "order_seed"});
        std::size_t order_index = 0;
        for (unsigned block = 0; block != config.repetitions; ++block) {
            std::shuffle(cases.begin(), cases.end(), order);
            for (auto scenario : cases) {
                // A skipped output write must fail validation even when the
                // previous case produced the same deterministic answers.
                std::transform(data.expected.begin(), data.expected.end(), values.begin(),
                               [](std::uint64_t value) { return ~value; });
#ifdef CTE_QUEUE_PROBES
                probe_session probes(data.config.tasks, scenario.producers, config.probe_stride);
                scoped_probe_session probe_scope(probes);
#endif
                const auto duration = run(data, scenario, values, config.warmups);
                const bool valid = duration > 0 && values == data.expected;
                const std::string mode_name(name(scenario.kind));
                const auto id = mode_name + "_w" + std::to_string(scenario.workers) + "_p" +
                                std::to_string(scenario.producers);
                output.row(
                    {"1",
                     config.session,
                     id,
                     std::to_string(block),
                     std::to_string(order_index++),
                     config.revision,
                     config.dirty,
                     mode_name,
                     std::string(name(data.config.kind)),
                     std::to_string(scenario.workers),
                     std::to_string(scenario.producers),
                     std::to_string(data.config.tasks),
                     data.config.rounds ? std::to_string(data.config.rounds) : "",
                     data.config.bytes_per_task ? std::to_string(data.config.bytes_per_task) : "",
                     std::to_string(data.config.seed),
                     std::to_string(duration),
                     std::to_string(data.config.tasks),
                     std::to_string(result_checksum(values)),
                     valid ? "true" : "false",
                     valid ? "ok" : "invalid",
                     build::profile,
                     build::compiler,
                     build::stdlib,
#ifdef CTE_QUEUE_PROBES
                     "true",
#else
                     std::string_view(build::sanitizer) == "none" ? "false" : "true",
#endif
                     std::to_string(config.warmups),
                     std::to_string(config.repetitions),
                     std::to_string(config.order_seed)});
                if (!valid) {
                    throw std::runtime_error("result does not match the precomputed answer");
                }
#ifdef CTE_QUEUE_PROBES
                for (std::size_t p = 0; p != probes.buffers.size(); ++p) {
                    const auto& buffer = probes.buffers[p];
                    if (buffer.dropped != 0) {
                        throw std::runtime_error("diagnostic sample capacity exhausted");
                    }
                    for (std::size_t i = 0; i != buffer.size; ++i) {
                        const auto sample = buffer.samples[i];
                        diagnostic_output.row(
                            {config.session, id, std::to_string(block), std::to_string(p),
                             std::to_string(i), std::to_string(config.probe_stride),
                             std::to_string(sample.wait_ns), std::to_string(sample.hold_ns)});
                    }
                }
#endif
                std::cerr << "block=" << block << ' ' << id
                          << " elapsed_ms=" << static_cast<double>(duration) / 1'000'000.0
                          << " validated\n";
            }
        }
        output.finish();
#ifdef CTE_QUEUE_PROBES
        diagnostic_output.finish();
#endif
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "benchmark error: " << error.what() << '\n';
        return 1;
    }
}
