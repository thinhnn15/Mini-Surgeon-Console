// safety_check: every fault, end to end (ctest console.REQ_SAF_002_EveryFaultEndsInASafeState).
//
//   safety_check --arm-exe=PATH
//
// Starts arm_sim as a real process and runs the console's arm link and teleop core on their
// own threads, as console_app does; this thread plays the hand. For each fault:
//   1. engage and move the hand, so the arm is moving;
//   2. inject the fault;
//   3. the console must go to Fault with the expected code (and the time is measured);
//   4. a reset while the fault is still injected must be refused;
//   5. clear the fault: the arm must have stopped and be holding;
//   6. reset must now be accepted and lead to Idle;
//   7. the arm must stay where it stopped.
// Prints a table (for the journal) and exits 0 only if every row passed.

#include "child_process.hpp"
#include "msc/common/clock.hpp"
#include "msc/comms/arm_link.hpp"
#include "msc/comms/udp_socket.hpp"
#include "msc/teleop/teleop_core.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using Steady = std::chrono::steady_clock;
using msc::comms::ArmState;
using msc::comms::Endpoint;
using msc::teleop::FaultCode;
using msc::teleop::StateKind;
using msc::tools::Child;

enum class Injection : std::uint8_t { Link, Corrupt, Hand, Video, Injected };

struct Scenario {
    Injection injection;
    std::string_view name;
    std::vector<FaultCode> expected;  // the first fault detected must be one of these
};

double ms(Steady::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
}

std::string_view codeName(FaultCode code) {
    return msc::teleop::toString(code);
}

class Console {
public:
    Console(const msc::Clock& clock, msc::comms::UdpSocket socket, const Endpoint& arm)
        : clock_(clock),
          link_(clock, std::move(socket), msc::comms::ArmLinkConfig{.arm = arm}),
          core_(clock, link_) {
        link_.start();
        core_.start();
    }
    Console(const Console&) = delete;
    Console& operator=(const Console&) = delete;
    Console(Console&&) = delete;
    Console& operator=(Console&&) = delete;
    ~Console() {
        core_.stop();
        link_.stop();
    }

    // Plays the hand every 5 ms (unless stalled) until `done` holds or `timeout` passes.
    bool pump(Steady::duration timeout, const std::function<bool(const msc::teleop::TeleopStatus&)>& done) {
        const auto giveUp = Steady::now() + timeout;
        while (Steady::now() < giveUp) {
            if (!handStalled) {
                core_.submitHand(
                    msc::teleop::HandSample{.pose = hand, .clutch = false, .time = clock_.now()});
            }
            static_cast<void>(core_.readStatus(status));
            if (done(status)) {
                return true;
            }
            std::this_thread::sleep_for(5ms);
        }
        return false;
    }
    void wait(Steady::duration d) {
        pump(d, [](const msc::teleop::TeleopStatus&) { return false; });
    }

    void inject(Injection injection, bool on) {
        switch (injection) {
            case Injection::Link:
                link_.setFault(on ? msc::comms::LinkFault::Drop : msc::comms::LinkFault::None);
                break;
            case Injection::Corrupt:
                link_.setFault(on ? msc::comms::LinkFault::Corrupt : msc::comms::LinkFault::None);
                break;
            case Injection::Hand:
                handStalled = on;
                break;
            case Injection::Video:
                core_.setVideoLive(!on);
                break;
            case Injection::Injected:
                core_.setInjected(on ? msc::teleop::faultSet(FaultCode::Injected) : msc::teleop::FaultSet{});
                break;
        }
    }

    void request(msc::teleop::Request r) { core_.request(r); }

    // The scenarios read and set these directly: this is a test driver, not an interface.
    // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes)
    msc::teleop::TeleopStatus status;
    msc::comms::JointArray hand{};
    bool handStalled = false;
    // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)

private:
    const msc::Clock& clock_;
    msc::comms::ArmLink link_;
    msc::teleop::TeleopCore core_;
};

struct Row {
    std::string_view name;
    std::string detectedAs;
    double detectMs = 0.0;
    float stoppedAt = 0.0F;
    std::string failure;  // empty: passed
};

