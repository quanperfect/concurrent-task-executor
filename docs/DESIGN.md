# Design

`task_executor` owns N `std::jthread` workers and one FIFO queue of type-erased tasks. One mutex, `state_mutex_`, guards everything that decides whether there is work: the queue, the open/closed state, and the number of tasks currently running. A second mutex, `join_mutex_`, is only used by `shutdown()` to serialize joining.

## Submitting a task

`submit(f, args...)`:

1. Copies or moves `f` and `args` into a lambda, the same way `std::thread` stores its arguments. `std::ref` is the only way to pass something by reference.
2. Wraps that lambda in a `std::packaged_task<R()>` and takes its future.
3. Puts the packaged task behind a small virtual interface (`detail::task`), so a single queue can hold tasks with different result types. `std::function<void()>` would need a copyable callable, which rules out a lambda that owns a `std::unique_ptr`.
4. Locks, throws `executor_closed` if the pool is closed, pushes the task, unlocks, and wakes one worker.

All allocation and copying happens before the lock is taken. If `push_back` throws `std::bad_alloc`, nothing is accepted, and the task is destroyed only after the lock is released. That matters because a destructor of something the task captured may call back into the executor.

## Workers

```
loop:
    lock; wait until the queue is non-empty or the pool is closed
    if the queue is empty: exit          // closed and fully drained
    pop the front task; ++active; unlock
    run the task                         // no lock held; exceptions go into the future
    destroy the task                     // no lock held
    lock; --active; if queue empty and active == 0: wake wait_idle() callers; unlock
```

Popping a task and incrementing `active_` happen in the same critical section. `wait_idle()` waits for `queue empty && active == 0` under the same mutex, so it can never see a false "idle" in between. Notifications are sent after unlocking, and every wait uses a predicate, so spurious or early wakeups don't matter.

`task::run()` is `noexcept`. Exceptions thrown by user code never reach it, because `packaged_task` stores them in the future. If a task were ever run twice, that would be a bug in the library, and the process terminates instead of continuing in a broken state.

## Shutdown

- `close()` marks the pool closed and wakes every worker. Tasks already in the queue still run.
- `shutdown()` is `close()` followed by joining every worker. It holds `join_mutex_` while joining, so several threads can call it at once. Later callers find no joinable threads and return.
- The destructor calls `shutdown()`. Workers are never detached.

If a task called `shutdown()` or `wait_idle()` on its own pool, its worker would end up waiting for itself. Each worker keeps a `thread_local` pointer to its executor, and both calls throw `std::logic_error` when they detect this. The check runs before `join_mutex_` is taken, because another thread may be holding that mutex while it joins this very worker. A task that destroys its own pool calls `std::terminate`.

Lock order is `join_mutex_` then `state_mutex_`, and only `shutdown()` takes both. Workers never touch `join_mutex_`.

## Construction

If creating the k-th thread fails, the constructor closes the pool and joins the workers that already started, then rethrows. This has to happen inside the constructor body. Once the exception leaves it, the members are destroyed, and those workers would be sleeping on a condition variable that no longer exists. For the same reason `workers_` is the last member: it is destroyed first, while the mutexes and condition variables are still alive.

## Trade-offs

- One shared queue with one mutex is simple to reason about, but it is the bottleneck for tiny tasks or many producers (see the [benchmarks](../benchmarks/README.md)). Each task costs two allocations (the task and the future's shared state), three lock acquisitions (submit, pop, completion) and a condition-variable notification.
- Per-worker queues with work stealing would reduce contention, but they give up the global FIFO order and make idle tracking and shutdown more complicated. A lock-free queue brings its own memory-reclamation problems. Neither seemed worth it for this API.
- The queue is unbounded, and there is no cancellation or backpressure.

## Test builds

Failure injection (failing thread creation, a failing deque allocator, and hooks that pause a worker at a chosen point) is compiled into a separate `cte_testing` library with `CTE_TESTING` defined. Lock-timing probes are a third variant, built with `CTE_QUEUE_PROBES`. The normal `cte` library contains neither. These variants define the class differently, so never link two of them into one program.
