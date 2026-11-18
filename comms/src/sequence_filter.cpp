#include "msc/comms/sequence_filter.hpp"

#include <algorithm>

namespace msc::comms {

SequenceVerdict SequenceFilter::check(std::uint32_t session, std::uint32_t sequence) noexcept {
    if (session_ != session) {
        if (isRetired(session)) {
            ++stats_.retired;
            return SequenceVerdict::RetiredSession;
        }
        if (session_) {
            // The sender restarted: the old session will never be followed again.
            retired_[nextRetired_] = session_;
            nextRetired_ = (nextRetired_ + 1) % kRetiredSessions;
            ++stats_.sessionChanges;
        }
        session_ = session;
        last_ = sequence;
        ++stats_.accepted;
        return SequenceVerdict::Accepted;
    }

    if (sequence == last_) {
        ++stats_.duplicates;
        return SequenceVerdict::Duplicate;
    }
    if (!isNewer(sequence, last_)) {
        ++stats_.old;
        return SequenceVerdict::Old;
    }
    stats_.lost += sequence - last_ - 1;  // unsigned: correct across the wrap
    last_ = sequence;
    ++stats_.accepted;
    return SequenceVerdict::Accepted;
}

void SequenceFilter::reset() noexcept {
    *this = SequenceFilter{};
}

bool SequenceFilter::isRetired(std::uint32_t session) const noexcept {
    return std::ranges::find(retired_, session) != retired_.end();
}

}  // namespace msc::comms
