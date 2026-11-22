#include "fault_injector.hpp"
#include "freeze_check.hpp"
#include "hud_check.hpp"
#include "latency_report.hpp"
#include "msc/common/clock.hpp"
#include "msc/comms/arm_link.hpp"
#include "msc/comms/udp_socket.hpp"
#include "msc/hud/arrival_jitter.hpp"
#include "msc/hud/pose_feed.hpp"
#include "msc/teleop/teleop_core.hpp"
#include "msc/video/frame_source.hpp"
#include "msc/video/synthetic_source.hpp"
#include "soak_driver.hpp"
#include "teleop_model.hpp"
#include "tool_scene.hpp"
#include "video_check.hpp"
#include "video_item.hpp"

#if MSC_HAVE_CAMERA
#include "camera_source.hpp"
#endif

#include <QGuiApplication>
#include <QProcess>
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

// `--soak=SECONDS [--soak-interval=SECONDS]`: nothing if not given; an error text if a value is not
// a positive whole number.
struct SoakOption {
    std::optional<SoakOptions> options;
    QString error;
};

SoakOption soakOption(const QStringList& args) {
    const std::optional<QString> duration = optionValue(args, QStringLiteral("--soak"));
    if (!duration) {
        return {};
    }
    const auto seconds = [](const QString& text) {
        bool ok = false;
        const int value = text.toInt(&ok);
        return ok && value > 0 ? std::optional<std::chrono::seconds>(value) : std::nullopt;
    };
    SoakOptions options;
    if (const auto d = seconds(*duration)) {
        options.duration = *d;
    } else {
        return {std::nullopt, QStringLiteral("--soak=SECONDS needs a positive whole number of seconds")};
    }
    if (const std::optional<QString> interval = optionValue(args, QStringLiteral("--soak-interval"))) {
        if (const auto i = seconds(*interval)) {
            options.interval = *i;
        } else {
            return {std::nullopt,
                    QStringLiteral("--soak-interval=SECONDS needs a positive whole number of seconds")};
        }
    }
    return {options, {}};
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
    const bool hudCheck = args.contains(QStringLiteral("--check-hud"));
    const SoakOption soak = soakOption(args);
    if (!soak.error.isEmpty()) {
        qCritical("console_app: %s", qPrintable(soak.error));
        return 1;
    }
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
    // The checks need a source they can stop, start and read from (the frame number, the
    // instrument the simulated camera draws).
    if (wantCamera && (videoCheck || freezeCheck || hudCheck)) {
        qCritical("console_app: --check-video, --check-freeze and --check-hud use the synthetic source");
        return 1;
    }
    // --video-delay=MS: the synthetic camera delivers each frame that long after capturing it
    // (M5.2), as a real camera chain does. Above 150 ms the picture would be too close to the
    // frozen-picture limit (250 ms from capture, REQ-VID-003) to count as live.
    // The overlay check needs a delay to tell a synchronised overlay from a naive one: 80 ms
    // unless another is asked for.
    std::chrono::milliseconds videoDelay{hudCheck ? 80 : 0};
    if (const std::optional<QString> delay = optionValue(args, QStringLiteral("--video-delay"))) {
        bool ok = false;
        const int ms = delay->toInt(&ok);
        if (!ok || ms < 0 || ms > 150) {
            qCritical("console_app: --video-delay=MS needs a whole number of milliseconds from 0 to 150");
            return 1;
        }
        if (wantCamera) {
            qCritical("console_app: --video-delay applies to the synthetic source");
            return 1;
        }
        videoDelay = std::chrono::milliseconds(ms);
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
    // The arm's poses, from the arm link's thread to whoever draws the arm (M5). Created first:
    // the synthetic camera and the arm link both hold on to it.
    const auto poses = std::make_shared<msc::hud::PoseFeed>();
    // How late telemetry arrives (M5.4): what stamping poses with their arrival time costs.
    const auto arrivals = std::make_shared<msc::hud::ArrivalJitter>();
    msc::video::SyntheticSourceConfig videoConfig;
    videoConfig.delay = videoDelay;
    videoConfig.scene = makeToolScene(poses);  // the camera sees the instrument
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

    // --arm-exe=PATH: start an arm_sim of our own, on a free loopback port, and stop it when
    // the console exits. For --check-hud under CTest, which has no arm running. Declared
    // before the arm link, so the link stops first.
    QProcess armProcess;
    std::optional<msc::comms::Endpoint> ownArm;
    if (const std::optional<QString> armExe = optionValue(args, QStringLiteral("--arm-exe"))) {
        std::uint16_t port = 0;
        {
            // Ask the OS for a free port, and give it back for arm_sim to take.
            const std::optional<msc::comms::UdpSocket> probe =
                msc::comms::UdpSocket::open(msc::comms::Endpoint::loopback(0));
            port = probe ? probe->localEndpoint().port : std::uint16_t{0};
        }
        ownArm = msc::comms::Endpoint::loopback(port);
        armProcess.setProcessChannelMode(QProcess::ForwardedChannels);  // its log into ours
        armProcess.start(*armExe,
                         {QStringLiteral("--listen=%1").arg(QString::fromStdString(ownArm->toString()))});
        if (port == 0 || !armProcess.waitForStarted(5000)) {
            qCritical("console_app: --arm-exe: could not start %s", qPrintable(*armExe));
            return 1;
        }
    }

    // The arm link (M3): its own thread, heartbeats every 10 ms. --arm=A.B.C.D:PORT is where
    // arm_sim listens. Our own port is picked by the OS; the arm answers whoever talks to it.
    std::optional<msc::comms::ArmLink> armLink;
    {
        msc::comms::ArmLinkConfig linkConfig;
        if (ownArm) {
            linkConfig.arm = *ownArm;
        } else if (const std::optional<QString> arm = optionValue(args, QStringLiteral("--arm"))) {
            const auto endpoint = msc::comms::Endpoint::parse(arm->toStdString());
            if (!endpoint) {
                qCritical("console_app: --arm needs A.B.C.D:PORT");
                return 1;
            }
            linkConfig.arm = *endpoint;
        }
        // On the loopback when the arm is (the default): nothing outside the machine can reach the
        // socket, and Windows does not ask for a firewall exception.
        // Every telemetry packet, stamped with its arrival on our clock, the clock the video
        // frames are stamped with. Runs on the link thread: lock-free hand-overs only.
        linkConfig.onTelemetry = [poses, arrivals](const msc::comms::TelemetrySample& sample) {
            arrivals->add(sample.receivedAt, sample.armTime);
            poses->publish(
                msc::hud::TimedPose{.time = sample.receivedAt, .position = sample.telemetry.position});
        };
        const bool local = linkConfig.arm.address[0] == 127;
        const msc::comms::Endpoint bindTo =
            local ? msc::comms::Endpoint::loopback(0) : msc::comms::Endpoint{{0, 0, 0, 0}, 0};
        std::string error;
        if (std::optional<msc::comms::UdpSocket> socket = msc::comms::UdpSocket::open(bindTo, &error)) {
            armLink.emplace(clock, std::move(*socket), std::move(linkConfig));
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
    video->setPoseFeed(poses);
    // --hud=synced (default) | naive | off: how the overlay's marker is placed (M5.3). Also
    // switched with H in the window.
    if (const std::optional<QString> hud = optionValue(args, QStringLiteral("--hud"))) {
        if (*hud == QLatin1String("synced")) {
            video->setHudMode(VideoItem::HudMode::HudSynced);
        } else if (*hud == QLatin1String("naive")) {
            video->setHudMode(VideoItem::HudMode::HudNaive);
        } else if (*hud == QLatin1String("off")) {
            video->setHudMode(VideoItem::HudMode::HudOff);
        } else {
            qCritical("console_app: --hud must be \"synced\", \"naive\" or \"off\"");
            return 1;
        }
    }
    auto* teleopModel = window->findChild<TeleopModel*>(QStringLiteral("teleop"));
    FaultInjector* faults = nullptr;
    if (teleopModel != nullptr && teleop) {
        // The overlay check and the soak test drive the arm with the scripted hand.
        teleopModel->setCore(
            &*teleop, &clock,
            args.contains(QStringLiteral("--hand-demo")) || hudCheck || soak.options.has_value());
        // REQ-VID-003 -> M4.2: a picture that is not live (frozen, or no frame yet) stops
        // tele-operation. The core reads the flag; it never waits for the video.
        const auto updateVideo = [&teleop, video] {
            teleop->setVideoLive(video->videoState() == VideoItem::VideoState::Live);
        };
        QObject::connect(video, &VideoItem::videoStateChanged, teleopModel, updateVideo);
        updateVideo();

        // M4.4: faults on purpose, from the status bar or --inject=NAME@START[-END],...
        faults = window->findChild<FaultInjector*>(QStringLiteral("faults"));
        if (faults != nullptr) {
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
        QString status = fallbackReason.isEmpty()
                             ? QStringLiteral("Synthetic test pattern")
                             : QStringLiteral("Synthetic test pattern (%1)").arg(fallbackReason);
        if (videoDelay.count() > 0) {
            status += QStringLiteral(", delayed %1 ms").arg(videoDelay.count());
        }
        video->setSourceStatus(status);
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
    } else if (soak.options) {
        if (teleopModel == nullptr || faults == nullptr || !teleop) {
            qCritical("console_app: --soak needs the arm link, the teleop model and the fault injector");
            return 1;
        }
        // On top, like the other checks: a window hidden behind others may be throttled.
        window->setFlag(Qt::WindowStaysOnTopHint, true);
        startSoak(*window, *video, *teleopModel, *faults, clock, *soak.options);
    } else if (hudCheck) {
        if (teleopModel == nullptr || faults == nullptr || !teleop) {
            qCritical("console_app: --check-hud needs the arm link, the teleop model and the fault injector");
            return 1;
        }
        window->setFlag(Qt::WindowStaysOnTopHint, true);
        startHudCheck(*window, *video, *teleopModel, *faults, *arrivals, videoConfig.width,
                      videoConfig.height);
    } else if (measure.duration) {
        // On top for the same reason as the video check: a window hidden behind others may be
        // throttled, and that is not the latency being measured.
        window->setFlag(Qt::WindowStaysOnTopHint, true);
        startLatencyMeasurement(*window, *video, *measure.duration, reportSource);
    }
    const int result = QGuiApplication::exec();
    if (armProcess.state() != QProcess::NotRunning) {
        armProcess.kill();  // our own arm_sim: it has no other way to know the console is done
        armProcess.waitForFinished(3000);
    }
    return result;
}
