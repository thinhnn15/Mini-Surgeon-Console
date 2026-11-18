#pragma once

#include "msc/comms/protocol.hpp"

namespace msc::hud {

// A point in the image as fractions of its size: (0, 0) is the top-left corner, (1, 1) the
// bottom-right. It may lie outside the image. Fractions, not pixels, so the same point serves
// a 1280x720 frame and the same frame scaled into any window.
struct ImagePoint {
    double x = 0.0;
    double y = 0.0;
};

// A point in front of the camera, in metres, in the camera's own frame: x to the right, y down
// (as image rows go), z forward into the scene.
struct ScenePoint {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

// How the scene is set up. The defaults put the instrument's entry point low and to the left
// of the camera and point it, at rest, at the centre of the picture 11 cm away.
struct ToolCameraConfig {
    // Where the instrument enters the body. A surgical instrument pivots about this point: the
    // shaft can swing around it and slide through it, but cannot move sideways there.
    ScenePoint pivot{.x = -0.06, .y = 0.045, .z = 0.04};
    // Where the tip is with every joint at zero.
    ScenePoint restTip{.x = 0.0, .y = 0.0, .z = 0.11};
    // Joint 3 (the elbow) slides the instrument in and out: metres per radian.
    double insertionPerRadian = 0.02;
    // Radius of the shaft, metres. (Thinner than a real 8 mm instrument: this camera is close.)
    double shaftRadius = 0.0025;
    // Focal length in units of the image width: a point 1 m to the side at 1 m depth lands this
    // many image widths from the centre. 0.45 is a 96 degree horizontal field of view, about a
    // wide endoscope. Chosen with the pivot so that the hand pad's whole range (±0.4 rad on
    // joints 1 and 2) keeps the tip inside a 16:9 image.
    double focalLength = 0.45;
    // Nothing closer to the camera than this is drawn, metres.
    double nearPlane = 0.005;
};

// What the camera sees of the instrument.
struct ToolView {
    bool visible = false;    // the tip is in front of the camera: there is something to draw
    bool tipInView = false;  // visible, and the tip is inside the image
    ImagePoint entry{};      // where the shaft comes from (usually outside the image)
    ImagePoint tip{};
    // Apparent radius of the shaft at each end, as a fraction of the image width: the near end
    // looks thicker than the far end.
    double entryRadius = 0.0;
    double tipRadius = 0.0;
    double tipDepth = 0.0;  // metres in front of the camera
};

// Where the instrument is in the scene for these joint angles (forward kinematics): joint 1
// swings the shaft left and right about the pivot, joint 2 up and down, joint 3 slides it in
// and out. The other joints turn the wrist, which this camera cannot tell apart.
[[nodiscard]] ScenePoint toolTip(const comms::JointArray& joints,
                                 const ToolCameraConfig& config = {}) noexcept;

// What a pinhole camera at the origin sees of it. `aspect` is the image's width divided by its
// height. Pure maths: the simulated camera draws the instrument with it (M5.2) and the overlay
// puts its marker with it (M5.3), so the two can only disagree about time, never about geometry.
[[nodiscard]] ToolView viewOfTool(const comms::JointArray& joints, double aspect,
                                  const ToolCameraConfig& config = {}) noexcept;

}  // namespace msc::hud
