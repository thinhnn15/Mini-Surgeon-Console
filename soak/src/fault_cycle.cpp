#include "msc/soak/fault_cycle.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace msc::soak {

namespace {

using teleop::FaultCode;
using teleop::StateKind;

constexpr std::array kLinkFaults{FaultCode::ArmLinkLost};
// The arm refuses every corrupted command: the console either sees the arm stop on its own (its
// heartbeat watchdog) or its commands go unacknowledged, whichever watchdog fires first.
constexpr std::array kCorruptFaults{FaultCode::ArmHoldingOnItsOwn, FaultCode::CommandNotAcknowledged};
constexpr std::array kHandFaults{FaultCode::HandInputStale};
constexpr std::array kVideoFaults{FaultCode::VideoFrozen};
constexpr std::array kGenericFaults{FaultCode::Injected};

std::string_view stateName(StateKind state) {
    switch (state) {
        case StateKind::Idle:
            return "idle";
        case StateKind::Engaged:
            return "engaged";
        case StateKind::Paused:
            return "paused";
        case StateKind::Fault:
            return "fault";
    }
    return "?";
}

double seconds(Nanoseconds t) {
    return std::chrono::duration<double>(t).count();
}

}  // namespace

std::string_view toString(Injection injection) noexcept {
    switch (injection) {
        case Injection::Link:
            return "link";
        case Injection::Corrupt:
            return "corrupt";
        case Injection::Hand:
            return "hand";
        case Injection::Video:
            return "video";
        case Injection::Generic:
            return "fault";
    }
    return "?";
}

std::span<const FaultCode> expectedFaults(Injection injection) noexcept {
    switch (injection) {
        case Injection::Link:
            return kLinkFaults;
        case Injection::Corrupt:
            return kCorruptFaults;
        case Injection::Hand:
            return kHandFaults;
        case Injection::Video:
            return kVideoFaults;
        case Injection::Generic:
            return kGenericFaults;
    }
    return {};
}

std::string_view FaultCycle::phaseName() const noexcept {
    switch (phase_) {
        case Phase::WaitEngageAllowed:
            return "waiting to engage";
        case Phase::WaitEngaged:
            return "engaging";
        case Phase::Running:
            return "engaged";
        case Phase::Detecting:
            return "injected";
        case Phase::Holding:
            return "faulted";
        case Phase::Recovering:
            return "recovering";
    }
    return "?";
}

Action FaultCycle::step(const Observation& seen) {
    if (!started_) {
        started_ = true;
        enter(Phase::WaitEngageAllowed, seen.now, config_.engageWithin);
    }
    switch (phase_) {
        case Phase::WaitEngageAllowed:
            return waitEngageAllowed(seen);
        case Phase::WaitEngaged:
            return waitEngaged(seen);
        case Phase::Running:
            return running(seen);
        case Phase::Detecting:
            return detecting(seen);
        case Phase::Holding:
            return holding(seen);
        case Phase::Recovering:
            return recovering(seen);
    }
    return {};
}

Action FaultCycle::waitEngageAllowed(const Observation& seen) {
    if (seen.state == StateKind::Fault) {
        return unexpectedFault(seen);
    }
    if (seen.state == StateKind::Engaged) {  // engaged already (the scripted hand engages once by itself)
        enter(Phase::Running, seen.now, config_.engagedFor);
        return {};
    }
    if (seen.engageAllowed) {
        enter(Phase::WaitEngaged, seen.now, config_.engageWithin);
        return {.kind = Action::Kind::Engage};
    }
    if (seen.now >= deadline_) {
        ++counts_.stuck;
        event(std::format("stuck: engaging not allowed for {:.1f} s", seconds(seen.now - since_)));
        enter(Phase::WaitEngageAllowed, seen.now, config_.engageWithin);
    }
    return {};
}

Action FaultCycle::waitEngaged(const Observation& seen) {
    if (seen.state == StateKind::Fault) {
        return unexpectedFault(seen);
    }
    if (seen.state == StateKind::Engaged) {
        enter(Phase::Running, seen.now, config_.engagedFor);
        return {};
    }
    if (seen.now >= deadline_) {
        ++counts_.stuck;
        event(std::format("stuck: engage requested {:.1f} s ago, still not engaged",
                          seconds(seen.now - since_)));
        enter(Phase::WaitEngageAllowed, seen.now, config_.engageWithin);
    }
    return {};
}

