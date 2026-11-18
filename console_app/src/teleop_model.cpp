#include "teleop_model.hpp"

#include <QtGlobal>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <numbers>

namespace {

using namespace std::chrono_literals;
using msc::teleop::StateKind;

// Hand readings go to the core at 100 Hz, well inside its 100 ms hand timeout.
constexpr auto kHandPeriod = 10ms;
// The panel is redrawn every 5th reading (20 Hz): faster cannot be read.
constexpr int kRefreshEvery = 5;
// Pad edge to edge is ±0.8 rad of hand motion (±0.4 rad at the arm, scale 0.5).
constexpr double kHandRange = 0.8;
// The scripted hand: joints 1 and 2 trace a slow ellipse, 10 s per turn.
constexpr double kDemoPeriodSeconds = 10.0;

}  // namespace

TeleopModel::TeleopModel(QObject* parent) : QObject(parent) {
    timer_.setInterval(kHandPeriod);
    timer_.setTimerType(Qt::PreciseTimer);
    connect(&timer_, &QTimer::timeout, this, &TeleopModel::tick);
}

void TeleopModel::setCore(msc::teleop::TeleopCore* core, const msc::Clock* clock, bool demo) {
    core_ = core;
    clock_ = clock;
    demo_ = demo;
    demoStart_ = std::chrono::steady_clock::now();
    timer_.start();
    emit changed();
}

void TeleopModel::tick() {
    if (core_ == nullptr || clock_ == nullptr) {
        return;
    }
    if (demo_) {
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - demoStart_).count();
        const double phase = 2.0 * std::numbers::pi * t / kDemoPeriodSeconds;
        handX_ = 0.8 * std::sin(phase);
        handY_ = 0.5 * std::sin(2.0 * phase);
        emit handChanged();
        if (!demoEngaged_ && hasStatus_ && status_.engageAllowed) {
            demoEngaged_ = true;
            request(msc::teleop::Request::Engage);
        }
    }

    msc::teleop::HandSample sample;
    sample.pose[0] = static_cast<float>(handX_ * kHandRange);
    sample.pose[1] = static_cast<float>(handY_ * kHandRange);
    sample.clutch = clutch_;
    sample.time = clock_->now();
    if (!stalled_) {
        core_->submitHand(sample);
    }

    if (++ticksSinceRefresh_ >= kRefreshEvery) {
        ticksSinceRefresh_ = 0;
        if (core_->readStatus(status_)) {
            hasStatus_ = true;
            emit changed();
        }
    }
}

void TeleopModel::request(msc::teleop::Request request) {
    if (core_ != nullptr) {
        core_->request(request);
    }
}

void TeleopModel::engage() {
    request(msc::teleop::Request::Engage);
}

void TeleopModel::disengage() {
    request(msc::teleop::Request::Disengage);
}

void TeleopModel::reset() {
    request(msc::teleop::Request::Reset);
}

void TeleopModel::setClutch(bool held) {
    if (held != clutch_) {
        clutch_ = held;
        emit handChanged();
    }
}

void TeleopModel::setHand(double x, double y) {
    if (demo_) {
        return;  // the script has the hand
    }
    handX_ = qBound(-1.0, x, 1.0);
    handY_ = qBound(-1.0, y, 1.0);
    emit handChanged();
}

QString TeleopModel::state() const {
    if (!hasStatus_) {
        return {};
    }
    switch (status_.state) {
        case StateKind::Idle:
            return QStringLiteral("idle");
        case StateKind::Engaged:
            return QStringLiteral("engaged");
        case StateKind::Paused:
            return QStringLiteral("paused");
        case StateKind::Fault:
            return QStringLiteral("fault");
    }
    return {};
}

QString TeleopModel::fault() const {
    if (!hasStatus_ || status_.state != StateKind::Fault) {
        return {};
    }
    const std::string_view text = msc::teleop::toString(status_.fault);
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

double TeleopModel::faultAgeSeconds() const {
    if (!hasStatus_ || status_.state != StateKind::Fault || clock_ == nullptr) {
        return 0.0;
    }
    return std::chrono::duration<double>(clock_->now() - status_.faultSince).count();
}

QStringList TeleopModel::present() const {
    QStringList list;
    if (!hasStatus_) {
        return list;
    }
    const msc::teleop::FaultSet set(status_.present);
    for (std::size_t i = 0; i < set.size(); ++i) {
        if (set.test(i)) {
            const std::string_view text = msc::teleop::toString(static_cast<msc::teleop::FaultCode>(i));
            list.append(QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())));
        }
    }
    return list;
}

QString TeleopModel::refusal() const {
    if (!hasStatus_) {
        return {};
    }
    return QString::fromUtf8(status_.lastRefusal.data(), static_cast<qsizetype>(status_.lastRefusal.size()));
}

QString TeleopModel::link() const {
    if (!hasStatus_) {
        return tr("waiting");
    }
    switch (status_.link.link) {
        case msc::comms::LinkState::Waiting:
            return tr("waiting");
        case msc::comms::LinkState::Alive:
            return tr("OK");
        case msc::comms::LinkState::Lost:
            return tr("lost");
    }
    return {};
}

QString TeleopModel::armState() const {
    if (!hasStatus_ || !status_.link.telemetrySeen) {
        return {};
    }
    switch (status_.link.telemetry.state) {
        case msc::comms::ArmState::Idle:
            return tr("idle");
        case msc::comms::ArmState::Following:
            return tr("following");
        case msc::comms::ArmState::Holding:
            return tr("holding");
    }
    return {};
}

QVariantList TeleopModel::joints() const {
    QVariantList list;
    if (hasStatus_ && status_.link.telemetrySeen) {
        for (const float value : status_.link.telemetry.position) {
            list.append(static_cast<double>(value));
        }
    }
    return list;
}
