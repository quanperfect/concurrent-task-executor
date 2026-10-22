#pragma once

#include <cte/task_executor.hpp>

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <utility>

namespace cte::test {

class gate {
  public:
    void wait() {
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [this] { return open_; });
    }
    void open() {
        {
            std::lock_guard lock(mutex_);
            open_ = true;
        }
        condition_.notify_all();
    }

  private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool open_ = false;
};

template <class F> class scope_exit {
  public:
    explicit scope_exit(F function) : function_(std::move(function)) {}
    ~scope_exit() {
        function_();
    }
    scope_exit(const scope_exit&) = delete;
    scope_exit& operator=(const scope_exit&) = delete;

  private:
    F function_;
};

using namespace std::chrono_literals;

} // namespace cte::test
