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

## 2026-11-18 · M2.3 · The end-to-end check grabbed an all-white window on OpenGL
- Symptom: `console_app --check-video` passed 5/5 on D3D11, D3D12 and Vulkan, but with `QSG_RHI_BACKEND=opengl` one run in five read the barcode as 4294967295 (every bit white). The saved grab was white from corner to corner, side panel included, so the video path was not the culprit.
- Cause (most likely): the check grabbed after a fixed 1.5 s. OpenGL start-up is slower (that run had produced 112 frames before the grab, the others ~90), and the grab caught a window that had not presented a real frame yet.
- Fix: wait until the window has presented 30 frames (`frameSwapped`, counted on the GUI thread), with a 20 s give-up that fails loudly. Wait for the event you need, not for a time you hope is long enough.
- Also checked the check itself: with a sabotaged renderer that took a new frame only every 30th draw, it failed every time, showing frames 17-20 behind the newest.
- Measured: before 4/5 pass on OpenGL → after 12/12 on each of OpenGL, D3D11 and Vulkan.
- Interview sentence: "My screen-readback test failed one run in five on OpenGL; the grab was blank, not stale, so I replaced a fixed sleep with counting presented frames — the flake was in the test's timing, and the picture proved it."

## 2026-11-18 · M2.3 · Resizing the window under test on a 100 % + 125 % desktop
- Symptom: after the check started resizing the window (to force bars on different sides), about 1 run in 6 failed with "the video item is not inside the window". The log showed the window going from 1600x900 at device pixel ratio 1.00 to 576x720 at 1.25 between two steps, and once a grab still had the previous step's size.
- Cause: resizing is asynchronous. The window straddled two screens with different scaling; a resize moved its centre onto the other screen, Windows changed its DPI, and the grab was taken while the size and the item's geometry were still catching up.
- Fix: centre the window on its own screen at a size that fits it, and before checking require that the window has the size asked for and that grab, window and item agree. If not, place it again and wait another warm-up, at most 10 times. Only geometry is retried; a wrong picture (bars not black, stale or unreadable barcode) fails at once.
- Measured: 40/40 passes over D3D11, D3D12, Vulkan and OpenGL, with 27 settle retries in those runs: the waits were needed, not decoration.
- Interview sentence: "A GUI test has to wait for the state it needs, not assume a resize took effect: on a mixed-DPI desktop the window manager moved my window between screens mid-test, so the check now waits for the geometry to settle, and only retries geometry, never the picture."

## 2026-11-18 · M2.3 · A sabotaged renderer passed because it beat in time with the test
- Symptom: a renderer sabotaged to take a new frame only every 30th draw (screen up to 30 frames stale) passed the check, which by then waited 30 presented frames before each grab.
- Cause: aliasing. The grab's own draw was always the 31st, exactly the one where the sabotaged renderer took a fresh frame. A fault with a period can hide from a test that samples at the same period.
- Fix: stagger the waits (30, 37, 44, 51, 58 frames). The same sabotage then failed 4/4; a period of 23 and a renderer that forgets `update()` (the window stops presenting, the check times out) fail too.
- Interview sentence: "My screen check sampled every 30 frames and a fault that repeated every 30 frames slipped through; re-running the sabotage after each change of the test is what caught it."
