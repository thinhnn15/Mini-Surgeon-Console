#include "hud_check.hpp"

#include "fault_injector.hpp"
#include "msc/common/clock.hpp"
#include "msc/hud/arrival_jitter.hpp"
#include "msc/hud/hud_overlay.hpp"
#include "msc/video/letterbox.hpp"
#include "msc/video/scene.hpp"
#include "teleop_model.hpp"
#include "video_item.hpp"

#include <QCoreApplication>
#include <QImage>
#include <QObject>
#include <QPointF>
#include <QQuickWindow>
#include <QRect>
#include <QTimer>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <vector>

namespace {

using namespace std::chrono_literals;
using msc::Nanoseconds;

// Captures per mode. About 100 ms apart they cover a third of the scripted hand's figure of
// eight, with the tip moving in different directions and at different speeds.
constexpr std::size_t kSamples = 30;
constexpr auto kSampleInterval = 90ms;
// Each capture waits a little longer than the one before, in a cycle, so the captures do not
// beat in time with the 60 Hz frames (the M2.3 lesson).
constexpr auto kSampleStagger = 13ms;

// Limits, in pixels of the video frame (1280 wide).
// Synchronised: the ring's centre on the tip's centre. What is left is the camera drawing the
// newest pose it had (up to 1 ms old) and both centres being found from whole pixels.
constexpr double kSyncedMedianLimit = 1.5;
constexpr double kSyncedMaxLimit = 4.0;
// Naive: off by speed x latency. With the scripted hand and an 80 ms delay that is 8 to 20 px.
constexpr double kNaiveMedianAtLeast = 5.0;
constexpr double kNaiveOverSynced = 3.0;  // and at least this many times the synchronised error
// Measured over predicted (speed x latency): the naive error must be the error the theory says.
constexpr double kPredictionLow = 0.6;
constexpr double kPredictionHigh = 1.4;
// Captures without a ring while the link is fine. None are expected; one or two are a hiccup.
constexpr std::size_t kMissedRingsAllowed = 2;

// After the link is cut, frames captured before the cut still get their ring; the first frame
// captured after it is drawn one video latency later (about 100 ms with the 80 ms delay). The
// limit is that latency, as measured in this run, plus this much: a slow machine (a CI runner
// drawing in software) has a longer latency, and that is not what is being checked.
constexpr auto kHiddenWithinLatencyPlus = 200ms;
constexpr auto kBackWithin = 1500ms;
constexpr auto kPollInterval = 8ms;

constexpr auto kReadyTimeout = 20s;
constexpr auto kPhaseTimeout = 15s;
constexpr auto kGiveUpAfter = 60s;
// A blob smaller than this is not a tip or a ring (stray pixels at a scaled edge).
constexpr int kMinBlobPixels = 30;

using Rgb = std::array<std::uint8_t, 3>;

// Is this pixel (nearly) that colour? The picture is scaled to the window, so edges blend;
// the inside of the tip disc and of the ring's line keep their colour.
bool isColour(const uchar* pixel, const Rgb& colour) {
    constexpr int kTolerance = 36;
    return std::abs(pixel[0] - colour[0]) <= kTolerance && std::abs(pixel[1] - colour[1]) <= kTolerance &&
           std::abs(pixel[2] - colour[2]) <= kTolerance;
}

struct Blob {
    int pixels = 0;
    double sumX = 0.0;
    double sumY = 0.0;

    void add(int x, int y) {
        ++pixels;
        sumX += x + 0.5;
        sumY += y + 0.5;
    }
    [[nodiscard]] bool found() const { return pixels >= kMinBlobPixels; }
    [[nodiscard]] QPointF centre() const { return {sumX / pixels, sumY / pixels}; }
};

// What one captured draw shows, in pixels of the video frame, and which frame it was.
struct Sighting {
    bool tip = false;
    bool ring = false;
    int ringPixels = 0;
    QPointF tipAt;
    QPointF ringAt;
    std::uint64_t sequence = 0;
    Nanoseconds captureTime{0};
    Nanoseconds drawnAt{0};
    msc::hud::HudState hud = msc::hud::HudState::NoData;
    Nanoseconds hudSince{0};  // the first draw with this overlay state (see FrameCapture)
};

double median(std::vector<double> values) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const std::size_t middle = values.size() / 2;
    return values.size() % 2 == 1 ? values[middle] : (values[middle - 1] + values[middle]) / 2.0;
}

