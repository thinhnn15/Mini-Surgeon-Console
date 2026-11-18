#include "msc/common/clock.hpp"
#include "msc/video/synthetic_source.hpp"
#include "video_check.hpp"
#include "video_item.hpp"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QTimer>

#include <memory>

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("Mini Surgeon Console"));
    QGuiApplication::setApplicationVersion(QStringLiteral(MSC_VERSION));

    const QStringList args = QCoreApplication::arguments();
    const bool smokeTest = args.contains(QStringLiteral("--smoke"));
    const bool videoCheck = args.contains(QStringLiteral("--check-video"));

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
    video->setFrameSource(source);
    source->start();

    if (smokeTest) {
        QTimer::singleShot(0, &app, &QCoreApplication::quit);
    } else if (videoCheck) {
        startVideoCheck(*window, *video, *source, videoConfig);
    }
    return QGuiApplication::exec();
}
