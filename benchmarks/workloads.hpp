#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace cte::bench {

enum class workload { minimal, compute, mixed_compute, buffer_checksum };

struct parameters {
    workload kind = workload::compute;
    std::size_t tasks = 10'000;
    std::uint64_t rounds = 1000;
    std::size_t bytes_per_task = 0;
    std::uint64_t seed = 20260917;
};

[[nodiscard]] workload parse_workload(std::string_view name);
[[nodiscard]] std::string_view name(workload kind);
[[nodiscard]] std::uint64_t compute(std::uint64_t input, std::uint64_t rounds) noexcept;
[[nodiscard]] std::uint64_t buffer_checksum(std::span<const unsigned char> bytes) noexcept;
[[nodiscard]] std::uint64_t result_checksum(std::span<const std::uint64_t> values) noexcept;

struct dataset {
    parameters config;
    std::vector<std::uint64_t> inputs;
    std::vector<unsigned char> bytes;
    std::vector<std::uint64_t> expected;
};

[[nodiscard]] dataset make_dataset(parameters config);
[[nodiscard]] std::uint64_t evaluate(const dataset& data, std::size_t index) noexcept;

} // namespace cte::bench
