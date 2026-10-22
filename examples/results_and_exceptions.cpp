#include <cte/task_executor.hpp>

#include <iostream>
#include <memory>
#include <stdexcept>

int main() {
    cte::task_executor executor(2);
    auto value = executor.submit([](std::unique_ptr<int> input) { return *input * 2; },
                                 std::make_unique<int>(21));
    auto failure =
        executor.submit([]() -> int { throw std::runtime_error("example task failed"); });
    auto continued = executor.submit([] { return 7; });
    executor.shutdown(); // Runs every accepted task, then joins the workers.

    // Futures outlive the workers that fulfilled them.
    std::cout << "Result: " << value.get() << '\n';
    try {
        (void)failure.get();
        return 1;
    } catch (const std::runtime_error& error) {
        std::cout << "Task exception: " << error.what() << '\n';
    }
    std::cout << "Other tasks still ran: " << continued.get() << '\n';
}
