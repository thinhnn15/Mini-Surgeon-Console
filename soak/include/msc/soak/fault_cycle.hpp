#pragma once

#include "msc/common/clock.hpp"
#include "msc/teleop/state_machine.hpp"
#include "msc/teleop/teleop_core.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace msc::soak {

// The faults the soak test injects, in the order it cycles through them. The names are the
// fault injector's (console_app's status bar chips and --inject).
enum class Injection : std::uint8_t { Link, Corrupt, Hand, Video, Generic };
inline constexpr std::array kInjections{Injection::Link, Injection::Corrupt, Injection::Hand,
                                        Injection::Video, Injection::Generic};

[[nodiscard]] std::string_view toString(Injection injection) noexcept;
// The fault codes the console may detect first for this injection (as in safety_check).
[[nodiscard]] std::span<const teleop::FaultCode> expectedFaults(Injection injection) noexcept;

// What the console shows at one moment.
struct Observation {
    Nanoseconds now{0};
    teleop::StateKind state = teleop::StateKind::Idle;
    teleop::FaultCode fault = teleop::FaultCode::Injected;  // valid in Fault
    bool engageAllowed = false;
    bool resetAllowed = false;
};

// What the operator does next.
struct Action {
    enum class Kind : std::uint8_t { None, Engage, Reset, Inject, Clear };
    Kind kind = Kind::None;
    Injection injection = Injection::Generic;  // for Inject and Clear

    friend bool operator==(const Action&, const Action&) = default;
};

struct FaultCycleConfig {
    Nanoseconds engagedFor = std::chrono::seconds(3);        // moving before each injection
    Nanoseconds detectWithin = std::chrono::seconds(2);      // the expected fault must show by then
    Nanoseconds faultHeld = std::chrono::milliseconds(500);  // injection kept on after detection
    Nanoseconds recoverWithin = std::chrono::seconds(5);     // cleared -> reset accepted -> Idle
    Nanoseconds engageWithin = std::chrono::seconds(5);      // engage allowed -> Engaged
};

struct FaultCycleCounts {
    std::uint64_t injected = 0;
    std::uint64_t detected = 0;                 // the expected fault, within detectWithin
    std::uint64_t wrongFault = 0;               // a fault, but not one of the expected codes
    std::uint64_t missed = 0;                   // no fault within detectWithin
    std::uint64_t earlyReset = 0;               // a reset was accepted while the injection was still on
    std::uint64_t recovered = 0;                // back to Idle after a fault (injected or not)
    std::uint64_t unexpected = 0;               // a fault nobody injected
    std::uint64_t stuck = 0;                    // could not engage, or not reset, within the limits
    std::vector<Nanoseconds> detectionTimes{};  // injection -> fault seen, per detected fault
    std::array<std::uint64_t, teleop::kFaultCodeCount> unexpectedByCode{};

    // Nothing that should not happen happened.
    [[nodiscard]] bool clean() const noexcept {
        return wrongFault == 0 && missed == 0 && earlyReset == 0 && unexpected == 0 && stuck == 0;
    }
};

// The operator of the soak test (M6.2, REQ-SYS-001): engages, lets the arm move, injects the next
// fault, checks that the console faults with an expected code and refuses a reset while the cause
// is there, clears it, resets, and starts again with the next fault, for as long as it is fed.
// A fault that nobody injected is counted, and recovered from like any other.
//
// Plain logic: it is fed what the console shows and says what to do, so it is tested with a
// fake clock. console_app --soak drives the real console with it.
class FaultCycle {
public:
    explicit FaultCycle(FaultCycleConfig config = {}) : config_(config) {}

    // Call often (every 20-50 ms). Returns at most one action to take now.
    Action step(const Observation& seen);

    [[nodiscard]] const FaultCycleCounts& counts() const noexcept { return counts_; }
    // A short description of the newest event worth printing (unexpected fault, stuck, ...), and
    // a counter that goes up each time there is a new one.
    [[nodiscard]] const std::string& lastEvent() const noexcept { return lastEvent_; }
    [[nodiscard]] std::uint64_t events() const noexcept { return events_; }
    // Where the cycle is, for the progress lines.
    [[nodiscard]] std::string_view phaseName() const noexcept;

private:
    enum class Phase : std::uint8_t {
        WaitEngageAllowed,
        WaitEngaged,
        Running,
        Detecting,
        Holding,
        Recovering
    };

    Action waitEngageAllowed(const Observation& seen);
    Action waitEngaged(const Observation& seen);
    Action running(const Observation& seen);
    Action detecting(const Observation& seen);
    Action holding(const Observation& seen);
    Action recovering(const Observation& seen);

    Action unexpectedFault(const Observation& seen);
    void enter(Phase phase, Nanoseconds now, Nanoseconds limit);
    void event(std::string text);
    [[nodiscard]] Injection current() const noexcept { return kInjections[next_ % kInjections.size()]; }

    FaultCycleConfig config_;
    FaultCycleCounts counts_;
    Phase phase_ = Phase::WaitEngageAllowed;
    Nanoseconds since_{0};     // when the phase began
    Nanoseconds deadline_{0};  // when the phase has taken too long
    bool started_ = false;
    bool resetTried_ = false;             // in Holding: the one reset that must be refused
    std::size_t next_ = 0;                // index of the next injection
    bool injectedBeforeRecovery_ = true;  // false while recovering from a fault nobody injected
    std::string lastEvent_;
    std::uint64_t events_ = 0;
};

}  // namespace msc::soak
