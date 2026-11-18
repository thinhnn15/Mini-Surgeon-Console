#pragma once

#include "msc/common/clock.hpp"
#include "msc/comms/protocol.hpp"

#include <bitset>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <variant>

namespace msc::teleop {

using comms::JointArray;

// Why the console stopped tele-operating. Each has its own watchdog (M4.2).
enum class FaultCode : std::uint8_t {
    ArmLinkLost,             // the arm's heartbeats stopped
    HandInputStale,          // the hand controller stopped reporting
    CommandNotAcknowledged,  // the arm's telemetry stopped confirming our commands
    ArmHoldingOnItsOwn,      // the arm went to Holding by itself (its own watchdog fired)
    VideoFrozen,  // the picture is not live (frozen or no frames): the surgeon cannot see (REQ-VID-003)
    Injected,     // the fault injector (M4.4)
};
inline constexpr std::size_t kFaultCodeCount = 6;

[[nodiscard]] std::string_view toString(FaultCode code) noexcept;

// The fault conditions present right now (several can be). The teleop core works this out every
// tick; the state machine uses it to refuse an engage or a reset while a cause is still there.
using FaultSet = std::bitset<kFaultCodeCount>;
[[nodiscard]] inline FaultSet faultSet(FaultCode code) noexcept {
    FaultSet set;
    set.set(static_cast<std::size_t>(code));
    return set;
}
// The first code in the set, if any (for messages).
[[nodiscard]] std::optional<FaultCode> firstFault(const FaultSet& set) noexcept;

// ---- States: each carries only the data that exists in it --------------------------------

// Not driving the arm. The arm is told to hold.
struct Idle {};

// The hand drives the arm. The anchors are the hand's and the arm's poses at the moment the
// hand took over (engage, or clutch release): motion is relative to them, so taking over never
// makes the arm jump to wherever the hand happens to be.
struct Engaged {
    JointArray handAnchor{};
    JointArray armAnchor{};
};

// Clutch held: the hand moves freely, the arm holds (REQ-SAF-003). Used to bring the hand back
// to a comfortable position without moving the instrument.
struct Paused {};

// Something went wrong. Latched: only an operator reset leaves it (REQ-SAF-002).
struct Fault {
    FaultCode code = FaultCode::Injected;  // the first fault; later ones do not overwrite it
    Nanoseconds since{0};
};

using State = std::variant<Idle, Engaged, Paused, Fault>;

[[nodiscard]] std::string_view stateName(const State& state) noexcept;

// ---- Events: operator actions and detected faults ------------------------------------------

struct Engage {
    JointArray hand{};   // the hand's pose now
    JointArray arm{};    // the arm's measured pose now
    FaultSet present{};  // conditions that would fault at once: engage is refused
};
struct Disengage {};
struct ClutchPressed {};
struct ClutchReleased {
    JointArray hand{};
    JointArray arm{};
};
struct FaultDetected {
    FaultCode code = FaultCode::Injected;
};
struct Reset {
    FaultSet present{};  // a reset is refused while a cause is still there
};

using Event = std::variant<Engage, Disengage, ClutchPressed, ClutchReleased, FaultDetected, Reset>;

// What handle() did with an event.
enum class Outcome : std::uint8_t {
    Changed,    // moved to another state (or re-anchored)
    Unchanged,  // the event means nothing in this state
    Refused,    // the event was asked for but is not allowed now; `why` says why
};

struct Result {
    Outcome outcome = Outcome::Unchanged;
    std::string_view why{};  // for Refused: shown to the operator
};

// The teleop state machine: which state the console is in, what every event does to it, and
// where the arm should go for a given hand pose. Plain C++, no threads, no I/O, no clock of its
// own: the teleop core (M4.2) feeds it events and the time, and acts on what it says.
//
// Motion: while Engaged, target = armAnchor + scale * (hand - handAnchor). Scaled down (0.5 by
// default): a 2 cm hand movement becomes 1 cm at the instrument, which steadies the hand and
// makes fine work possible. Clutching re-anchors on release, so the hand can be repositioned
// without moving the arm, as on a real surgical console.
class StateMachine {
public:
    explicit StateMachine(double scale = 0.5) noexcept : scale_(scale) {}

    Result handle(const Event& event, Nanoseconds now);

    [[nodiscard]] const State& state() const noexcept { return state_; }
    // Where the arm should be for this hand pose: only while Engaged. Nothing in any other state,
    // and the teleop core then tells the arm to hold.
    [[nodiscard]] std::optional<JointArray> target(const JointArray& hand) const noexcept;
    [[nodiscard]] double scale() const noexcept { return scale_; }

private:
    State state_;
    double scale_;
};

}  // namespace msc::teleop
