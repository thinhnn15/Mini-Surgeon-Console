#include "msc/soak/fault_cycle.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using msc::Nanoseconds;
using msc::soak::Action;
using msc::soak::FaultCycle;
using msc::soak::Injection;
using msc::teleop::FaultCode;
using msc::teleop::StateKind;

// Behaves like the teleop core as far as the operator can see: engages when allowed, faults some
// time after an injection with the injection's first expected code, refuses a reset while the
// cause is there, and goes to Idle on a reset once it is gone. Each behaviour can be broken.
class FakeConsole {
public:
    Nanoseconds detectionDelay = 120ms;
    std::optional<Injection> ignored;       // this injection never causes a fault
    std::optional<Injection> wrongCodeFor;  // this one faults with FaultCode::Injected
    bool resetIgnoresCause = false;         // accepts a reset with the cause still there
    bool neverAllowsEngaging = false;
    std::function<void(FakeConsole&, Nanoseconds)> extra;  // anything else, each step

    void apply(const Action& action, Nanoseconds now) {
        switch (action.kind) {
            case Action::Kind::None:
                break;
            case Action::Kind::Engage:
                if (engageAllowed()) {
                    state = StateKind::Engaged;
                }
                break;
            case Action::Kind::Inject:
                cause = action.injection;
                faultAt = now + detectionDelay;
                injections.push_back(action.injection);
                break;
            case Action::Kind::Clear:
                cause.reset();
                faultAt.reset();
                break;
            case Action::Kind::Reset:
                if (state == StateKind::Fault && (!cause || resetIgnoresCause)) {
                    state = StateKind::Idle;
                }
                break;
        }
    }

    void advance(Nanoseconds now) {
        if (faultAt && now >= *faultAt && cause && cause != ignored && state != StateKind::Fault) {
            state = StateKind::Fault;
            fault = cause == wrongCodeFor ? FaultCode::Injected : msc::soak::expectedFaults(*cause).front();
            faultAt.reset();  // a fault once per injection; after a reset the cause only blocks engaging
        }
        if (extra) {
            extra(*this, now);
        }
    }

    [[nodiscard]] msc::soak::Observation seen(Nanoseconds now) const {
        return {.now = now,
                .state = state,
                .fault = fault,
                .engageAllowed = engageAllowed(),
                .resetAllowed = state == StateKind::Fault && !cause};
    }

    [[nodiscard]] bool engageAllowed() const {
        return state == StateKind::Idle && !cause && !neverAllowsEngaging;
    }

    StateKind state = StateKind::Idle;
    FaultCode fault = FaultCode::Injected;
    std::optional<Injection> cause;
    std::optional<Nanoseconds> faultAt;
    std::vector<Injection> injections;
};

// Runs the operator against the console for `duration`, one step every 20 ms.
void run(FaultCycle& cycle, FakeConsole& console, Nanoseconds duration) {
    for (Nanoseconds now{0}; now < duration; now += 20ms) {
        console.advance(now);
        console.apply(cycle.step(console.seen(now)), now);
    }
}

}  // namespace

TEST(FaultCycle, InjectionNamesAreTheFaultInjectors) {
    EXPECT_EQ(msc::soak::toString(Injection::Link), "link");
    EXPECT_EQ(msc::soak::toString(Injection::Corrupt), "corrupt");
    EXPECT_EQ(msc::soak::toString(Injection::Hand), "hand");
    EXPECT_EQ(msc::soak::toString(Injection::Video), "video");
    EXPECT_EQ(msc::soak::toString(Injection::Generic), "fault");
    for (const auto injection : msc::soak::kInjections) {
        EXPECT_FALSE(msc::soak::expectedFaults(injection).empty()) << msc::soak::toString(injection);
    }
}

