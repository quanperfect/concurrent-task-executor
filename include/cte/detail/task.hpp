#ifndef CTE_DETAIL_TASK_HPP
#define CTE_DETAIL_TASK_HPP

#include <concepts>
#include <future>
#include <memory>
#include <type_traits>
#include <utility>

namespace cte::detail {

template <class F, class... Args>
using task_result_t = std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>;

template <class F, class... Args>
concept submittable = std::constructible_from<std::decay_t<F>, F> &&
                      (std::constructible_from<std::decay_t<Args>, Args> && ...) &&
                      std::move_constructible<std::decay_t<F>> &&
                      (std::move_constructible<std::decay_t<Args>> && ...) &&
                      std::invocable<std::decay_t<F>, std::decay_t<Args>...> &&
                      (!std::is_reference_v<task_result_t<F, Args...>>) &&
                      (std::is_void_v<task_result_t<F, Args...>> ||
                       std::move_constructible<task_result_t<F, Args...>>);

// Only execution is erased. Each accepted task has exactly one execution owner.
class task {
  public:
    task() = default;
    task(const task&) = delete;
    task& operator=(const task&) = delete;
    virtual ~task() = default;
    virtual void run() noexcept = 0;
};

using task_ptr = std::unique_ptr<task>;

template <class R> class task_model final : public task {
  public:
    template <class F>
        requires(!std::same_as<std::remove_cvref_t<F>, task_model>) && std::is_invocable_r_v<R, F&>
    explicit task_model(F&& function) : task_(std::forward<F>(function)) {}

    task_model(const task_model&) = delete;
    task_model& operator=(const task_model&) = delete;

    [[nodiscard]] std::future<R> get_future() {
        return task_.get_future();
    }

    void run() noexcept override {
        // User exceptions become future results. Invalid/double invocation is a
        // library invariant failure, and intentionally terminates through noexcept.
        task_();
    }

  private:
    std::packaged_task<R()> task_;
};

} // namespace cte::detail

#endif // CTE_DETAIL_TASK_HPP
