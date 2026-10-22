#include <cte/task_executor.hpp>

#include <memory>

int main() {
    cte::task_executor executor(2);
    auto answer = executor.submit([](std::unique_ptr<int> value) { return *value; },
                                  std::make_unique<int>(42));
    executor.shutdown();
    return answer.get() == 42 ? 0 : 1;
}
