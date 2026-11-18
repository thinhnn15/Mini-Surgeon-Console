#include "msc/hud/pose_history.hpp"

#include <algorithm>
#include <stdexcept>

namespace msc::hud {

const char* toString(PoseLookup lookup) noexcept {
    switch (lookup) {
        case PoseLookup::Found:
            return "found";
        case PoseLookup::NoData:
            return "no arm data yet";
        case PoseLookup::BeforeHistory:
            return "frame older than the pose history";
        case PoseLookup::AfterNewest:
            return "no arm data for this frame's time";
        case PoseLookup::InGap:
            return "arm data missing around this frame's time";
    }
    return "unknown";
}

PoseHistory::PoseHistory(PoseHistoryConfig config) : config_(config), samples_(config.capacity) {
    if (config.capacity < 2) {
        throw std::invalid_argument("PoseHistory: capacity must be at least 2");
    }
    if (config.maxGap <= Nanoseconds::zero()) {
        throw std::invalid_argument("PoseHistory: maxGap must be positive");
    }
}

bool PoseHistory::push(const TimedPose& sample) noexcept {
    if (size_ > 0 && sample.time < this->sample(size_ - 1).time) {
        return false;
    }
    if (size_ < samples_.size()) {
        samples_[(first_ + size_) % samples_.size()] = sample;
        ++size_;
    } else {
        samples_[first_] = sample;  // full: the oldest makes room
        first_ = (first_ + 1) % samples_.size();
    }
    return true;
}

PoseAt PoseHistory::at(Nanoseconds time) const noexcept {
    if (size_ == 0) {
        return {.result = PoseLookup::NoData, .position = {}};
    }
    // Binary search for the first sample newer than `time`. The one before it, if any, is the
    // newest sample at or before `time`.
    std::size_t low = 0;
    std::size_t high = size_;
    while (low < high) {
        const std::size_t middle = low + ((high - low) / 2);
        if (sample(middle).time <= time) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    if (low == 0) {
        return {.result = PoseLookup::BeforeHistory, .position = {}};
    }
    const TimedPose& before = sample(low - 1);
    if (before.time == time) {
        return {.result = PoseLookup::Found, .position = before.position};  // an exact sample
    }
    if (low == size_) {
        // Newer than everything we have. Guessing where the arm went after the last sample is
        // exactly what REQ-HUD-001 forbids.
        return {.result = PoseLookup::AfterNewest, .position = {}};
    }
    const TimedPose& after = sample(low);
    if (after.time - before.time > config_.maxGap) {
        return {.result = PoseLookup::InGap, .position = {}};
    }
    // A straight line between the two samples around `time`. In double: nanosecond counts do
    // not fit a float.
    const double fraction = static_cast<double>((time - before.time).count()) /
                            static_cast<double>((after.time - before.time).count());
    PoseAt found{.result = PoseLookup::Found, .position = {}};
    for (std::size_t i = 0; i < found.position.size(); ++i) {
        const double a = before.position[i];
        const double b = after.position[i];
        found.position[i] = static_cast<float>(a + ((b - a) * fraction));
    }
    return found;
}

std::optional<TimedPose> PoseHistory::newest() const noexcept {
    return size_ > 0 ? std::optional(sample(size_ - 1)) : std::nullopt;
}

std::optional<TimedPose> PoseHistory::oldest() const noexcept {
    return size_ > 0 ? std::optional(sample(0)) : std::nullopt;
}

void PoseHistory::clear() noexcept {
    first_ = 0;
    size_ = 0;
}

}  // namespace msc::hud
