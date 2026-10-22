#include <cte/task_executor.hpp>
#include <gtest/gtest.h>

#include "test_helpers.hpp"

#include <array>
#include <atomic>
#include <latch>
#include <memory>
#include <vector>

namespace {
using cte::task_executor;
using namespace std::chrono_literals;

TEST(Lifecycle, IdleWaitIncludesActiveWorkAndWakesAllWaiters) {
    cte::test::gate gate;
    std::latch running(1);
    task_executor executor(2);
    auto task = executor.submit([&] {
        running.count_down();
        gate.wait();
    });
    running.wait();
    std::vector<std::future<void>> waiters;
    for (int i = 0; i != 4; ++i) {
        waiters.push_back(std::async(std::launch::async, [&] { executor.wait_idle(); }));
    }
    cte::test::scope_exit cleanup([&] { gate.open(); });
    for (auto& waiter : waiters) {
        EXPECT_EQ(waiter.wait_for(20ms), std::future_status::timeout);
    }
    gate.open();
    for (auto& waiter : waiters) {
        waiter.get();
    }
    task.get();
    EXPECT_EQ(executor.submit([] { return 11; }).get(), 11);
}

TEST(Lifecycle, ConcurrentShutdownCallersAllWaitForDrain) {
    cte::test::gate gate;
    task_executor executor(4);
    std::atomic<int> completed{0};
    std::vector<std::future<void>> tasks;
    for (int i = 0; i != 64; ++i) {
        tasks.push_back(executor.submit([&] {
            gate.wait();
            ++completed;
        }));
    }
    std::vector<std::future<void>> closers;
    for (int i = 0; i != 4; ++i) {
        closers.push_back(std::async(std::launch::async, [&] { executor.shutdown(); }));
    }
    cte::test::scope_exit cleanup([&] { gate.open(); });
    for (auto& closer : closers) {
        EXPECT_EQ(closer.wait_for(20ms), std::future_status::timeout);
    }
    gate.open();
    for (auto& closer : closers) {
        closer.get();
        EXPECT_EQ(completed.load(), 64);
    }
    for (auto& task : tasks) {
        task.get();
    }
    executor.shutdown();
}

TEST(Lifecycle, DestructionDrainsAcceptedWork) {
    cte::test::gate gate;
    std::atomic<int> completed{0};
    auto executor = std::make_unique<task_executor>(2);
    for (int i = 0; i != 100; ++i) {
        (void)executor->submit([&] {
            gate.wait();
            ++completed;
        });
    }
    auto destroy =
        std::async(std::launch::async, [pool = std::move(executor)]() mutable { pool.reset(); });
    cte::test::scope_exit cleanup([&] { gate.open(); });
    EXPECT_EQ(destroy.wait_for(20ms), std::future_status::timeout);
    gate.open();
    destroy.get();
    EXPECT_EQ(completed.load(), 100);
}

TEST(Lifecycle, WorkerCannotWaitOrShutdownAndRejectionDoesNotClosePool) {
    task_executor executor(1);
    auto rejected = executor.submit([&] {
        unsigned count = 0;
        try {
            executor.wait_idle();
        } catch (const std::logic_error&) {
            ++count;
        }
        try {
            executor.shutdown();
        } catch (const std::logic_error&) {
            ++count;
        }
        return count;
    });
    EXPECT_EQ(rejected.get(), 2U);
    EXPECT_EQ(executor.submit([] { return 5; }).get(), 5);
}

TEST(Lifecycle, WorkerMayCloseAndPreviouslyAcceptedTasksStillRun) {
    cte::test::gate gate;
    task_executor executor(1);
    auto closer = executor.submit([&] {
        gate.wait();
        executor.close();
    });
    cte::test::scope_exit cleanup([&] { gate.open(); });
    auto previously_accepted = executor.submit([] { return 31; });
    gate.open();
    closer.get();
    EXPECT_EQ(previously_accepted.get(), 31);
    EXPECT_THROW((void)executor.submit([] {}), cte::executor_closed);
}

TEST(Lifecycle, WorkerMaySubmitIndependentChild) {
    task_executor executor(1);
    auto parent = executor.submit([&] { return executor.submit([] { return 91; }); });
    auto child = parent.get(); // Only the external caller waits for the child.
    EXPECT_EQ(child.get(), 91);
}

TEST(Lifecycle, WorkerContextIsSpecificToThePool) {
    task_executor first(1);
    task_executor second(1);
    auto task = first.submit([&] {
        second.wait_idle();
        second.shutdown();
        return 7;
    });
    EXPECT_EQ(task.get(), 7);
    EXPECT_EQ(first.submit([] { return 8; }).get(), 8);
}

TEST(Lifecycle, ClosureWinsWhileCallableIsBeingCopied) {
    struct slow_copy {
        cte::test::gate* gate;
        std::latch* preparing;
        std::atomic<int>* calls;
        slow_copy(cte::test::gate& g, std::latch& p, std::atomic<int>& c)
            : gate(&g), preparing(&p), calls(&c) {}
        slow_copy(const slow_copy& other)
            : gate(other.gate), preparing(other.preparing), calls(other.calls) {
            preparing->count_down();
            gate->wait();
        }
        slow_copy(slow_copy&&) = default;
        void operator()() {
            ++*calls;
        }
    };
    cte::test::gate gate;
    std::latch preparing(1);
    std::atomic<int> calls{0};
    task_executor executor(1);
    slow_copy callable(gate, preparing, calls);
    auto producer = std::async(std::launch::async, [&] {
        try {
            (void)executor.submit(callable);
            return false;
        } catch (const cte::executor_closed&) {
            return true;
        }
    });
    cte::test::scope_exit cleanup([&] { gate.open(); });
    preparing.wait();
    executor.close();
    gate.open();
    EXPECT_TRUE(producer.get());
    executor.shutdown();
    EXPECT_EQ(calls.load(), 0);
}

TEST(Lifecycle, CaptureCleanupCanReenterClosure) {
    struct cleanup_action {
        task_executor& executor;
        std::atomic<int>& cleaned;
        cleanup_action(task_executor& e, std::atomic<int>& c) : executor(e), cleaned(c) {}
        ~cleanup_action() {
            executor.close();
            ++cleaned;
        }
    };
    cte::test::gate gate;
    std::atomic<int> cleaned{0};
    task_executor executor(1);
    auto task = executor.submit(
        [capture = std::make_shared<cleanup_action>(executor, cleaned), &gate] { gate.wait(); });
    cte::test::scope_exit cleanup([&] { gate.open(); });
    task = {}; // Make the worker's envelope the final execution-side owner.
    gate.open();
    executor.wait_idle();
    EXPECT_EQ(cleaned.load(), 1);
    EXPECT_THROW((void)executor.submit([] {}), cte::executor_closed);
}

TEST(Lifecycle, IdleAndNewBatchesDoNotLoseWakeups) {
    task_executor executor(4);
    for (int iteration = 0; iteration != 200; ++iteration) {
        auto value = executor.submit([iteration] { return iteration; });
        executor.wait_idle();
        EXPECT_EQ(value.get(), iteration);
    }
}

TEST(ExecutorDeathTest, SelfDestructionTerminatesInsteadOfDetaching) {
    EXPECT_DEATH(
        {
            auto* executor = new task_executor(1);
            std::latch release(1);
            auto result = executor->submit([executor, &release] {
                release.wait();
                delete executor;
            });
            release.count_down();
            result.wait();
        },
        "");
}

TEST(ExecutorDeathTest, DuplicateTaskInvocationIsAnInvariantFailure) {
    EXPECT_DEATH(
        {
            cte::detail::task_model<void> task([] {});
            task.run();
            task.run();
        },
        "");
}

} // namespace