double largest(const std::vector<double>& values) {
    return values.empty() ? 0.0 : *std::max_element(values.begin(), values.end());
}

double distance(const QPointF& a, const QPointF& b) {
    return std::hypot(a.x() - b.x(), a.y() - b.y());
}

double milliseconds(Nanoseconds t) {
    return std::chrono::duration<double, std::milli>(t).count();
}

// Now, on the clock the frames and the captures are stamped with (msc::SteadyClock).
Nanoseconds now() {
    return msc::SteadyClock().now();
}

class HudCheck : public QObject {
public:
    HudCheck(QQuickWindow& window, VideoItem& video, TeleopModel& teleop, FaultInjector& faults,
             const msc::hud::ArrivalJitter& arrivals, std::uint32_t frameWidth, std::uint32_t frameHeight)
        : QObject(&window),
          window_(window),
          video_(video),
          teleop_(teleop),
          faults_(faults),
          arrivals_(arrivals),
          frameWidth_(frameWidth),
          frameHeight_(frameHeight) {
        timer_.setSingleShot(true);
        timer_.setTimerType(Qt::PreciseTimer);
        connect(&timer_, &QTimer::timeout, this, &HudCheck::step);
        QTimer::singleShot(kGiveUpAfter, this, [this] { fail("not finished in time"); });
        std::printf("hud check: waiting for the arm to follow the scripted hand and the overlay to show\n");
        std::fflush(stdout);
        enter(Phase::WaitReady);
        video_.requestCapture();
        timer_.start(100ms);
    }

private:
    enum class Phase : std::uint8_t { WaitReady, Synced, Naive, BeforeCut, WaitHidden, WaitBack };

    // Where the instrument's tip and the ring of this colour are in a captured draw.
    [[nodiscard]] std::optional<Sighting> look(const FrameCapture& capture, const Rgb& ringColour) const {
        const QImage& image = capture.image;
        if (image.isNull() || image.format() != QImage::Format_RGBA8888) {
            return std::nullopt;
        }
        // The picture inside the item, as the renderer placed it. Only the picture is searched.
        const msc::video::PixelRect box =
            msc::video::letterbox(frameWidth_, frameHeight_, static_cast<std::uint32_t>(image.width()),
                                  static_cast<std::uint32_t>(image.height()));
        const QRect picture(static_cast<int>(box.x), static_cast<int>(box.y), static_cast<int>(box.width),
                            static_cast<int>(box.height));
        if (picture.isEmpty()) {
            return std::nullopt;
        }

        const Rgb tipColour{msc::video::kToolTipColour[0], msc::video::kToolTipColour[1],
                            msc::video::kToolTipColour[2]};
        Blob tip;
        Blob ring;
        for (int y = picture.top(); y <= picture.bottom(); ++y) {
            const uchar* pixel = image.constScanLine(y) + (static_cast<std::ptrdiff_t>(picture.left()) * 4);
            for (int x = picture.left(); x <= picture.right(); ++x, pixel += 4) {
                if (isColour(pixel, tipColour)) {
                    tip.add(x, y);
                } else if (isColour(pixel, ringColour)) {
                    ring.add(x, y);
                }
            }
        }
        // From pixels of the item to pixels of the video frame.
        const double scale = static_cast<double>(frameWidth_) / picture.width();
        const auto toFrame = [&](const QPointF& p) {
            return QPointF((p.x() - picture.left()) * scale, (p.y() - picture.top()) * scale);
        };
        Sighting seen;
        seen.sequence = capture.sequence;
        seen.captureTime = capture.captureTime;
        seen.drawnAt = capture.drawnAt;
        seen.hud = capture.hud;
        seen.hudSince = capture.hudSince;
        seen.tip = tip.found();
        seen.ring = ring.found();
        seen.ringPixels = ring.pixels;
        if (seen.tip) {
            seen.tipAt = toFrame(tip.centre());
        }
        if (seen.ring) {
            seen.ringAt = toFrame(ring.centre());
        }
        return seen;
    }

    static Rgb colourOf(VideoItem::HudMode mode) {
        return mode == VideoItem::HudMode::HudNaive ? msc::hud::kHudNaiveColour : msc::hud::kHudSyncedColour;
    }

