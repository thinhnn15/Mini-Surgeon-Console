#include "video_check.hpp"

#include "graphics_api.hpp"
#include "msc/video/frame.hpp"
#include "msc/video/letterbox.hpp"
#include "msc/video/test_pattern.hpp"
#include "video_item.hpp"

#include <QCoreApplication>
#include <QImage>
#include <QObject>
#include <QQuickWindow>
#include <QScreen>
#include <QTimer>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>

namespace {

using namespace std::chrono_literals;

// Frames the window must present (after showing, and after each resize) before the check
// grabs it. Counted rather than timed: with OpenGL the window took long enough to come up
// that a fixed 1.5 s wait sometimes grabbed a still blank (all white) window.
constexpr int kWarmUpFrames = 30;
// Each grab waits this many frames longer than the one before. A fault that repeats every N
// frames could otherwise line up with the grabs every time: a renderer sabotaged to take a
// new frame only every 30th draw passed with a fixed 30-frame wait, and failed with this.
constexpr int kWarmUpStagger = 7;

// Resizing is asynchronous on every desktop: the window manager may take a while, adjust the
// size, or (with screens at different scaling) move the window to another screen. Before
// checking, the window must have the size asked for and the grab must agree with it; if not,
// the window is placed again and given another warm-up, this many times at most. Only the
// geometry is retried: a wrong picture fails at once.
constexpr int kMaxSettleAttempts = 10;
// Window managers round sizes a little (asked for 1700 wide, got 1702).
constexpr int kSizeTolerance = 16;

// If the whole check has not finished by then, the window is not rendering.
constexpr auto kGiveUpAfter = 45s;

// How many frames older than the newest one the screen may be. The grab renders a fresh Qt
// Quick frame, so the item normally picks up the newest frame there and then; one or two
// frames of slack cover a renderer that reused the frame from the vsync just before.
// A queue between source and screen would show up as a larger gap.
constexpr std::uint64_t kMaxFramesBehind = 2;

// The bars around the picture must be this dark on every channel (they are cleared to black).
constexpr int kBarMaxChannel = 16;
// Bars thinner than this are neither checked nor expected.
constexpr int kMinBar = 4;

// The window sizes checked, in order. The first is the size from Main.qml; the others force
// bars on different sides, so a stretched picture or a renderer that ignores a resize would
// show up as a missing bar or an unreadable barcode. Sizes are shrunk to fit the screen, so
// each one must keep its shape on a small screen too: the CI runner's is 1024x768, where the
// window becomes at most 984x648. Wide at 360 high leaves the video area well over 16:9 even
// at 984 wide (about 656x292); an earlier 1700x560 shrank to a shape with bars top and bottom.
enum class Bars : std::uint8_t { Any, TopAndBottom, LeftAndRight };
struct Step {
    const char* name;
    QSize windowSize;  // empty = keep the size the window opened with
    Bars bars;         // where this size must put the bars
};
constexpr std::array kSteps{
    Step{"default size", QSize(), Bars::Any},
    Step{"tall, bars top and bottom", QSize(720, 900), Bars::TopAndBottom},
    Step{"wide, bars left and right", QSize(1700, 360), Bars::LeftAndRight},
};

// Centres the window on its screen at (at most) the wanted size and returns the size asked
// for. A window that straddles two screens with different scaling can jump to the other
// screen when resized; seen on a 100 % + 125 % setup, where the device pixel ratio changed
// under the check. Wholly on one screen, it normally stays there.
QSize placeOnItsScreen(QQuickWindow& window, QSize wanted) {
    const QRect area = window.screen()->availableGeometry();
    // Room for the title bar and frame, which are outside the window's geometry.
    const QSize frameAllowance(40, 80);
    QRect geometry(QPoint(), wanted.boundedTo(area.size() - frameAllowance));
    geometry.moveCenter(area.center());
    window.setGeometry(geometry);
    return geometry.size();
}

bool near(int a, int b, int tolerance) {
    return std::abs(a - b) <= tolerance;
}

bool isBar(const QImage& grab, int x, int y) {
    const QColor c = grab.pixelColor(x, y);
    return c.red() <= kBarMaxChannel && c.green() <= kBarMaxChannel && c.blue() <= kBarMaxChannel;
}

// Drives the check step by step from the window's frameSwapped signal. Lives as a child of
// the window, on the GUI thread, and ends the application when done.
class VideoCheck : public QObject {
public:
    VideoCheck(QQuickWindow& window, const VideoItem& item, const msc::video::SyntheticSource& source,
               msc::video::SyntheticSourceConfig config)
        : QObject(&window),
          window_(window),
          item_(item),
          source_(source),
          config_(config),
          defaultSize_(window.size()) {
        // frameSwapped is emitted on the render thread; the queued connection counts on the
        // GUI thread, where grabbing and resizing are allowed.
        connect(&window_, &QQuickWindow::frameSwapped, this, &VideoCheck::onFramePresented,
                Qt::QueuedConnection);
        // Keep the window above everything else while the check runs. Windows does not let a
        // new window take the foreground from the app the user is working in, so on a desktop
        // in use it can open behind other windows, and what is grabbed should not depend on
        // that. (It does not help when the display itself is off: on the dev PC, with the
        // monitors probably asleep during a remote session, every backend dropped to a few
        // frames per second and the check timed out. Run it with the display on.)
        window_.setFlag(Qt::WindowStaysOnTopHint, true);
        place();
        QTimer::singleShot(kGiveUpAfter, this, [this] {
            std::printf("video check: FAIL - not finished after %lld s (step %zu, %d frames presented)\n",
                        static_cast<long long>(kGiveUpAfter.count()), step_ + 1, presented_);
            finish(false);
        });
    }

private:
    enum class Outcome : std::uint8_t { Pass, Fail, NotSettled };

