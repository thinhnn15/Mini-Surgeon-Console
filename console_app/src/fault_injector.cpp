#include "fault_injector.hpp"

#include "teleop_model.hpp"

#include <QRegularExpression>
#include <QStringList>
#include <QTimer>

#include <chrono>

void FaultInjector::setTargets(msc::comms::ArmLink* link, msc::teleop::TeleopCore* core, TeleopModel* hand,
                               msc::video::FrameSource* video) {
    armLink_ = link;
    core_ = core;
    handModel_ = hand;
    videoSource_ = video;
    emit changed();
}

void FaultInjector::applyLink() {
    if (armLink_ == nullptr) {
        return;
    }
    // A pulled cable outranks corruption: nothing goes out at all.
    armLink_->setFault(link_      ? msc::comms::LinkFault::Drop
                       : corrupt_ ? msc::comms::LinkFault::Corrupt
                                  : msc::comms::LinkFault::None);
}

void FaultInjector::setLink(bool on) {
    if (on != link_) {
        link_ = on;
        applyLink();
        emit changed();
    }
}

void FaultInjector::setCorrupt(bool on) {
    if (on != corrupt_) {
        corrupt_ = on;
        applyLink();
        emit changed();
    }
}

void FaultInjector::setHand(bool on) {
    if (on != hand_ && handModel_ != nullptr) {
        hand_ = on;
        handModel_->setStalled(on);
        emit changed();
    }
}

void FaultInjector::setVideo(bool on) {
    if (on != video_ && videoSource_ != nullptr) {
        video_ = on;
        if (on) {
            videoSource_->stop();
        } else {
            videoSource_->start();
        }
        emit changed();
    }
}

void FaultInjector::setFault(bool on) {
    if (on != fault_ && core_ != nullptr) {
        fault_ = on;
        core_->setInjected(on ? msc::teleop::faultSet(msc::teleop::FaultCode::Injected)
                              : msc::teleop::FaultSet{});
        emit changed();
    }
}

bool FaultInjector::set(const QString& name, bool on) {
    if (name == QLatin1String("link")) {
        setLink(on);
    } else if (name == QLatin1String("corrupt")) {
        setCorrupt(on);
    } else if (name == QLatin1String("hand")) {
        setHand(on);
    } else if (name == QLatin1String("video")) {
        setVideo(on);
    } else if (name == QLatin1String("fault")) {
        setFault(on);
    } else {
        return false;
    }
    return true;
}

bool FaultInjector::schedule(const QString& script, QString* error) {
    static const QRegularExpression item(
        QStringLiteral(R"(^(link|corrupt|hand|video|fault)@(\d+(?:\.\d+)?)(?:-(\d+(?:\.\d+)?))?$)"));
    for (const QString& part : script.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QRegularExpressionMatch m = item.match(part.trimmed());
        if (!m.hasMatch()) {
            if (error != nullptr) {
                *error = QStringLiteral(
                             "cannot read \"%1\": NAME@START[-END], NAME one of link, corrupt, hand, "
                             "video, fault, times in seconds")
                             .arg(part);
            }
            return false;
        }
        const QString name = m.captured(1);
        const auto start =
            std::chrono::milliseconds(static_cast<long long>(m.captured(2).toDouble() * 1000.0));
        QTimer::singleShot(start, this, [this, name] { set(name, true); });
        if (m.hasCaptured(3)) {
            const auto end =
                std::chrono::milliseconds(static_cast<long long>(m.captured(3).toDouble() * 1000.0));
            QTimer::singleShot(end, this, [this, name] { set(name, false); });
        }
    }
    return true;
}
