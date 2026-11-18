#pragma once

#include "msc/comms/arm_link.hpp"

#include <QtQml/qqmlregistration.h>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>

#include <chrono>

// The arm link as QML sees it: link state, arm state and joint positions, read from the
// comms thread's ArmLink a few times per second (the link itself runs on its own thread at
// 100 Hz; this only shows it). GUI thread.
//
// main.cpp finds it by objectName ("armLink") and hands it the link, like the video item.
class ArmLinkModel : public QObject {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QString link READ link NOTIFY changed FINAL)  // "waiting", "OK", "lost"
    Q_PROPERTY(bool linkOk READ linkOk NOTIFY changed FINAL)
    Q_PROPERTY(QString armState READ armState NOTIFY changed FINAL)   // "idle", "following", "holding", ""
    Q_PROPERTY(QVariantList joints READ joints NOTIFY changed FINAL)  // radians, empty before telemetry
    Q_PROPERTY(bool demo READ demo NOTIFY changed FINAL)

public:
    explicit ArmLinkModel(QObject* parent = nullptr);

    // GUI thread. `link` must outlive this object. `demo`: command a slow back-and-forth
    // motion (a stand-in for the hand controller of M4), so the arm has something to follow.
    void setLink(msc::comms::ArmLink* link, bool demo);

    [[nodiscard]] QString link() const;
    [[nodiscard]] bool linkOk() const noexcept { return status_.link == msc::comms::LinkState::Alive; }
    [[nodiscard]] QString armState() const;
    [[nodiscard]] QVariantList joints() const;
    [[nodiscard]] bool demo() const noexcept { return demo_; }

signals:
    void changed();

private:
    void refresh();

    msc::comms::ArmLink* link_ = nullptr;
    msc::comms::ArmLinkStatus status_;
    bool demo_ = false;
    std::chrono::steady_clock::time_point demoStart_;
    QTimer timer_;
};
