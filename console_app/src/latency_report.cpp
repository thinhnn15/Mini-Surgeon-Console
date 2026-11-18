#include "latency_report.hpp"

#include "graphics_api.hpp"
#include "msc/video/presentation_tracker.hpp"
#include "video_item.hpp"

#include <QCoreApplication>
#include <QQuickWindow>
#include <QScreen>
#include <QTimer>

#include <chrono>
#include <cstdio>
#include <optional>

namespace {

using namespace std::chrono_literals;

// Start-up is not what a surgeon sees for an hour: shader compilation, the first swap chain,
// the window appearing. Leave it out.
constexpr auto kWarmUp = 2s;

double ms(const std::optional<msc::Nanoseconds>& t) {
    return t ? std::chrono::duration<double, std::milli>(*t).count() : 0.0;
}

void report(const QQuickWindow& window, const msc::video::PresentationStats& s) {
    const bool vsync = window.requestedFormat().swapInterval() != 0;
    std::printf("latency: %s, vsync %s, screen %s at %.0f Hz\n",
                graphicsApiName(window.rendererInterface()->graphicsApi()), vsync ? "on" : "off",
                qPrintable(window.screen()->name()), window.screen()->refreshRate());

    double seconds = 0.0;
    if (s.firstSwap && s.lastSwap && *s.lastSwap > *s.firstSwap) {
        seconds = std::chrono::duration<double>(*s.lastSwap - *s.firstSwap).count();
    }
    const double renderFps = seconds > 0.0 ? static_cast<double>(s.swaps - 1) / seconds : 0.0;
    const double videoFps = seconds > 0.0 ? static_cast<double>(s.framesShown) / seconds : 0.0;
    std::printf("  over %.1f s: render %.1f fps, video %.1f fps, %llu frames shown, %llu dropped\n", seconds,
                renderFps, videoFps, static_cast<unsigned long long>(s.framesShown),
                static_cast<unsigned long long>(s.framesDropped));
    std::printf("  capture -> present: p50 %.2f ms, p99 %.2f ms, max %.2f ms (%zu samples)\n",
                ms(s.captureToPresentP50), ms(s.captureToPresentP99), ms(s.captureToPresentMax), s.samples);
    std::printf("  capture -> upload:  p50 %.2f ms, p99 %.2f ms\n", ms(s.captureToUploadP50),
                ms(s.captureToUploadP99));
}

}  // namespace

void startLatencyMeasurement(QQuickWindow& window, VideoItem& item, std::chrono::seconds duration) {
    QTimer::singleShot(kWarmUp, &item, [&window, &item, duration] {
        item.resetStats();
        QTimer::singleShot(duration, &item, [&window, &item] {
            const msc::video::PresentationStats& s = item.stats();
            report(window, s);
            const bool measured = s.samples > 0 && s.captureToPresentP50 && s.captureToPresentP99;
            if (!measured) {
                std::printf("  FAIL - no frame was presented, so nothing was measured\n");
            }
            std::fflush(stdout);
            QCoreApplication::exit(measured ? 0 : 1);
        });
    });
}
