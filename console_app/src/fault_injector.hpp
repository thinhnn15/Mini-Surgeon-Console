#pragma once

#include "msc/comms/arm_link.hpp"
#include "msc/teleop/teleop_core.hpp"
#include "msc/video/frame_source.hpp"

#include <QtQml/qqmlregistration.h>
#include <QObject>
#include <QString>

class TeleopModel;

// Makes faults happen on purpose (M4.4), from the window (the toggles in the status bar) or
// from the command line (--inject=NAME@START[-END],...). Testing only: on a real console this
// would not exist, or be behind a service key.
//
//   link     the cable is pulled: the arm link sends nothing and ignores what arrives
//   corrupt  one bit flipped in every packet to the arm: the arm refuses them all
//   hand     the hand controller stops reporting
//   video    the video source stops (the picture freezes)
//   fault    a generic fault, injected straight into the teleop core
//
// GUI thread. main.cpp finds it by objectName ("faults") and gives it what it needs.
class FaultInjector : public QObject {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(bool link READ link WRITE setLink NOTIFY changed FINAL)
    Q_PROPERTY(bool corrupt READ corrupt WRITE setCorrupt NOTIFY changed FINAL)
    Q_PROPERTY(bool hand READ hand WRITE setHand NOTIFY changed FINAL)
    Q_PROPERTY(bool video READ video WRITE setVideo NOTIFY changed FINAL)
    Q_PROPERTY(bool fault READ fault WRITE setFault NOTIFY changed FINAL)
    Q_PROPERTY(bool available READ available NOTIFY changed FINAL)

public:
    explicit FaultInjector(QObject* parent = nullptr) : QObject(parent) {}

    // All must outlive this object. Any may be null (that fault is then not available).
    void setTargets(msc::comms::ArmLink* link, msc::teleop::TeleopCore* core, TeleopModel* hand,
                    msc::video::FrameSource* video);

    // "link@5-8,hand@12": each NAME on at START seconds and off again at END (if given).
    // False, with `error` set, if the text is not that.
    bool schedule(const QString& script, QString* error);

    [[nodiscard]] bool link() const noexcept { return link_; }
    [[nodiscard]] bool corrupt() const noexcept { return corrupt_; }
    [[nodiscard]] bool hand() const noexcept { return hand_; }
    [[nodiscard]] bool video() const noexcept { return video_; }
    [[nodiscard]] bool fault() const noexcept { return fault_; }
    [[nodiscard]] bool available() const noexcept { return core_ != nullptr; }

    void setLink(bool on);
    void setCorrupt(bool on);
    void setHand(bool on);
    void setVideo(bool on);
    void setFault(bool on);

signals:
    void changed();

private:
    bool set(const QString& name, bool on);
    void applyLink();

    msc::comms::ArmLink* armLink_ = nullptr;
    msc::teleop::TeleopCore* core_ = nullptr;
    TeleopModel* handModel_ = nullptr;
    msc::video::FrameSource* videoSource_ = nullptr;
    bool link_ = false;
    bool corrupt_ = false;
    bool hand_ = false;
    bool video_ = false;
    bool fault_ = false;
};
