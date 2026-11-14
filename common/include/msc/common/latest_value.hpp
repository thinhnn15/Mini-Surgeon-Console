#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace msc {

// A mailbox that always holds the newest value. One writer thread, one reader thread.
//
//   - The writer never waits, whatever the reader is doing (no lock, no spin).
//   - Every read returns one complete value, never half of one value and half of another.
//   - Values the reader did not get to in time are dropped, not queued (REQ-VID-001):
//     a display wants the latest frame, not a backlog.
//
// How: a triple buffer. Three slots; at any moment each slot belongs to exactly one role:
//   back   - the writer's private slot, being filled
//   middle - the newest complete value, waiting to be picked up (shared)
//   front  - the reader's private slot, being read
// The writer fills `back`, then swaps it with `middle` in one atomic exchange. The reader,
// if `middle` holds something new, swaps it with `front`. Because there is always a third
// slot to swap through, the two threads never touch the same slot at the same time.
//
// Not for: more than one writer or more than one reader, or data that must not be lost
// (use a queue for that).
// MSVC warns (C4324) that the alignas below pads the class: that padding is the point.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
template <typename T>
class LatestValue {
    // publish/read copy T with plain assignment while no lock is held. Trivially copyable
    // keeps that a memcpy that cannot throw or allocate, and rules out types whose copy
    // would touch shared state (shared_ptr's refcount, for example).
    static_assert(std::is_trivially_copyable_v<T>, "LatestValue<T> needs a trivially copyable T");

public:
    LatestValue() = default;

    // Both threads hold a reference to the same object; a copy or move would split them.
    LatestValue(const LatestValue&) = delete;
    LatestValue& operator=(const LatestValue&) = delete;
    LatestValue(LatestValue&&) = delete;
    LatestValue& operator=(LatestValue&&) = delete;
    ~LatestValue() = default;

    // Writer thread only. Never blocks.
    void publish(const T& value) noexcept {
        slots_[back_].value = value;

        // Release: the write above becomes visible to whoever later acquires this slot.
        // Acquire: the slot we get back may have just been given up by the reader; its
        // reads of that slot must be finished before we overwrite it.
        const std::uint8_t previous = middle_.exchange(back_ | kFreshBit, std::memory_order_acq_rel);
        back_ = previous & kIndexMask;
    }

    // Reader thread only. Never blocks. Copies the newest value into `out` and returns
    // true if it is new since the last read; false means `out` got the same value as
    // last time (or a value-initialised T if nothing was ever published).
    [[nodiscard]] bool read(T& out) noexcept {
        bool fresh = false;
        // Relaxed is enough for this peek: it only decides whether to swap. The exchange
        // below is the operation that synchronises with the writer.
        if ((middle_.load(std::memory_order_relaxed) & kFreshBit) != 0) {
            const std::uint8_t previous = middle_.exchange(front_, std::memory_order_acq_rel);
            front_ = previous & kIndexMask;
            fresh = true;
        }
        out = slots_[front_].value;
        return fresh;
    }

private:
    static constexpr std::uint8_t kIndexMask = 0b011;  // which slot, 0..2
    static constexpr std::uint8_t kFreshBit = 0b100;   // set by the writer, cleared by the reader
    // Typical cache line size. std::hardware_destructive_interference_size would say the
    // same, but GCC warns that its value can differ between compiler flags, which would
    // make the layout of this class differ between translation units.
    static constexpr std::size_t kCacheLine = 64;

    // One cache line per slot and per index, so the writer and the reader never write to
    // the same cache line (false sharing: correct, but the line ping-pongs between cores).
    struct alignas(kCacheLine) Slot {
        T value{};
    };

    std::array<Slot, 3> slots_{};
    alignas(kCacheLine) std::atomic<std::uint8_t> middle_{1};
    alignas(kCacheLine) std::uint8_t back_ = 0;   // writer only
    alignas(kCacheLine) std::uint8_t front_ = 2;  // reader only

    static_assert(std::atomic<std::uint8_t>::is_always_lock_free,
                  "the index exchange must be a single CPU instruction, not a hidden lock");
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

}  // namespace msc
