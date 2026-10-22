#pragma once

#include "workloads.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace cte::bench {

enum class mode { sequential, pool, cold_batch, producer_pool };

struct scenario {
    mode kind = mode::pool;
    std::size_t workers = 1;
    std::size_t producers = 1;
};

[[nodiscard]] std::string_view name(mode kind);
// Setup/validation excluded; output storage must be preallocated by the caller.
[[nodiscard]] std::int64_t run(const dataset& data, scenario config,
                               std::span<std::uint64_t> output, unsigned warmups);

} // namespace cte::bench
