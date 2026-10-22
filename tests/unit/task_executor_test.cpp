#include <cte/task_executor.hpp>
#include <gtest/gtest.h>

#include "test_helpers.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <type_traits>

namespace {

using cte::task_executor;

template <class F, class... Args>
concept accepts = requires(task_executor& executor, F&& f, Args&&... args) {
    executor.submit(std::forward<F>(f), std::forward<Args>(args)...);
};

struct rvalue_only {
    std::unique_ptr<int> value;
    int operator()() && {
        return *value;
    }
};
struct lvalue_only {
    int operator()() & {
        return 19;
    }
};
struct reference_result {
    int& operator()() const;
};
struct throwing_destructor {
    ~throwing_destructor() noexcept(false);
    void operator()() const;
};

static_assert(!std::copy_constructible<task_executor>);
static_assert(!std::move_constructible<task_executor>);
static_assert(accepts<rvalue_only>);
static_assert(!accepts<rvalue_only&>);
static_assert(!accepts<lvalue_only>);
static_assert(accepts<std::reference_wrapper<lvalue_only>>);
static_assert(!accepts<reference_result>);
static_assert(!accepts<int>);
static_assert(!accepts<throwing_destructor>);

TEST(Construction, RejectsZeroAndKeepsConfiguredCount) {
    EXPECT_THROW(task_executor(0), std::invalid_argument);
    task_executor executor(3);
    EXPECT_EQ(executor.worker_count(), 3U);
    executor.shutdown();
    EXPECT_EQ(executor.worker_count(), 3U);
}

TEST(Results, ValuesVoidAndIndependentFutureStates) {
    task_executor executor(2);
    auto integer = executor.submit([] { return 42; });
    auto text = executor.submit([](std::string value) { return value + "!"; }, "hello");
    std::atomic<int> count{0};
    auto nothing = executor.submit([&count] { ++count; });
    EXPECT_EQ(integer.get(), 42);
    EXPECT_EQ(text.get(), "hello!");
    nothing.get();
    EXPECT_EQ(count.load(), 1);
    auto callable = [] { return 7; };
    auto first = executor.submit(callable);
    auto second = executor.submit(callable);
    EXPECT_EQ(first.get(), 7);
    EXPECT_EQ(second.get(), 7);
}

TEST(Results, MoveOnlyCaptureArgumentAndResult) {
    task_executor executor(2);
    auto captured = executor.submit([value = std::make_unique<int>(17)] { return *value; });
    auto moved = executor.submit([](std::unique_ptr<int> value) { return value; },
                                 std::make_unique<int>(29));
    auto qualified = executor.submit(rvalue_only{std::make_unique<int>(31)});
    EXPECT_EQ(captured.get(), 17);
    EXPECT_EQ(*moved.get(), 29);
    EXPECT_EQ(qualified.get(), 31);
}

TEST(Results, InvocationForms) {
    task_executor executor(2);
    lvalue_only callable;
    auto borrowed = executor.submit(std::ref(callable));
    auto mutable_lambda = executor.submit([value = 1]() mutable { return ++value; });
    auto member = executor.submit(&std::string::size, std::string("hello"));
    auto function = executor.submit(+[](int value) { return value * 2; }, 9);
    EXPECT_EQ(borrowed.get(), 19);
    EXPECT_EQ(mutable_lambda.get(), 2);
    EXPECT_EQ(member.get(), 5U);
    EXPECT_EQ(function.get(), 18);
}

TEST(Ownership, CopiesLvaluesAndBorrowsOnlyExplicitly) {
    cte::test::gate gate;
    task_executor executor(1);
    auto blocked = executor.submit([&] { gate.wait(); });
    cte::test::scope_exit cleanup([&] { gate.open(); });
    int value = 3;
    auto copy = executor.submit([](int v) { return v; }, value);
    auto reference = executor.submit([](int& v) { return ++v; }, std::ref(value));
    value = 10;
    gate.open();
    blocked.get();
    EXPECT_EQ(copy.get(), 3);
    EXPECT_EQ(reference.get(), 11);
    EXPECT_EQ(value, 11);
}

TEST(Exceptions, PreservesOriginalExceptionAndWorkerContinues) {
    struct user_error : std::runtime_error {
        using std::runtime_error::runtime_error;
    };
    task_executor executor(1);
    auto failed = executor.submit([]() -> int { throw user_error("original message"); });
    auto next = executor.submit([] { return 23; });
    // The single worker released `failed` before running `next`, so the exception
    // object is destroyed on this thread. Otherwise ThreadSanitizer reports a false
    // race: libstdc++'s exception reference count is not instrumented.
    EXPECT_EQ(next.get(), 23);
    try {
        (void)failed.get();
        FAIL() << "expected the original task exception";
    } catch (const user_error& error) {
        EXPECT_STREQ(error.what(), "original message");
    }
}

TEST(Exceptions, PreparationFailureDoesNotAcceptWork) {
    struct bad_copy {
        std::atomic<int>* calls;
        explicit bad_copy(std::atomic<int>& value) : calls(&value) {}
        bad_copy(const bad_copy&) {
            throw std::runtime_error("copy failed");
        }
        bad_copy(bad_copy&&) = default;
        void operator()() {
            ++*calls;
        }
    };
    task_executor executor(1);
    std::atomic<int> calls{0};
    bad_copy callable(calls);
    EXPECT_THROW((void)executor.submit(callable), std::runtime_error);
    EXPECT_EQ(executor.submit([] { return 8; }).get(), 8);
    EXPECT_EQ(calls.load(), 0);
}

TEST(Ownership, DiscardedFutureDoesNotCancelAndFutureOutlivesExecutor) {
    std::atomic<int> calls{0};
    std::future<std::unique_ptr<int>> result;
    std::future<void> error;
    {
        task_executor executor(2);
        (void)executor.submit([&] { ++calls; });
        result = executor.submit([] { return std::make_unique<int>(99); });
        error = executor.submit([] { throw std::runtime_error("survives"); });
    }
    EXPECT_EQ(calls.load(), 1);
    EXPECT_EQ(*result.get(), 99);
    EXPECT_THROW(error.get(), std::runtime_error);
}

TEST(Closure, RejectsAfterCloseAndAfterShutdown) {
    task_executor executor(1);
    executor.close();
    EXPECT_THROW((void)executor.submit([] {}), cte::executor_closed);
    executor.close();
    executor.shutdown();
    executor.shutdown();
    EXPECT_THROW((void)executor.submit([] {}), cte::executor_closed);
}

TEST(Closure, RejectionMayConsumeRvalues) {
    task_executor executor(1);
    executor.close();
    auto owned = std::make_unique<int>(1);
    EXPECT_THROW((void)executor.submit([](std::unique_ptr<int>) {}, std::move(owned)),
                 cte::executor_closed);
    EXPECT_EQ(owned, nullptr);
}

} // namespace
