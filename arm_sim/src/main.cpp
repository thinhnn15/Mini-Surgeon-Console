// arm_sim: the simulated robot arm, its own process (ADR 0001). Plain C++, no Qt.
//
//   arm_sim [--listen=A.B.C.D:PORT] [--console=A.B.C.D:PORT] [--duration=SECONDS] [--report]
//
// Listens for JointCommand packets, runs the 1 kHz joint loop and sends JointTelemetry to
// --console (default: back to whoever sent the newest command). Runs until Ctrl+C, or for
// --duration seconds. --report prints the loop's timing and the link's counters at the end.

#include "msc/arm/arm_controller.hpp"
#include "msc/common/clock.hpp"
#include "msc/comms/protocol.hpp"
#include "msc/comms/udp_socket.hpp"

#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace {

constexpr std::uint16_t kDefaultPort = 47001;

// Set by Ctrl+C. A lock-free atomic is one of the few things a signal handler may touch, and
// the handler can only reach it as a global.
std::atomic<bool> gStop{false};  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
static_assert(std::atomic<bool>::is_always_lock_free);

extern "C" void onSignal(int /*signal*/) {
    gStop.store(true, std::memory_order_relaxed);
}

struct Options {
    msc::comms::Endpoint listen = msc::comms::Endpoint::loopback(kDefaultPort);
    msc::arm::ArmControllerConfig controller;
    std::optional<std::chrono::seconds> duration;
    bool report = false;
};

// `--name=value`: the value, or nothing if `arg` is not that option.
std::optional<std::string_view> valueOf(std::string_view arg, std::string_view name) {
    if (arg.size() > name.size() && arg.substr(0, name.size()) == name && arg[name.size()] == '=') {
        return arg.substr(name.size() + 1);
    }
    return std::nullopt;
}

std::optional<int> positiveInteger(std::string_view text) {
    int value = 0;
    const char* last = std::to_address(text.end());
    const auto [end, error] = std::from_chars(std::to_address(text.begin()), last, value);
    if (error != std::errc{} || end != last || value <= 0) {
        return std::nullopt;
    }
    return value;
}

double ms(std::optional<msc::Nanoseconds> t) {
    return t ? std::chrono::duration<double, std::milli>(*t).count() : 0.0;
}

const char* stateName(msc::comms::ArmState state) {
    switch (state) {
        case msc::comms::ArmState::Idle:
            return "idle";
        case msc::comms::ArmState::Following:
            return "following";
        case msc::comms::ArmState::Holding:
            return "holding";
    }
    return "?";
}

void printReport(const msc::arm::ArmController& arm, std::chrono::duration<double> ran) {
    const auto& s = arm.stats();
    const auto& c = arm.consoleStats();
    std::uint64_t refused = 0;
    for (const auto count : s.refused) {
        refused += count;
    }
    const double hz = ran.count() > 0 ? static_cast<double>(s.ticks) / ran.count() : 0.0;
    std::cout << std::format("arm_sim: {:.1f} s, {} ticks ({:.1f} Hz), {} overruns\n", ran.count(), s.ticks,
                             hz, s.overruns);
    std::cout << std::format(
        "  tick lateness: p50 {:.3f} ms, p99 {:.3f} ms, max {:.3f} ms (last {} ticks; max over the run "
        "{:.3f} ms)\n",
        ms(arm.lateness().p50()), ms(arm.lateness().p99()), ms(arm.lateness().max()), arm.lateness().count(),
        ms(arm.maxLateness()));
    std::cout << std::format(
        "  from the console: {} commands applied, {} replaced within a tick, {} ignored (console not alive), "
        "{} heartbeats; {} old or duplicate, {} malformed, {} lost\n",
        s.commandsApplied, s.commandsReplaced, s.commandsIgnored, s.heartbeatsReceived, s.oldOrDuplicate,
        refused, c.lost);
    std::cout << std::format("  to the console: {} telemetry, {} heartbeats, {} failed; holds: {}\n",
                             s.telemetrySent, s.heartbeatsSent, s.sendFailures, s.holds);
    std::cout << std::format("  longest gap between console heartbeats: {:.1f} ms (holds after {:.0f} ms)\n",
                             ms(s.longestHeartbeatGap), ms(arm.heartbeatLimit()));
    const auto p = arm.model().position();
    std::cout << std::format("  state: {}, joints [{:.3f} {:.3f} {:.3f} {:.3f} {:.3f} {:.3f}] rad\n",
                             stateName(arm.state()), p[0], p[1], p[2], p[3], p[4], p[5])
              << std::flush;
}

int run(std::span<char*> args) {
    Options options;
    for (const std::string_view arg : args.subspan(1)) {
        if (arg == "--version") {
            std::cout << "arm_sim " << MSC_VERSION << '\n';
            return 0;
        }
        if (arg == "--report") {
            options.report = true;
        } else if (const auto listen = valueOf(arg, "--listen")) {
            const auto endpoint = msc::comms::Endpoint::parse(*listen);
            if (!endpoint) {
                std::cerr << "arm_sim: --listen needs A.B.C.D:PORT\n";
                return 1;
            }
            options.listen = *endpoint;
        } else if (const auto console = valueOf(arg, "--console")) {
            const auto endpoint = msc::comms::Endpoint::parse(*console);
            if (!endpoint) {
                std::cerr << "arm_sim: --console needs A.B.C.D:PORT\n";
                return 1;
            }
            options.controller.console = *endpoint;
        } else if (const auto duration = valueOf(arg, "--duration")) {
            const std::optional<int> seconds = positiveInteger(*duration);
            if (!seconds) {
                std::cerr << "arm_sim: --duration needs a positive whole number of seconds\n";
                return 1;
            }
            options.duration = std::chrono::seconds(*seconds);
        } else {
            std::cerr << "arm_sim: unknown option " << arg << '\n';
            return 1;
        }
    }

    std::string error;
    std::optional<msc::comms::UdpSocket> socket = msc::comms::UdpSocket::open(options.listen, &error);
    if (!socket) {
        // Most likely another arm_sim already has the port: two arms answering one console
        // would be worse than none.
        std::cerr << "arm_sim: cannot listen on " << options.listen.toString() << ": " << error << '\n';
        return 1;
    }
    std::cout << std::format("arm_sim {}: listening on {}, {:.0f} Hz loop\n", MSC_VERSION,
                             socket->localEndpoint().toString(),
                             1.0 / std::chrono::duration<double>(options.controller.period).count())
              << std::flush;

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    // State changes go to stdout as they happen: the demo of ADR 0001 (kill the console, watch
    // the arm hold) is read from here.
    const msc::SteadyClock clock;
    const msc::Nanoseconds started = clock.now();
    options.controller.log = [&clock, started](std::string_view message) {
        const double at = std::chrono::duration<double>(clock.now() - started).count();
        std::cout << std::format("arm_sim [{:8.3f} s] {}\n", at, message) << std::flush;
    };
    msc::arm::ArmController arm(clock, std::move(*socket), options.controller);
    const auto start = std::chrono::steady_clock::now();
    arm.run(gStop, options.duration);
    if (options.report) {
        printReport(arm, std::chrono::steady_clock::now() - start);
    }
    return 0;
}

}  // namespace

// The handlers write to std::cerr, which could in theory throw too: nothing is left to catch it.
// NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, char* argv[]) {
    try {
        return run(std::span(argv, static_cast<std::size_t>(argc)));
    } catch (const std::exception& e) {
        std::cerr << "arm_sim: " << e.what() << '\n';
    } catch (...) {
        std::cerr << "arm_sim: unknown error\n";
    }
    return 1;
}
