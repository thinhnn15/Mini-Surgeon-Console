#pragma once

#include "msc/common/clock.hpp"

#include <cstddef>
#include <optional>
#include <vector>

namespace msc {

// Percentiles over the last N latency samples (a rolling window: the oldest sample drops
// out when a new one arrives once the window is full).
//
// Why percentiles and not the average: a surgeon does not feel the average frame, they feel
// the slow ones. 99 frames at 10 ms and one at 200 ms average 12 ms, which looks fine; p99
// says 200 ms, which is the stutter they see. p50 is the typical case, p99 the bad case
// that still happens every couple of seconds at 60 fps, max the worst seen in the window.
//
// One thread only (no locking). Queries cost O(N) (std::nth_element on a scratch copy);
// meant for N of a few hundred, queried a few times per second, not per sample.
class LatencyStats {
public:
    // capacity: how many of the most recent samples are kept. Must be at least 1.
    explicit LatencyStats(std::size_t capacity);

    void add(Nanoseconds sample);
    void clear() noexcept;

    [[nodiscard]] std::size_t count() const noexcept { return samples_.size(); }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

    // The nearest-rank percentile: the smallest sample such that at least `percent` % of the
    // samples are less than or equal to it. Always one of the samples, never interpolated.
    // `percent` is clamped to 1..100 (integer, so p99 of 100 samples is exactly the 99th:
    // no floating-point rounding can turn 0.99 * 100 into rank 100).
    // Nothing if there are no samples.
    [[nodiscard]] std::optional<Nanoseconds> percentile(unsigned percent) const;

    [[nodiscard]] std::optional<Nanoseconds> p50() const { return percentile(50); }
    [[nodiscard]] std::optional<Nanoseconds> p99() const { return percentile(99); }
    [[nodiscard]] std::optional<Nanoseconds> max() const { return percentile(100); }

private:
    std::size_t capacity_;
    std::vector<Nanoseconds> samples_;  // ring buffer once full
    std::size_t next_ = 0;              // where the next sample goes once full
    // Reused by percentile() so a query does not allocate after the first one.
    mutable std::vector<Nanoseconds> scratch_;
};

}  // namespace msc
