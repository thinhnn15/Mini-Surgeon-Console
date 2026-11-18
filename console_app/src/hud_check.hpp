#pragma once

#include <cstdint>

namespace msc::hud {
class ArrivalJitter;
}

class FaultInjector;
class QQuickWindow;
class TeleopModel;
class VideoItem;

// End-to-end check of the overlay for `console_app --check-hud` (M5.4, REQ-HUD-001), from the
// pixels the renderer drew, with the real window, renderer, arm link and a real arm_sim:
//
//   1. With the arm moving (the scripted hand), reads back what the renderer drew and measures,
//      in pixels of the video frame, how far the overlay's ring is from the instrument's tip:
//      first synchronised, then naive. Synchronised must be on the tip; naive must be off by
//      about speed x latency, and clearly told apart from synchronised.
//   2. Cuts the arm link: the ring must be gone from the screen soon after, and back when the
//      link is.
//   3. Reports how late telemetry arrived (`arrivals`), which is what stamping poses with
//      their arrival time costs. A figure for the journal, not a pass / fail.
//
// The frame size is the synthetic source's, to turn positions in what was drawn into frame pixels.
// Exits the application with 0 on success, 1 on failure.
void startHudCheck(QQuickWindow& window, VideoItem& video, TeleopModel& teleop, FaultInjector& faults,
                   const msc::hud::ArrivalJitter& arrivals, std::uint32_t frameWidth,
                   std::uint32_t frameHeight);
