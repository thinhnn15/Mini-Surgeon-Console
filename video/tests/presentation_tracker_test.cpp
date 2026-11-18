#include "msc/video/presentation_tracker.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>

namespace msc::video {
namespace {

using std::chrono::milliseconds;

// One video frame through the path: captured at `capture`, uploaded at `upload`, presented
// at `present` (all in ms).
void showFrame(PresentationTracker& t, std::uint64_t seq, int capture, int upload, int present) {
    t.frameUploaded(seq, milliseconds(capture), milliseconds(upload));
    t.swapped(milliseconds(present));
}

TEST(PresentationTracker, REQ_VID_002_NothingBeforeTheFirstFrame) {
    PresentationTracker t;
    const PresentationStats s = t.takeStats(milliseconds(0));
    EXPECT_EQ(s.samples, 0U);
    EXPECT_FALSE(s.captureToPresentP50.has_value());
    EXPECT_FALSE(s.captureToPresentP99.has_value());
    EXPECT_EQ(s.framesShown, 0U);
    EXPECT_EQ(s.framesDropped, 0U);
    EXPECT_DOUBLE_EQ(s.renderFps, 0.0);
}

TEST(PresentationTracker, REQ_VID_002_MeasuresCaptureToPresentAndCaptureToUpload) {
    PresentationTracker t;
    showFrame(t, 1, 0, 5, 12);    // 12 ms total, 5 ms of it waiting to be picked up
    showFrame(t, 2, 16, 20, 30);  // 14 ms, 4 ms
    showFrame(t, 3, 33, 40, 46);  // 13 ms, 7 ms
    const PresentationStats s = t.takeStats(milliseconds(50));
    EXPECT_EQ(s.samples, 3U);
    EXPECT_EQ(s.captureToPresentP50, milliseconds(13));
    EXPECT_EQ(s.captureToPresentP99, milliseconds(14));
    EXPECT_EQ(s.captureToPresentMax, milliseconds(14));
    EXPECT_EQ(s.captureToUploadP50, milliseconds(5));
    EXPECT_EQ(s.captureToUploadP99, milliseconds(7));
    EXPECT_EQ(s.framesShown, 3U);
}

TEST(PresentationTracker, REQ_VID_002_GapsInSequenceAreDroppedFrames) {
    PresentationTracker t;
    showFrame(t, 10, 0, 1, 2);
    showFrame(t, 11, 0, 1, 2);
    showFrame(t, 14, 0, 1, 2);  // 12 and 13 never shown
    showFrame(t, 20, 0, 1, 2);  // 15..19 never shown
    const PresentationStats s = t.takeStats(milliseconds(10));
    EXPECT_EQ(s.framesShown, 4U);
    EXPECT_EQ(s.framesDropped, 2U + 5U);
}

TEST(PresentationTracker, FirstFrameIsNotADrop) {
    PresentationTracker t;
    showFrame(t, 500, 0, 1, 2);  // joining a source that has been running for a while
    EXPECT_EQ(t.takeStats(milliseconds(5)).framesDropped, 0U);
}

TEST(PresentationTracker, RestartedSourceIsNotADrop) {
    PresentationTracker t;
    showFrame(t, 100, 0, 1, 2);
    showFrame(t, 1, 3, 4, 5);  // sequence went backwards: a new source
    showFrame(t, 2, 6, 7, 8);
    EXPECT_EQ(t.takeStats(milliseconds(10)).framesDropped, 0U);
}

TEST(PresentationTracker, SwapsWithoutANewFrameCountForRenderFpsOnly) {
    PresentationTracker t;
    static_cast<void>(t.takeStats(milliseconds(0)));  // start of the interval
    showFrame(t, 1, 0, 1, 5);
    t.swapped(milliseconds(10));  // same frame presented again
    t.swapped(milliseconds(15));
    showFrame(t, 2, 12, 16, 20);
    const PresentationStats s = t.takeStats(milliseconds(1000));
    EXPECT_EQ(s.swaps, 4U);
    EXPECT_EQ(s.framesShown, 2U);
    EXPECT_EQ(s.samples, 2U);            // only frames with a new picture are latency samples
    EXPECT_DOUBLE_EQ(s.renderFps, 4.0);  // 4 swaps in 1 s
    EXPECT_DOUBLE_EQ(s.videoFps, 2.0);
}

TEST(PresentationTracker, FpsCoversOnlyTheLastInterval) {
    PresentationTracker t;
    static_cast<void>(t.takeStats(milliseconds(0)));
    for (int i = 0; i < 60; ++i) {
        t.swapped(milliseconds(i * 16));
    }
    EXPECT_NEAR(t.takeStats(milliseconds(1000)).renderFps, 60.0, 1e-9);
    for (int i = 0; i < 15; ++i) {
        t.swapped(milliseconds(1000 + (i * 33)));
    }
    EXPECT_NEAR(t.takeStats(milliseconds(1500)).renderFps, 30.0, 1e-9);  // 15 in 0.5 s
}

TEST(PresentationTracker, UploadWithoutSwapIsReplacedByTheNextOne) {
    // A window grab renders (and uploads) without presenting. That frame must not be
    // measured against the swap of the next real frame.
    PresentationTracker t;
    t.frameUploaded(1, milliseconds(0), milliseconds(1));  // grabbed, never presented
    showFrame(t, 2, 16, 17, 25);
    const PresentationStats s = t.takeStats(milliseconds(30));
    EXPECT_EQ(s.samples, 1U);
    EXPECT_EQ(s.captureToPresentP50, milliseconds(9));
}

TEST(PresentationTracker, ResetForgetsEverything) {
    PresentationTracker t;
    showFrame(t, 1, 0, 1, 100);
    showFrame(t, 5, 0, 1, 100);
    t.reset();
    showFrame(t, 9, 200, 201, 210);
    const PresentationStats s = t.takeStats(milliseconds(300));
    EXPECT_EQ(s.samples, 1U);
    EXPECT_EQ(s.captureToPresentMax, milliseconds(10));
    EXPECT_EQ(s.framesDropped, 0U);
    EXPECT_EQ(s.swaps, 1U);
    EXPECT_EQ(s.firstSwap, milliseconds(210));
}

}  // namespace
}  // namespace msc::video
