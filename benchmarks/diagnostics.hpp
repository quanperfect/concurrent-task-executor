#pragma once

#include <cte/detail/queue_probe.hpp>

namespace cte::bench {

struct probe_session {
    probe_session(std::size_t tasks, std::size_t producers, std::size_t stride) {
        buffers.reserve(producers);
        for (std::size_t p = 0; p != producers; ++p) {
            buffers.emplace_back(tasks / producers + 1, stride);
        }
    }
    std::vector<detail::queue_probe> buffers;
};

inline thread_local probe_session* current_probes = nullptr;

class scoped_probe_session {
  public:
    explicit scoped_probe_session(probe_session& session) noexcept : previous_(current_probes) {
        current_probes = &session;
    }
    ~scoped_probe_session() {
        current_probes = previous_;
    }
    scoped_probe_session(const scoped_probe_session&) = delete;
    scoped_probe_session& operator=(const scoped_probe_session&) = delete;

  private:
    probe_session* previous_;
};

} // namespace cte::bench