    void place() {
        const QSize wanted = kSteps.at(step_).windowSize;
        asked_ = placeOnItsScreen(window_, wanted.isEmpty() ? defaultSize_ : wanted);
    }

    void onFramePresented() {
        if (finished_) {
            return;
        }
        // The software backend never draws the item, so the window stops presenting frames and
        // the warm-up would never end. The API is known once the first frame is out: say so now.
        if (presented_ == 0 && grabs_ == 0 &&
            window_.rendererInterface()->graphicsApi() == QSGRendererInterface::Software) {
            ++grabs_;
            static_cast<void>(checkStep(kSteps.at(step_)));  // reports and saves the grab
            finish(false);
            return;
        }
        // Cycles 30, 37, 44, 51, 58 frames, so retries cannot stretch the wait without bound.
        if (++presented_ < kWarmUpFrames + (kWarmUpStagger * (grabs_ % 5))) {
            return;
        }
        presented_ = 0;
        ++grabs_;
        switch (checkStep(kSteps.at(step_))) {
            case Outcome::Fail:
                finish(false);
                return;
            case Outcome::NotSettled:
                if (++settleAttempts_ > kMaxSettleAttempts) {
                    std::printf("  FAIL - the window did not settle at the size asked for\n");
                    finish(false);
                    return;
                }
                place();  // again: it may have moved to another screen
                return;
            case Outcome::Pass:
                break;
        }
        settleAttempts_ = 0;
        if (++step_ == kSteps.size()) {
            finish(true);
            return;
        }
        place();
    }

    Outcome checkStep(const Step& step);

    void finish(bool pass) {
        if (finished_) {
            return;
        }
        finished_ = true;
        std::printf("video check: %s\n", pass ? "PASS" : "FAIL");
        std::fflush(stdout);
        QCoreApplication::exit(pass ? 0 : 1);
    }

