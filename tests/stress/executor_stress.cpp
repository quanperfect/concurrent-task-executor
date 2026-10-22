#include <cte/task_executor.hpp>

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <exception>
#include <future>
#include <iostream>
#include <latch>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {
std::uint64_t number(std::string_view value) {
    std::uint64_t result = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size()) {
        throw std::invalid_argument("invalid unsigned integer");
    }
    return result;
}

struct task_error {};

void run_round(std::size_t round, std::size_t tasks, std::uint64_t seed) {
    const std::size_t worker_count = std::size_t{1} << (round % 4);
    const std::size_t producer_count = std::size_t{1} << ((round / 4) % 4);
    const bool race_close = round % 3 != 0;
    std::vector<std::atomic<unsigned>> executed(tasks);
    std::vector<unsigned char> accepted(tasks, 0);
    using result = std::pair<std::size_t, std::future<std::size_t>>;
    std::vector<std::vector<result>> results(producer_count);
    std::vector<std::exception_ptr> failures(producer_count);
    std::latch start(1);
    std::latch may_close(1);
    bool startup_aborted = false; // Published by releasing start, before producers read it.
    cte::task_executor executor(worker_count);
    std::vector<std::jthread> producers;
    std::vector<std::jthread> closers;
    producers.reserve(producer_count);
    closers.reserve(3);
    try {
        for (std::size_t p = 0; p != producer_count; ++p) {
            results[p].reserve(tasks / producer_count + 1);
            producers.emplace_back([&, p] {
                start.wait();
                if (startup_aborted) {
                    return;
                }
                const auto begin = tasks * p / producer_count;
                const auto end = tasks * (p + 1) / producer_count;
                bool close_signaled = false;
                try {
                    for (auto id = begin; id != end; ++id) {
                        if (p == 0 && id == (end - begin) / 2) {
                            may_close.count_down();
                            close_signaled = true;
                        }
                        try {
                            auto future = executor.submit(
                                [&, token = std::make_unique<std::size_t>(id), seed] {
                                    const auto index = *token;
                                    executed[index].fetch_add(1, std::memory_order_relaxed);
                                    if ((index ^ seed) % 11 == 0) {
                                        std::this_thread::yield();
                                    }
                                    if (index % 17 == 0) {
                                        throw task_error{};
                                    }
                                    return index;
                                });
                            accepted[id] = 1;
                            results[p].emplace_back(id, std::move(future));
                        } catch (const cte::executor_closed&) {
                            // Expected precisely when closure wins the insertion boundary.
                        }
                    }
                } catch (...) {
                    failures[p] = std::current_exception();
                    if (p == 0 && !close_signaled) {
                        may_close.count_down();
                    }
                }
            });
        }
        if (race_close) {
            for (int i = 0; i != 3; ++i) {
                closers.emplace_back([&] {
                    may_close.wait();
                    executor.shutdown();
                });
            }
        }
    } catch (...) {
        // jthread destruction joins. Release both gates before unwinding a
        // partially created group; aborted producers never signal may_close.
        startup_aborted = true;
        start.count_down();
        may_close.count_down();
        throw;
    }
    start.count_down();
    producers.clear();
    closers.clear();
    executor.shutdown();
    for (const auto& failure : failures) {
        if (failure) {
            std::rethrow_exception(failure);
        }
    }
    for (std::size_t id = 0; id != tasks; ++id) {
        if (executed[id].load() != accepted[id] || (!race_close && accepted[id] != 1)) {
            throw std::runtime_error("accepted/executed mismatch at task " + std::to_string(id));
        }
    }
    for (auto& group : results) {
        for (auto& [id, future] : group) {
            try {
                if (future.get() != id || id % 17 == 0) {
                    throw std::runtime_error("invalid future value");
                }
            } catch (const task_error&) {
                if (id % 17 != 0) {
                    throw std::runtime_error("unexpected task exception");
                }
            }
        }
    }
}
} // namespace

int main(int argc, char** argv) {
    try {
        std::uint64_t rounds = 24;
        std::uint64_t tasks = 4000;
        std::uint64_t seed = 20260917;
        std::uint64_t seconds = 0;
        for (int i = 1; i < argc; i += 2) {
            if (i + 1 >= argc) {
                throw std::invalid_argument("expected --rounds/--tasks/--seed/--seconds VALUE");
            }
            const std::string_view key(argv[i]);
            const auto value = number(argv[i + 1]);
            if (key == "--rounds") {
                rounds = value;
            } else if (key == "--tasks") {
                tasks = value;
            } else if (key == "--seed") {
                seed = value;
            } else if (key == "--seconds") {
                seconds = value;
            } else {
                throw std::invalid_argument("unknown option");
            }
        }
        if (rounds == 0 || tasks < 16 || tasks > 1'000'000 || seconds > 86400) {
            throw std::invalid_argument(
                "rounds > 0, 16 <= tasks <= 1000000, seconds <= 86400 required");
        }
        const auto start = std::chrono::steady_clock::now();
        const auto deadline = start + std::chrono::seconds(seconds);
        std::size_t completed = 0;
        do {
            run_round(completed, static_cast<std::size_t>(tasks), seed + completed);
            ++completed;
        } while (seconds != 0 ? std::chrono::steady_clock::now() < deadline : completed < rounds);
        std::cout << "PASS rounds=" << completed << " tasks_per_round=" << tasks << " seed=" << seed
                  << " elapsed_seconds="
                  << std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count()
                  << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
