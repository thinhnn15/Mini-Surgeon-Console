#include "msc/comms/arm_link.hpp"

#include "msc/common/fine_timer.hpp"
#include "msc/comms/session.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <utility>
#include <variant>

namespace msc::comms {

ArmLink::ArmLink(const Clock& clock, UdpSocket socket, ArmLinkConfig config)
    : clock_(&clock),
      socket_(std::move(socket)),
      config_(config),
      heartbeat_(config.heartbeatPeriod, config.missedHeartbeats),
      session_(newSessionId()) {}

ArmLink::~ArmLink() {
    stop();
}

void ArmLink::start() {
    if (thread_.joinable()) {
        return;
    }
    thread_ = std::jthread([this](const std::stop_token& stop) { run(stop); });
}

void ArmLink::stop() {
    if (!thread_.joinable()) {
        return;
    }
    thread_.request_stop();
    thread_.join();
}

void ArmLink::move(const JointArray& target) noexcept {
    commandIn_.publish(Command{.kind = Command::Kind::Move, .target = target});
}

void ArmLink::hold() noexcept {
    commandIn_.publish(Command{.kind = Command::Kind::Hold, .target = {}});
}

void ArmLink::run(const std::stop_token& stop) {
    // The waits below are socket timeouts, which follow the OS timer tick: 15.6 ms on Windows by
    // default, enough to miss the arm's 30 ms limit with two late heartbeats in a row. Measured
    // gaps were at most 11 ms, but only because arm_sim on the same machine had raised the
    // resolution already; the console must not depend on that.
    [[maybe_unused]] const FineTimerResolution fineTimer;
    Nanoseconds next = clock_->now();
    while (!stop.stop_requested()) {
        const Nanoseconds now = clock_->now();
        if (now >= next) {
            static_cast<void>(commandIn_.read(command_));
            send(now, Heartbeat{});
            if (command_.kind == Command::Kind::Move) {
                current_.lastCommandSent = nextSequence_;
                send(now, JointCommand{.target = command_.target});
            } else if (command_.kind == Command::Kind::Hold) {
                current_.lastCommandSent = nextSequence_;
                send(now, HoldCommand{});
            }
            current_.link = heartbeat_.state(now);
            current_.silence = heartbeat_.silence(now).value_or(Nanoseconds{0});
            status_.publish(current_);
            next += config_.heartbeatPeriod;
            if (next < now) {
                next = now + config_.heartbeatPeriod;  // fell behind: no burst of heartbeats
            }
        }
        // Wait for the arm until the next send is due. Socket timeouts are whole milliseconds;
        // rounding down means waking a little early and waiting again, never late.
        const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(next - clock_->now());
        receive(std::max(wait, std::chrono::milliseconds{0}));
    }
}

void ArmLink::receive(std::chrono::milliseconds timeout) {
    std::array<std::byte, kMaxPacketSize> buffer{};
    ReceiveResult r = socket_.receive(buffer, timeout);
    // After the first datagram, take whatever else is already waiting, without waiting again.
    while (r.status == ReceiveStatus::Datagram) {
        if (fault_.load(std::memory_order_relaxed) != LinkFault::Drop) {
            handle(r, std::span(buffer).first(r.size));
        }
        r = socket_.receive(buffer, std::chrono::milliseconds{0});
    }
}

void ArmLink::handle(const ReceiveResult& received, std::span<const std::byte> bytes) {
    if (received.truncated) {
        ++current_.refused;
        return;
    }
    const DecodeResult decoded = decode(bytes);
    if (std::holds_alternative<DecodeError>(decoded)) {
        ++current_.refused;
        return;
    }
    const auto& packet = std::get<Packet>(decoded);
    if (std::holds_alternative<JointCommand>(packet.message) ||
        std::holds_alternative<HoldCommand>(packet.message)) {
        return;  // not for a console
    }
    if (fromArm_.check(packet.session, packet.sequence) != SequenceVerdict::Accepted) {
        ++current_.oldOrDuplicate;
        return;
    }
    current_.lost = fromArm_.stats().lost;
    if (std::holds_alternative<Heartbeat>(packet.message)) {
        heartbeat_.received(clock_->now());  // arrival on our clock, not the arm's timestamp
        ++current_.heartbeatsReceived;
    } else if (const auto* telemetry = std::get_if<JointTelemetry>(&packet.message)) {
        current_.telemetry = *telemetry;
        current_.telemetrySeen = true;
        ++current_.telemetryReceived;
    }
}

void ArmLink::send(Nanoseconds now, const Message& message) {
    const Packet packet{
        .session = session_, .sequence = nextSequence_++, .timestamp = now, .message = message};
    std::array<std::byte, kMaxPacketSize> buffer{};
    const std::size_t size = encode(packet, buffer);
    // A failed send is not retried: the next period sends newer data anyway, and if sends keep
    // failing the arm stops hearing heartbeats and holds.
    const LinkFault fault = fault_.load(std::memory_order_relaxed);
    if (fault == LinkFault::Drop) {
        return;
    }
    if (fault == LinkFault::Corrupt) {
        buffer[kHeaderSize / 2] ^= std::byte{0x10};  // one bit in the header: only the CRC can tell
    }
    static_cast<void>(socket_.sendTo(std::span(buffer).first(size), config_.arm));
}

}  // namespace msc::comms
