#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>

namespace msc::soak {

// One measurement over a long run: when (seconds since the start) and what (bytes, handles, ms).
struct Point {
    double t = 0.0;
    double value = 0.0;
};

// Least squares: value ≈ slope * t + intercept. Nothing for fewer than two points or a single
// time.
struct Line {
    double slope = 0.0;
    double intercept = 0.0;
};
[[nodiscard]] std::optional<Line> fitLine(std::span<const Point> points);

// Does a resource keep growing (REQ-SYS-001)?
//
// A leak and a cache that fills up both grow at first; only the leak is still growing at the end.
// So the samples taken during warm-up are left out, and the line is fitted to the second half of
// what is left: a cache that has filled up is flat there, a leak is not.
// It is a leak only if both are true over that half:
//   - the slope is above maxPerHour (it would matter over a session), and
//   - the line rises more than noise over the half (one allocation more or less is not a trend).
struct GrowthLimit {
    double warmUp = 30.0;     // seconds left out at the start
    double maxPerHour = 0.0;  // in the value's unit per hour
    double noise = 0.0;       // in the value's unit: smaller rises are not judged
};

struct GrowthVerdict {
    bool ok = true;
    double perHour = 0.0;  // slope of the judged half, per hour
    double rise = 0.0;     // what the fitted line rises over the judged half
    double from = 0.0;     // judged half: first and last time (s)
    double to = 0.0;
    std::size_t points = 0;  // judged
    std::string why{};       // when not ok, or when there was too little to judge
};
[[nodiscard]] GrowthVerdict judgeGrowth(std::span<const Point> points, const GrowthLimit& limit);

// Does a latency get worse over the run? Compares the median of the first and the last third of
// the samples after warm-up. Worse means both: more than maxIncrease (absolute) and more than
// maxRatio times the start (relative), so neither 0.2 ms on a 1 ms figure nor 2 % of a large one
// counts.
struct DriftLimit {
    double warmUp = 30.0;
    double maxIncrease = 0.0;
    double maxRatio = 1.5;
};

struct DriftVerdict {
    bool ok = true;
    double start = 0.0;  // median of the first third
    double end = 0.0;    // median of the last third
    std::size_t points = 0;
    std::string why{};
};
[[nodiscard]] DriftVerdict judgeDrift(std::span<const Point> points, const DriftLimit& limit);

}  // namespace msc::soak
