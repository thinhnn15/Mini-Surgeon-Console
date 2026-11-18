#include "msc/arm/arm_controller.hpp"

#include "msc/common/fine_timer.hpp"
#include "msc/comms/session.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <optional>
#include <utility>
#include <variant>

namespace msc::arm {
namespace {

using namespace std::chrono_literals;

// The longest step the model is given after a stall (debugger, overloaded machine): a joint
// moves at most this long at its current speed before the next tick sees it. Without the cap a
// 2 s stall would be integrated as one 2 s step.
constexpr int kMaxStepPeriods = 10;

}  // namespace

ArmController::ArmController(const Clock& clock, comms::UdpSocket socket, ArmControllerConfig config,
                             const ArmModel& model)
    : clock_(&clock),
      socket_(std::move(socket)),
      config_(config),
      model_(model),
      session_(comms::newSessionId()) {}

void ArmController::tick() {
    const Nanoseconds now = clock_->now();
    receiveAll();
    const Nanoseconds dt =
        lastTick_ ? std::clamp(now - *lastTick_, Nanoseconds{0}, kMaxStepPeriods * config_.period)
                  : Nanoseconds{0};
    lastTick_ = now;
    model_.step(dt);
    sendTelemetry(now);
    ++stats_.ticks;
}

void ArmController::receiveAll() {
    std::array<std::byte, comms::kMaxPacketSize> buffer{};
    std::optional<comms::JointCommand> newest;
    for (;;) {
        const comms::ReceiveResult r = socket_.receive(buffer, 0ms);
        if (r.status != comms::ReceiveStatus::Datagram) {
            break;  // nothing more waiting (an error is treated the same: try again next tick)
        }
        if (r.truncated) {
            ++stats_.refused[static_cast<std::size_t>(comms::DecodeError::BadLength)];
            continue;
        }
        const comms::DecodeResult decoded = comms::decode(std::span(buffer).first(r.size));
        if (const auto* error = std::get_if<comms::DecodeError>(&decoded)) {
            ++stats_.refused[static_cast<std::size_t>(*error)];
            continue;
        }
        const auto& packet = std::get<comms::Packet>(decoded);
        const auto* command = std::get_if<comms::JointCommand>(&packet.message);
        if (command == nullptr) {
            continue;  // heartbeats: M3.4
        }
        if (commands_.check(packet.session, packet.sequence) != comms::SequenceVerdict::Accepted) {
            ++stats_.oldOrDuplicate;
            continue;
        }
        if (newest) {
            ++stats_.commandsReplaced;
        }
        newest = *command;
        lastCommandSequence_ = packet.sequence;
        replyTo_ = r.from;
    }
    if (newest) {
        model_.setTarget(newest->target);
        state_ = comms::ArmState::Following;
        ++stats_.commandsApplied;
    }
}

void ArmController::sendTelemetry(Nanoseconds now) {
    const comms::Endpoint to =
        config_.console.port != 0 ? config_.console : replyTo_.value_or(comms::Endpoint{});
    if (to.port == 0) {
        return;  // nobody to tell yet
    }
    const comms::Packet packet{.session = session_,
                               .sequence = nextSequence_++,
                               .timestamp = now,
                               .message = comms::JointTelemetry{.position = model_.position(),
                                                                .velocity = model_.velocity(),
                                                                .state = state_,
                                                                .lastCommandSequence = lastCommandSequence_}};
    std::array<std::byte, comms::kMaxPacketSize> buffer{};
    const std::size_t size = comms::encode(packet, buffer);
    if (socket_.sendTo(std::span(buffer).first(size), to)) {
        ++stats_.telemetrySent;
    } else {
        ++stats_.sendFailures;
    }
}

void ArmController::run(const std::atomic<bool>& stop, std::optional<Nanoseconds> duration) {
    [[maybe_unused]] const FineTimerResolution fineTimer;
    using Steady = std::chrono::steady_clock;
    const auto start = Steady::now();
    auto next = start;
    while (!stop.load(std::memory_order_relaxed)) {
        if (duration && Steady::now() - start >= *duration) {
            break;
        }
        sleepUntil(next);
        const auto woke = Steady::now();
        const Nanoseconds late =
            std::max(Nanoseconds{0}, std::chrono::duration_cast<Nanoseconds>(woke - next));
        lateness_.add(late);
        maxLateness_ = std::max(maxLateness_, late);

        tick();

        next += config_.period;
        if (Steady::now() > next + config_.period) {
            ++stats_.overruns;
            next = Steady::now();
        }
    }
}

}  // namespace msc::arm
