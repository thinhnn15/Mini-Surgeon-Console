#include "fault_injector.hpp"
#include "freeze_check.hpp"
#include "latency_report.hpp"
#include "msc/common/clock.hpp"
#include "msc/comms/arm_link.hpp"
#include "msc/comms/udp_socket.hpp"
#include "msc/teleop/teleop_core.hpp"
#include "msc/video/frame_source.hpp"
#include "msc/video/synthetic_source.hpp"
#include "teleop_model.hpp"
#include "video_check.hpp"
#include "video_item.hpp"

#if MSC_HAVE_CAMERA
#include "camera_source.hpp"
#endif

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QSurfaceFormat>
#include <QTimer>

#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace {

// `--name=value` or `--name value`. Nothing if the option is absent; an empty string if it is
// given without a value.
std::optional<QString> optionValue(const QStringList& args, const QString& name) {
    for (qsizetype i = 1; i < args.size(); ++i) {
        if (args[i] == name) {
            return i + 1 < args.size() ? args[i + 1] : QString();
        }
        if (args[i].startsWith(name + QLatin1Char('='))) {
            return args[i].mid(name.size() + 1);
        }
    }
    return std::nullopt;
}

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
    const bool freezeCheck = args.contains(QStringLiteral("--check-freeze"));
    const MeasureOption measure = measureOption(args);
    if (measure.given && !measure.duration) {
        qCritical("console_app: --measure-latency=SECONDS needs a positive whole number of seconds");
        return 1;
    }

    // --source synthetic (default) | camera. The rest of the console does not know which.
    const QString sourceName =
        optionValue(args, QStringLiteral("--source")).value_or(QStringLiteral("synthetic"));
    const bool wantCamera = sourceName == QLatin1String("camera");
    if (!wantCamera && sourceName != QLatin1String("synthetic")) {
        qCritical("console_app: --source must be \"synthetic\" or \"camera\"");
        return 1;
    }
    // Both checks need a source they can stop, start and read the frame number from.
    if (wantCamera && (videoCheck || freezeCheck)) {
        qCritical("console_app: --check-video and --check-freeze use the synthetic source");
        return 1;
    }
    if (args.contains(QStringLiteral("--list-cameras"))) {
#if MSC_HAVE_CAMERA
        CameraSource::listDevices();
        return 0;
#else
        std::printf("Built without Qt Multimedia: no camera support.\n");
        return 1;
#endif
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
    // renderer) goes first, then the sources, then the clock they stamp frames with. The
    // sources are destroyed here, on the GUI thread, after the renderer has let go of them
    // (the camera's Qt Multimedia objects must be deleted on this thread).
    const msc::SteadyClock clock;
    const msc::video::SyntheticSourceConfig videoConfig{};
    std::shared_ptr<msc::video::FrameSource> source;
    std::shared_ptr<msc::video::SyntheticSource> synthetic;
    QString fallbackReason;  // why the camera was asked for but the pattern is shown
#if MSC_HAVE_CAMERA
    std::shared_ptr<CameraSource> camera;
    if (wantCamera) {
        // --camera=TEXT picks a camera by name or id; without it, the system default.
        const QString wanted = optionValue(args, QStringLiteral("--camera")).value_or(QString());
        if (const std::optional<QCameraDevice> device = CameraSource::findDevice(wanted)) {
            camera = std::make_shared<CameraSource>(clock, *device);
            source = camera;
        } else {
            fallbackReason = wanted.isEmpty() ? QStringLiteral("no camera found")
                                              : QStringLiteral("no camera matches \"%1\"").arg(wanted);
        }
    }
#else
    if (wantCamera) {
        fallbackReason = QStringLiteral("built without camera support");
    }
#endif
    if (!source) {
        // Without a camera, the pattern: clearly labelled, so it is never mistaken for one.
        if (!fallbackReason.isEmpty()) {
            qWarning("console_app: %s, showing the synthetic test pattern instead",
                     qPrintable(fallbackReason));
        }
        synthetic = std::make_shared<msc::video::SyntheticSource>(clock, videoConfig);
        source = synthetic;
    }

    // The arm link (M3): its own thread, heartbeats every 10 ms. --arm=A.B.C.D:PORT is where
    // arm_sim listens. Our own port is picked by the OS; the arm answers whoever talks to it.
    std::optional<msc::comms::ArmLink> armLink;
    {
        msc::comms::ArmLinkConfig linkConfig;
        if (const std::optional<QString> arm = optionValue(args, QStringLiteral("--arm"))) {
            const auto endpoint = msc::comms::Endpoint::parse(arm->toStdString());
            if (!endpoint) {
                qCritical("console_app: --arm needs A.B.C.D:PORT");
                return 1;
            }
            linkConfig.arm = *endpoint;
        }
        // On the loopback when the arm is (the default): nothing outside the machine can reach the
        // socket, and Windows does not ask for a firewall exception.
        const bool local = linkConfig.arm.address[0] == 127;
        const msc::comms::Endpoint bindTo =
            local ? msc::comms::Endpoint::loopback(0) : msc::comms::Endpoint{{0, 0, 0, 0}, 0};
        std::string error;
        if (std::optional<msc::comms::UdpSocket> socket = msc::comms::UdpSocket::open(bindTo, &error)) {
            armLink.emplace(clock, std::move(*socket), linkConfig);
            armLink->start();
        } else {
            qWarning("console_app: no arm link: %s", error.c_str());
        }
    }

    // The teleop core (M4): its own 200 Hz thread, the arm link's only reader. Declared after the
    // link, so it stops first; before the engine, so the QML model never outlives it.
    std::optional<msc::teleop::TeleopCore> teleop;
    if (armLink) {
        teleop.emplace(clock, *armLink);
        teleop->start();
    }

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
    if (auto* teleopModel = window->findChild<TeleopModel*>(QStringLiteral("teleop"));
        teleopModel != nullptr && teleop) {
        teleopModel->setCore(&*teleop, &clock, args.contains(QStringLiteral("--hand-demo")));
        // REQ-VID-003 -> M4.2: a picture that is not live (frozen, or no frame yet) stops
        // tele-operation. The core reads the flag; it never waits for the video.
        const auto updateVideo = [&teleop, video] {
            teleop->setVideoLive(video->videoState() == VideoItem::VideoState::Live);
        };
        QObject::connect(video, &VideoItem::videoStateChanged, teleopModel, updateVideo);
        updateVideo();

        // M4.4: faults on purpose, from the status bar or --inject=NAME@START[-END],...
        if (auto* faults = window->findChild<FaultInjector*>(QStringLiteral("faults")); faults != nullptr) {
            faults->setTargets(&*armLink, &*teleop, teleopModel, source.get());
            if (const std::optional<QString> script = optionValue(args, QStringLiteral("--inject"))) {
                QString error;
                if (!faults->schedule(*script, &error)) {
                    qCritical("console_app: --inject: %s", qPrintable(error));
                    return 1;
                }
            }
        }
    }
    std::function<void()> reportSource;
    if (synthetic) {
        video->setSourceStatus(fallbackReason.isEmpty()
                                   ? QStringLiteral("Synthetic test pattern")
                                   : QStringLiteral("Synthetic test pattern (%1)").arg(fallbackReason));
    }
#if MSC_HAVE_CAMERA
    if (camera) {
        QObject::connect(camera.get(), &CameraSource::statusChanged, video, &VideoItem::setSourceStatus);
        reportSource = [&camera] {
            const CameraSource::Report r = camera->report();
            const auto ms = [](const std::optional<msc::Nanoseconds>& t) {
                return t ? std::chrono::duration<double, std::milli>(*t).count() : 0.0;
            };
            std::printf(
                "  camera: %s %dx%d, delivered on %s, %llu frames converted%s, %llu replaced before "
                "conversion\n",
                qPrintable(r.pixelFormat), r.size.width(), r.size.height(),
                r.deliveredOnGuiThread ? "the GUI thread" : "a backend thread",
                static_cast<unsigned long long>(r.frames), r.viaImage ? " by Qt (toImage)" : "",
                static_cast<unsigned long long>(r.replaced));
            std::printf("  camera: conversion to RGBA p50 %.2f ms, p99 %.2f ms (camera thread)\n",
                        ms(r.conversionP50), ms(r.conversionP99));
        };
    }
#endif
    source->start();
#if MSC_HAVE_CAMERA
    if (camera) {
        video->setSourceStatus(camera->status());
    }
#endif

    if (smokeTest) {
        QTimer::singleShot(0, &app, &QCoreApplication::quit);
    } else if (const std::optional<QString> shot = optionValue(args, QStringLiteral("--screenshot"))) {
        // --screenshot=FILE: let the window settle, save what it shows, quit. For the README and
        // for checking the layout without a desktop (QT_QPA_PLATFORM=offscreen works).
        QTimer::singleShot(std::chrono::milliseconds(2500), window, [window, file = *shot] {
            const bool saved = window->grabWindow().save(file);
            if (!saved) {
                qCritical("console_app: could not save %s", qPrintable(file));
            }
            QCoreApplication::exit(saved ? 0 : 1);
        });
    } else if (videoCheck) {
        startVideoCheck(*window, *video, *synthetic, videoConfig);
    } else if (freezeCheck) {
        window->setFlag(Qt::WindowStaysOnTopHint, true);
        startFreezeCheck(*window, *video, *source);
    } else if (measure.duration) {
        // On top for the same reason as the video check: a window hidden behind others may be
        // throttled, and that is not the latency being measured.
        window->setFlag(Qt::WindowStaysOnTopHint, true);
        startLatencyMeasurement(*window, *video, *measure.duration, reportSource);
    }
    return QGuiApplication::exec();
}