    void enter(Phase phase) {
        phase_ = phase;
        phaseStarted_ = now();
    }

    [[nodiscard]] Nanoseconds inPhase() const { return now() - phaseStarted_; }

    // Every tick: take the capture asked for last time, if it has arrived, and ask for the next.
    void step() {
        if (finished_) {
            return;
        }
        std::optional<Sighting> seen;
        if (const std::optional<FrameCapture> capture = video_.takeCapture()) {
            seen = look(*capture, colourOf(video_.hudMode()));
        }
        video_.requestCapture();
        switch (phase_) {
            case Phase::WaitReady:
                waitReady(seen);
                break;
            case Phase::Synced:
            case Phase::Naive:
                measure(seen);
                break;
            case Phase::BeforeCut:
                cutTheLink(seen);
                break;
            case Phase::WaitHidden:
                waitHidden(seen);
                break;
            case Phase::WaitBack:
                waitBack(seen);
                break;
        }
    }

    void waitReady(const std::optional<Sighting>& seen) {
        if (window_.rendererInterface()->graphicsApi() == QSGRendererInterface::Software) {
            fail("the software backend cannot draw the video item or the overlay");
            return;
        }
        const bool ready = video_.videoState() == VideoItem::VideoState::Live &&
                           teleop_.state() == QLatin1String("engaged") &&
                           teleop_.armState() == QLatin1String("following") && video_.hudShown();
        if (ready && seen && seen->tip && seen->ring) {
            std::printf("hud check: arm following, overlay shown; measuring\n");
            std::fflush(stdout);
            enter(Phase::Synced);
            timer_.start(1000ms);  // let the arm get up to speed
            return;
        }
        if (inPhase() > kReadyTimeout) {
            std::printf("  video %s, teleop \"%s\", arm \"%s\", overlay: %s\n",
                        video_.videoState() == VideoItem::VideoState::Live ? "live" : "not live",
                        qPrintable(teleop_.state()), qPrintable(teleop_.armState()),
                        qPrintable(video_.hudStatus()));
            fail("the arm never followed the hand with the overlay shown (is arm_sim running?)");
            return;
        }
        timer_.start(100ms);
    }

    // One sample of the mode being measured; after kSamples of them, on to the next mode.
    void measure(const std::optional<Sighting>& seen) {
        const bool naive = phase_ == Phase::Naive;
        std::vector<Sighting>& samples = naive ? naive_ : synced_;
        std::size_t& missed = naive ? naiveMissed_ : syncedMissed_;
        // A capture taken before the mode was switched does not count.
        if (seen && seen->drawnAt > phaseStarted_ + Nanoseconds{100ms}) {
            if (seen->tip && seen->ring) {
                samples.push_back(*seen);
            } else {
                ++missed;
            }
        }
        if (samples.size() < kSamples) {
            if (inPhase() > kPhaseTimeout) {
                std::printf("  %zu captures with ring and tip, %zu without, in %.0f ms\n", samples.size(),
                            missed, milliseconds(inPhase()));
                fail(naive ? "too few captures showed the naive ring and the tip"
                           : "too few captures showed the ring and the tip");
                return;
            }
            timer_.start(kSampleInterval + (kSampleStagger * static_cast<int>(samples.size() % 5)));
            return;
        }
        if (!naive) {
            video_.setHudMode(VideoItem::HudMode::HudNaive);
            enter(Phase::Naive);
        } else {
            video_.setHudMode(VideoItem::HudMode::HudSynced);
            enter(Phase::BeforeCut);
        }
        timer_.start(kSampleInterval);
    }

    void cutTheLink(const std::optional<Sighting>& seen) {
        // Back in synchronised mode, with the ring on screen: now pull the cable.
        if (seen && seen->ring && seen->drawnAt > phaseStarted_ + Nanoseconds{100ms}) {
            faults_.setLink(true);
            enter(Phase::WaitHidden);
            timer_.start(kPollInterval);
            return;
        }
        if (inPhase() > 3s) {
            fail("the synchronised ring did not come back after the naive measurement");
            return;
        }
        timer_.start(20ms);
    }

