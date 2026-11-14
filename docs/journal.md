# Dev journal

One entry per real problem met while building. These become interview stories (STAR).

Template:

```
## YYYY-MM-DD · M? · short title
- Symptom:
- Cause:
- Fix:
- Measured (before → after):
- What I would say in an interview (one sentence):
```

## 2026-11-14 · M2.1 · Relaxed ordering passes every test except TSan
- Symptom: with `memory_order_relaxed` on the index `exchange` in `LatestValue`, the Windows, Linux and ASan builds were all green, including the 1,000,000-iteration tearing stress test. Only the ThreadSanitizer job failed: "data race" between `latest_value.hpp:52` (`publish` writes the slot) and `:73` (`read` copies it).
- Cause: relaxed makes the index swap itself atomic, but it does not order the slot write before the swap, or the slot read after it. Nothing says "the writer finished filling the slot" happens-before "the reader copies it". On x86 an atomic exchange compiles to a locked `xchg`, which is a full barrier whatever order you ask for, so the hardware could not show the bug. The C++ memory model does not promise that: the compiler may move the slot accesses across a relaxed exchange, and an ARM CPU (Apple, many embedded boards) reorders them for real.
- Fix: `acq_rel` on both exchanges (release publishes the slot contents, acquire receives them). The `load` that only peeks at the fresh bit stays relaxed.
- Also checked the test itself: a deliberately broken one-slot mailbox (no synchronisation) produced 540,000 to 850,000 torn reads per million writes; `LatestValue` produced 0. A stress test is only evidence once you have seen it fail.
- Measured: on the dev PC (Debug, MSVC), 1,000,000 publishes take ~35 ms and the spinning reader gets ~460,000 fresh values; the rest are dropped by design.
- Interview sentence: "My lock-free mailbox passed a million-iteration stress test with relaxed ordering on x86 — only ThreadSanitizer showed the race, which is why TSan runs in CI and why I don't trust 'it works on my machine' for memory ordering."

## 2026-11-14 · M2.2 · The test pattern took 130 ms per frame in Debug
- Symptom: the test that renders one 1280x720 pattern took 134 ms in a Debug build; a 60 fps source has 16.7 ms per frame.
- Cause: `fillRect` copied 4 bytes per pixel through `std::copy` on vector iterators. In MSVC Debug every iterator operation is checked, so ~1 million tiny copies were slow.
- Fix: paint the first row of the rectangle pixel by pixel, then copy that row down with one `std::copy_n` per row (a single memmove each).
- Measured: 134 ms -> 1 ms per 720p frame in Debug.
- Interview sentence: "I measure Debug too: a pipeline that only meets its frame budget in Release is painful to debug, and the fix was a better loop, not turning checks off."

## 2026-11-14 · M2.2 · 60 fps on Windows came out as 15.6 / 31.2 ms gaps
- Symptom: a 5 ms-period source produced ~40 frames in 300 ms instead of 60; at 60 fps the gap between frames ranged 15.0-32.2 ms (some frames shown twice).
- Cause: Windows wakes sleeping threads on its default ~15.6 ms timer tick, so any wait is rounded up to a tick. At 5 ms the producer was always more than a period late, hit the "skip ahead, don't burst" rule, and lost frames.
- Fix: `timeBeginPeriod(1)` for as long as the producer thread runs (RAII guard, Windows only; per process since Windows 10 2004). Deadline pacing (`next += period`) kept the 60 fps average right even before the fix; the fix is about evenness.
- Measured (60 fps, 2 s): before 120 frames, gaps 15.0-32.2 ms -> after 120 frames, gaps 14.8-19.0 ms. 5 ms period: ~40 -> 61-63 frames in 300 ms.
- Interview sentence: "On Windows a sleep is only as precise as the system timer tick, 15.6 ms by default; I found it because a pacing test came out a third short, and fixed it with a scoped 1 ms timer request."
