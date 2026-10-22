# API

```cpp
#include <cte/task_executor.hpp>   // link cte::cte

namespace cte {

class task_executor {
public:
    explicit task_executor(std::size_t workers);  // std::invalid_argument if 0
    ~task_executor();                              // same as shutdown()

    template <class F, class... Args>
    std::future<R> submit(F&& f, Args&&... args);  // R = result of f(args...)

    void close();       // stop accepting; queued tasks still run
    void wait_idle();   // block until the queue is empty and nothing is running
    void shutdown();    // close(), finish every accepted task, join the workers
    std::size_t worker_count() const noexcept;
};

class executor_closed : public std::runtime_error {};  // thrown by submit() after close()

}
```

The executor is neither copyable nor movable, because its workers keep a pointer to it.

## submit()

- Arguments are stored the way `std::thread` stores them: lvalues are copied, rvalues are moved. Pass `std::ref(x)` or `std::cref(x)` to share an object instead; its lifetime and synchronization are then up to you. Reference captures in a lambda are still references, as usual.
- Move-only callables, arguments and results work (`std::unique_ptr` and so on), as do callables with an `&&`-qualified `operator()`, function pointers and member-function pointers.
- These are rejected at compile time: callables that can only be called as lvalues (wrap them in `std::ref`), functions that return a reference, and types whose destructor may throw.
- After `close()`, `submit()` throws `executor_closed`. Rvalue arguments may already have been moved from at that point.
- Exceptions from copying or moving the arguments, or from allocation, propagate out of `submit()`, and nothing is queued.
- An exception thrown by the task itself is stored in the future and rethrown by `get()`. The worker carries on with the next task.
- Dropping the future does not cancel the task. A future may outlive the executor.

## Ordering

Tasks leave the queue in the order they were accepted. With more than one worker, they can start and finish in any order. Submissions made concurrently from different threads have no defined order relative to each other.

## Lifecycle

| Call | Effect |
| --- | --- |
| `close()` | Stops accepting new tasks and wakes the workers; returns immediately. Calling it again does nothing. |
| `wait_idle()` | Returns once the queue is empty and no task is running. It doesn't close the pool, so another thread may submit right after it returns. |
| `shutdown()` | `close()`, then waits for every accepted task and joins the workers. Can be called repeatedly and from several threads at once. |
| destructor | Same as `shutdown()`. |

From inside a task:

- `submit()` and `close()` are fine.
- `wait_idle()` and `shutdown()` on the task's own pool throw `std::logic_error`, because the worker would be waiting for itself.
- Destroying the task's own pool terminates the process.

## Things to watch out for

- A task that waits on another task's future from the same pool can deadlock once every worker is waiting.
- The queue is unbounded.
- `shutdown()` waits for every accepted task. There is no timeout and no cancellation.
- Don't destroy the executor while other threads are still calling it.
- A ready future doesn't mean the worker is finished: `get()` can return just before the worker destroys the task and updates its bookkeeping. Use `wait_idle()` or `shutdown()` when you need "everything is done".
