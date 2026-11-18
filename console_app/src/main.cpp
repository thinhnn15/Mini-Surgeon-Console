#include "latency_report.hpp"
#include "msc/common/clock.hpp"
#include "msc/video/synthetic_source.hpp"
#include "video_check.hpp"
#include "video_item.hpp"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QSurfaceFormat>
#include <QTimer>

#include <chrono>
#include <memory>
#include <optional>

namespace {

// `--measure-latency` (10 s) or `--measure-latency=SECONDS`.
struct MeasureOption {
    bool given = false;
    std::optional<std::chrono::seconds> duration;  // empty if given but not a positive integer
};

MeasureOption measureOption(const QStringList& args) {
    const QString option = QStringLiteral("--measure-latency");
    for (const QString& arg : args) {
        if (arg == option) {
            return {true, std::chrono::seconds(10)};
        }
        if (arg.startsWith(option + QLatin1Char('='))) {
            bool ok = false;
            const int seconds = arg.mid(option.size() + 1).toInt(&ok);
            if (ok && seconds > 0) {
                return {true, std::chrono::seconds(seconds)};
            }
            return {true, std::nullopt};
        }
    }
    return {};
}

}  // namespace

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("Mini Surgeon Console"));
    QGuiApplication::setApplicationVersion(QStringLiteral(MSC_VERSION));

    const QStringList args = QCoreApplication::arguments();
    const bool smokeTest = args.contains(QStringLiteral("--smoke"));
    const bool videoCheck = args.contains(QStringLiteral("--check-video"));
    const MeasureOption measure = measureOption(args);
    if (measure.given && !measure.duration) {
        qCritical("console_app: --measure-latency=SECONDS needs a positive whole number of seconds");
        return 1;
    }

    // --no-vsync: present as soon as a frame is drawn instead of waiting for the display's
    // refresh. For measuring what vsync costs in latency (and what it buys: no tearing).
    // Must be set before the window is created.
    if (args.contains(QStringLiteral("--no-vsync"))) {
        QSurfaceFormat format = QSurfaceFormat::defaultFormat();
        format.setSwapInterval(0);
        QSurfaceFormat::setDefaultFormat(format);
    }

    // Declaration order is destruction order, reversed: the engine (window, render thread,
    // renderer) goes first, then the source, then the clock the source stamps frames with.
    const msc::SteadyClock clock;
    const msc::video::SyntheticSourceConfig videoConfig{};
    const auto source = std::make_shared<msc::video::SyntheticSource>(clock, videoConfig);

    QQmlApplicationEngine engine;
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app, [] { QCoreApplication::exit(1); },
        Qt::QueuedConnection);
    engine.loadFromModule("MiniConsole", "Main");
    if (engine.rootObjects().isEmpty()) {
        return 1;
    }

    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst());
    auto* video = window != nullptr ? window->findChild<VideoItem*>(QStringLiteral("video")) : nullptr;
    if (video == nullptr) {
        qCritical("console_app: Main.qml has no VideoItem named \"video\"");
        return 1;
    }
    video->setFrameSource(source, clock);
    source->start();

    if (smokeTest) {
        QTimer::singleShot(0, &app, &QCoreApplication::quit);
    } else if (videoCheck) {
        startVideoCheck(*window, *video, *source, videoConfig);
    } else if (measure.duration) {
        // On top for the same reason as the video check: a window hidden behind others may be
        // throttled, and that is not the latency being measured.
        window->setFlag(Qt::WindowStaysOnTopHint, true);
        startLatencyMeasurement(*window, *video, *measure.duration);
    }
    return QGuiApplication::exec();
}
