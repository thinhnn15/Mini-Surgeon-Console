#pragma once

#include "msc/common/clock.hpp"
#include "msc/common/latest_value.hpp"
#include "msc/comms/arm_link.hpp"
#include "msc/teleop/state_machine.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <stop_token>
#include <string_view>
#include <thread>

namespace msc::teleop {

// One reading of the hand controller.
struct HandSample {
    JointArray pose{};
    bool clutch = false;  // the clutch pedal / button is held
    Nanoseconds time{0};  // when the hand reported it, on the console's clock
};

// What the operator asks for with the console's buttons.
enum class Request : std::uint8_t {
    None,
    Engage,
    Disengage,
    Reset,
};

struct TeleopConfig {
    Nanoseconds period = std::chrono::milliseconds(5);  // 200 Hz
    // Watchdog limits. Each is a fault after this long; see the journal for why these values.
    Nanoseconds handTimeout = std::chrono::milliseconds(100);
    Nanoseconds ackTimeout = std::chrono::milliseconds(100);
    Nanoseconds holdingTimeout = std::chrono::milliseconds(100);
    double scale = 0.5;
};

enum class StateKind : std::uint8_t {
    Idle,
    Engaged,
    Paused,
    Fault,
};

// What the GUI shows. Trivially copyable: it crosses threads through a LatestValue.
struct TeleopStatus {
    StateKind state = StateKind::Idle;
    FaultCode fault = FaultCode::Injected;  // valid in Fault
    Nanoseconds faultSince{0};
    unsigned long present = 0;       // FaultSet bits: conditions present right now
    bool engageAllowed = false;      // an engage request would be accepted now
    bool resetAllowed = false;       // a reset request would be accepted now
    bool commandingMove = false;     // false: the arm is told to hold
    JointArray target{};             // the newest move target, when commandingMove
    std::string_view lastRefusal{};  // why the last request was refused (static text), or empty
    comms::ArmLinkStatus link{};     // forwarded: the core is the arm link's only reader
    std::uint64_t ticks = 0;
};

// The teleop core from the README: a fixed-rate thread that reads the hand controller, the arm
// link and the video state, runs the state machine, watches for faults and tells the arm what to
// do. It never waits for the GUI, the video or the network: everything comes in through
// lock-free mailboxes and goes out the same way (README: the control loop never waits).
//
// Watchdogs (a fault while Engaged or Paused; in Idle they only block engaging):
//   ArmLinkLost             the arm's heartbeats stopped (the arm holds on its own as well)
//   HandInputStale          no hand reading for handTimeout: the hand device froze or unplugged
//   CommandNotAcknowledged  the arm's telemetry has not confirmed a new command for ackTimeout
//   ArmHoldingOnItsOwn      engaged, but the arm reports Holding for longer than holdingTimeout
//   VideoFrozen             the surgeon cannot see what the arm does (REQ-VID-003)
//   Injected                the fault injector (M4.4)
//
// Output every tick: while Engaged a move to the scaled target, otherwise a hold. The arm is
// therefore never left going toward a target nobody controls any more (REQ-SAF-003 in Paused,
// REQ-SAF-002 in Fault).
//
// tick() does one period's work at the clock's time with no waiting: tests drive it with a
// ManualClock and a fake ArmPort. start() runs it on its own thread.
class TeleopCore {
public:
    TeleopCore(const Clock& clock, comms::ArmPort& arm, TeleopConfig config = {});
    ~TeleopCore();
    TeleopCore(const TeleopCore&) = delete;
    TeleopCore& operator=(const TeleopCore&) = delete;
    TeleopCore(TeleopCore&&) = delete;
    TeleopCore& operator=(TeleopCore&&) = delete;

    // Producers (each from one thread at a time).
    void submitHand(const HandSample& sample) noexcept { hand_.publish(sample); }
    void request(Request request) noexcept;
    void setVideoLive(bool live) noexcept { videoLive_.store(live, std::memory_order_relaxed); }
    void setInjected(const FaultSet& faults) noexcept {
        injected_.store(faults.to_ulong(), std::memory_order_relaxed);
    }

    // Consumer (one thread): the newest status, if there is a new one.
    [[nodiscard]] bool readStatus(TeleopStatus& status) noexcept { return status_.read(status); }

    void tick();
    void start();
    void stop();

    [[nodiscard]] const StateMachine& machine() const noexcept { return machine_; }

private:
    struct RequestBox {
        std::uint32_t id = 0;  // +1 per request, so the same request is never handled twice
        Request request = Request::None;
    };

    [[nodiscard]] FaultSet presentFaults(Nanoseconds now) const;
    [[nodiscard]] FaultSet notReadyToEngage() const;
    void apply(const Event& event, Nanoseconds now);
    void run(const std::stop_token& stop);

    const Clock* clock_;
    comms::ArmPort* arm_;
    TeleopConfig config_;

    LatestValue<HandSample> hand_;
    LatestValue<RequestBox> requests_;
    std::atomic<std::uint32_t> nextRequestId_{1};  // requester side only
    std::atomic<bool> videoLive_{true};
    std::atomic<unsigned long> injected_{0};
    LatestValue<TeleopStatus> status_;

    // Core thread only.
    StateMachine machine_;
    std::optional<HandSample> lastHand_;
    comms::ArmLinkStatus link_;
    std::uint32_t lastRequestId_ = 0;
    std::uint32_t lastAck_ = 0;
    Nanoseconds lastAckChange_{0};
    Nanoseconds enteredEngaged_{0};
    std::string_view lastRefusal_;
    std::uint64_t ticks_ = 0;

    std::jthread thread_;  // last: joined before the members it uses go away
};

}  // namespace msc::teleop
