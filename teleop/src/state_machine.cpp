#include "msc/teleop/state_machine.hpp"

#include <cstddef>
#include <optional>
#include <utility>
#include <variant>

namespace msc::teleop {
namespace {

// What a transition decided: the next state (nothing: stay) and how to report it.
struct Step {
    std::optional<State> next;
    Result result;
};

Step stay() {
    return Step{.next = std::nullopt, .result = Result{.outcome = Outcome::Unchanged, .why = {}}};
}
Step refuse(std::string_view why) {
    return Step{.next = std::nullopt, .result = Result{.outcome = Outcome::Refused, .why = why}};
}
Step go(State next) {
    return Step{.next = next, .result = Result{.outcome = Outcome::Changed, .why = {}}};
}

// Every (state, event) pair, decided on purpose. std::visit picks the most specific overload;
// the order of the groups below is only for reading. The two generic overloads at the end are
// explicit decisions too: a fault wins in every state, and anything not listed is ignored.
struct Transitions {
    Nanoseconds now;

    // -- Idle: waiting for the operator to take over.
    Step operator()(const Idle& /*state*/, const Engage& e) const {
        if (e.present.any()) {
            return refuse("a fault condition is present");
        }
        return go(Engaged{.handAnchor = e.hand, .armAnchor = e.arm});
    }

    // -- Engaged: the hand drives the arm.
    Step operator()(const Engaged& /*state*/, const Disengage& /*event*/) const { return go(Idle{}); }
    Step operator()(const Engaged& /*state*/, const ClutchPressed& /*event*/) const { return go(Paused{}); }

    // -- Paused: clutch held, the arm holds whatever the hand does (REQ-SAF-003).
    Step operator()(const Paused& /*state*/, const Disengage& /*event*/) const { return go(Idle{}); }
    Step operator()(const Paused& /*state*/, const ClutchReleased& e) const {
        // Re-index: motion continues from where the hand and the arm are now, so moving the
        // hand while clutched is never replayed on the arm.
        return go(Engaged{.handAnchor = e.hand, .armAnchor = e.arm});
    }

    // -- Fault: latched (REQ-SAF-002).
    Step operator()(const Fault& /*state*/, const Reset& e) const {
        if (e.present.any()) {
            return refuse("the cause of the fault is still present");
        }
        // To Idle, not back to Engaged: after a fault the operator takes over again on purpose.
        return go(Idle{});
    }
    Step operator()(const Fault& /*state*/, const FaultDetected& /*event*/) const {
        return stay();  // keep the first fault: it is the cause, later ones are usually effects
    }
    Step operator()(const Fault& /*state*/, const Engage& /*event*/) const {
        return refuse("in fault: reset first");
    }
    Step operator()(const Fault& /*state*/, const ClutchReleased& /*event*/) const { return stay(); }

    // -- Any other state: a detected fault always wins.
    template <typename S>
    Step operator()(const S& /*state*/, const FaultDetected& e) const {
        return go(Fault{.code = e.code, .since = now});
    }

    // -- Everything else means nothing in that state: engage while engaged, clutch while idle,
    //    reset with nothing to reset, disengage in fault (only a reset leaves Fault), ...
    template <typename S, typename E>
    Step operator()(const S& /*state*/, const E& /*event*/) const {
        return stay();
    }
};

}  // namespace

Result StateMachine::handle(const Event& event, Nanoseconds now) {
    Step step = std::visit(Transitions{.now = now}, state_, event);
    if (step.next) {
        state_ = *step.next;
    }
    return step.result;
}

std::optional<JointArray> StateMachine::target(const JointArray& hand) const noexcept {
    const auto* engaged = std::get_if<Engaged>(&state_);
    if (engaged == nullptr) {
        return std::nullopt;
    }
    JointArray target{};
    for (std::size_t i = 0; i < target.size(); ++i) {
        target[i] = static_cast<float>(static_cast<double>(engaged->armAnchor[i]) +
                                       (scale_ * static_cast<double>(hand[i] - engaged->handAnchor[i])));
    }
    return target;
}

std::string_view toString(FaultCode code) noexcept {
    switch (code) {
        case FaultCode::ArmLinkLost:
            return "arm link lost";
        case FaultCode::HandInputStale:
            return "hand controller not responding";
        case FaultCode::CommandNotAcknowledged:
            return "arm not confirming commands";
        case FaultCode::ArmHoldingOnItsOwn:
            return "arm stopped on its own";
        case FaultCode::VideoFrozen:
            return "video frozen";
        case FaultCode::Injected:
            return "injected fault";
    }
    return "unknown fault";
}

std::optional<FaultCode> firstFault(const FaultSet& set) noexcept {
    for (std::size_t i = 0; i < set.size(); ++i) {
        if (set.test(i)) {
            return static_cast<FaultCode>(i);
        }
    }
    return std::nullopt;
}

std::string_view stateName(const State& state) noexcept {
    // By type, not by index: reordering the variant must not rename the states.
    if (std::holds_alternative<Idle>(state)) {
        return "idle";
    }
    if (std::holds_alternative<Engaged>(state)) {
        return "engaged";
    }
    if (std::holds_alternative<Paused>(state)) {
        return "paused";
    }
    return "fault";
}

}  // namespace msc::teleop
