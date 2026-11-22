#pragma once

#include <chrono>

namespace msc {
class Clock;
}

class FaultInjector;
class QQuickWindow;
class TeleopModel;
class VideoItem;

// `console_app --soak=SECONDS [--soak-interval=SECONDS]` (M6.2, REQ-SYS-001): the real console,
// window, renderer, arm link and teleop core, driven for a long time by msc::soak::FaultCycle:
// engage, move (the scripted hand), inject the next fault, check it is detected and a reset is
// refused while it lasts, clear it, reset, again, through every fault in turn.
//
// Prints, one line each, for tools/soak_check to read:
//   soak event t=SECONDS TEXT           something that should not happen (an unexpected fault, ...)
//   soak t=SECONDS key=value ...        every interval: the fault cycle's counts, the video's
//                                       latency (last 10 s), frame rates and drops, the arm link
//   soak done ...                       at the end, with the same counts
// Memory and handles are measured from outside (soak_check), not by the process itself.
//
// Exits 0 if the fault cycle stayed clean (no unexpected, missed or wrong fault, no early reset,
// never stuck), 1 if not, 3 if the run is invalid because the window stopped being shown (a
// covered window is not drawn on Wayland), 77 if this machine draws on the CPU (graphics_api.hpp).
struct SoakOptions {
    std::chrono::seconds duration{120};
    std::chrono::seconds interval{10};
};

void startSoak(QQuickWindow& window, VideoItem& video, TeleopModel& teleop, FaultInjector& faults,
               const msc::Clock& clock, SoakOptions options);
