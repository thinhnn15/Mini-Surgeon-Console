#pragma once

#include "msc/common/clock.hpp"
#include "msc/teleop/teleop_core.hpp"

#include <QtQml/qqmlregistration.h>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>

#include <atomic>
#include <stop_token>
#include <thread>

// The operator's side of the teleop core, for QML. GUI thread.
//
// It is the hand controller: every 10 ms the hand's pose and clutch go to the core.
//   - From the pad in the window, on the GUI thread. If the GUI thread stalls for longer than the
//     core's hand timeout, the readings stop and the core faults, exactly as with a real hand
//     device that froze: the pad cannot know where the mouse went while the GUI was not looking.
//   - With --hand-demo, from a scripted hand on a thread of its own. A scripted hand needs
//     nothing from the window, so, like a real hand device, it does not depend on it (M5.2: a
//     desktop that stalled the GUI thread for 300 ms at a time kept faulting the demo).
//
// It only shows the core's status and passes on requests: whether an engage or a reset is
// allowed, and what state the console is in, is decided by the core alone.
//
// main.cpp finds it by objectName ("teleop") and hands it the core.
class TeleopModel : public QObject {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QString state READ state NOTIFY changed FINAL)  // "idle", "engaged", "paused", "fault", ""
    Q_PROPERTY(QString fault READ fault NOTIFY changed FINAL)  // the fault's reason, in Fault
    Q_PROPERTY(double faultAgeSeconds READ faultAgeSeconds NOTIFY changed FINAL)
    Q_PROPERTY(QStringList present READ present NOTIFY changed FINAL)  // fault conditions present now
    Q_PROPERTY(bool engageAllowed READ engageAllowed NOTIFY changed FINAL)
    Q_PROPERTY(bool resetAllowed READ resetAllowed NOTIFY changed FINAL)
    Q_PROPERTY(QString refusal READ refusal NOTIFY changed FINAL)
    Q_PROPERTY(QString link READ link NOTIFY changed FINAL)          // "waiting", "OK", "lost"
    Q_PROPERTY(QString armState READ armState NOTIFY changed FINAL)  // "idle", "following", "holding", ""
    Q_PROPERTY(QVariantList joints READ joints NOTIFY changed FINAL)
    Q_PROPERTY(bool clutch READ clutch NOTIFY handChanged FINAL)
    Q_PROPERTY(double handX READ handX NOTIFY handChanged FINAL)  // -1 .. 1 on the pad
    Q_PROPERTY(double handY READ handY NOTIFY handChanged FINAL)
    Q_PROPERTY(bool demo READ demo NOTIFY changed FINAL)

public:
    explicit TeleopModel(QObject* parent = nullptr);

    // `core` and `clock` must outlive this object. `demo`: a scripted hand that engages by itself
    // once the core allows it and moves slowly back and forth.
    void setCore(msc::teleop::TeleopCore* core, const msc::Clock* clock, bool demo);

    Q_INVOKABLE void engage();
    Q_INVOKABLE void disengage();
    Q_INVOKABLE void reset();
    Q_INVOKABLE void setClutch(bool held);
    // The pad: x and y from -1 to 1. Moves joints 1 and 2 of the hand by up to kHandRange.
    Q_INVOKABLE void setHand(double x, double y);
    // Fault injector: the hand controller stops reporting (no more samples to the core).
    void setStalled(bool stalled) noexcept { stalled_.store(stalled, std::memory_order_relaxed); }

    [[nodiscard]] QString state() const;
    [[nodiscard]] QString fault() const;
    [[nodiscard]] double faultAgeSeconds() const;
    [[nodiscard]] QStringList present() const;
    [[nodiscard]] bool engageAllowed() const noexcept { return hasStatus_ && status_.engageAllowed; }
    [[nodiscard]] bool resetAllowed() const noexcept { return hasStatus_ && status_.resetAllowed; }
    [[nodiscard]] QString refusal() const;
    [[nodiscard]] QString link() const;
    [[nodiscard]] QString armState() const;
    [[nodiscard]] QVariantList joints() const;
    [[nodiscard]] bool clutch() const noexcept { return clutch_.load(std::memory_order_relaxed); }
    [[nodiscard]] double handX() const noexcept { return handX_; }
    [[nodiscard]] double handY() const noexcept { return handY_; }
    [[nodiscard]] bool demo() const noexcept { return demo_; }
    // The core's newest status as this model last read it (every 50 ms), or null before the first.
    // For the checks driven from C++ (--soak); QML uses the properties above.
    [[nodiscard]] const msc::teleop::TeleopStatus* coreStatus() const noexcept {
        return hasStatus_ ? &status_ : nullptr;
    }

signals:
    void changed();
    void handChanged();

private:
    void tick();
    void request(msc::teleop::Request request);
    void runScriptedHand(const std::stop_token& stop);

    msc::teleop::TeleopCore* core_ = nullptr;
    const msc::Clock* clock_ = nullptr;
    msc::teleop::TeleopStatus status_;
    bool hasStatus_ = false;
    bool demo_ = false;
    bool demoEngaged_ = false;
    // Atomic: read by the scripted hand's thread, written by the GUI thread.
    std::atomic<bool> clutch_{false};
    std::atomic<bool> stalled_{false};
    // Where the scripted hand is, for the pad's dot: written by its thread, read by the GUI.
    std::atomic<double> scriptedX_{0.0};
    std::atomic<double> scriptedY_{0.0};
    double handX_ = 0.0;
    double handY_ = 0.0;
    int ticksSinceRefresh_ = 0;
    QTimer timer_;
    std::jthread scriptedHand_;  // last: stopped and joined before the members it uses go away
};