    void waitHidden(const std::optional<Sighting>& seen) {
        // Gone means gone: not a single pixel of the ring's colour, in a draw made after the
        // cut, and the renderer saying it drew none. The time is that of the first draw without
        // the ring, which the renderer noted itself: this check's own timer may be late.
        if (seen && seen->drawnAt > phaseStarted_ && seen->ringPixels == 0 &&
            seen->hud != msc::hud::HudState::Shown) {
            hiddenAfter_ = milliseconds(seen->hudSince - phaseStarted_);
            hiddenStatus_ = QString::fromLatin1(msc::hud::toString(seen->hud));
            faults_.setLink(false);  // plugged back in
            enter(Phase::WaitBack);
            timer_.start(kPollInterval);
            return;
        }
        if (inPhase() > 2s) {
            fail("the ring stayed on screen after the arm link was cut (REQ-HUD-001)");
            return;
        }
        timer_.start(kPollInterval);
    }

    void waitBack(const std::optional<Sighting>& seen) {
        if (seen && seen->drawnAt > phaseStarted_ && seen->ring && seen->tip &&
            seen->hud == msc::hud::HudState::Shown) {
            backAfter_ = milliseconds(seen->hudSince - phaseStarted_);
            backError_ = distance(seen->ringAt, seen->tipAt);
            report();
            return;
        }
        if (inPhase() > 4s) {
            fail("the ring did not come back after the arm link was restored");
            return;
        }
        timer_.start(kPollInterval);
    }

    // Distance from the ring's centre to the tip's centre in every sample.
    static std::vector<double> errors(const std::vector<Sighting>& samples) {
        std::vector<double> result;
        result.reserve(samples.size());
        for (const Sighting& s : samples) {
            result.push_back(distance(s.ringAt, s.tipAt));
        }
        return result;
    }

    struct Prediction {
        double speed = 0.0;      // median tip speed, frame pixels per second
        double latencyMs = 0.0;  // median time from a frame's capture to its draw
        double error = 0.0;      // median of speed x latency, sample by sample
    };

    // What the naive error should be: in each sample, how fast the tip was moving (from the
    // samples before and after it, by the frames' capture times) times how old the frame was
    // when it was drawn. The naive ring is drawn at the arm's newest pose at that moment.
    static Prediction predict(const std::vector<Sighting>& samples) {
        std::vector<double> speeds;
        std::vector<double> latencies;
        std::vector<double> predicted;
        for (std::size_t i = 1; i + 1 < samples.size(); ++i) {
            const double seconds =
                std::chrono::duration<double>(samples[i + 1].captureTime - samples[i - 1].captureTime)
                    .count();
            if (seconds <= 0.0) {
                continue;
            }
            const double speed = distance(samples[i + 1].tipAt, samples[i - 1].tipAt) / seconds;
            const double latency =
                std::chrono::duration<double>(samples[i].drawnAt - samples[i].captureTime).count();
            speeds.push_back(speed);
            latencies.push_back(latency * 1000.0);
            predicted.push_back(speed * latency);
        }
        return Prediction{
            .speed = median(speeds), .latencyMs = median(latencies), .error = median(predicted)};
    }