Action FaultCycle::running(const Observation& seen) {
    if (seen.state == StateKind::Fault) {
        return unexpectedFault(seen);
    }
    if (seen.state != StateKind::Engaged) {  // disengaged by someone else: engage again
        enter(Phase::WaitEngageAllowed, seen.now, config_.engageWithin);
        return {};
    }
    if (seen.now >= deadline_) {
        ++counts_.injected;
        enter(Phase::Detecting, seen.now, config_.detectWithin);
        return {.kind = Action::Kind::Inject, .injection = current()};
    }
    return {};
}

Action FaultCycle::detecting(const Observation& seen) {
    if (seen.state == StateKind::Fault) {
        const auto expected = expectedFaults(current());
        if (std::ranges::find(expected, seen.fault) != expected.end()) {
            ++counts_.detected;
            counts_.detectionTimes.push_back(seen.now - since_);
        } else {
            ++counts_.wrongFault;
            event(std::format("injected {}, but the fault was \"{}\"", toString(current()),
                              toString(seen.fault)));
        }
        enter(Phase::Holding, seen.now, config_.faultHeld);
        resetTried_ = true;
        return {.kind = Action::Kind::Reset};  // must be refused: the cause is still there
    }
    if (seen.now >= deadline_) {
        ++counts_.missed;
        event(std::format("injected {}, no fault after {:.1f} s", toString(current()),
                          seconds(seen.now - since_)));
        // Nothing to recover from: clear it and go on with the next fault.
        const Injection missed = current();
        ++next_;
        enter(Phase::WaitEngageAllowed, seen.now, config_.engageWithin);
        return {.kind = Action::Kind::Clear, .injection = missed};
    }
    return {};
}

Action FaultCycle::holding(const Observation& seen) {
    if (seen.state != StateKind::Fault && resetTried_) {
        resetTried_ = false;
        ++counts_.earlyReset;
        event(std::format("a reset was accepted while {} was still injected", toString(current())));
    }
    if (seen.now >= deadline_) {
        enter(Phase::Recovering, seen.now, config_.recoverWithin);
        return {.kind = Action::Kind::Clear, .injection = current()};
    }
    return {};
}

Action FaultCycle::recovering(const Observation& seen) {
    if (seen.state == StateKind::Idle) {
        ++counts_.recovered;
        if (injectedBeforeRecovery_) {
            ++next_;
        }
        injectedBeforeRecovery_ = true;
        enter(Phase::WaitEngageAllowed, seen.now, config_.engageWithin);
        return waitEngageAllowed(seen);
    }
    if (seen.now >= deadline_) {
        ++counts_.stuck;
        event(std::format("stuck: not back to Idle {:.1f} s after the cause was cleared (state {}, reset {})",
                          seconds(seen.now - since_), stateName(seen.state),
                          seen.resetAllowed ? "allowed" : "refused"));
        enter(Phase::Recovering, seen.now, config_.recoverWithin);
    }
    if (seen.state == StateKind::Fault && seen.resetAllowed) {
        return {.kind = Action::Kind::Reset};
    }
    if (seen.state == StateKind::Engaged || seen.state == StateKind::Paused) {
        // Never expected (a reset leads to Idle), but if it happens, do not leave the arm moving.
        enter(Phase::Running, seen.now, config_.engagedFor);
    }
    return {};
}

Action FaultCycle::unexpectedFault(const Observation& seen) {
    ++counts_.unexpected;
    ++counts_.unexpectedByCode.at(static_cast<std::size_t>(seen.fault));
    event(std::format("unexpected fault while {}: \"{}\"", phaseName(), toString(seen.fault)));
    injectedBeforeRecovery_ = false;  // nothing was injected: the next one is still due
    enter(Phase::Recovering, seen.now, config_.recoverWithin);
    return {};
}

void FaultCycle::enter(Phase phase, Nanoseconds now, Nanoseconds limit) {
    phase_ = phase;
    since_ = now;
    deadline_ = now + limit;
}

void FaultCycle::event(std::string text) {
    lastEvent_ = std::move(text);
    ++events_;
}

}  // namespace msc::soak
