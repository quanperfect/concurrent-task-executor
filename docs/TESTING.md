# Testing

```sh
cmake --preset clang-tsan -DCTE_FETCH_TEST_DEPS=ON
cmake --build --preset clang-tsan
ctest --preset clang-tsan
```

Presets: `gcc-debug`, `gcc-release`, `clang-debug`, `clang-tsan` (ThreadSanitizer) and `clang-asan-ubsan` (AddressSanitizer + UndefinedBehaviorSanitizer). CI runs all five on Ubuntu 24.04. It also runs a clang-format check and builds `tests/consumer`, a separate CMake project that only links `cte::cte`, which checks that the library works as a dependency.

## What the tests cover

| CTest name | Covers |
| --- | --- |
| `executor_contract` | Results, move-only types, exceptions, FIFO order, concurrent producers, `wait_idle()`, races between `submit()` and `close()`, concurrent `shutdown()` callers, misuse from inside a task (including death tests) |
| `executor_faults` | Built against `cte_testing`: thread creation failing in the constructor while earlier workers are already waiting, deque allocation failure, a worker calling `shutdown()` while another thread holds the join lock, a future becoming ready before the worker's bookkeeping is done |
| `executor_stress` | Randomized rounds with 1–8 workers and 1–8 producers. In most rounds three threads call `shutdown()` while producers are still submitting, and some tasks throw. Checks that every accepted task ran exactly once and that rejected ones never ran |
| `example_results`, `example_checksums` | The examples build and run |
| `benchmark_contract` | Benchmark workloads produce known answers in every mode |
| `benchmark_tools` | The Python analysis scripts reject incomplete or inconsistent data |

The race tests don't use sleeps to order events. They use latches and gates to force a specific interleaving, for example keeping a task parked inside a worker while another thread calls `shutdown()`. Short `wait_for` timeouts appear only to check that something is still blocked.

Long stress run under ThreadSanitizer:

```sh
TSAN_OPTIONS=halt_on_error=1 build/clang-tsan/tests/cte_stress --seconds 600 --tasks 10000
```

Last run, on 2026-10-22: 12,222 rounds of 10,000 submissions in 600 seconds, with no reports. Sanitizers only catch problems in executions they actually see, so this is evidence, not proof.

## Two ThreadSanitizer false positives

Running the suite under TSan hundreds of times in a row turned up two intermittent reports. Both were false positives caused by synchronization inside the prebuilt, uninstrumented `libstdc++.so`:

1. **Publishing data through `std::latch` alone.** In GCC 16's libstdc++, the final load in `latch::wait()` can happen inside the shared library, so TSan never sees the happens-before edge from `count_down()`. The affected test now passes its data back through futures.
2. **An exception object freed on another thread.** When a task throws, the exception lives in the future's shared state. If the main thread finishes handling it before the worker has dropped its reference to the task, the worker frees the exception object. The reference count is atomic, but it is updated inside libsupc++, where TSan can't see it. The affected test and example now make sure the worker has released the task first.

Neither involves the executor's own synchronization. After these fixes, 500 consecutive TSan runs of `executor_contract`, 200 of `executor_faults` and 500 of `example_results` were all clean.

## Formatting and static analysis

```sh
git ls-files -z '*.cpp' '*.hpp' | xargs -0 clang-format-18 --dry-run --Werror
clang-tidy src/task_executor.cpp -p build/clang-debug
```
