#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace msc::comms {

// What SequenceFilter decided about one packet.
enum class SequenceVerdict : std::uint8_t {
    Accepted,        // newer than anything accepted before: use it
    Duplicate,       // the same sequence as the last accepted packet
    Old,             // older than the last accepted packet (reordered or delayed)
    RetiredSession,  // from a session that has since been replaced (a restarted sender)
};

struct SequenceStats {
    std::uint64_t accepted = 0;
    std::uint64_t lost = 0;  // gaps in the sequence when a packet was accepted
    std::uint64_t duplicates = 0;
    std::uint64_t old = 0;  // includes late packets that were already counted as lost
    std::uint64_t retired = 0;
    std::uint64_t sessionChanges = 0;
};

// REQ-COM-001's "out-of-date packets are discarded", for one sender.
//
// UDP may deliver a packet twice, late, or out of order. For a robot arm an old packet is not
// harmless just because it is intact: an old command is an order to go back to where the arm
// was a moment ago. So only packets newer than the last accepted one get through; the newest
// one always wins, as with the video frames.
//
// Sequence numbers are 32 bits and wrap around (after 50 days at 1 kHz). "Newer" is serial
// number arithmetic (RFC 1982): b is newer than a when b - a, as a signed 32-bit number, is
// positive. That holds across the wrap, as long as the two are less than 2^31 apart.
//
// When the sender restarts, it starts counting again from 1, and every packet would look old.
// Each sender therefore picks a random session id when it starts. A new session id starts the
// filter over. The ids of a few replaced sessions are remembered, so a late packet from before
// the restart cannot switch the filter back to the old session.
//
// One thread (no locking).
class SequenceFilter {
public:
    [[nodiscard]] SequenceVerdict check(std::uint32_t session, std::uint32_t sequence) noexcept;

    [[nodiscard]] const SequenceStats& stats() const noexcept { return stats_; }
    // The session currently followed, if any packet was accepted.
    [[nodiscard]] std::optional<std::uint32_t> session() const noexcept { return session_; }

    // Forget everything, including retired sessions.
    void reset() noexcept;

    // True if `b` comes after `a` in serial number arithmetic.
    [[nodiscard]] static constexpr bool isNewer(std::uint32_t b, std::uint32_t a) noexcept {
        return static_cast<std::int32_t>(b - a) > 0;
    }

private:
    static constexpr std::size_t kRetiredSessions = 4;

    [[nodiscard]] bool isRetired(std::uint32_t session) const noexcept;

    std::optional<std::uint32_t> session_;
    std::uint32_t last_ = 0;
    std::array<std::optional<std::uint32_t>, kRetiredSessions> retired_{};
    std::size_t nextRetired_ = 0;
    SequenceStats stats_;
};

}  // namespace msc::comms
