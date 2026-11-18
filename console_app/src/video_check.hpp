#pragma once

#include "msc/video/synthetic_source.hpp"

class QQuickWindow;
class VideoItem;

// End-to-end check of the video path for REQ-VID-001, run by `console_app --check-video`.
//
// Lets the real window render for a moment, grabs what Qt Quick draws, cuts the video out,
// reads the test-pattern barcode back and compares it with what the source has produced:
// the picture must be the newest frame (give or take a frame), not an old or queued one.
// It goes through everything a frame really goes through: producer thread, triple buffer,
// render thread, texture upload, shader, scaling. Repeated at three window sizes (as
// opened, tall, wide), checking each time that the bars around the picture are black, so a
// stretched picture or a renderer that does not follow a resize fails too.
//
// Needs a real GPU backend (D3D11, Vulkan, Metal or OpenGL); the software backend cannot
// draw a QQuickRhiItem. Quits the application when done, with exit code 0 for a pass, and
// prints what it saw to stdout either way.
void startVideoCheck(QQuickWindow& window, const VideoItem& item, const msc::video::SyntheticSource& source,
                     msc::video::SyntheticSourceConfig config);
