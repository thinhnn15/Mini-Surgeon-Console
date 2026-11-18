// link_check: REQ-SAF-001 end to end, with real processes (ctest
// arm_sim.REQ_SAF_001_HoldsWhenTheConsoleDies).
//
//   link_check --arm-exe=PATH     start arm_sim and a console, kill the console, check the arm
//   link_check --as-console --arm=A.B.C.D:PORT    (internal) the console that gets killed
//
// 1. Starts arm_sim, with its telemetry sent here (the observer).
// 2. Starts a second copy of itself as the console: heartbeats every 10 ms and a far target.
// 3. Waits until the arm is following and moving, then kills the console process outright
//    (TerminateProcess / SIGKILL: no goodbye packet, as in a real crash).
// 4. The arm must report Holding within 3 heartbeat periods (+ margin), brake to a stop and
//    stay there.

#include "msc/common/clock.hpp"
#include "msc/comms/arm_link.hpp"
#include "msc/comms/protocol.hpp"
#include "msc/comms/udp_socket.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <functional>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <csignal>
extern char** environ;  // NOLINT(readability-redundant-declaration): POSIX declares it nowhere else
#endif

namespace {

using namespace std::chrono_literals;
using Steady = std::chrono::steady_clock;
using msc::comms::ArmState;
using msc::comms::Endpoint;

constexpr auto kHeartbeatPeriod = 10ms;
// REQ-SAF-001: 3 missed periods. The margin covers the moment of the kill within a period
// (the last heartbeat went out up to 10 ms before it), the arm's 1 ms tick, and a busy CI
// machine scheduling three processes.
constexpr auto kHoldWithin = 3 * kHeartbeatPeriod + 50ms;

// A child process, killed and reaped when this object goes.
class Child {
public:
    static std::optional<Child> spawn(const std::vector<std::string>& args) {
#if defined(_WIN32)
        std::string commandLine;
        for (const std::string& arg : args) {
            commandLine += '"' + arg + "\" ";
        }
        STARTUPINFOA startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION info{};
        if (CreateProcessA(nullptr, commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                           &startup, &info) == 0) {
            return std::nullopt;
        }
        CloseHandle(info.hThread);
        return Child(info.hProcess);
#else
        std::vector<std::string> copies(args);
        std::vector<char*> argv;
        argv.reserve(copies.size() + 1);
        for (std::string& arg : copies) {
            argv.push_back(arg.data());
        }
        argv.push_back(nullptr);
        pid_t pid = 0;
        if (posix_spawn(&pid, argv[0], nullptr, nullptr, argv.data(), environ) != 0) {
            return std::nullopt;
        }
        return Child(pid);
#endif
    }

    Child(Child&& other) noexcept : process_(std::exchange(other.process_, kNone)) {}
    Child& operator=(Child&&) = delete;
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
    ~Child() { kill(); }

