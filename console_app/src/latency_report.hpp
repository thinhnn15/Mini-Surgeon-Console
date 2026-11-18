#pragma once

#include <chrono>
#include <functional>

class QQuickWindow;
class VideoItem;

// `console_app --measure-latency[=SECONDS]` (REQ-VID-002): lets the window run for a warm-up,
// starts the statistics from zero, measures for SECONDS, prints one report to stdout and
// quits. Exit code 0 if latency was measured (at least one frame presented), 1 otherwise.
//
// The report names the graphics API, vsync, the screen's refresh rate and the averages over
// the whole run, so runs can be compared (D3D11 vs D3D12, vsync on vs off, synthetic vs
// camera) in the journal. `reportSource`, if set, adds lines about the source (the camera's
// format and conversion cost).
void startLatencyMeasurement(QQuickWindow& window, VideoItem& item, std::chrono::seconds duration,
                             std::function<void()> reportSource = {});
