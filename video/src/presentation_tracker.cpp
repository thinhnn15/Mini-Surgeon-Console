#include "msc/video/presentation_tracker.hpp"

#include <chrono>

namespace msc::video {

PresentationTracker::PresentationTracker(std::size_t window)
    : captureToPresent_(window), captureToUpload_(window) {}

void PresentationTracker::frameUploaded(std::uint64_t sequence, Nanoseconds captureTime,
                                        Nanoseconds uploadTime) {
    if (lastSequence_ != 0 && sequence > lastSequence_ + 1) {
        dropped_ += sequence - lastSequence_ - 1;
    }
    // A sequence that goes backwards means the source was restarted or replaced: start
    // counting gaps again from here rather than calling it a huge drop.
    lastSequence_ = sequence;
    // If a previous upload never reached a swap (a window grab renders without presenting),
    // this one replaces it: only frames that were really presented are measured.
    pending_ = Pending{.captureTime = captureTime, .uploadTime = uploadTime};
}

void PresentationTracker::swapped(Nanoseconds presentTime) {
    ++swaps_;
    if (!firstSwap_) {
        firstSwap_ = presentTime;
    }
    lastSwap_ = presentTime;
    if (!pending_) {
        return;  // the window presented the same video frame again
    }
    captureToPresent_.add(presentTime - pending_->captureTime);
    captureToUpload_.add(pending_->uploadTime - pending_->captureTime);
    ++shown_;
    pending_.reset();
}

PresentationStats PresentationTracker::takeStats(Nanoseconds now) {
    PresentationStats s;
    s.samples = captureToPresent_.count();
    s.captureToPresentP50 = captureToPresent_.p50();
    s.captureToPresentP99 = captureToPresent_.p99();
    s.captureToPresentMax = captureToPresent_.max();
    s.captureToUploadP50 = captureToUpload_.p50();
    s.captureToUploadP99 = captureToUpload_.p99();
    s.swaps = swaps_;
    s.framesShown = shown_;
    s.framesDropped = dropped_;
    s.firstSwap = firstSwap_;
    s.lastSwap = lastSwap_;

    if (intervalStart_ && now > *intervalStart_) {
        const double seconds = std::chrono::duration<double>(now - *intervalStart_).count();
        s.renderFps = static_cast<double>(swaps_ - swapsAtIntervalStart_) / seconds;
        s.videoFps = static_cast<double>(shown_ - shownAtIntervalStart_) / seconds;
    }
    intervalStart_ = now;
    swapsAtIntervalStart_ = swaps_;
    shownAtIntervalStart_ = shown_;
    return s;
}

void PresentationTracker::reset() {
    captureToPresent_.clear();
    captureToUpload_.clear();
    pending_.reset();
    lastSequence_ = 0;
    swaps_ = 0;
    shown_ = 0;
    dropped_ = 0;
    firstSwap_.reset();
    lastSwap_.reset();
    intervalStart_.reset();
    swapsAtIntervalStart_ = 0;
    shownAtIntervalStart_ = 0;
}

}  // namespace msc::video
