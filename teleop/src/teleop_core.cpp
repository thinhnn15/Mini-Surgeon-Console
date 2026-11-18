#include "msc/teleop/teleop_core.hpp"

#include "msc/common/fine_timer.hpp"

#include <chrono>
#include <optional>
#include <variant>

namespace msc::teleop {
namespace {

StateKind kindOf(const State& state) noexcept {
    if (std::holds_alternative<Engaged>(state)) {
        return StateKind::Engaged;
    }
    if (std::holds_alternative<Paused>(state)) {
        return StateKind::Paused;
    }
    if (std::holds_alternative<Fault>(state)) {
        return StateKind::Fault;
    }
    return StateKind::Idle;
}

}  // namespace

TeleopCore::TeleopCore(const Clock& clock, comms::ArmPort& arm, TeleopConfig config)
    : clock_(&clock), arm_(&arm), config_(config), machine_(config.scale) {}

TeleopCore::~TeleopCore() {
    stop();
}

void TeleopCore::request(Request request) noexcept {
    requests_.publish(
        RequestBox{.id = nextRequestId_.fetch_add(1, std::memory_order_relaxed), .request = request});
}

FaultSet TeleopCore::presentFaults(Nanoseconds now) const {
    FaultSet present;
    const StateKind kind = kindOf(machine_.state());
    const bool operating = kind == StateKind::Engaged || kind == StateKind::Paused;

    if (link_.link == comms::LinkState::Lost) {
        present |= faultSet(FaultCode::ArmLinkLost);
    }
    if (!lastHand_ || now - lastHand_->time > config_.handTimeout) {
        present |= faultSet(FaultCode::HandInputStale);
    }
    // Only while we are sending commands that should be acknowledged.
    if (operating && now - lastAckChange_ > config_.ackTimeout) {
        present |= faultSet(FaultCode::CommandNotAcknowledged);
    }
    // Engaged, we send moves: the arm must be following them. Holding for longer than it takes
    // the first move to arrive means its own watchdog (or something else) stopped it.
    if (kind == StateKind::Engaged && link_.telemetrySeen &&
        link_.telemetry.state == comms::ArmState::Holding && now - enteredEngaged_ > config_.holdingTimeout) {
        present |= faultSet(FaultCode::ArmHoldingOnItsOwn);
    }
    if (!videoLive_.load(std::memory_order_relaxed)) {
        present |= faultSet(FaultCode::VideoFrozen);
    }
    present |= FaultSet(injected_.load(std::memory_order_relaxed));
    return present;
}

// Not faults, but no reason to take over either: there is no live arm to take over yet.
FaultSet TeleopCore::notReadyToEngage() const {
    if (link_.link != comms::LinkState::Alive || !link_.telemetrySeen) {
        return faultSet(FaultCode::ArmLinkLost);
    }
    return {};
}

void TeleopCore::apply(const Event& event, Nanoseconds now) {
    const StateKind before = kindOf(machine_.state());
    const Result result = machine_.handle(event, now);
    if (result.outcome == Outcome::Refused) {
        lastRefusal_ = result.why;
    } else if (result.outcome == Outcome::Changed) {
        lastRefusal_ = {};
    }
    const StateKind after = kindOf(machine_.state());
    if (after == StateKind::Engaged && result.outcome == Outcome::Changed) {
        enteredEngaged_ = now;  // engage or clutch release: the arm gets a moment to follow
    }
    if (before == StateKind::Idle && after == StateKind::Engaged) {
        lastAckChange_ = now;  // acknowledgements are counted from the moment we start sending
    }
}

void TeleopCore::tick() {
    const Nanoseconds now = clock_->now();
    ++ticks_;

    // Inputs: whatever is newest in each mailbox.
    if (arm_->readStatus(link_) && link_.telemetrySeen && link_.telemetry.lastCommandSequence != lastAck_) {
        lastAck_ = link_.telemetry.lastCommandSequence;
        lastAckChange_ = now;
    }
    const std::optional<HandSample> previousHand = lastHand_;
    HandSample hand;
    if (hand_.read(hand)) {
        lastHand_ = hand;
    }

    // Watchdogs first: a fault outranks whatever the operator asked for in the same tick.
    const FaultSet present = presentFaults(now);
    const StateKind kind = kindOf(machine_.state());
    if ((kind == StateKind::Engaged || kind == StateKind::Paused) && present.any()) {
        apply(FaultDetected{.code = *firstFault(present)}, now);
    }

    // The clutch is a level on the hand; the state machine wants its edges.
    if (lastHand_ && previousHand && lastHand_->clutch != previousHand->clutch) {
        if (lastHand_->clutch) {
            apply(ClutchPressed{}, now);
        } else {
            apply(ClutchReleased{.hand = lastHand_->pose, .arm = link_.telemetry.position}, now);
        }
    }

    RequestBox box;
    if (requests_.read(box) && box.id != lastRequestId_) {
        lastRequestId_ = box.id;
        switch (box.request) {
            case Request::Engage:
                apply(Engage{.hand = lastHand_ ? lastHand_->pose : JointArray{},
                             .arm = link_.telemetry.position,
                             .present = present | notReadyToEngage()},
                      now);
                break;
            case Request::Disengage:
                apply(Disengage{}, now);
                break;
            case Request::Reset:
                apply(Reset{.present = present}, now);
                break;
            case Request::None:
                break;
        }
    }

    // Output: move while engaged, hold otherwise.
    const std::optional<JointArray> target =
        lastHand_ ? machine_.target(lastHand_->pose) : std::optional<JointArray>{};
    if (target) {
        arm_->move(*target);
    } else {
        arm_->hold();
    }

    TeleopStatus status;
    status.state = kindOf(machine_.state());
    if (const auto* fault = std::get_if<Fault>(&machine_.state())) {
        status.fault = fault->code;
        status.faultSince = fault->since;
    }
    const FaultSet afterPresent = presentFaults(now);
    status.present = afterPresent.to_ulong();
    status.engageAllowed = status.state == StateKind::Idle && (afterPresent | notReadyToEngage()).none();
    status.resetAllowed = status.state == StateKind::Fault && afterPresent.none();
    status.commandingMove = target.has_value();
    status.target = target.value_or(JointArray{});
    status.lastRefusal = lastRefusal_;
    status.link = link_;
    status.ticks = ticks_;
    status_.publish(status);
}

void TeleopCore::start() {
    if (thread_.joinable()) {
        return;
    }
    thread_ = std::jthread([this](const std::stop_token& stop) { run(stop); });
}

void TeleopCore::stop() {
    if (!thread_.joinable()) {
        return;
    }
    thread_.request_stop();
    thread_.join();
}

void TeleopCore::run(const std::stop_token& stop) {
    [[maybe_unused]] const FineTimerResolution fineTimer;
    using Steady = std::chrono::steady_clock;
    auto next = Steady::now();
    while (!stop.stop_requested()) {
        tick();
        next += config_.period;
        if (Steady::now() > next + config_.period) {
            next = Steady::now();  // fell behind (debugger, overload): no burst of ticks
        }
        sleepUntil(next);
    }
}

}  // namespace msc::teleop
