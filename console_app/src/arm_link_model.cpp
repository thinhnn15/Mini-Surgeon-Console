#include "arm_link_model.hpp"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <numbers>

namespace {

using namespace std::chrono_literals;

// The panel cannot be read faster than this; the link itself runs at 100 Hz on its own thread.
constexpr auto kRefresh = 50ms;

// The demo motion: every joint swings ±0.8 rad, 8 s per cycle, each a little out of phase, so
// the arm is always moving and well inside its limits.
constexpr double kDemoAmplitude = 0.8;
constexpr double kDemoPeriodSeconds = 8.0;

}  // namespace

ArmLinkModel::ArmLinkModel(QObject* parent) : QObject(parent) {
    timer_.setInterval(kRefresh);
    connect(&timer_, &QTimer::timeout, this, &ArmLinkModel::refresh);
}

void ArmLinkModel::setLink(msc::comms::ArmLink* link, bool demo) {
    link_ = link;
    demo_ = demo;
    demoStart_ = std::chrono::steady_clock::now();
    timer_.start();
    emit changed();
}

void ArmLinkModel::refresh() {
    if (link_ == nullptr) {
        return;
    }
    if (demo_) {
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - demoStart_).count();
        msc::comms::JointArray target{};
        for (std::size_t i = 0; i < target.size(); ++i) {
            const double phase =
                2.0 * std::numbers::pi * ((t / kDemoPeriodSeconds) + (static_cast<double>(i) / 12.0));
            target[i] = static_cast<float>(kDemoAmplitude * std::sin(phase));
        }
        link_->setTarget(target);
    }
    if (link_->readStatus(status_)) {
        emit changed();
    }
}

QString ArmLinkModel::link() const {
    switch (status_.link) {
        case msc::comms::LinkState::Waiting:
            return tr("waiting");
        case msc::comms::LinkState::Alive:
            return tr("OK");
        case msc::comms::LinkState::Lost:
            return tr("lost");
    }
    return {};
}

QString ArmLinkModel::armState() const {
    if (!status_.telemetrySeen) {
        return {};
    }
    switch (status_.telemetry.state) {
        case msc::comms::ArmState::Idle:
            return tr("idle");
        case msc::comms::ArmState::Following:
            return tr("following");
        case msc::comms::ArmState::Holding:
            return tr("holding");
    }
    return {};
}

QVariantList ArmLinkModel::joints() const {
    QVariantList list;
    if (status_.telemetrySeen) {
        for (const float value : status_.telemetry.position) {
            list.append(static_cast<double>(value));
        }
    }
    return list;
}
