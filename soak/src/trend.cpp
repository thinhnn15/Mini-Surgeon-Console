#include "msc/soak/trend.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <iterator>
#include <optional>
#include <span>
#include <vector>

namespace msc::soak {

namespace {

constexpr double kSecondsPerHour = 3600.0;
constexpr std::size_t kMinPoints = 3;  // fewer cannot tell a trend from a step

std::vector<Point> afterWarmUp(std::span<const Point> points, double warmUp) {
    std::vector<Point> kept;
    std::ranges::copy_if(points, std::back_inserter(kept),
                         [warmUp](const Point& p) { return p.t >= warmUp; });
    return kept;
}

double median(std::vector<double> values) {
    std::ranges::sort(values);
    const std::size_t n = values.size();
    return n % 2 == 1 ? values[n / 2] : (values[(n / 2) - 1] + values[n / 2]) / 2.0;
}

}  // namespace

std::optional<Line> fitLine(std::span<const Point> points) {
    if (points.size() < 2) {
        return std::nullopt;
    }
    double meanT = 0.0;
    double meanV = 0.0;
    for (const auto& p : points) {
        meanT += p.t;
        meanV += p.value;
    }
    meanT /= static_cast<double>(points.size());
    meanV /= static_cast<double>(points.size());
    double stt = 0.0;
    double stv = 0.0;
    for (const auto& p : points) {
        stt += (p.t - meanT) * (p.t - meanT);
        stv += (p.t - meanT) * (p.value - meanV);
    }
    if (stt <= 0.0) {
        return std::nullopt;
    }
    const double slope = stv / stt;
    return Line{.slope = slope, .intercept = meanV - (slope * meanT)};
}

GrowthVerdict judgeGrowth(std::span<const Point> points, const GrowthLimit& limit) {
    const auto kept = afterWarmUp(points, limit.warmUp);
    const std::span<const Point> half = std::span(kept).subspan(kept.size() / 2);
    GrowthVerdict verdict;
    verdict.points = half.size();
    const auto line = fitLine(half);
    if (half.size() < kMinPoints || !line) {
        verdict.why = std::format("too short to judge: {} samples after warm-up", kept.size());
        return verdict;
    }
    verdict.from = half.front().t;
    verdict.to = half.back().t;
    verdict.perHour = line->slope * kSecondsPerHour;
    verdict.rise = line->slope * (verdict.to - verdict.from);
    if (verdict.perHour > limit.maxPerHour && verdict.rise > limit.noise) {
        verdict.ok = false;
        verdict.why = std::format(
            "still growing at the end: {:.4g} per hour (limit {:.4g}), up {:.4g} from {:.0f} s to "
            "{:.0f} s (noise {:.4g})",
            verdict.perHour, limit.maxPerHour, verdict.rise, verdict.from, verdict.to, limit.noise);
    }
    return verdict;
}

DriftVerdict judgeDrift(std::span<const Point> points, const DriftLimit& limit) {
    const auto kept = afterWarmUp(points, limit.warmUp);
    DriftVerdict verdict;
    verdict.points = kept.size();
    if (kept.size() < 2 * kMinPoints) {
        verdict.why = std::format("too short to judge: {} samples after warm-up", kept.size());
        return verdict;
    }
    const std::size_t third = kept.size() / 3;
    std::vector<double> first;
    std::vector<double> last;
    for (std::size_t i = 0; i < third; ++i) {
        first.push_back(kept[i].value);
        last.push_back(kept[kept.size() - third + i].value);
    }
    verdict.start = median(first);
    verdict.end = median(last);
    const double increase = verdict.end - verdict.start;
    if (increase > limit.maxIncrease && verdict.end > verdict.start * limit.maxRatio) {
        verdict.ok = false;
        verdict.why = std::format(
            "got worse: median {:.3g} in the first third, {:.3g} in the last (limits +{:.3g} and "
            "x{:.3g})",
            verdict.start, verdict.end, limit.maxIncrease, limit.maxRatio);
    }
    return verdict;
}

}  // namespace msc::soak
