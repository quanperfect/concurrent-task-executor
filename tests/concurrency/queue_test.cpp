#include <cte/task_executor.hpp>
#include <gtest/gtest.h>

#include "test_helpers.hpp"

#include <atomic>
#include <future>
#include <latch>
#include <set>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;

TEST(Queue, OneWorkerExecutesInInsertionOrder) {
    cte::test::gate gate;
    cte::task_executor executor(1);
    auto blocked = executor.submit([&] { gate.wait(); });
    cte::test::scope_exit cleanup([&] { gate.open(); });
    std::vector<int> observed;
    for (int i = 0; i != 200; ++i) {
        (void)executor.submit([&, i] { observed.push_back(i); });
    }
    gate.open();
    executor.shutdown();
    blocked.get();
    ASSERT_EQ(observed.size(), 200U);
    for (int i = 0; i != 200; ++i) {
        EXPECT_EQ(observed[static_cast<std::size_t>(i)], i);
    }
}

TEST(Queue, ConcurrentProducersExecuteEveryIdExactlyOnce) {
    constexpr std::size_t producers = 8;
    constexpr std::size_t per_producer = 1000;
    cte::task_executor executor(4);
    std::vector<std::atomic<unsigned>> calls(producers * per_producer);
    std::vector<std::jthread> threads;
    for (std::size_t p = 0; p != producers; ++p) {
        threads.emplace_back([&, p] {
            for (std::size_t i = 0; i != per_producer; ++i) {
                const auto id = p * per_producer + i;
                (void)executor.submit(
                    [&, id] { calls[id].fetch_add(1, std::memory_order_relaxed); });
            }
        });
    }
    threads.clear();
    executor.shutdown();
    for (const auto& count : calls) {
        EXPECT_EQ(count.load(), 1U);
    }
}

TEST(Queue, UsesExactlyTheConfiguredWorkers) {
    constexpr std::size_t workers = 4;
    cte::test::gate gate;
    std::latch started(static_cast<std::ptrdiff_t>(workers));
    cte::task_executor executor(workers);
    cte::test::scope_exit cleanup([&] { gate.open(); });
    // Thread ids travel through futures: latch-only publication is invisible to
    // ThreadSanitizer when libstdc++ performs the final wait inside the shared library.
    std::vector<std::future<std::thread::id>> ids;
    for (std::size_t i = 0; i != workers; ++i) {
        ids.push_back(executor.submit([&] {
            started.count_down();
            gate.wait();
            return std::this_thread::get_id();
        }));
    }
    started.wait(); // Every worker is now blocked in its own task.
    auto extra = executor.submit([] {});
    EXPECT_EQ(extra.wait_for(20ms), std::future_status::timeout);
    gate.open();
    std::set<std::thread::id> distinct;
    for (auto& id : ids) {
        distinct.insert(id.get());
    }
    EXPECT_EQ(distinct.size(), workers);
    extra.get();
}

TEST(Queue, MultipleWorkersMayCompleteOutOfOrder) {
    cte::test::gate gate;
    cte::task_executor executor(2);
    auto first = executor.submit([&] {
        gate.wait();
        return 1;
    });
    cte::test::scope_exit cleanup([&] { gate.open(); });
    auto second = executor.submit([] { return 2; });
    EXPECT_EQ(second.get(), 2);
    EXPECT_EQ(first.wait_for(std::chrono::milliseconds(0)), std::future_status::timeout);
    gate.open();
    EXPECT_EQ(first.get(), 1);
}

} // namespace
