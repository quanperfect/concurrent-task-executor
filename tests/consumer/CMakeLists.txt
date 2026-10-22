cmake_minimum_required(VERSION 3.25)
project(cte_consumer LANGUAGES CXX)

# Deliberately do not set CXX_STANDARD here: cte::cte must propagate C++20.
add_subdirectory(../.. cte)
if(TARGET cte_tests OR TARGET cte_bench OR TARGET cte_example_results)
    message(FATAL_ERROR "Embedding cte should not enable its development targets by default")
endif()
add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE cte::cte)
enable_testing()
add_test(NAME consumer COMMAND consumer)
