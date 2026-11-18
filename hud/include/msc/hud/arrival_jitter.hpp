#pragma once

#include "msc/common/clock.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>

namespace msc::hud {

// How late telemetry arrives, packet by packet (M5.4).
//
// The pose history stamps each pose with its arrival time on the console's clock (M5.1). That
// time is later than the moment the arm measured the pose by the trip on the wire plus however
// late the link thread woke up, and the overlay is off by that delay times the arm's speed.
// The trip cannot be measured with two clocks that do not agree on what time it is. Its
// variation can: for each packet, arrival time minus the arm's own send time is "the offset
// between the two clocks plus this packet's delay". The smallest value seen is the offset plus
// the quickest trip; how far a packet is above that is how much later than the quickest it
// arrived.
//
// add() from one thread (the link thread); report() from any thread. No locks.
class ArrivalJitter {
public:
    struct Report {
        std::uint64_t samples = 0;
        std::uint64_t over1ms = 0;  // packets that arrived more than 1 ms later than the quickest
        std::uint64_t over2ms = 0;
        std::uint64_t over5ms = 0;
        Nanoseconds worst{0};
    };

    void add(Nanoseconds receivedAt, Nanoseconds armTime) noexcept {
        const std::int64_t offset = (receivedAt - armTime).count();
        // Relaxed everywhere: single numbers, one writer, and the reader only wants figures.
        std::int64_t quickest = quickest_.load(std::memory_order_relaxed);
        if (offset < quickest) {
            quickest = offset;
            quickest_.store(offset, std::memory_order_relaxed);
        }
        const std::int64_t late = offset - quickest;
        samples_.fetch_add(1, std::memory_order_relaxed);
        if (late > kMillisecond) {
            over1ms_.fetch_add(1, std::memory_order_relaxed);
        }
        if (late > 2 * kMillisecond) {
            over2ms_.fetch_add(1, std::memory_order_relaxed);
        }
        if (late > 5 * kMillisecond) {
            over5ms_.fetch_add(1, std::memory_order_relaxed);
        }
        if (late > worst_.load(std::memory_order_relaxed)) {
            worst_.store(late, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] Report report() const noexcept {
        return Report{.samples = samples_.load(std::memory_order_relaxed),
                      .over1ms = over1ms_.load(std::memory_order_relaxed),
                      .over2ms = over2ms_.load(std::memory_order_relaxed),
                      .over5ms = over5ms_.load(std::memory_order_relaxed),
                      .worst = Nanoseconds{worst_.load(std::memory_order_relaxed)}};
    }

private:
    static constexpr std::int64_t kMillisecond = 1'000'000;

    std::atomic<std::int64_t> quickest_{std::numeric_limits<std::int64_t>::max()};
    std::atomic<std::uint64_t> samples_{0};
    std::atomic<std::uint64_t> over1ms_{0};
    std::atomic<std::uint64_t> over2ms_{0};
    std::atomic<std::uint64_t> over5ms_{0};
    std::atomic<std::int64_t> worst_{0};
};

}  // namespace msc::hud
