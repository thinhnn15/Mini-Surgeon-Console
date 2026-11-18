#pragma once

#include "msc/common/clock.hpp"
#include "msc/teleop/teleop_core.hpp"

#include <QtQml/qqmlregistration.h>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>

#include <chrono>

// The operator's side of the teleop core, for QML. GUI thread.
//
// It is the hand controller: every 10 ms it sends the hand's pose and clutch to the core, from
// the pad in the window (or a scripted motion with --hand-demo). If the GUI thread stalls for
// longer than the core's hand timeout, the readings stop and the core faults, exactly as with a
// real hand device that froze.
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
    [[nodiscard]] bool clutch() const noexcept { return clutch_; }
    [[nodiscard]] double handX() const noexcept { return handX_; }
    [[nodiscard]] double handY() const noexcept { return handY_; }
    [[nodiscard]] bool demo() const noexcept { return demo_; }

signals:
    void changed();
    void handChanged();

private:
    void tick();
    void request(msc::teleop::Request request);

    msc::teleop::TeleopCore* core_ = nullptr;
    const msc::Clock* clock_ = nullptr;
    msc::teleop::TeleopStatus status_;
    bool hasStatus_ = false;
    bool demo_ = false;
    bool demoEngaged_ = false;
    std::chrono::steady_clock::time_point demoStart_;
    bool clutch_ = false;
    double handX_ = 0.0;
    double handY_ = 0.0;
    int ticksSinceRefresh_ = 0;
    QTimer timer_;
};
