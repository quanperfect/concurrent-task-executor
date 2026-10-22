#ifndef CTE_TASK_EXECUTOR_HPP
#define CTE_TASK_EXECUTOR_HPP

#include <cte/detail/task.hpp>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#ifdef CTE_TESTING
#include <cte_test_support.hpp>
#endif

namespace cte {

class executor_closed final : public std::runtime_error {
  public:
    executor_closed() : std::runtime_error("task_executor no longer accepts work") {}
};

/// A fixed pool for independent tasks. FIFO describes dequeue order only.
/// The owner must stop all external member calls before destruction begins.
class task_executor final {
  public:
    explicit task_executor(std::size_t worker_count);
    ~task_executor() noexcept;

    task_executor(const task_executor&) = delete;
    task_executor& operator=(const task_executor&) = delete;
    task_executor(task_executor&&) = delete;
    task_executor& operator=(task_executor&&) = delete;

    /// Own decayed copies/moves; use std::ref explicitly for borrowed arguments.
    /// Rejection may consume rvalues. No task is accepted if this function throws.
    template <class F, class... Args>
        requires detail::submittable<F, Args...>
    [[nodiscard]] auto submit(F&& function,
                              Args&&... args) -> std::future<detail::task_result_t<F, Args...>> {
        using result_type = detail::task_result_t<F, Args...>;
        auto bound = [callable = std::forward<F>(function),
                      arguments = std::tuple<std::decay_t<Args>...>(
                          std::forward<Args>(args)...)]() mutable -> result_type {
            return std::apply(
                [&callable](auto&... stored) -> result_type {
                    return std::invoke(std::move(callable), std::move(stored)...);
                },
                arguments);
        };
        auto owned = std::make_unique<detail::task_model<result_type>>(std::move(bound));
        auto result = owned->get_future();
        enqueue(std::move(owned));
        return result;
    }

    /// Stop acceptance, wake workers, and leave all accepted work to drain.
    void close();
    /// Observe an empty queue and no active tasks. Concurrent producers may add more.
    /// Throws logic_error on this executor's workers.
    void wait_idle();
    /// Close and join. Concurrent external callers are serialized; never detaches.
    /// Throws logic_error on this executor's workers, before changing state.
    void shutdown();

    [[nodiscard]] std::size_t worker_count() const noexcept {
        return worker_count_;
    }

  private:
    enum class state : std::uint8_t { accepting, draining, joined };

    void enqueue(detail::task_ptr owned);
    void worker_loop() noexcept;
    void finish_task() noexcept;
    void require_external_caller() const;

    static thread_local const task_executor* current_worker_;
    const std::size_t worker_count_;
    std::mutex state_mutex_;
    std::condition_variable work_available_;
    std::condition_variable idle_;
    state state_ = state::accepting;
    std::size_t active_ = 0;
#ifdef CTE_TESTING
    friend struct detail::executor_test_access;
    task_executor(std::size_t worker_count, detail::test_control* control);
    detail::test_control* control_ = nullptr;
    std::deque<detail::task_ptr, detail::test_allocator<detail::task_ptr>> queue_;
#else
    std::deque<detail::task_ptr> queue_;
#endif
    std::mutex join_mutex_;
    // Declared last: every synchronization object outlives all workers.
    std::vector<std::jthread> workers_;
};

} // namespace cte

#endif // CTE_TASK_EXECUTOR_HPP
