#pragma once

#include "msc/video/frame_source.hpp"

#include <QtQml/qqmlregistration.h>
#include <QQuickRhiItem>

#include <memory>

// Shows the newest frame of a FrameSource, scaled to fit at its own aspect ratio (black bars
// around it). Drawn with QRhi on the Qt Quick render thread; see ADR 0003.
//
// Threads:
//   - GUI thread: the item itself, setFrameSource().
//   - Render thread: the renderer. It is the source's one reader: it calls acquireLatest()
//     once per rendered frame and uploads straight from latest(), without a copy.
//   The source is handed from one to the other in synchronize(), while the GUI thread is
//   blocked, which is Qt's one safe meeting point between the two.
//
// The item renders continuously (every vsync) while it has a source: a video view has a new
// picture nearly every frame, so there is nothing to gain from waiting to be asked.
class VideoItem : public QQuickRhiItem {
    Q_OBJECT
    QML_ELEMENT

public:
    explicit VideoItem(QQuickItem* parent = nullptr);

    // GUI thread. Shared ownership: the render thread may still hold the old source for a
    // moment after this call, and must never see it destroyed under its feet.
    void setFrameSource(std::shared_ptr<msc::video::FrameSource> source);
    [[nodiscard]] const std::shared_ptr<msc::video::FrameSource>& frameSource() const noexcept {
        return source_;
    }

protected:
    QQuickRhiItemRenderer* createRenderer() override;

private:
    std::shared_ptr<msc::video::FrameSource> source_;
};
