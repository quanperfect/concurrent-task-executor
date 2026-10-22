#pragma once

#ifndef CTE_TESTING
#error "Test support must only be compiled into the isolated cte_testing target"
#endif

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <latch>
#include <limits>
#include <memory>
#include <mutex>

namespace cte {
class task_executor;
}

namespace cte::detail {

// The same gate can be encountered again after release without decrementing a
// completed latch. It performs no allocation when used on a worker.
class test_gate {
  public:
    void arrive_and_wait() {
        if (!arrived_.exchange(true)) {
            entered.count_down();
        }
        std::unique_lock lock(mutex_);
        released_.wait(lock, [this] { return open_; });
    }
    void open() {
        {
            std::lock_guard lock(mutex_);
            open_ = true;
        }
        released_.notify_all();
    }
    std::latch entered{1};

  private:
    std::atomic<bool> arrived_{false};
    std::mutex mutex_;
    std::condition_variable released_;
    bool open_ = false;
};

struct test_control {
    std::size_t fail_worker_at = std::numeric_limits<std::size_t>::max();
    bool wait_before_failure = false;
    std::atomic<std::size_t> workers_started{0};
    std::atomic<std::size_t> workers_waiting{0};
    std::atomic<std::size_t> workers_exited{0};
    std::atomic<bool> fail_allocation{false};
    test_gate* after_execute = nullptr;
    std::latch* idle_wait_entered = nullptr;
    std::latch* join_entered = nullptr;
    std::atomic<bool> idle_signaled{false};
    std::atomic<bool> join_signaled{false};
};

// Fail only this executor's deque allocations, including rebound map storage.
template <class T> struct test_allocator {
    using value_type = T;
    test_control* control = nullptr;

    test_allocator() = default;
    explicit test_allocator(test_control* value) : control(value) {}
    template <class U> test_allocator(const test_allocator<U>& other) : control(other.control) {}

    [[nodiscard]] T* allocate(std::size_t count) {
        if (control && control->fail_allocation.load(std::memory_order_relaxed)) {
            throw std::bad_alloc();
        }
        return std::allocator<T>{}.allocate(count);
    }
    void deallocate(T* pointer, std::size_t count) noexcept {
        std::allocator<T>{}.deallocate(pointer, count);
    }
    template <class U> bool operator==(const test_allocator<U>& other) const noexcept {
        return control == other.control;
    }
};

struct executor_test_access {
    static std::unique_ptr<task_executor> make(std::size_t workers, test_control& control);
};

} // namespace cte::detail
