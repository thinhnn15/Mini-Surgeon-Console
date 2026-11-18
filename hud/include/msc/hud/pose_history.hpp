#pragma once

#include "msc/common/clock.hpp"
#include "msc/comms/protocol.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace msc::hud {

// Where the arm was, and when. `time` is on the console's clock, the same clock video frames
// are stamped with (Frame::captureTime): that is what makes "the pose for this frame" a
// question with an answer.
struct TimedPose {
    Nanoseconds time{0};
    comms::JointArray position{};
};

// Why at() gave a pose, or did not.
enum class PoseLookup : std::uint8_t {
    Found,          // an exact sample, or interpolated between the two around the time asked
    NoData,         // the history is empty
    BeforeHistory,  // the time asked is older than everything kept
    AfterNewest,    // the time asked is newer than the newest sample: not known (yet)
    InGap,          // between two samples that are too far apart to say what happened in between
};

[[nodiscard]] const char* toString(PoseLookup lookup) noexcept;

struct PoseAt {
    PoseLookup result = PoseLookup::NoData;
    comms::JointArray position{};  // valid when found()

    [[nodiscard]] bool found() const noexcept { return result == PoseLookup::Found; }
};

struct PoseHistoryConfig {
    // Samples kept. At the arm's 1 kHz, 1024 samples are the last second: several times the
    // oldest frame that can still be on screen as live (REQ-VID-003: 250 ms).
    std::size_t capacity = 1024;
    // Two neighbouring samples further apart than this are a hole in the data (a lost link, a
    // stalled reader), not two points of one motion: nothing is said about the time between.
    // 30 ms is the arm link's own "lost" limit. Interpolating a straight line across 30 ms is
    // wrong by at most a·h²/8 = 6 rad/s² · (0.03 s)² / 8 = 0.0007 rad, under one pixel.
    Nanoseconds maxGap = std::chrono::milliseconds(30);
};

// The arm's recent poses, by time (M5.1). Answers "where was the arm at time T?" for the
// overlay: a video frame shows the world as it was when the frame was captured, some tens of
// milliseconds ago, so the overlay for that frame needs the pose of that moment, not the
// newest one.
//
// REQ-HUD-001: at() gives a pose only when the samples say where the arm was at that time. It
// interpolates between two samples; it never extrapolates past the newest one and never
// bridges a gap. "Not known" is an answer, and the overlay then draws nothing.
//
// A fixed ring: when full, a new sample replaces the oldest. No allocation after construction.
// Not thread safe: one thread owns it (the render thread, fed through an SpscQueue).
class PoseHistory {
public:
    explicit PoseHistory(PoseHistoryConfig config = {});

    // Adds a sample. Samples must come in time order; one older than the newest is refused
    // (false), because a history that is not ordered cannot be searched.
    bool push(const TimedPose& sample) noexcept;

    // The pose at `time`, or why there is none.
    [[nodiscard]] PoseAt at(Nanoseconds time) const noexcept;

    [[nodiscard]] std::optional<TimedPose> newest() const noexcept;
    [[nodiscard]] std::optional<TimedPose> oldest() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return samples_.size(); }
    void clear() noexcept;

private:
    // The i-th sample in time order, 0 = oldest.
    [[nodiscard]] const TimedPose& sample(std::size_t i) const noexcept {
        return samples_[(first_ + i) % samples_.size()];
    }

    PoseHistoryConfig config_;
    std::vector<TimedPose> samples_;
    std::size_t first_ = 0;  // index of the oldest sample
    std::size_t size_ = 0;
};

}  // namespace msc::hud
