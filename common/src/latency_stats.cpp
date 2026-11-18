#include "msc/common/latency_stats.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <stdexcept>

namespace msc {

LatencyStats::LatencyStats(std::size_t capacity) : capacity_(capacity) {
    if (capacity == 0) {
        throw std::invalid_argument("LatencyStats: capacity must be at least 1");
    }
    samples_.reserve(capacity);
    scratch_.reserve(capacity);
}

void LatencyStats::add(Nanoseconds sample) {
    if (samples_.size() < capacity_) {
        samples_.push_back(sample);
        return;
    }
    samples_[next_] = sample;  // overwrite the oldest
    next_ = (next_ + 1) % capacity_;
}

void LatencyStats::clear() noexcept {
    samples_.clear();
    next_ = 0;
}

std::optional<Nanoseconds> LatencyStats::percentile(unsigned percent) const {
    if (samples_.empty()) {
        return std::nullopt;
    }
    const std::size_t n = samples_.size();
    const std::size_t p = std::clamp(percent, 1U, 100U);
    // Nearest rank: ceil(p * n / 100), in integers. At least 1, at most n.
    const std::size_t rank = ((p * n) + 99) / 100;

    scratch_.assign(samples_.begin(), samples_.end());
    const auto nth = scratch_.begin() + static_cast<std::ptrdiff_t>(rank - 1);
    std::nth_element(scratch_.begin(), nth, scratch_.end());
    return *nth;
}

}  // namespace msc
