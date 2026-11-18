#pragma once

#include <atomic>
#include <bit>
#include <cstddef>
#include <type_traits>
#include <vector>

namespace msc {

// A bounded queue between exactly one writer thread and one reader thread, with no lock.
//
// The other mailbox in this project, LatestValue, keeps only the newest value: right for a
// picture, where an old frame is worth nothing. This one keeps every value, in order: right
// for a history, where each sample is needed (M5: where was the arm at time T?).
//
//   - Neither side ever waits for the other, and nothing is allocated after construction.
//   - When the queue is full, tryPush refuses the new value and returns false. The writer
//     decides what that means (here: count it and carry on); it is never blocked.
//   - Values come out in the order they went in, each exactly once.
//
// How: a ring of slots and two counters. `tail_` counts values pushed and is written only by
// the writer; `head_` counts values popped and is written only by the reader. Each side reads
// the other's counter to know how far it may go. A slot between head and tail belongs to the
// reader, every other slot to the writer, so the two never touch the same slot.
//
// MSVC warns (C4324) that the alignas below pads the class: that padding is the point.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
template <typename T>
class SpscQueue {
    // Values are copied in and out with plain assignment; trivially copyable keeps that a
    // memcpy that cannot throw, so both operations can be noexcept.
    static_assert(std::is_trivially_copyable_v<T>, "SpscQueue<T> needs a trivially copyable T");

public:
    // Room for at least `minCapacity` values (rounded up to a power of two, so that "which
    // slot" is a bit mask instead of a division).
    explicit SpscQueue(std::size_t minCapacity)
        : mask_(std::bit_ceil(minCapacity < 2 ? std::size_t{2} : minCapacity) - 1), slots_(mask_ + 1) {}

    // Both threads hold a reference to the same object; a copy or move would split them.
    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;
    SpscQueue(SpscQueue&&) = delete;
    SpscQueue& operator=(SpscQueue&&) = delete;
    ~SpscQueue() = default;

    [[nodiscard]] std::size_t capacity() const noexcept { return mask_ + 1; }

    // ---- Writer thread only ---------------------------------------------------------

    // Appends `value`. False (and nothing changes) if the queue is full. Never blocks.
    [[nodiscard]] bool tryPush(const T& value) noexcept {
        // Relaxed: only this thread writes tail_.
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        // Acquire: pairs with the reader's release in tryPop, so the reader has finished
        // copying out of every slot it has given back before we overwrite one.
        const std::size_t head = head_.load(std::memory_order_acquire);
        if (tail - head > mask_) {
            return false;  // all slots hold values the reader has not taken yet
        }
        slots_[tail & mask_] = value;
        // Release: the value written above becomes visible to a reader that sees this tail.
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    // ---- Reader thread only ---------------------------------------------------------

    // Takes the oldest value into `out`. False (and `out` untouched) if the queue is empty.
    // Never blocks.
    [[nodiscard]] bool tryPop(T& out) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);  // only we write head_
        if (head == tail_.load(std::memory_order_acquire)) {
            return false;
        }
        out = slots_[head & mask_];
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

private:
    // Typical cache line size (see TripleBuffer for why not the standard constant).
    static constexpr std::size_t kCacheLine = 64;

    // The counters never wrap back to a smaller capacity-relative value in practice: at a
    // million pushes per second a 64-bit counter lasts half a million years, and unsigned
    // subtraction stays correct across the wrap anyway.
    std::size_t mask_;
    std::vector<T> slots_;
    // Each counter on its own cache line: the writer and the reader each write one of them
    // on every operation, and sharing a line would make it bounce between their cores.
    alignas(kCacheLine) std::atomic<std::size_t> tail_{0};  // written by the writer
    alignas(kCacheLine) std::atomic<std::size_t> head_{0};  // written by the reader

    static_assert(std::atomic<std::size_t>::is_always_lock_free,
                  "the counters must be plain CPU loads and stores, not hidden locks");
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

}  // namespace msc