TEST(FaultCycle, REQ_SYS_001_AHealthyConsoleGoesThroughEveryFaultAndRecovers) {
    FaultCycle cycle;
    FakeConsole console;
    run(cycle, console, 120s);

    const auto& c = cycle.counts();
    EXPECT_TRUE(c.clean());
    EXPECT_GE(c.injected, 2 * msc::soak::kInjections.size());  // every fault at least twice
    EXPECT_EQ(c.detected, c.injected);
    EXPECT_GE(c.recovered + 1, c.injected);  // the last one may still be under way
    ASSERT_EQ(c.detectionTimes.size(), c.detected);
    for (const auto t : c.detectionTimes) {
        EXPECT_GE(t, 120ms);
        EXPECT_LE(t, 160ms);
    }
    // In the fixed order, round and round.
    for (std::size_t i = 0; i < console.injections.size(); ++i) {
        EXPECT_EQ(console.injections[i], msc::soak::kInjections[i % msc::soak::kInjections.size()]) << i;
    }
}

TEST(FaultCycle, REQ_SYS_001_AFaultNobodyInjectedIsCountedAndRecoveredFrom) {
    FaultCycle cycle;
    FakeConsole console;
    // At 1.5 s, while engaged and before the first injection (at 3 s), the arm link drops for
    // 200 ms by itself.
    console.extra = [](FakeConsole& c, Nanoseconds now) {
        if (now == 1500ms && c.state == StateKind::Engaged) {
            c.state = StateKind::Fault;
            c.fault = FaultCode::ArmLinkLost;
            c.cause = Injection::Link;  // the cause is present...
        }
        if (now == 1700ms && c.faultAt == std::nullopt) {
            c.cause.reset();  // ...and goes away by itself
        }
    };
    run(cycle, console, 30s);

    const auto& c = cycle.counts();
    EXPECT_EQ(c.unexpected, 1U);
    EXPECT_EQ(c.unexpectedByCode.at(static_cast<std::size_t>(FaultCode::ArmLinkLost)), 1U);
    EXPECT_FALSE(c.clean());
    EXPECT_NE(cycle.lastEvent().find("unexpected fault while engaged: \"arm link lost\""), std::string::npos)
        << cycle.lastEvent();
    // Recovered, and the cycle starts from the first injection as if nothing had happened.
    ASSERT_GE(console.injections.size(), 2U);
    EXPECT_EQ(console.injections[0], Injection::Link);
    EXPECT_EQ(console.injections[1], Injection::Corrupt);
    EXPECT_EQ(c.detected, c.injected);
}

TEST(FaultCycle, REQ_SYS_001_AFaultThatDoesNotShowIsCountedAndTheCycleGoesOn) {
    FaultCycle cycle;
    FakeConsole console;
    console.ignored = Injection::Hand;
    run(cycle, console, 60s);

    const auto& c = cycle.counts();
    EXPECT_GE(c.missed, 1U);
    EXPECT_EQ(c.detected + c.missed, c.injected);
    EXPECT_NE(cycle.lastEvent().find("injected hand, no fault after"), std::string::npos)
        << cycle.lastEvent();
    // The ones after it still ran.
    EXPECT_GE(std::ranges::count(console.injections, Injection::Video), 1);
}

TEST(FaultCycle, REQ_SYS_001_TheWrongFaultIsCounted) {
    FaultCycle cycle;
    FakeConsole console;
    console.wrongCodeFor = Injection::Video;
    run(cycle, console, 60s);
    EXPECT_GE(cycle.counts().wrongFault, 1U);
    EXPECT_FALSE(cycle.counts().clean());
}

TEST(FaultCycle, REQ_SYS_001_AResetAcceptedWhileTheCauseIsThereIsCounted) {
    FaultCycle cycle;
    FakeConsole console;
    console.resetIgnoresCause = true;
    run(cycle, console, 30s);
    EXPECT_GE(cycle.counts().earlyReset, 1U);
    EXPECT_NE(cycle.lastEvent().find("a reset was accepted while"), std::string::npos) << cycle.lastEvent();
}

TEST(FaultCycle, REQ_SYS_001_AConsoleThatNeverLetsUsEngageIsStuck) {
    FaultCycle cycle;
    FakeConsole console;
    console.neverAllowsEngaging = true;
    run(cycle, console, 21s);
    EXPECT_EQ(cycle.counts().stuck, 4U);  // one per 5 s
    EXPECT_EQ(cycle.counts().injected, 0U);
}
