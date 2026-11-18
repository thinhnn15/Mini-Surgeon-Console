#include "teleop_model.hpp"

#include "msc/common/fine_timer.hpp"

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
// The scripted hand: joints 1 and 2 trace a slow figure of eight, 10 s per turn.
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
    if (demo_) {
        scriptedHand_ = std::jthread([this](const std::stop_token& stop) { runScriptedHand(stop); });
    }
    timer_.start();
    emit changed();
}

void TeleopModel::tick() {
    if (core_ == nullptr || clock_ == nullptr) {
        return;
    }
    if (demo_) {
        // The scripted hand reports to the core from its own thread; here only the pad's dot
        // follows it, and the engage request is made once the core allows it.
        handX_ = scriptedX_.load(std::memory_order_relaxed);
        handY_ = scriptedY_.load(std::memory_order_relaxed);
        emit handChanged();
        if (!demoEngaged_ && hasStatus_ && status_.engageAllowed) {
            demoEngaged_ = true;
            request(msc::teleop::Request::Engage);
        }
    } else if (!stalled_.load(std::memory_order_relaxed)) {
        msc::teleop::HandSample sample;
        sample.pose[0] = static_cast<float>(handX_ * kHandRange);
        sample.pose[1] = static_cast<float>(handY_ * kHandRange);
        sample.clutch = clutch_.load(std::memory_order_relaxed);
        sample.time = clock_->now();
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

// The scripted hand (--hand-demo): joints 1 and 2 trace a slow figure of eight. A thread of
// its own, paced like the other fixed-rate loops, so it is the core's only hand producer in
// this mode and reports every 10 ms whatever the GUI thread is doing.
void TeleopModel::runScriptedHand(const std::stop_token& stop) {
    [[maybe_unused]] const msc::FineTimerResolution fineTimer;
    const auto start = std::chrono::steady_clock::now();
    auto next = start;
    while (!stop.stop_requested()) {
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        const double phase = 2.0 * std::numbers::pi * t / kDemoPeriodSeconds;
        const double x = 0.8 * std::sin(phase);
        const double y = 0.5 * std::sin(2.0 * phase);
        scriptedX_.store(x, std::memory_order_relaxed);
        scriptedY_.store(y, std::memory_order_relaxed);
        if (!stalled_.load(std::memory_order_relaxed)) {
            msc::teleop::HandSample sample;
            sample.pose[0] = static_cast<float>(x * kHandRange);
            sample.pose[1] = static_cast<float>(y * kHandRange);
            sample.clutch = clutch_.load(std::memory_order_relaxed);
            sample.time = clock_->now();
            core_->submitHand(sample);
        }
        next += kHandPeriod;
        msc::sleepUntil(next);
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
    if (held != clutch_.load(std::memory_order_relaxed)) {
        clutch_.store(held, std::memory_order_relaxed);
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
