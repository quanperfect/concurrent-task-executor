#include "workloads.hpp"

#include <exception>
#include <stdexcept>

namespace cte::bench {

workload parse_workload(std::string_view value) {
    if (value == "minimal") {
        return workload::minimal;
    }
    if (value == "compute") {
        return workload::compute;
    }
    if (value == "mixed_compute") {
        return workload::mixed_compute;
    }
    if (value == "buffer_checksum") {
        return workload::buffer_checksum;
    }
    throw std::invalid_argument("unknown workload");
}

std::string_view name(workload kind) {
    switch (kind) {
    case workload::minimal:
        return "minimal";
    case workload::compute:
        return "compute";
    case workload::mixed_compute:
        return "mixed_compute";
    case workload::buffer_checksum:
        return "buffer_checksum";
    }
    throw std::invalid_argument("invalid workload enum");
}

std::uint64_t compute(std::uint64_t value, std::uint64_t rounds) noexcept {
    // A dependent xorshift/multiply chain: defined unsigned arithmetic and
    // runtime inputs. This is a synthetic CPU kernel, not a random-number API.
    for (std::uint64_t i = 0; i != rounds; ++i) {
        value ^= value >> 12U;
        value ^= value << 25U;
        value ^= value >> 27U;
        value *= 2685821657736338717ULL;
    }
    return value;
}

std::uint64_t buffer_checksum(std::span<const unsigned char> bytes) noexcept {
    std::uint64_t value = 14695981039346656037ULL;
    for (auto byte : bytes) {
        value = (value ^ byte) * 1099511628211ULL;
    }
    return value;
}

std::uint64_t result_checksum(std::span<const std::uint64_t> values) noexcept {
    std::uint64_t result = 14695981039346656037ULL;
    for (auto value : values) {
        // Numeric folding is independent of host endianness.
        result = (result ^ value) * 1099511628211ULL;
    }
    return result;
}

std::uint64_t evaluate(const dataset& data, std::size_t index) noexcept {
    switch (data.config.kind) {
    case workload::minimal:
        return data.inputs[index];
    case workload::compute:
        return compute(data.inputs[index], data.config.rounds);
    case workload::mixed_compute:
        return compute(data.inputs[index], data.config.rounds * (index % 8 == 0 ? 8U : 1U));
    case workload::buffer_checksum:
        return buffer_checksum(
            std::span<const unsigned char>(data.bytes)
                .subspan(index * data.config.bytes_per_task, data.config.bytes_per_task));
    }
    std::terminate();
}

dataset make_dataset(parameters config) {
    if (config.tasks == 0 || config.tasks > 1'000'000 || config.rounds > 10'000'000) {
        throw std::invalid_argument("tasks must be in [1,1000000], rounds <= 10000000");
    }
    if ((config.kind == workload::compute || config.kind == workload::mixed_compute) &&
        config.rounds == 0) {
        throw std::invalid_argument("compute workloads require positive rounds");
    }
    if (config.kind == workload::buffer_checksum) {
        constexpr std::size_t input_budget = 256U * 1024U * 1024U;
        if (config.bytes_per_task == 0 || config.bytes_per_task > input_budget / config.tasks) {
            throw std::invalid_argument(
                "buffer workload needs positive bytes/task and <= 256 MiB input");
        }
    } else if (config.bytes_per_task != 0) {
        throw std::invalid_argument("bytes/task applies only to buffer_checksum");
    }
    dataset data{config,
                 std::vector<std::uint64_t>(config.tasks),
                 {},
                 std::vector<std::uint64_t>(config.tasks)};
    for (std::size_t i = 0; i != config.tasks; ++i) {
        data.inputs[i] = config.seed ^ (static_cast<std::uint64_t>(i) + 0x9e3779b97f4a7c15ULL);
    }
    if (config.kind == workload::buffer_checksum) {
        data.bytes.resize(config.tasks * config.bytes_per_task);
        for (std::size_t i = 0; i != data.bytes.size(); ++i) {
            data.bytes[i] = static_cast<unsigned char>((i * 37U) ^ config.seed);
        }
    }
    for (std::size_t i = 0; i != config.tasks; ++i) {
        data.expected[i] = evaluate(data, i);
    }
    return data;
}

} // namespace cte::bench
