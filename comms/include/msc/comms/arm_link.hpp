#pragma once

#include "msc/common/clock.hpp"
#include "msc/common/latest_value.hpp"
#include "msc/comms/heartbeat_monitor.hpp"
#include "msc/comms/protocol.hpp"
#include "msc/comms/sequence_filter.hpp"
#include "msc/comms/udp_socket.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stop_token>
#include <thread>

namespace msc::comms {

struct ArmLinkConfig {
    Endpoint arm = Endpoint::loopback(47001);
    // Our heartbeats (and commands) go out every period; the arm's must come in at the same
    // rate, or the link counts as lost after `missedHeartbeats` periods (as on the arm's side).
    Nanoseconds heartbeatPeriod = std::chrono::milliseconds(10);
    unsigned missedHeartbeats = 3;
};

// What the console knows about the arm. Trivially copyable: it crosses threads through a
// LatestValue.
struct ArmLinkStatus {
    LinkState link = LinkState::Waiting;
    bool telemetrySeen = false;
    JointTelemetry telemetry{};  // the newest one
    Nanoseconds silence{0};      // since the arm's last heartbeat
    std::uint64_t heartbeatsReceived = 0;
    std::uint64_t telemetryReceived = 0;
    std::uint64_t refused = 0;  // malformed or corrupted datagrams
    std::uint64_t oldOrDuplicate = 0;
    std::uint64_t lost = 0;
    std::uint32_t lastCommandSent = 0;  // sequence of our newest move or hold (0: none yet)
};

// What the teleop core needs from the arm link: an interface, so the core is tested against a
// fake arm with a fake clock, and runs against ArmLink for real.
class ArmPort {
public:
    ArmPort() = default;
    ArmPort(const ArmPort&) = delete;
    ArmPort& operator=(const ArmPort&) = delete;
    ArmPort(ArmPort&&) = delete;
    ArmPort& operator=(ArmPort&&) = delete;
    virtual ~ArmPort() = default;

    // From now on, command the arm to this target (sent every period).
    virtual void move(const JointArray& target) noexcept = 0;
    // From now on, command the arm to hold (sent every period).
    virtual void hold() noexcept = 0;
    // The newest link status, if there is a new one. One reader.
    [[nodiscard]] virtual bool readStatus(ArmLinkStatus& status) noexcept = 0;
};

// The console's end of the arm link: a thread of its own (the comms thread in the README),
// never the GUI thread, so a busy or stalled UI cannot stop the heartbeats. If it did, the arm
// would hold, which is safe, but it would hold for no reason.
//
// Every heartbeat period it sends a heartbeat and the newest command: a JointCommand after
// move(), a HoldCommand after hold(), nothing before either; in between it waits on the socket
// for the arm's telemetry and heartbeats. Everything it sends is one stream (one session, one sequence
// counter), which is what the arm's SequenceFilter expects.
//
// Threads: construct, start(), stop() on one thread; move() / hold() from one producer thread
// (the teleop core) and readStatus() from one consumer thread (the teleop core too): both go
// through lock-free LatestValue mailboxes.
class ArmLink final : public ArmPort {
public:
    ArmLink(const Clock& clock, UdpSocket socket, ArmLinkConfig config = {});
    ~ArmLink() override;
    ArmLink(const ArmLink&) = delete;
    ArmLink& operator=(const ArmLink&) = delete;
    ArmLink(ArmLink&&) = delete;
    ArmLink& operator=(ArmLink&&) = delete;

    void start();
    // Returns once the thread has finished. The arm notices the silence and holds.
    void stop();

    void move(const JointArray& target) noexcept override;
    void hold() noexcept override;
    [[nodiscard]] bool readStatus(ArmLinkStatus& status) noexcept override { return status_.read(status); }

    [[nodiscard]] const Endpoint& localEndpoint() const noexcept { return socket_.localEndpoint(); }

private:
    struct Command {
        enum class Kind : std::uint8_t { None, Move, Hold };
        Kind kind = Kind::None;
        JointArray target{};
    };

    void run(const std::stop_token& stop);
    void receive(std::chrono::milliseconds timeout);
    void handle(const ReceiveResult& received, std::span<const std::byte> bytes);
    void send(Nanoseconds now, const Message& message);

    const Clock* clock_;
    UdpSocket socket_;
    ArmLinkConfig config_;

    LatestValue<Command> commandIn_;     // producer -> link thread
    LatestValue<ArmLinkStatus> status_;  // link thread -> consumer

    // Link thread only.
    Command command_;
    ArmLinkStatus current_;
    HeartbeatMonitor heartbeat_;
    SequenceFilter fromArm_;
    std::uint32_t session_;
    std::uint32_t nextSequence_ = 1;

    std::jthread thread_;  // last: stopped and joined before the members it uses go away
};

}  // namespace msc::comms
