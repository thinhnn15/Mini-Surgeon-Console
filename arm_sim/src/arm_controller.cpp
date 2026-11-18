#include "msc/arm/arm_controller.hpp"

#include "msc/common/fine_timer.hpp"
#include "msc/comms/session.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace msc::arm {
namespace {

using namespace std::chrono_literals;

// The longest step the model is given after a stall (debugger, overloaded machine): a joint
// moves at most this long at its current speed before the next tick sees it. Without the cap a
// 2 s stall would be integrated as one 2 s step.
constexpr int kMaxStepPeriods = 10;

double ms(Nanoseconds t) {
    return std::chrono::duration<double, std::milli>(t).count();
}

}  // namespace

ArmController::ArmController(const Clock& clock, comms::UdpSocket socket, ArmControllerConfig config,
                             const ArmModel& model)
    : clock_(&clock),
      socket_(std::move(socket)),
      config_(std::move(config)),
      model_(model),
      heartbeat_(config_.heartbeatPeriod, config_.missedHeartbeats),
      session_(comms::newSessionId()) {}

void ArmController::tick() {
    const Nanoseconds now = clock_->now();
    receiveAll(now);
    const Nanoseconds dt =
        lastTick_ ? std::clamp(now - *lastTick_, Nanoseconds{0}, kMaxStepPeriods * config_.period)
                  : Nanoseconds{0};
    lastTick_ = now;
    model_.step(dt);

    send(now,
         comms::JointTelemetry{.position = model_.position(),
                               .velocity = model_.velocity(),
                               .state = state_,
                               .lastCommandSequence = lastCommandSequence_},
         stats_.telemetrySent);
    if (!nextHeartbeat_ || now >= *nextHeartbeat_) {
        send(now, comms::Heartbeat{}, stats_.heartbeatsSent);
        nextHeartbeat_ = now + config_.heartbeatPeriod;
    }
    ++stats_.ticks;
}

void ArmController::receiveAll(Nanoseconds now) {
    std::array<std::byte, comms::kMaxPacketSize> buffer{};
    std::optional<Command> newest;
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
        if (std::holds_alternative<comms::JointTelemetry>(packet.message)) {
            continue;  // not for an arm
        }
        if (console_.check(packet.session, packet.sequence) != comms::SequenceVerdict::Accepted) {
            ++stats_.oldOrDuplicate;
            continue;
        }
        replyTo_ = r.from;
        if (std::holds_alternative<comms::Heartbeat>(packet.message)) {
            if (const auto gap = heartbeat_.silence(now); gap && *gap <= heartbeat_.limit()) {
                stats_.longestHeartbeatGap = std::max(stats_.longestHeartbeatGap, *gap);
            }
            heartbeat_.received(now);
            ++stats_.heartbeatsReceived;
        } else if (const auto* move = std::get_if<comms::JointCommand>(&packet.message)) {
            if (newest) {
                ++stats_.commandsReplaced;
            }
            newest = *move;
            lastCommandSequence_ = packet.sequence;
        } else if (std::holds_alternative<comms::HoldCommand>(packet.message)) {
            if (newest) {
                ++stats_.commandsReplaced;
            }
            newest = comms::HoldCommand{};
            lastCommandSequence_ = packet.sequence;
        }
    }
    updateState(now, newest);
}

void ArmController::updateState(Nanoseconds now, const std::optional<Command>& command) {
    const comms::LinkState link = heartbeat_.state(now);
    if (link != link_) {
        if (link == comms::LinkState::Lost) {
            log(std::format("console link lost: no heartbeat for {:.1f} ms",
                            ms(heartbeat_.silence(now).value_or(Nanoseconds{0}))));
        } else if (link == comms::LinkState::Alive) {
            log(link_ == comms::LinkState::Lost ? "console link back" : "console link up");
        }
        link_ = link;
    }

    if (link != comms::LinkState::Alive) {
        if (command) {
            ++stats_.commandsIgnored;  // no orders from a console that is not known to be alive
        }
        if (state_ == comms::ArmState::Following) {
            model_.hold();
            state_ = comms::ArmState::Holding;
            ++stats_.holds;
            log("holding position until the console is back and sends a new command");
        }
        return;
    }
    if (!command) {
        return;
    }
    if (std::holds_alternative<comms::HoldCommand>(*command)) {
        // The console says nobody is driving: brake and stay. From Idle there is nothing to stop.
        if (state_ == comms::ArmState::Following) {
            model_.hold();
            state_ = comms::ArmState::Holding;
            log("hold command from the console: holding position");
        }
        ++stats_.holdCommands;
        return;
    }
    if (const auto* move = std::get_if<comms::JointCommand>(&*command)) {
        if (state_ == comms::ArmState::Holding) {
            log("new command: following again");
        }
        model_.setTarget(move->target);
        state_ = comms::ArmState::Following;
        ++stats_.commandsApplied;
    }
}

void ArmController::send(Nanoseconds now, const comms::Message& message, std::uint64_t& counter) {
    const comms::Endpoint to =
        config_.console.port != 0 ? config_.console : replyTo_.value_or(comms::Endpoint{});
    if (to.port == 0) {
        return;  // nobody to tell yet
    }
    const comms::Packet packet{
        .session = session_, .sequence = nextSequence_++, .timestamp = now, .message = message};
    std::array<std::byte, comms::kMaxPacketSize> buffer{};
    const std::size_t size = comms::encode(packet, buffer);
    if (socket_.sendTo(std::span(buffer).first(size), to)) {
        ++counter;
    } else {
        ++stats_.sendFailures;
    }
}

void ArmController::log(std::string_view message) const {
    if (config_.log) {
        config_.log(message);
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
