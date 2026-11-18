#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace msc {

// The newest-value mailbox, used in place: the writer fills its slot directly and the
// reader uses its slot directly, so nothing is copied and nothing is allocated after
// construction. Made for big values such as video frames (see ADR 0002).
// One writer thread, one reader thread.
//
// Three slots; at any moment each slot belongs to exactly one role:
//   back   - the writer's private slot, being filled
//   middle - the newest complete value, waiting to be picked up (shared)
//   front  - the reader's private slot, being read
// publish() swaps back and middle; acquireLatest() swaps middle and front if middle is
// new. Only slot indices move, never the data. Because there is always a third slot to
// swap through, the two threads never touch the same slot at the same time.
//
// The rules that make in-place access safe:
//   - The reference from writeSlot() is only valid until publish(). After that the slot
//     belongs to someone else.
//   - The reference from readSlot() stays valid and unchanged until the next
//     acquireLatest(), however often the writer publishes in between. A renderer can
//     upload straight from it.
//
// For small trivially copyable values, LatestValue<T> wraps this with copy semantics.
// MSVC warns (C4324) that the alignas below pads the class: that padding is the point.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
template <typename T>
class TripleBuffer {
public:
    TripleBuffer() = default;

    // Each slot starts as a copy of `initial`. For frames, pass one allocated frame so all
    // three buffers exist up front and the writer never has to allocate.
    explicit TripleBuffer(const T& initial) : slots_{{Slot{initial}, Slot{initial}, Slot{initial}}} {}

    // Both threads hold a reference to the same object; a copy or move would split them.
    TripleBuffer(const TripleBuffer&) = delete;
    TripleBuffer& operator=(const TripleBuffer&) = delete;
    TripleBuffer(TripleBuffer&&) = delete;
    TripleBuffer& operator=(TripleBuffer&&) = delete;
    ~TripleBuffer() = default;

    // ---- Writer thread only ---------------------------------------------------------

    // The writer's private slot. It still holds whatever was in it last time (a value
    // from a few publishes ago), so overwrite everything that matters.
    [[nodiscard]] T& writeSlot() noexcept { return slots_[back_].value; }

    // Hands the filled slot over as the newest value. Never blocks.
    void publish() noexcept {
        // Release: everything written into the slot becomes visible to whoever later
        // acquires it. Acquire: the slot we get back may have just been given up by the
        // reader; its reads of that slot must be finished before we overwrite it.
        const std::uint8_t previous = middle_.exchange(back_ | kFreshBit, std::memory_order_acq_rel);
        back_ = previous & kIndexMask;
    }

    // ---- Reader thread only ---------------------------------------------------------

    // Takes the newest value if there is one since the last call. Returns true if
    // readSlot() now refers to a new value. Never blocks.
    [[nodiscard]] bool acquireLatest() noexcept {
        // Relaxed is enough for this peek: it only decides whether to swap. The exchange
        // below is the operation that synchronises with the writer.
        if ((middle_.load(std::memory_order_relaxed) & kFreshBit) == 0) {
            return false;
        }
        const std::uint8_t previous = middle_.exchange(front_, std::memory_order_acq_rel);
        front_ = previous & kIndexMask;
        return true;
    }

    // The reader's private slot: the value taken by the last acquireLatest() (or the
    // initial value if nothing was acquired yet).
    [[nodiscard]] const T& readSlot() const noexcept { return slots_[front_].value; }

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
