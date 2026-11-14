#pragma once

#include "msc/video/frame.hpp"

namespace msc::video {

// Anything that produces video frames: the synthetic pattern now, a webcam later (M2.5).
// The renderer only knows this interface, so swapping the source never touches the UI.
//
// Threads: start()/stop() from the owning thread; acquireLatest()/latest() from exactly one
// reader thread (the renderer). The source produces frames on its own thread.
class FrameSource {
public:
    FrameSource() = default;
    FrameSource(const FrameSource&) = delete;
    FrameSource& operator=(const FrameSource&) = delete;
    FrameSource(FrameSource&&) = delete;
    FrameSource& operator=(FrameSource&&) = delete;
    virtual ~FrameSource() = default;

    virtual void start() = 0;
    // Stops producing and returns once the producer thread has finished. Safe to call twice.
    virtual void stop() = 0;

    // Reader thread only. Takes the newest frame if there is one since the last call;
    // returns true if latest() now refers to a new frame. Never blocks.
    [[nodiscard]] virtual bool acquireLatest() noexcept = 0;

    // Reader thread only. The frame taken by the last acquireLatest(). It stays valid and
    // unchanged until the next acquireLatest(), so it can be uploaded without copying.
    // Before the first frame it is empty (width 0, sequence 0).
    [[nodiscard]] virtual const Frame& latest() const noexcept = 0;
};

}  // namespace msc::video
