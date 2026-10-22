#include "runner.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <array>

namespace {
using namespace cte::bench;

TEST(Workloads, KnownAnswers) {
    EXPECT_EQ(compute(1, 0), 1U);
    EXPECT_EQ(compute(1, 1), 5180492295206395165ULL);
    EXPECT_EQ(compute(0, 100), 0U);
    const std::array<unsigned char, 5> hello{'h', 'e', 'l', 'l', 'o'};
    EXPECT_EQ(buffer_checksum(hello), 0xa430d84680aabd0bULL);
    EXPECT_EQ(buffer_checksum({}), 14695981039346656037ULL);
}

TEST(Workloads, RejectsInvalidOrOversizedInputs) {
    EXPECT_THROW((void)make_dataset({workload::compute, 0, 10, 0, 1}), std::invalid_argument);
    EXPECT_THROW((void)make_dataset({workload::compute, 10, 0, 0, 1}), std::invalid_argument);
    EXPECT_THROW((void)make_dataset({workload::buffer_checksum, 1'000'000, 0, 4096, 1}),
                 std::invalid_argument);
    EXPECT_THROW((void)make_dataset({workload::minimal, 10, 0, 64, 1}), std::invalid_argument);
}

TEST(Batches, EveryModeAndWorkloadMatchesTheSequentialOracle) {
    for (auto kind : {workload::minimal, workload::compute, workload::mixed_compute,
                      workload::buffer_checksum}) {
        auto data = make_dataset({kind, 73, 31, kind == workload::buffer_checksum ? 64U : 0U, 123});
        for (auto scenario :
             {cte::bench::scenario{mode::sequential, 0, 1}, cte::bench::scenario{mode::pool, 1, 1},
              cte::bench::scenario{mode::pool, 4, 1}, cte::bench::scenario{mode::cold_batch, 2, 1},
              cte::bench::scenario{mode::producer_pool, 3, 1},
              cte::bench::scenario{mode::producer_pool, 3, 5}}) {
            std::vector<std::uint64_t> output(data.config.tasks);
            std::transform(data.expected.begin(), data.expected.end(), output.begin(),
                           [](std::uint64_t value) { return ~value; });
            EXPECT_GT(run(data, scenario, output, 1), 0);
            EXPECT_EQ(output, data.expected);
        }
    }
}

TEST(Batches, RejectsInvalidScenario) {
    auto data = make_dataset({workload::minimal, 10, 0, 0, 1});
    std::vector<std::uint64_t> output(10);
    EXPECT_THROW((void)run(data, {mode::pool, 0, 1}, output, 1), std::invalid_argument);
    EXPECT_THROW((void)run(data, {mode::pool, 2, 2}, output, 1), std::invalid_argument);
    EXPECT_THROW((void)run(data, {mode::sequential, 1, 1}, output, 1), std::invalid_argument);
    EXPECT_THROW((void)run(data, {mode::pool, 1, 1}, {}, 1), std::invalid_argument);
}

} // namespace
