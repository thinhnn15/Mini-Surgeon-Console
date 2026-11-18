#pragma once

class QQuickWindow;
class VideoItem;

namespace msc::video {
class FrameSource;
}

// End-to-end check of REQ-VID-003 for `console_app --check-freeze`, with the real window and
// renderer: the picture must be Live while the source runs, be flagged Frozen soon after the
// source stops (as a camera does when its cable is pulled), and come back to Live when it
// starts again. Exits the application with 0 on success, 1 on failure.
void startFreezeCheck(QQuickWindow& window, VideoItem& item, msc::video::FrameSource& source);
