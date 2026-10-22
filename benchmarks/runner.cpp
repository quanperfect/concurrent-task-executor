#include "runner.hpp"

#include <cte/task_executor.hpp>

#include <algorithm>
#include <chrono>
#include <exception>
#include <future>
#include <latch>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

#ifdef CTE_QUEUE_PROBES
#include "diagnostics.hpp"
#endif

namespace cte::bench {
namespace {
using clock = std::chrono::steady_clock;

std::int64_t elapsed(clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - start).count();
}

void warmup(task_executor& executor, const dataset& data, unsigned passes) {
    struct worker_gate {
        explicit worker_gate(std::size_t count) : arrived(static_cast<std::ptrdiff_t>(count)) {}
        std::latch arrived;
        std::latch release{1};
    };
    auto gate = std::make_shared<worker_gate>(executor.worker_count());
    try {
        for (std::size_t i = 0; i != executor.worker_count(); ++i) {
            (void)executor.submit([gate] {
                gate->arrived.count_down();
                gate->release.wait();
            });
        }
    } catch (...) {
        gate->release.count_down();
        executor.wait_idle();
        throw;
    }
    gate->arrived.wait();
    gate->release.count_down();
    executor.wait_idle();
    const auto count = std::min(data.config.tasks, executor.worker_count() * 4);
    for (unsigned pass = 0; pass != passes; ++pass) {
        for (std::size_t i = 0; i != count; ++i) {
            (void)executor.submit([&data, i] { return evaluate(data, i); });
        }
        executor.wait_idle();
    }
}

std::int64_t run_pool(const dataset& data, scenario config, std::span<std::uint64_t> output,
                      unsigned warmups) {
    std::vector<std::future<std::uint64_t>> futures;
    futures.reserve(data.config.tasks);
    std::optional<task_executor> executor;
    const bool cold = config.kind == mode::cold_batch;
    if (!cold) {
        executor.emplace(config.workers);
        warmup(*executor, data, warmups);
    }
#ifdef CTE_QUEUE_PROBES
    detail::scoped_queue_probe probe(current_probes ? &current_probes->buffers[0] : nullptr);
#endif
    const auto start = clock::now();
    if (cold) {
        executor.emplace(config.workers);
    }
    for (std::size_t i = 0; i != data.config.tasks; ++i) {
        futures.push_back(executor->submit([&data, i] { return evaluate(data, i); }));
    }
    for (std::size_t i = 0; i != data.config.tasks; ++i) {
        output[i] = futures[i].get();
    }
    executor->wait_idle();
    if (cold) {
        executor.reset();
    }
    return elapsed(start);
}

std::int64_t run_producers(const dataset& data, scenario config, std::span<std::uint64_t> output,
                           unsigned warmups) {
    std::vector<std::vector<std::future<std::uint64_t>>> futures(config.producers);
    std::vector<std::exception_ptr> failures(config.producers);
    std::latch ready(static_cast<std::ptrdiff_t>(config.producers));
    std::latch start_gate(1);
    std::latch finished(static_cast<std::ptrdiff_t>(config.producers));
    task_executor executor(config.workers);
    warmup(executor, data, warmups);
    std::vector<std::jthread> producers;
    producers.reserve(config.producers);
    for (std::size_t p = 0; p != config.producers; ++p) {
        futures[p].reserve(data.config.tasks / config.producers + 1);
    }
#ifdef CTE_QUEUE_PROBES
    auto* probes = current_probes;
#endif
    try {
        for (std::size_t p = 0; p != config.producers; ++p) {
            producers.emplace_back([&, p] {
                ready.count_down();
                start_gate.wait();
#ifdef CTE_QUEUE_PROBES
                detail::scoped_queue_probe probe(probes ? &probes->buffers[p] : nullptr);
#endif
                try {
                    const auto begin = data.config.tasks * p / config.producers;
                    const auto end = data.config.tasks * (p + 1) / config.producers;
                    for (auto i = begin; i != end; ++i) {
                        futures[p].push_back(
                            executor.submit([&data, i] { return evaluate(data, i); }));
                    }
                    for (auto i = begin; i != end; ++i) {
                        output[i] = futures[p][i - begin].get();
                    }
                } catch (...) {
                    failures[p] = std::current_exception();
                }
                finished.count_down();
            });
        }
    } catch (...) {
        start_gate.count_down();
        producers.clear();
        throw;
    }
    ready.wait();
    const auto start = clock::now();
    start_gate.count_down();
    finished.wait();
    executor.wait_idle();
    const auto duration = elapsed(start);
    producers.clear(); // Joining producer handles is outside the timing boundary.
    for (const auto& failure : failures) {
        if (failure) {
            std::rethrow_exception(failure);
        }
    }
    return duration;
}
} // namespace

std::string_view name(mode kind) {
    switch (kind) {
    case mode::sequential:
        return "sequential";
    case mode::pool:
        return "pool";
    case mode::cold_batch:
        return "cold_batch";
    case mode::producer_pool:
        return "producer_pool";
    }
    throw std::invalid_argument("invalid mode enum");
}

std::int64_t run(const dataset& data, scenario config, std::span<std::uint64_t> output,
                 unsigned warmups) {
    if (output.size() != data.config.tasks || config.producers == 0 ||
        config.producers > data.config.tasks || config.producers > 256 || config.workers > 1024 ||
        warmups > 100) {
        throw std::invalid_argument("invalid benchmark scenario or output size");
    }
    if (config.kind == mode::sequential) {
        if (config.workers != 0 || config.producers != 1) {
            throw std::invalid_argument("sequential requires zero workers and one producer");
        }
        const auto start = clock::now();
        for (std::size_t i = 0; i != data.config.tasks; ++i) {
            output[i] = evaluate(data, i);
        }
        return elapsed(start);
    }
    if (config.workers == 0 || (config.kind != mode::producer_pool && config.producers != 1)) {
        throw std::invalid_argument(
            "pool needs workers; multiple producers need producer_pool mode");
    }
    if (config.kind == mode::producer_pool) {
        return run_producers(data, config, output, warmups);
    }
    return run_pool(data, config, output, warmups);
}

} // namespace cte::bench