    // Ends the process at once, the way a crash would: no cleanup, no last packet.
    void kill() noexcept {
        if (process_ == kNone) {
            return;
        }
#if defined(_WIN32)
        TerminateProcess(process_, 1);
        WaitForSingleObject(process_, INFINITE);
        CloseHandle(process_);
#else
        ::kill(process_, SIGKILL);
        waitpid(process_, nullptr, 0);
#endif
        process_ = kNone;
    }

private:
#if defined(_WIN32)
    using Handle = HANDLE;
    static inline const Handle kNone = nullptr;
#else
    using Handle = pid_t;
    static constexpr Handle kNone = 0;
#endif
    explicit Child(Handle process) noexcept : process_(process) {}
    Handle process_;
};

msc::comms::JointArray all(float value) {
    msc::comms::JointArray a{};
    a.fill(value);
    return a;
}

std::optional<std::string_view> valueOf(std::string_view arg, std::string_view name) {
    if (arg.size() > name.size() && arg.substr(0, name.size()) == name && arg[name.size()] == '=') {
        return arg.substr(name.size() + 1);
    }
    return std::nullopt;
}

double ms(Steady::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
}

// The console that gets killed: heartbeats and a far target until the end of its life.
int runConsole(const Endpoint& arm) {
    const msc::SteadyClock clock;
    std::optional<msc::comms::UdpSocket> socket = msc::comms::UdpSocket::open(Endpoint::loopback(0));
    if (!socket) {
        return 1;
    }
    msc::comms::ArmLink link(clock, std::move(*socket),
                             msc::comms::ArmLinkConfig{.arm = arm, .heartbeatPeriod = kHeartbeatPeriod});
    link.setTarget(all(1.8F));
    link.start();
    std::this_thread::sleep_for(60s);  // killed long before this
    return 0;
}

// Reads the arm's telemetry until `done` returns true; false on timeout.
bool watch(msc::comms::UdpSocket& observer, Steady::duration timeout,
           const std::function<bool(const msc::comms::JointTelemetry&)>& done) {
    std::array<std::byte, msc::comms::kMaxPacketSize> buffer{};
    const auto giveUp = Steady::now() + timeout;
    while (Steady::now() < giveUp) {
        const msc::comms::ReceiveResult r = observer.receive(buffer, 20ms);
        if (r.status != msc::comms::ReceiveStatus::Datagram) {
            continue;
        }
        const msc::comms::DecodeResult decoded = msc::comms::decode(std::span(buffer).first(r.size));
        if (const auto* packet = std::get_if<msc::comms::Packet>(&decoded)) {
            if (const auto* t = std::get_if<msc::comms::JointTelemetry>(&packet->message)) {
                if (done(*t)) {
                    return true;
                }
            }
        }
    }
    return false;
}

int fail(std::string_view why) {
    std::cout << "link check: FAIL - " << why << '\n' << std::flush;
    return 1;
}

int runCheck(const std::string& self, const std::string& armExe) {
    std::optional<msc::comms::UdpSocket> observer = msc::comms::UdpSocket::open(Endpoint::loopback(0));
    if (!observer) {
        return fail("cannot open the observer socket");
    }
    // A free port for the arm: let the OS pick one, then release it (end of the lambda) for arm_sim.
    const std::uint16_t armPort = [] {
        const std::optional<msc::comms::UdpSocket> probe = msc::comms::UdpSocket::open(Endpoint::loopback(0));
        return probe ? probe->localEndpoint().port : std::uint16_t{0};
    }();
    if (armPort == 0) {
        return fail("cannot find a free port");
    }
    const Endpoint armEndpoint = Endpoint::loopback(armPort);

    const std::optional<Child> arm =
        Child::spawn({armExe, "--listen=" + armEndpoint.toString(),
                      "--console=" + observer->localEndpoint().toString(), "--duration=60"});
    if (!arm) {
        return fail("cannot start " + armExe);
    }
    std::optional<Child> console = Child::spawn({self, "--as-console", "--arm=" + armEndpoint.toString()});
    if (!console) {
        return fail("cannot start the console");
    }

    // Following, and actually moving: the interesting moment to lose the console.
    if (!watch(*observer, 10s, [](const msc::comms::JointTelemetry& t) {
            return t.state == ArmState::Following && std::abs(t.velocity[0]) > 0.5F;
        })) {
        return fail("the arm never started following the console's command");
    }

    // Timed from before the kill: killing and reaping a process takes a few milliseconds, and
    // timing from after it made the arm look faster than it is (15 ms instead of 20-30).
    const auto killed = Steady::now();
    console->kill();
    std::cout << "link check: console killed while the arm was moving\n";

    if (!watch(*observer, 2s,
               [](const msc::comms::JointTelemetry& t) { return t.state == ArmState::Holding; })) {
        return fail("the arm did not hold after the console died");
    }
    const auto noticed = Steady::now() - killed;
    std::cout << std::format("link check: arm holding {:.1f} ms after the kill (limit {:.0f} ms)\n",
                             ms(noticed), ms(kHoldWithin));
    if (noticed > kHoldWithin) {
        return fail("held too late");
    }

    msc::comms::JointArray stoppedAt{};
    if (!watch(*observer, 3s, [&stoppedAt](const msc::comms::JointTelemetry& t) {
            stoppedAt = t.position;
            return std::ranges::all_of(t.velocity, [](float v) { return v == 0.0F; });
        })) {
        return fail("the arm did not come to a stop");
    }
    bool stayed = true;
    watch(*observer, 300ms, [&](const msc::comms::JointTelemetry& t) {
        stayed = stayed && t.position == stoppedAt && t.state == ArmState::Holding;
        return false;
    });
    if (!stayed) {
        return fail("the arm moved again after stopping");
    }
    std::cout << std::format("link check: stopped at {:.3f} rad (joint 1) and stayed there\nPASS\n",
                             stoppedAt[0])
              << std::flush;
    return 0;
}

int run(std::span<char*> args) {
    std::optional<std::string> armExe;
    std::optional<Endpoint> armEndpoint;
    bool asConsole = false;
    for (const std::string_view arg : args.subspan(1)) {
        if (arg == "--as-console") {
            asConsole = true;
        } else if (const auto exe = valueOf(arg, "--arm-exe")) {
            armExe = std::string(*exe);
        } else if (const auto endpoint = valueOf(arg, "--arm")) {
            armEndpoint = Endpoint::parse(*endpoint);
        }
    }
    if (asConsole) {
        return armEndpoint ? runConsole(*armEndpoint) : 1;
    }
    if (!armExe) {
        std::cerr << "usage: link_check --arm-exe=PATH\n";
        return 1;
    }
    return runCheck(args[0], *armExe);
}

}  // namespace

// The handlers write to std::cerr, which could in theory throw too: nothing is left to catch it.
// NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, char* argv[]) {
    try {
        return run(std::span(argv, static_cast<std::size_t>(argc)));
    } catch (const std::exception& e) {
        std::cerr << "link_check: " << e.what() << '\n';
    }
    return 1;
}
