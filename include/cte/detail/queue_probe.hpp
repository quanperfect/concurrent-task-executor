#ifndef CTE_DETAIL_QUEUE_PROBE_HPP
#define CTE_DETAIL_QUEUE_PROBE_HPP

#ifndef CTE_QUEUE_PROBES
#error "Queue probes are available only in the isolated diagnostic build"
#endif

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace cte::detail {

struct queue_sample {
    std::int64_t wait_ns;
    std::int64_t hold_ns;
};

// One producer owns each buffer; read it only after that producer finishes.
// All storage is allocated before timing. Sampling and recording never allocate.
class queue_probe {
  public:
    queue_probe(std::size_t max_submissions, std::size_t stride) : stride_(stride) {
        if (stride == 0) {
            throw std::invalid_argument("queue probe stride must be positive");
        }
        samples.resize(max_submissions / stride + 1);
    }

    [[nodiscard]] bool sample_next() noexcept {
        return (calls_++ % stride_) == 0;
    }
    void record(queue_sample sample) noexcept {
        if (size < samples.size()) {
            samples[size++] = sample;
        } else {
            ++dropped;
        }
    }

    std::vector<queue_sample> samples;
    std::size_t size = 0;
    std::size_t dropped = 0;

  private:
    std::size_t stride_;
    std::size_t calls_ = 0;
};

inline thread_local queue_probe* current_queue_probe = nullptr;

// Times one sampled enqueue critical section. record() is called after unlocking.
class enqueue_timer {
  public:
    enqueue_timer() noexcept
        : probe_(current_queue_probe && current_queue_probe->sample_next() ? current_queue_probe
                                                                           : nullptr) {
        if (probe_) {
            before_ = clock::now();
        }
    }
    void locked() noexcept {
        if (probe_) {
            acquired_ = clock::now();
        }
    }
    void unlocking() noexcept {
        if (probe_) {
            releasing_ = clock::now();
        }
    }
    void record() noexcept {
        if (probe_) {
            probe_->record({nanoseconds(acquired_ - before_), nanoseconds(releasing_ - acquired_)});
        }
    }

  private:
    using clock = std::chrono::steady_clock;
    static std::int64_t nanoseconds(clock::duration value) noexcept {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(value).count();
    }
    queue_probe* probe_;
    clock::time_point before_;
    clock::time_point acquired_;
    clock::time_point releasing_;
};

class scoped_queue_probe {
  public:
    explicit scoped_queue_probe(queue_probe* probe) noexcept : previous_(current_queue_probe) {
        current_queue_probe = probe;
    }
    ~scoped_queue_probe() {
        current_queue_probe = previous_;
    }
    scoped_queue_probe(const scoped_queue_probe&) = delete;
    scoped_queue_probe& operator=(const scoped_queue_probe&) = delete;

  private:
    queue_probe* previous_;
};

} // namespace cte::detail

#endif // CTE_DETAIL_QUEUE_PROBE_HPP
