#pragma once

#include "msc/arm/arm_model.hpp"
#include "msc/common/clock.hpp"
#include "msc/common/latency_stats.hpp"
#include "msc/comms/heartbeat_monitor.hpp"
#include "msc/comms/protocol.hpp"
#include "msc/comms/sequence_filter.hpp"
#include "msc/comms/udp_socket.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>

namespace msc::arm {

struct ArmControllerConfig {
    // Where telemetry and heartbeats go. Port 0: back to whoever sent the newest accepted packet.
    comms::Endpoint console{{127, 0, 0, 1}, 0};
    Nanoseconds period = std::chrono::milliseconds(1);  // 1 kHz
    // The console's heartbeat period and how many may be missed before the arm holds
    // (REQ-SAF-001: 3). The arm sends its own heartbeats at the same period.
    Nanoseconds heartbeatPeriod = std::chrono::milliseconds(10);
    unsigned missedHeartbeats = 3;
    // Told about state changes ("holding: ..."), for the operator's log. Called on the loop's
    // thread; may be empty.
    std::function<void(std::string_view)> log{};
};

struct ArmControllerStats {
    std::uint64_t ticks = 0;
    std::uint64_t overruns = 0;  // ticks more than a whole period late: skipped, not caught up
    std::uint64_t commandsApplied = 0;
    std::uint64_t commandsReplaced = 0;      // accepted, but a newer one came in the same tick
    std::uint64_t commandsIgnored = 0;       // intact and new, but the console was not alive
    std::array<std::uint64_t, 8> refused{};  // by comms::DecodeError
    std::uint64_t oldOrDuplicate = 0;        // refused by the SequenceFilter
    std::uint64_t heartbeatsReceived = 0;
    // The longest wait between two console heartbeats that still counted as alive: how close the
    // link came to a false hold.
    Nanoseconds longestHeartbeatGap{0};
    std::uint64_t heartbeatsSent = 0;
    std::uint64_t holds = 0;  // times the console went silent while the arm was following
    std::uint64_t telemetrySent = 0;
    std::uint64_t sendFailures = 0;
};

// The arm's controller: a fixed-rate loop around ArmModel and the UDP link.
//
// One tick: take every datagram waiting on the socket, keep the newest valid command (older
// ones are dropped, never queued: a backlog of commands is a backlog of old positions), check
// the console's heartbeat, step the model by the time since the last tick, and send telemetry
// (every tick) and a heartbeat (every heartbeat period).
//
// REQ-SAF-001, the watchdog: lives here, in the arm, not in the console, because it has to work
// exactly when the console does not. When the console's heartbeats stop for more than
// `missedHeartbeats` periods while the arm is following, the arm holds: every joint brakes at
// its limit and stays where it stopped. Not an instant stop (that would mean infinite
// deceleration, which the joints cannot do and which jerks whatever the arm holds), and not a
// move "home" (moving on its own, with nobody watching, is the danger being avoided). Commands
// are ignored while the console is not alive. When heartbeats come back the arm still holds
// until a new command arrives: it never resumes toward a target from before the silence.
//
// Why a fixed rate: the motion is integrated in steps, and steps of a known, even size keep it
// predictable. Running "when a command arrives" would make the arm's motion depend on the
// network's timing; "as fast as possible" would burn a core and still not be even.
//
// tick() does one period's work at the clock's current time, with no waiting, so tests drive
// it with a ManualClock. run() paces tick() with real time.
class ArmController {
public:
    ArmController(const Clock& clock, comms::UdpSocket socket, ArmControllerConfig config = {},
                  const ArmModel& model = ArmModel{});

    void tick();

    // Calls tick() every period until `stop` is set or `duration` (if given) has passed.
    // Deadline pacing (next += period); a tick more than one period late is counted as an
    // overrun and the schedule restarts from now, instead of a burst of ticks to catch up.
    void run(const std::atomic<bool>& stop, std::optional<Nanoseconds> duration = std::nullopt);

    [[nodiscard]] const ArmModel& model() const noexcept { return model_; }
    [[nodiscard]] comms::ArmState state() const noexcept { return state_; }
    [[nodiscard]] comms::LinkState consoleLink() const noexcept { return link_; }
    [[nodiscard]] Nanoseconds heartbeatLimit() const noexcept { return heartbeat_.limit(); }
    [[nodiscard]] const ArmControllerStats& stats() const noexcept { return stats_; }
    [[nodiscard]] const comms::SequenceStats& consoleStats() const noexcept { return console_.stats(); }
    [[nodiscard]] const comms::Endpoint& localEndpoint() const noexcept { return socket_.localEndpoint(); }

    // Pacing quality over the last few seconds of run(): how late each tick started.
    [[nodiscard]] const LatencyStats& lateness() const noexcept { return lateness_; }
    [[nodiscard]] Nanoseconds maxLateness() const noexcept { return maxLateness_; }

private:
    void receiveAll(Nanoseconds now);
    void updateState(Nanoseconds now, const std::optional<comms::JointCommand>& command);
    void send(Nanoseconds now, const comms::Message& message, std::uint64_t& counter);
    void log(std::string_view message) const;

    const Clock* clock_;
    comms::UdpSocket socket_;
    ArmControllerConfig config_;
    ArmModel model_;
    comms::ArmState state_ = comms::ArmState::Idle;
    comms::LinkState link_ = comms::LinkState::Waiting;

    // Everything the console sends (heartbeats and commands) is one stream with one sequence.
    comms::SequenceFilter console_;
    comms::HeartbeatMonitor heartbeat_;
    std::uint32_t lastCommandSequence_ = 0;
    std::optional<comms::Endpoint> replyTo_;

    std::uint32_t session_;
    std::uint32_t nextSequence_ = 1;
    std::optional<Nanoseconds> lastTick_;
    std::optional<Nanoseconds> nextHeartbeat_;

    ArmControllerStats stats_;
    LatencyStats lateness_{5000};  // 5 s at 1 kHz
    Nanoseconds maxLateness_{0};
};

}  // namespace msc::arm
