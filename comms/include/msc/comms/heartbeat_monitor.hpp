#pragma once

#include "msc/common/clock.hpp"

#include <chrono>
#include <cstdint>
#include <optional>

namespace msc::comms {

enum class LinkState : std::uint8_t {
    Waiting,  // no heartbeat yet
    Alive,    // the newest heartbeat is recent
    Lost,     // more than the allowed number of heartbeats missed
};

// Knows whether the other side of the link is alive, from when its heartbeats arrived
// (REQ-SAF-001). No clock of its own and no I/O: the caller passes the time in.
//
// The other side sends a heartbeat every `period`. Lost means: more than `missedLimit` periods
// since the last one arrived. Measured from the arrival time on this side's own clock, never
// from the sender's timestamp: the two processes' clocks have nothing to do with each other
// (ADR 0001), and a sender whose clock stopped must not look alive.
//
// The limit is a trade-off. Too few periods and a busy machine or one late packet causes false
// alarms (an arm that stops for no reason); too many and the arm keeps moving on an old command
// for that long after the console died. 3 periods of 10 ms: the arm starts holding within 30 ms.
class HeartbeatMonitor {
public:
    explicit HeartbeatMonitor(Nanoseconds period = std::chrono::milliseconds(10),
                              unsigned missedLimit = 3) noexcept
        : limit_(period * missedLimit) {}

    void received(Nanoseconds now) noexcept { last_ = now; }
    [[nodiscard]] LinkState state(Nanoseconds now) const noexcept {
        if (!last_) {
            return LinkState::Waiting;
        }
        return now - *last_ > limit_ ? LinkState::Lost : LinkState::Alive;
    }
    // Time since the last heartbeat arrived, if one has.
    [[nodiscard]] std::optional<Nanoseconds> silence(Nanoseconds now) const noexcept {
        return last_ ? std::optional<Nanoseconds>(now - *last_) : std::nullopt;
    }
    [[nodiscard]] Nanoseconds limit() const noexcept { return limit_; }
    void reset() noexcept { last_.reset(); }

private:
    Nanoseconds limit_;
    std::optional<Nanoseconds> last_;
};

}  // namespace msc::comms