    void report() {
        const std::vector<double> syncedErrors = errors(synced_);
        const std::vector<double> naiveErrors = errors(naive_);
        const double syncedMedian = median(syncedErrors);
        const double syncedMax = largest(syncedErrors);
        const double naiveMedian = median(naiveErrors);
        const double naiveMax = largest(naiveErrors);
        const Prediction predicted = predict(naive_);
        const double ratio = predicted.error > 0.0 ? naiveMedian / predicted.error : 0.0;
        const double hiddenLimit = predicted.latencyMs + milliseconds(kHiddenWithinLatencyPlus);

        std::printf(
            "hud check: distance from the ring's centre to the instrument's tip, in frame pixels (%u wide)\n",
            frameWidth_);
        std::printf("  synchronised: median %.2f px, max %.2f px (%zu captures, %zu without a ring)\n",
                    syncedMedian, syncedMax, synced_.size(), syncedMissed_);
        std::printf("  naive:        median %.2f px, max %.2f px (%zu captures, %zu without a ring)\n",
                    naiveMedian, naiveMax, naive_.size(), naiveMissed_);
        std::printf(
            "  predicted for naive: tip speed %.0f px/s x frame age when drawn %.1f ms = %.2f px; measured / "
            "predicted = %.2f\n",
            predicted.speed, predicted.latencyMs, predicted.error, ratio);
        std::printf(
            "  arm link cut: ring gone after %.0f ms (limit %.0f ms; overlay: %s); back %.0f ms after the "
            "link, "
            "%.2f px from the tip\n",
            hiddenAfter_, hiddenLimit, qPrintable(hiddenStatus_), backAfter_, backError_);

        // What stamping poses with their arrival time costs: a packet that arrives late puts
        // its pose late in the history, and the ring trails by that delay times the tip's speed.
        const msc::hud::ArrivalJitter::Report arrivals = arrivals_.report();
        if (arrivals.samples > 0) {
            const auto percent = [&arrivals](std::uint64_t count) {
                return 100.0 * static_cast<double>(count) / static_cast<double>(arrivals.samples);
            };
            std::printf(
                "  telemetry arrival: %llu packets; later than the quickest by more than 1 ms: %.2f %%, 2 "
                "ms: "
                "%.2f %%, 5 ms: %.2f %%; worst %.1f ms (%.2f px at this tip speed)\n",
                static_cast<unsigned long long>(arrivals.samples), percent(arrivals.over1ms),
                percent(arrivals.over2ms), percent(arrivals.over5ms), milliseconds(arrivals.worst),
                predicted.speed * std::chrono::duration<double>(arrivals.worst).count());
        }

        bool pass = true;
        const auto expect = [&pass](bool ok, const char* what) {
            if (!ok) {
                pass = false;
                std::printf("  FAIL - %s\n", what);
            }
        };
        expect(syncedMedian <= kSyncedMedianLimit, "synchronised: the ring is not on the tip (median)");
        expect(syncedMax <= kSyncedMaxLimit,
               "synchronised: the ring left the tip in at least one capture (max)");
        expect(syncedMissed_ <= kMissedRingsAllowed, "synchronised: the ring was missing with the link fine");
        expect(naiveMedian >= kNaiveMedianAtLeast,
               "naive: the ring is on the tip too, so this check cannot tell the two apart");
        expect(naiveMedian >= kNaiveOverSynced * syncedMedian,
               "naive is not clearly worse than synchronised");
        expect(ratio >= kPredictionLow && ratio <= kPredictionHigh,
               "the naive error is not speed x latency: something else is off");
        expect(hiddenAfter_ <= hiddenLimit,
               "REQ-HUD-001: the ring stayed too long after the arm link was cut");
        expect(hiddenStatus_ == QLatin1String(msc::hud::toString(msc::hud::HudState::Stale)),
               "the ring was hidden for another reason than missing arm data");
        expect(backAfter_ <= milliseconds(kBackWithin), "the ring took too long to come back with the link");
        expect(backError_ <= kSyncedMaxLimit, "the ring came back in the wrong place");
        finish(pass);
    }

    void fail(const char* why) {
        if (finished_) {
            return;
        }
        std::printf("  FAIL - %s\n", why);
        finish(false);
    }

    void finish(bool pass) {
        if (finished_) {
            return;
        }
        finished_ = true;
        faults_.setLink(false);
        std::printf("hud check: %s\n", pass ? "PASS" : "FAIL");
        std::fflush(stdout);
        QCoreApplication::exit(pass ? 0 : 1);
    }

    QQuickWindow& window_;
    VideoItem& video_;
    TeleopModel& teleop_;
    FaultInjector& faults_;
    const msc::hud::ArrivalJitter& arrivals_;
    std::uint32_t frameWidth_;
    std::uint32_t frameHeight_;
    QTimer timer_;
    Phase phase_ = Phase::WaitReady;
    Nanoseconds phaseStarted_{0};
    std::vector<Sighting> synced_;
    std::vector<Sighting> naive_;
    std::size_t syncedMissed_ = 0;
    std::size_t naiveMissed_ = 0;
    double hiddenAfter_ = 0.0;
    double backAfter_ = 0.0;
    double backError_ = 0.0;
    QString hiddenStatus_;
    bool finished_ = false;
};

}  // namespace

void startHudCheck(QQuickWindow& window, VideoItem& video, TeleopModel& teleop, FaultInjector& faults,
                   const msc::hud::ArrivalJitter& arrivals, std::uint32_t frameWidth,
                   std::uint32_t frameHeight) {
    new HudCheck(window, video, teleop, faults, arrivals, frameWidth, frameHeight);  // owned by the window
}