Row run(Console& console, const Scenario& scenario, float direction) {
    Row row{.name = scenario.name};
    const auto fail = [&row](std::string why) {
        row.failure = std::move(why);
        return row;
    };
    using Status = msc::teleop::TeleopStatus;

    // 1. Engaged and moving.
    if (!console.pump(5s, [](const Status& s) { return s.engageAllowed; })) {
        return fail("engage never allowed");
    }
    console.request(msc::teleop::Request::Engage);
    if (!console.pump(1s, [](const Status& s) { return s.state == StateKind::Engaged; })) {
        return fail(std::format("did not engage ({})", console.status.lastRefusal));
    }
    console.hand[0] += direction * 1.6F;  // the arm sets off on a 0.8 rad move
    if (!console.pump(2s, [](const Status& s) { return std::abs(s.link.telemetry.velocity[0]) > 0.5F; })) {
        return fail("the arm never moved");
    }

    // 2.-3. Inject: Fault with the expected code.
    console.inject(scenario.injection, true);
    const auto injected = Steady::now();
    if (!console.pump(2s, [](const Status& s) { return s.state == StateKind::Fault; })) {
        console.inject(scenario.injection, false);
        return fail("never went to Fault");
    }
    row.detectMs = ms(Steady::now() - injected);
    row.detectedAs = std::string(codeName(console.status.fault));
    if (std::ranges::find(scenario.expected, console.status.fault) == scenario.expected.end()) {
        console.inject(scenario.injection, false);
        return fail("unexpected fault code");
    }

    // 4. Reset refused while the cause is there.
    console.request(msc::teleop::Request::Reset);
    console.wait(100ms);
    if (console.status.state != StateKind::Fault || console.status.lastRefusal.empty()) {
        console.inject(scenario.injection, false);
        return fail("reset was not refused while the fault was injected");
    }

    // 5. Clear: the arm has stopped and holds.
    console.inject(scenario.injection, false);
    if (!console.pump(3s, [](const Status& s) {
            return s.link.link == msc::comms::LinkState::Alive &&
                   s.link.telemetry.state == ArmState::Holding &&
                   std::ranges::all_of(s.link.telemetry.velocity, [](float v) { return v == 0.0F; });
        })) {
        return fail("the arm was not stopped and holding");
    }

    // 6. Reset accepted, to Idle.
    if (!console.pump(3s, [](const Status& s) { return s.resetAllowed; })) {
        return fail("reset never allowed after clearing");
    }
    console.request(msc::teleop::Request::Reset);
    if (!console.pump(1s, [](const Status& s) { return s.state == StateKind::Idle; })) {
        return fail("reset did not lead to Idle");
    }

    // 7. The arm stays put.
    const msc::comms::JointArray stopped = console.status.link.telemetry.position;
    row.stoppedAt = stopped[0];
    console.wait(300ms);
    if (console.status.link.telemetry.position != stopped) {
        return fail("the arm moved after the reset");
    }
    return row;
}

int check(const std::string& armExe) {
    const std::uint16_t armPort = [] {
        const std::optional<msc::comms::UdpSocket> probe = msc::comms::UdpSocket::open(Endpoint::loopback(0));
        return probe ? probe->localEndpoint().port : std::uint16_t{0};
    }();
    std::optional<msc::comms::UdpSocket> socket = msc::comms::UdpSocket::open(Endpoint::loopback(0));
    if (armPort == 0 || !socket) {
        std::cout << "safety check: FAIL - no sockets\n";
        return 1;
    }
    const Endpoint arm = Endpoint::loopback(armPort);
    const std::optional<Child> armProcess =
        Child::spawn({armExe, "--listen=" + arm.toString(), "--duration=120"});
    if (!armProcess) {
        std::cout << "safety check: FAIL - cannot start " << armExe << '\n';
        return 1;
    }

    const msc::SteadyClock clock;
    Console console(clock, std::move(*socket), arm);
    const std::vector<Scenario> scenarios{
        {.injection = Injection::Link, .name = "cable pulled", .expected = {FaultCode::ArmLinkLost}},
        {.injection = Injection::Corrupt,
         .name = "corrupted commands",
         .expected = {FaultCode::ArmHoldingOnItsOwn, FaultCode::CommandNotAcknowledged}},
        {.injection = Injection::Hand,
         .name = "hand controller frozen",
         .expected = {FaultCode::HandInputStale}},
        {.injection = Injection::Video, .name = "video not live", .expected = {FaultCode::VideoFrozen}},
        {.injection = Injection::Injected, .name = "generic fault", .expected = {FaultCode::Injected}},
    };

    std::cout << std::format("{:<24} {:<32} {:>10} {:>12}  {}\n", "injected", "detected as", "after",
                             "arm stopped", "result");
    bool allPassed = true;
    float direction = 1.0F;
    for (const Scenario& scenario : scenarios) {
        const Row row = run(console, scenario, direction);
        direction = -direction;  // back and forth, so the arm stays inside its range
        allPassed = allPassed && row.failure.empty();
        std::cout << std::format("{:<24} {:<32} {:>7.1f} ms {:>8.3f} rad  {}\n", row.name, row.detectedAs,
                                 row.detectMs, row.stoppedAt,
                                 row.failure.empty() ? "PASS" : "FAIL - " + row.failure)
                  << std::flush;
    }
    std::cout << (allPassed ? "PASS\n" : "FAIL\n") << std::flush;
    return allPassed ? 0 : 1;
}

int runMain(std::span<char*> args) {
    for (const std::string_view arg : args.subspan(1)) {
        if (arg.starts_with("--arm-exe=")) {
            return check(std::string(arg.substr(10)));
        }
    }
    std::cerr << "usage: safety_check --arm-exe=PATH\n";
    return 1;
}

}  // namespace

// The handlers write to std::cerr, which could in theory throw too: nothing is left to catch it.
// NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, char* argv[]) {
    try {
        return runMain(std::span(argv, static_cast<std::size_t>(argc)));
    } catch (const std::exception& e) {
        std::cerr << "safety_check: " << e.what() << '\n';
    }
    return 1;
}