    QQuickWindow& window_;
    const VideoItem& item_;
    const msc::video::SyntheticSource& source_;
    msc::video::SyntheticSourceConfig config_;
    QSize defaultSize_;
    QSize asked_;
    std::size_t step_ = 0;
    int presented_ = 0;
    int settleAttempts_ = 0;
    int grabs_ = 0;
    bool finished_ = false;
};

VideoCheck::Outcome VideoCheck::checkStep(const Step& step) {
    const auto api = window_.rendererInterface()->graphicsApi();
    const auto notSettled = [](const char* what) {
        std::printf("  window not settled yet (%s); placing it again\n", what);
        return Outcome::NotSettled;
    };

    if (!near(window_.width(), asked_.width(), kSizeTolerance) ||
        !near(window_.height(), asked_.height(), kSizeTolerance)) {
        std::printf("video check [%s]: window %dx%d, asked for %dx%d (state %d, visibility %d, screen %s)\n",
                    step.name, window_.width(), window_.height(), asked_.width(), asked_.height(),
                    static_cast<int>(window_.windowStates().toInt()), static_cast<int>(window_.visibility()),
                    qPrintable(window_.screen()->name()));
        return notSettled("size differs from what was asked");
    }

    // Frame numbers before and after the grab bracket what the screen can possibly show.
    const std::uint64_t producedBefore = source_.framesProduced();
    const QImage grab = window_.grabWindow().convertToFormat(QImage::Format_RGBA8888);
    const std::uint64_t producedAfter = source_.framesProduced();

    std::printf("video check [%s]: %s, window %dx%d, grab %dx%d at device pixel ratio %.2f\n", step.name,
                graphicsApiName(api), window_.width(), window_.height(), grab.width(), grab.height(),
                grab.devicePixelRatio());

    const auto fail = [&grab](const char* why) {
        const QString path = QStringLiteral("video_check_failed.png");
        grab.save(path);
        std::printf("  FAIL - %s; window grab saved to %s\n", why, qPrintable(path));
        return Outcome::Fail;
    };

    if (api == QSGRendererInterface::Software) {
        return fail("the software backend cannot draw the video item (it shows a notice instead)");
    }

    // Where the item is in the grab, and where the picture should be inside it.
    const qreal dpr = grab.devicePixelRatio();
    if (!near(grab.width(), qRound(window_.width() * dpr), 2) ||
        !near(grab.height(), qRound(window_.height() * dpr), 2)) {
        return notSettled("grab size does not match the window");
    }
    const QPointF topLeft = item_.mapToScene(QPointF(0, 0));
    const QRect itemPx(qRound(topLeft.x() * dpr), qRound(topLeft.y() * dpr), qRound(item_.width() * dpr),
                       qRound(item_.height() * dpr));
    // One pixel of slack: at a fractional ratio such as 1.25 the rounded item can stick out.
    if (itemPx.width() <= 0 || itemPx.height() <= 0 || !grab.rect().adjusted(-1, -1, 1, 1).contains(itemPx)) {
        return notSettled("item geometry does not match the grab");
    }
    const msc::video::PixelRect box =
        msc::video::letterbox(config_.width, config_.height, static_cast<std::uint32_t>(itemPx.width()),
                              static_cast<std::uint32_t>(itemPx.height()));
    const QRect videoPx(itemPx.x() + static_cast<int>(box.x), itemPx.y() + static_cast<int>(box.y),
                        static_cast<int>(box.width), static_cast<int>(box.height));
    std::printf("  item %dx%d, picture %dx%d at (%d, %d) inside it\n", itemPx.width(), itemPx.height(),
                videoPx.width(), videoPx.height(), static_cast<int>(box.x), static_cast<int>(box.y));
    // The window has the size asked for and the grab agrees, so the item geometry is current:
    // bars on the wrong side mean the step's size does not suit this screen. Waiting would not
    // change that (it is what made the first CI run retry until it gave up).
    if ((step.bars == Bars::TopAndBottom && static_cast<int>(box.y) < kMinBar) ||
        (step.bars == Bars::LeftAndRight && static_cast<int>(box.x) < kMinBar)) {
        return fail("at this window size the bars are not where the step needs them (check the step sizes)");
    }

    // From here on, a mismatch is a wrong picture, not a window still moving: fail at once.

    // The bars: sampled in the middle of each one that is at least a few pixels thick.
    if (videoPx.top() - itemPx.top() >= kMinBar &&
        (!isBar(grab, itemPx.center().x(), (itemPx.top() + videoPx.top()) / 2) ||
         !isBar(grab, itemPx.center().x(), (videoPx.bottom() + itemPx.bottom()) / 2))) {
        return fail("the bars above and below the picture are not black (stretched picture?)");
    }
    if (videoPx.left() - itemPx.left() >= kMinBar &&
        (!isBar(grab, (itemPx.left() + videoPx.left()) / 2, itemPx.center().y()) ||
         !isBar(grab, (videoPx.right() + itemPx.right()) / 2, itemPx.center().y()))) {
        return fail("the bars left and right of the picture are not black (stretched picture?)");
    }

    // The picture: scaled back to the source's frame size, so the test pattern's own decoder
    // can read it. The barcode cells are sampled in their middle, so the blur from scaling
    // twice does not matter.
    const QImage picture = grab.copy(videoPx)
                               .scaled(static_cast<int>(config_.width), static_cast<int>(config_.height),
                                       Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                               .convertToFormat(QImage::Format_RGBA8888);
    msc::video::Frame frame = msc::video::Frame::allocate(config_.width, config_.height);
    const std::size_t stride = frame.strideBytes();
    for (std::uint32_t y = 0; y < frame.height; ++y) {
        std::memcpy(frame.pixels.data() + (y * stride), picture.constScanLine(static_cast<int>(y)), stride);
    }
    const auto shown = msc::video::decodeSequence(frame);
    std::printf("  produced %llu before the grab, %llu after; screen shows %s\n",
                static_cast<unsigned long long>(producedBefore),
                static_cast<unsigned long long>(producedAfter),
                shown ? std::to_string(*shown).c_str() : "no readable test pattern");

    // Frame numbers stay far below 2^32 in a check this short, so the 32-bit barcode is enough.
    if (!shown || *shown == 0 || *shown > producedAfter || *shown + kMaxFramesBehind < producedBefore) {
        return fail("the screen does not show the newest frame");
    }
    std::printf("  ok - newest frame on screen (%llu behind, limit %llu)\n",
                static_cast<unsigned long long>(producedBefore > *shown ? producedBefore - *shown : 0),
                static_cast<unsigned long long>(kMaxFramesBehind));
    return Outcome::Pass;
}

}  // namespace

void startVideoCheck(QQuickWindow& window, const VideoItem& item, const msc::video::SyntheticSource& source,
                     msc::video::SyntheticSourceConfig config) {
    new VideoCheck(window, item, source, config);  // owned by the window
}
