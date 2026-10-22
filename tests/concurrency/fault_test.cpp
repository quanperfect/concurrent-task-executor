#include <cte/task_executor.hpp>
#include <gtest/gtest.h>

#include "test_helpers.hpp"

#include <atomic>
#include <future>
#include <latch>
#include <vector>

namespace cte::detail {
std::unique_ptr<task_executor> executor_test_access::make(std::size_t workers,
                                                          test_control& control) {
    return std::unique_ptr<task_executor>(new task_executor(workers, &control));
}
} // namespace cte::detail

namespace {
using namespace std::chrono_literals;

TEST(Faults, RollsBackAfterAWorkerIsAlreadyWaiting) {
    for (std::size_t fail_at : {1U, 3U}) {
        cte::detail::test_control control;
        control.fail_worker_at = fail_at;
        control.wait_before_failure = true;
        EXPECT_THROW((void)cte::detail::executor_test_access::make(5, control), std::system_error);
        EXPECT_EQ(control.workers_started.load(), fail_at);
        EXPECT_EQ(control.workers_exited.load(), fail_at);
    }
}

TEST(Faults, FutureReadinessDoesNotMeanWorkerBookkeepingIsComplete) {
    cte::detail::test_gate gate;
    std::latch idle_entered(1);
    cte::detail::test_control control;
    control.after_execute = &gate;
    control.idle_wait_entered = &idle_entered;
    auto executor = cte::detail::executor_test_access::make(1, control);
    auto result = executor->submit([] { return 77; });
    gate.entered.wait();
    EXPECT_EQ(result.get(), 77);
    auto idle = std::async(std::launch::async, [&] { executor->wait_idle(); });
    cte::test::scope_exit cleanup([&] { gate.open(); });
    idle_entered.wait();
    EXPECT_EQ(idle.wait_for(20ms), std::future_status::timeout);
    gate.open();
    idle.get();
}

TEST(Faults, WorkerRejectsShutdownBeforeWaitingForTheJoinMutex) {
    std::latch joining(1);
    cte::test::gate gate;
    cte::detail::test_control control;
    control.join_entered = &joining;
    auto executor = cte::detail::executor_test_access::make(1, control);
    auto task = executor->submit([&] {
        gate.wait();
        try {
            executor->shutdown();
            return false;
        } catch (const std::logic_error&) {
            return true;
        }
    });
    auto closer = std::async(std::launch::async, [&] { executor->shutdown(); });
    cte::test::scope_exit cleanup([&] { gate.open(); });
    joining.wait(); // The external caller holds join_mutex while the task is gated.
    gate.open();
    EXPECT_TRUE(task.get());
    closer.get();
}

TEST(Faults, QueueAllocationFailureDoesNotAcceptOrDestroyUserStateUnderLock) {
    struct cleanup_action {
        cte::task_executor& executor;
        std::atomic<unsigned>& cleaned;
        cleanup_action(cte::task_executor& e, std::atomic<unsigned>& c) : executor(e), cleaned(c) {}
        ~cleanup_action() {
            executor.close(); // Deadlocks if failed insertion still holds state_mutex.
            ++cleaned;
        }
    };
    cte::test::gate gate;
    cte::detail::test_control control;
    std::atomic<unsigned> cleaned{0};
    std::atomic<unsigned> executed{0};
    auto executor = cte::detail::executor_test_access::make(1, control);
    auto blocked = executor->submit([&] { gate.wait(); });
    cte::test::scope_exit cleanup([&] { gate.open(); });
    control.fail_allocation.store(true);
    std::vector<std::future<void>> accepted;
    bool allocation_failed = false;
    for (int i = 0; i != 4096; ++i) {
        try {
            accepted.push_back(
                executor->submit([capture = std::make_shared<cleanup_action>(*executor, cleaned),
                                  &executed] { ++executed; }));
        } catch (const std::bad_alloc&) {
            allocation_failed = true;
            break;
        }
    }
    EXPECT_TRUE(allocation_failed);
    EXPECT_EQ(cleaned.load(), 1U); // Only the unaccepted envelope has been released.
    control.fail_allocation.store(false);
    gate.open();
    blocked.get();
    for (auto& result : accepted) {
        result.get();
    }
    executor->shutdown();
    EXPECT_EQ(executed.load(), accepted.size());
    EXPECT_EQ(cleaned.load(), accepted.size() + 1);
}

} // namespace
