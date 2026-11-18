#include "msc/hud/tool_camera.hpp"

#include <cmath>

namespace msc::hud {

namespace {

// The shaft's direction as two angles about the pivot: yaw turns it left/right (about the
// camera's y axis), pitch lifts it (toward the top of the image, which is -y).
struct Aim {
    double yaw = 0.0;
    double pitch = 0.0;
    double length = 0.0;
};

Aim restAim(const ToolCameraConfig& config) noexcept {
    const double dx = config.restTip.x - config.pivot.x;
    const double dy = config.restTip.y - config.pivot.y;
    const double dz = config.restTip.z - config.pivot.z;
    const double length = std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
    return Aim{.yaw = std::atan2(dx, dz), .pitch = std::asin(-dy / length), .length = length};
}

ScenePoint along(const ScenePoint& pivot, const Aim& aim) noexcept {
    const double flat = std::cos(aim.pitch);  // the part of the shaft in the horizontal plane
    return ScenePoint{.x = pivot.x + (aim.length * std::sin(aim.yaw) * flat),
                      .y = pivot.y - (aim.length * std::sin(aim.pitch)),
                      .z = pivot.z + (aim.length * std::cos(aim.yaw) * flat)};
}

// Pinhole projection: a point twice as far away lands half as far from the centre.
ImagePoint project(const ScenePoint& p, double aspect, double focalLength) noexcept {
    return ImagePoint{.x = 0.5 + (focalLength * p.x / p.z),
                      // The focal length is in image widths; one image height is 1/aspect widths.
                      .y = 0.5 + (focalLength * aspect * p.y / p.z)};
}

}  // namespace

ScenePoint toolTip(const comms::JointArray& joints, const ToolCameraConfig& config) noexcept {
    Aim aim = restAim(config);
    aim.yaw += static_cast<double>(joints[0]);
    aim.pitch += static_cast<double>(joints[1]);
    aim.length += config.insertionPerRadian * static_cast<double>(joints[2]);
    return along(config.pivot, aim);
}

ToolView viewOfTool(const comms::JointArray& joints, double aspect, const ToolCameraConfig& config) noexcept {
    ToolView view;
    const ScenePoint tip = toolTip(joints, config);
    if (tip.z <= config.nearPlane || aspect <= 0.0) {
        return view;  // behind the camera (or no image): nothing to see
    }
    // The visible shaft starts at the pivot, or where it comes through the near plane if the
    // pivot is behind it.
    ScenePoint entry = config.pivot;
    if (entry.z < config.nearPlane) {
        const double t = (config.nearPlane - entry.z) / (tip.z - entry.z);
        entry = ScenePoint{.x = entry.x + (t * (tip.x - entry.x)),
                           .y = entry.y + (t * (tip.y - entry.y)),
                           .z = config.nearPlane};
    }

    view.visible = true;
    view.tip = project(tip, aspect, config.focalLength);
    view.entry = project(entry, aspect, config.focalLength);
    view.tipRadius = config.focalLength * config.shaftRadius / tip.z;
    view.entryRadius = config.focalLength * config.shaftRadius / entry.z;
    view.tipDepth = tip.z;
    view.tipInView = view.tip.x >= 0.0 && view.tip.x <= 1.0 && view.tip.y >= 0.0 && view.tip.y <= 1.0;
    return view;
}

}  // namespace msc::hud
