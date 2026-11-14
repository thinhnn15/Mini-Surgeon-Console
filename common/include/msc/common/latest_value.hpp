#pragma once

#include "msc/common/triple_buffer.hpp"

#include <type_traits>

namespace msc {

// A mailbox that always holds the newest value, with copy semantics. One writer thread,
// one reader thread.
//
//   - The writer never waits, whatever the reader is doing (no lock, no spin).
//   - Every read returns one complete value, never half of one value and half of another.
//   - Values the reader did not get to in time are dropped, not queued (REQ-VID-001):
//     a display wants the latest frame, not a backlog.
//
// A thin wrapper over TripleBuffer<T>: publish copies into the writer's slot, read
// copies out of the reader's slot. Good for small values (a status struct). For big ones
// such as video frames, use TripleBuffer directly and skip both copies.
//
// Not for: more than one writer or more than one reader, or data that must not be lost
// (use a queue for that).
template <typename T>
class LatestValue {
    // publish/read copy T with plain assignment. Trivially copyable keeps that a memcpy
    // that cannot throw or allocate, so both can be noexcept and cheap.
    static_assert(std::is_trivially_copyable_v<T>, "LatestValue<T> needs a trivially copyable T");

public:
    // Writer thread only. Never blocks.
    void publish(const T& value) noexcept {
        buffer_.writeSlot() = value;
        buffer_.publish();
    }

    // Reader thread only. Never blocks. Copies the newest value into `out` and returns
    // true if it is new since the last read; false means `out` got the same value as
    // last time (or a value-initialised T if nothing was ever published).
    [[nodiscard]] bool read(T& out) noexcept {
        const bool fresh = buffer_.acquireLatest();
        out = buffer_.readSlot();
        return fresh;
    }

private:
    TripleBuffer<T> buffer_;
};

}  // namespace msc
