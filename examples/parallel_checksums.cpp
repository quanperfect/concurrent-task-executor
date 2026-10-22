#include <cte/task_executor.hpp>

#include <cstdint>
#include <iostream>
#include <span>
#include <vector>

namespace {
// FNV-1a is an inexpensive integrity example, not a cryptographic hash.
std::uint64_t checksum(std::span<const unsigned char> bytes) {
    std::uint64_t result = 14695981039346656037ULL;
    for (auto byte : bytes) {
        result = (result ^ byte) * 1099511628211ULL;
    }
    return result;
}
} // namespace

int main() {
    constexpr std::size_t chunks = 128;
    constexpr std::size_t bytes_per_chunk = 4096;
    // Inputs outlive every worker. Tasks read disjoint, immutable slices.
    std::vector<unsigned char> input(chunks * bytes_per_chunk);
    for (std::size_t i = 0; i < input.size(); ++i) {
        input[i] = static_cast<unsigned char>(i * 37U);
    }
    cte::task_executor executor(4);
    std::vector<std::future<std::uint64_t>> results;
    results.reserve(chunks);
    for (std::size_t i = 0; i < chunks; ++i) {
        const auto slice =
            std::span<const unsigned char>(input).subspan(i * bytes_per_chunk, bytes_per_chunk);
        results.push_back(executor.submit(checksum, slice));
    }
    for (std::size_t i = 0; i < chunks; ++i) {
        const auto slice =
            std::span<const unsigned char>(input).subspan(i * bytes_per_chunk, bytes_per_chunk);
        if (results[i].get() != checksum(slice)) {
            std::cerr << "Checksum mismatch\n";
            return 1;
        }
    }
    executor.shutdown();
    std::cout << "Validated " << chunks << " independent buffers (" << input.size() << " bytes)\n";
}
