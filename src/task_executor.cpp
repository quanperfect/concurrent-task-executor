#include <cte/task_executor.hpp>

#include <cassert>
#include <exception>
#include <system_error>

#ifdef CTE_QUEUE_PROBES
#include <cte/detail/queue_probe.hpp>
#else
namespace cte::detail {
// The release library compiles no timing code; these calls are empty.
struct enqueue_timer {
    void locked() noexcept {}
    void unlocking() noexcept {}
    void record() noexcept {}
};
} // namespace cte::detail
#endif

namespace cte {

thread_local const task_executor* task_executor::current_worker_ = nullptr;

#ifdef CTE_TESTING
task_executor::task_executor(std::size_t worker_count) : task_executor(worker_count, nullptr) {}

task_executor::task_executor(std::size_t worker_count, detail::test_control* control)
    : worker_count_(worker_count), control_(control),
      queue_(detail::test_allocator<detail::task_ptr>{control}) {
#else
task_executor::task_executor(std::size_t worker_count) : worker_count_(worker_count) {
#endif
    if (worker_count == 0) {
        throw std::invalid_argument("task_executor requires at least one worker");
    }
    workers_.reserve(worker_count);
    // This catch must run before member unwinding: sleeping workers still need
    // the mutex, condition variables, and state to finish their rollback.
    try {
        for (std::size_t i = 0; i < worker_count; ++i) {
#ifdef CTE_TESTING
            if (control_ && i == control_->fail_worker_at) {
                if (control_->wait_before_failure && i != 0) {
                    while (control_->workers_waiting.load() == 0) {
                        control_->workers_waiting.wait(0);
                    }
                }
                throw std::system_error(
                    std::make_error_code(std::errc::resource_unavailable_try_again));
            }
#endif
            workers_.emplace_back([this] { worker_loop(); });
        }
    } catch (...) {
        close();
        for (auto& worker : workers_) {
            worker.join();
        }
        throw;
    }
}

task_executor::~task_executor() noexcept {
    try {
        shutdown();
    } catch (...) {
        // Self-destruction or an unrecoverable join failure cannot safely leave
        // live workers referring to this object. Never detach or swallow it.
        std::terminate();
    }
}

void task_executor::enqueue(detail::task_ptr owned) {
    detail::enqueue_timer timer;
    {
        std::lock_guard lock(state_mutex_);
        timer.locked();
        if (state_ != state::accepting) {
            throw executor_closed();
        }
        // unique_ptr's move is noexcept. Deque end insertion has no effect on
        // allocation failure, leaving owned to be destroyed after unlocking.
        queue_.push_back(std::move(owned));
        timer.unlocking();
    }
    timer.record();
    work_available_.notify_one();
}

void task_executor::close() {
    bool changed = false;
    {
        std::lock_guard lock(state_mutex_);
        if (state_ == state::accepting) {
            state_ = state::draining;
            changed = true;
        }
    }
    if (changed) {
        work_available_.notify_all();
    }
}

void task_executor::require_external_caller() const {
    if (current_worker_ == this) {
        throw std::logic_error("cannot wait for or shut down an executor from its own worker");
    }
}

void task_executor::wait_idle() {
    require_external_caller();
    std::unique_lock lock(state_mutex_);
#ifdef CTE_TESTING
    if (control_ && control_->idle_wait_entered && !control_->idle_signaled.exchange(true)) {
        control_->idle_wait_entered->count_down();
    }
#endif
    idle_.wait(lock, [this] { return queue_.empty() && active_ == 0; });
}

void task_executor::shutdown() {
    // Must precede the join lock: its owner may already be joining this worker.
    require_external_caller();
    close();
    std::lock_guard join_lock(join_mutex_);
#ifdef CTE_TESTING
    if (control_ && control_->join_entered && !control_->join_signaled.exchange(true)) {
        control_->join_entered->count_down();
    }
#endif
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    std::lock_guard state_lock(state_mutex_);
    assert(queue_.empty() && active_ == 0);
    state_ = state::joined;
}

void task_executor::finish_task() noexcept {
    bool became_idle;
    {
        std::lock_guard lock(state_mutex_);
        assert(active_ > 0);
        --active_;
        became_idle = queue_.empty() && active_ == 0;
    }
    if (became_idle) {
        idle_.notify_all();
    }
}

void task_executor::worker_loop() noexcept {
    assert(current_worker_ == nullptr);
    current_worker_ = this;
#ifdef CTE_TESTING
    if (control_) {
        control_->workers_started.fetch_add(1, std::memory_order_relaxed);
    }
#endif
    for (;;) {
        detail::task_ptr owned;
        {
            std::unique_lock lock(state_mutex_);
#ifdef CTE_TESTING
            if (control_) {
                control_->workers_waiting.fetch_add(1);
                control_->workers_waiting.notify_all();
            }
#endif
            work_available_.wait(lock,
                                 [this] { return !queue_.empty() || state_ != state::accepting; });
            if (queue_.empty()) {
                break;
            }
            owned = std::move(queue_.front());
            queue_.pop_front();
            ++active_;
        }
        owned->run(); // noexcept: task exceptions are stored in the future.
#ifdef CTE_TESTING
        if (control_ && control_->after_execute) {
            control_->after_execute->arrive_and_wait();
        }
#endif
        // Release the task outside the lock (user destructors may re-enter the
        // executor) and before reporting completion to wait_idle().
        owned.reset();
        finish_task();
    }
#ifdef CTE_TESTING
    if (control_) {
        control_->workers_exited.fetch_add(1, std::memory_order_relaxed);
    }
#endif
    current_worker_ = nullptr;
}

} // namespace cte
