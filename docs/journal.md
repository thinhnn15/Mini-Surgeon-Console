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

## 2026-11-18 · M2.4 · The latency figures stayed at zero: synchronize() is not called every frame
- Symptom: the first `--measure-latency` run reported 0 frames presented in 5 s, while `--check-video` in the same session saw the window present about 30 frames per second.
- Cause: the renderer handed its statistics to the item in `synchronize()`, assuming that runs every frame. It does not. The renderer keeps itself going with `QQuickRhiItemRenderer::update()`, which by design only schedules another `render()` (continuous rendering must not block the GUI thread every frame); `synchronize()` runs only when the GUI side calls `QQuickItem::update()`. After the reset at the end of the warm-up, nothing on the GUI side changed, so the statistics never came back.
- Fix: a `LatestValue<PresentationStats>` mailbox (the REQ-VID-001 one from M2.1) shared by item and renderer. The render thread publishes four times a second from the `frameSwapped` handler; the GUI thread reads it on a timer. A reset request goes the other way as an `std::atomic<bool>`. The source and clock still go over in `synchronize()`, which is right for them: they only change when the GUI side changes something.
- Interview sentence: "I assumed Qt Quick's synchronize step ran every frame and my stats never arrived; for data that flows continuously from the render thread I use a lock-free mailbox instead of piggy-backing on Qt's sync, and I compared against a second measurement path to see the bug was mine, not the machine's."

## 2026-11-18 · M2.4 · Capture-to-present latency, D3D11 vs D3D12, vsync on vs off
- Setup: Debug build, synthetic 1280x720 source at 60 fps, `console_app --measure-latency=8`, two runs each (they agreed within 0.3 ms at p50). **Measured over Remote Desktop**: the window was on the Microsoft Remote Display Adapter at 1920x1080 and 32 Hz, not on the local 60 Hz monitor, so the vsync-on figures are for a 32 Hz display. To repeat on the local monitor.

| API | vsync | render fps | video fps | dropped | capture→present p50 / p99 / max | capture→upload p50 / p99 |
| --- | --- | --- | --- | --- | --- | --- |
| D3D11 | on | 32.0 | 32.0 | ~47 % | 9.5 / 18.5 / 19.1 ms | 9.1 / 18.0 ms |
| D3D12 | on | 32.0 | 32.0 | ~47 % | 10.3 / 18.9 / 19.4 ms | 9.6 / 18.1 ms |
| D3D11 | off | ~9 300 | 60.0 | 0 | 1.25 / 2.75 / 3.0 ms | 0.85 / 2.25 ms |
| D3D12 | off | ~3 300 | 59–60 | 0–10 | 1.4 / 3.4 / 4.1 ms | 0.95 / 2.9 ms |

- Where the time goes, with vsync: almost all of it is **capture→upload**, the frame waiting for the next draw. The renderer always takes the newest frame, so a frame's age when it is drawn is spread over one source period (16.7 ms): p50 ≈ half of that plus ~1 ms of drawing the test pattern, p99 ≈ the whole period. Upload→present is ~0.4 ms: `Present()` only queues the frame, it returns before the frame is on screen.
- What is not in these numbers: after `Present()` the frame still waits in the swap chain queue (up to 2 frames, `max frame latency = 2` in the D3D11 log) for vsync, then the compositor, scan-out and the panel. That remainder is the difference between capture-to-present and glass-to-glass, and needs a camera pointed at the screen (M2.5). With vsync off it shrinks but the picture can tear.
- Dropped frames with vsync on (~47 %) are the newest-frame rule doing its job: 60 frames per second in, 32 shown. A display slower than the source must drop; queueing them instead would add latency every frame (RISK-003).
- D3D11 vs D3D12: the same with vsync on (D3D12 about 0.5–0.8 ms higher at p50). Uncapped, Qt's D3D12 backend draws about a third as many frames per second, i.e. more CPU time per frame; irrelevant at 60 Hz, but D3D11 stays the default.
- Interview sentence: "At 60 fps the latency floor with a newest-frame mailbox is about half a source period at p50 and a whole one at p99 — the render path itself was under half a millisecond — so the next win is a faster camera or presenting in sync with capture, not a faster renderer."

## 2026-11-18 · M2.5 · Running windeployqt into the build folder broke the offscreen tests
- Symptom: after copying Qt next to `console_app.exe` (so it starts from Explorer), the next `ctest` stopped on a modal "no Qt platform plugin could be initialized ... Available platform plugins are: windows" dialog.
- Cause: with `Qt6Cored.dll` beside the exe, Qt looks for plugins there. windeployqt copies the `windows` platform plugin, not `offscreen`, which the smoke tests ask for. `qFatal` in a Debug build on Windows shows the CRT's Abort/Retry/Ignore dialog, which blocks the test run instead of failing it.
- Fix: the tests set `QT_PLUGIN_PATH` to the Qt they were built with, next to the `PATH` they already set. Deployed or not, they find every plugin.
- Interview sentence: "A test environment should not depend on what happens to be lying next to the binary: deploying Qt for a manual run broke the headless tests, so the tests now name their Qt explicitly."

## 2026-11-18 · M2.5 · Camera frames arrive on the GUI thread, and the conversion was running there
- Question from the issue: on which thread is `QVideoSink::videoFrameChanged` emitted? The first `CameraSource` assumed a Media Foundation worker thread and converted the frame right in the (direct) connection.
- Measured instead of assumed: logging the thread of the first delivery said **the GUI thread** (Qt 6.8, Windows, FFmpeg media backend). So the CPU colour conversion was running on the GUI thread: 42 ms per 720p frame in Debug, i.e. at 15 fps 63 % of the GUI thread, and at the camera's 30 fps more than all of it. The GUI thread also runs the frozen-picture watchdog.
- Fix: delivery only stamps the arrival time and drops the frame (a reference-counted handle, no pixels copied) into a one-frame hand-off slot, replacing an older one if the camera thread is behind (counted, never queued). A camera thread of our own converts and publishes. It is the TripleBuffer's only writer whatever thread the backend uses, which also makes the one-writer rule hold on backends that deliver on several threads.
- Interview sentence: "The docs did not say which thread delivers camera frames, so I logged it: on Windows it was the GUI thread, where my 40 ms Debug conversion would have starved the UI. Delivery now only hands a reference over to a conversion thread."

## 2026-11-18 · M2.5 · What the C310 really sends, and what converting it costs
- Formats (`console_app --list-cameras`): uncompressed YUYV only up to 640x480 at 30 fps; 1280x720 YUYV only at 7.5 fps. 720p at 30 fps would be 1280 x 720 x 2 bytes x 30 = 55 MB/s, more than USB 2.0 really carries. At 720p30 the camera offers JPEG, and the list has an identical set of "NV12" formats: most likely Media Foundation's MJPEG decoder, so the decoding happens in the driver stack before the frame reaches the console and is not in the numbers below. `chooseFormat` takes smooth (>= 25 fps) first, then a format we convert ourselves, then the most pixels up to 720p: 1280x720 NV12 at 30 fps.
- The camera delivered about 15 fps in every run, Debug and Release alike, in a dim room: the camera lengthens its exposure in poor light. The conversion was not the limit.
- Setup as in M2.4 (Remote Desktop, 32 Hz, vsync on, D3D11), 8 s after a 2 s warm-up. The camera rows were measured with the conversion still on the GUI thread (before the fix above); the hand-off adds microseconds. Later runs the same afternoon rendered at 10.6 fps for the synthetic source too (the Remote Desktop session throttling), and were thrown away.

| Source | Build | capture→present p50 / p99 | capture→upload p50 / p99 | conversion to RGBA p50 / p99 |
| --- | --- | --- | --- | --- |
| synthetic 720p, 60 fps | Debug | 9.5 / 18.5 ms | 9.1 / 18.0 ms | none (rendered as RGBA) |
| C310 720p NV12, ~15 fps | Debug | 60.5 / 72.4 ms | 60.0 / 72.0 ms | 41.7 / 43.1 ms |
| C310 720p NV12, ~15 fps | Release | 19.3 / 33.4 ms | 18.8 / 32.8 ms | 2.2 / 2.7 ms |

- Reading it: in Release the latency is the conversion (2 ms) plus the wait for the next draw. The camera (15 fps) is slower than the display (32 Hz), so that wait is spread over one *display* period (31 ms): p50 ≈ 2 + 16, p99 ≈ 2 + 31. The synthetic source is faster than the display, so there the wait is spread over one *source* period (16.7 ms). The wait is bounded by whichever is faster, source or display. In Debug the conversion alone is 42 ms, longer than a camera frame period.
- "Capture" for the camera is arrival in the process. Exposure, the camera's processing, USB, MJPEG decoding and the driver come before it and are not seen here; measuring them needs the camera pointed at a clock on the screen (glass-to-glass).
- CPU vs shader conversion: on the CPU it is simple, testable without a GPU, and the renderer stays format-agnostic, but it costs a pass over every pixel (2 ms Release, 42 ms Debug at 720p) and uploads 4 bytes per pixel instead of 1.5. In the fragment shader the conversion is nearly free and the upload smaller, but each format needs its own shader and textures (two planes for NV12). Next step if the cost matters.
- The converters check once per frame that each plane really holds the rows its size and stride promise (a driver bug must not make the console read past a buffer), then run on plain pointers. Checking every access instead (std::span indexing) would cost per pixel, and Debug is already the slow case.
- Interview sentence: "The webcam's latency is dominated by waiting for the next display refresh and, in Debug, by my CPU colour conversion — 42 ms against 2 ms in Release — which is why YUV-to-RGB belongs in the shader next."

## 2026-11-18 · M2.5 · Telling a frozen picture from a live one (REQ-VID-003)
- The hazard: when the camera is unplugged the renderer keeps drawing the last frame. A frozen picture looks exactly like a live one of a still scene. Black would at least be noticed.
- Rule: watch the capture timestamp of the newest frame that reached the screen. Older than 250 ms: frozen (red banner with the age); none yet: waiting. A sequence number that stops moving would work too, but the age also catches a source that keeps publishing old frames, and it has a unit a person understands.
- Where: on the GUI thread, on its own 50 ms timer, reading one atomic the renderer updates per new frame. It does not depend on the source or the renderer, the things it watches. What it cannot catch: a hung GUI thread, which also cannot draw the banner. That needs a watchdog outside the process (M4/M6).
- Test: `--check-freeze` stops the synthetic source the way an unplugged camera stops and checks the age of the picture when it is flagged. The first version measured time since the stop against 500 ms and once came out at 480 ms. That figure also contains how old the frame already was at the stop, which depends on the display. Measured over Remote Desktop: flagged at 269–395 ms of age; values above 300 ms are the GUI thread waiting for the render thread's sync when QML changes (4 times a second for the statistics) at 32 Hz.
- Interview sentence: "A frozen frame is more dangerous than a black one because it looks live, so the console watches the age of the newest frame on screen, from a thread that does not depend on the video pipeline, and shows how old the picture is."

## 2026-11-18 · M3.1 · The wire protocol, and proving the bit-flip test can fail
- Layout: a 24-byte header (magic, version, type, payload length, session, sequence, sender timestamp), the payload, and a CRC-32 over both. Written byte by byte in little-endian. A struct copied from memory would carry the compiler's padding and the CPU's byte order onto the wire. A test pins the header's bytes, because a round trip alone passes with any pair of matching bugs.
- Refused, each with its reason: too short, wrong magic or version, a length that does not match the datagram, bad CRC, unknown type, wrong payload size for the type, and values no sender may send (NaN or infinite joint values, an unknown arm state). The CRC is checked before the type or payload is read, so corruption is reported as corruption.
- Old packets: an intact old command is still dangerous, because it tells the arm to go back to where it was. `SequenceFilter` lets through only packets newer than the last accepted one, using serial number arithmetic so it holds across the 32-bit wrap. A random session id per sender start makes a restarted sender (counting from 1 again) get followed instead of ignored. The last few replaced sessions are remembered, so a late packet from before the restart cannot switch back.
- Sabotage: with the CRC check disabled, 1087 of the 1288 single-bit flips of the three message types decoded as valid packets, and three tests failed. The other 201 were still caught by the magic, version and length checks. With the check on: 0. 200,000 random buffers (half of them behind a valid header) never decode, and under ASan/UBSan in CI they also never read outside the buffer.
- Interview sentence: "My packet decoder refuses every single-bit flip, and I proved the test is real by disabling the CRC check and watching 1087 of 1288 flips get through; old-but-intact packets are dropped by sequence number, with a session id so a restarted sender is not mistaken for an old one."

## 2026-11-18 · M3.2 · Windows breaks a UDP socket's next receive after sending to nobody
- Question from the issue: what does Windows do to a UDP socket after it sends to a port nobody listens on?
- Measured: the next receive on that socket fails with error 10054 (WSAECONNRESET, "connection reset"), 1 ms after the send, although UDP has no connection. The other machine (here loopback) answered the datagram with ICMP "port unreachable", and Winsock reports that on the socket. Linux reports ICMP errors only on connected UDP sockets, so an unconnected one just keeps waiting.
- Why it matters here: when the console restarts, the arm keeps sending telemetry to a port with nobody behind it for a moment. With the default behaviour the arm's receive loop would see an error at exactly the moment it most needs to keep running, and a naive loop might give up.
- Fix: `WSAIoctl(SIO_UDP_CONNRESET, FALSE)` on every socket. Test `UdpSocket.SendingToAPortNobodyListensOnDoesNotBreakReceiving` checks it; with the ioctl set back to TRUE it fails with error 10054.
- Also: a datagram larger than the buffer is reported as truncated, not silently cut. Linux needs `recvmsg` and `MSG_TRUNC` to tell; Windows says `WSAEMSGSIZE`. The rest of a UDP datagram is gone, never delivered by the next receive.
- Interview sentence: "On Windows, sending UDP to a dead port makes the next recvfrom fail with 'connection reset' — on a connectionless socket. I found it with a test of the restart case and turned it off with SIO_UDP_CONNRESET, because the arm must keep running while the console comes and goes."

## 2026-11-18 · M3.3 · The joints overshot: continuous braking maths in a stepped simulation
- Symptom: a 1 rad move of the base joint (1.5 rad/s, 6 rad/s²) should take 0.917 s by the trapezoid; the model took 0.947 s and passed the target by 0.12 mrad before crawling back.
- Cause: the braking speed was `sqrt(2 a d)`, which is right for continuous motion. In steps of 1 ms the step first moves `v·dt` and only then brakes, and braking in steps covers `v²/2a − v·dt/2`. Every step started braking a little late.
- Fix: the largest speed that still stops at the target in the stepped motion, `v = −a·dt/2 + sqrt((a·dt/2)² + 2ad)`. Now no overshoot, and the move takes 0.91–0.93 s.
- The fix exposed a second bug, caught by the test that checks every step against the limits: the "arrived, stop exactly here" snap set the speed to 0 after the speed had already changed in the same step, i.e. up to 2·a·dt, measured 8 rad/s² against a limit of 6. The snap now looks at the speed at the start of the step.
- Interview sentence: "A model that is integrated in steps needs the stepped version of the physics: my joints overshot until I derived the braking speed for discrete steps, and a test that checks the acceleration limit on every step caught the snap that broke it."

## 2026-11-18 · M3.3 · A 1 kHz loop on Windows 11: 635 Hz until two fixes
- Question from the issue: how far off is a 1 kHz loop that sleeps until each deadline?
- Measured (5 s, Debug, the loop alone):

| Setup | Rate | Overruns | Tick lateness p50 / p99 / max |
| --- | --- | --- | --- |
| `timeBeginPeriod(1)` + `sleep_until` (the M2.2 fix) | 635 Hz | 200 | 0.97 / 15.0 / 15.6 ms |
| + opt out of power throttling of timer resolution | 976–981 Hz | 82–107 | 0.95–0.98 / 2.05–2.11 / 2.5–2.8 ms |
| + high-resolution waitable timer | 1000.1 Hz | 0 | 0.24–0.25 / 0.64–0.66 / 0.96–0.99 ms |
| high-resolution timer without the opt-out | 1000.1 Hz | 0 | 0.26–0.27 / 0.60–0.70 / 1.26–1.36 ms |
| all, Release | 1000.2 Hz | 0 | 0.28 / 0.73 / 1.86 ms |

- Cause 1, the 15.6 ms ticks: Windows 11 ignores a process's timer resolution request while the process has no visible window, to save power. `arm_sim` never has a window, so the M2.2 fix did nothing for it. (The console has a window, which is why the video producer was fine.) `SetProcessInformation(ProcessPowerThrottling, PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION)` opts out.
- Cause 2, the ~1 ms lateness on every tick: even at 1 ms resolution, a plain sleep wakes on the next 1 ms tick, so a 1 ms loop is always about one period late, and every tenth tick or so is late enough to count as an overrun. A waitable timer created with `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` is not tied to the tick at all. With it the loop runs at 1000 Hz, no overruns, worst tick under 1 ms late, and it no longer needs the throttling opt-out (which stays for the other waits in the process).
- Does 0.25 ms of lateness matter? The model is stepped by the measured time since the last tick, not by the nominal 1 ms, so the motion stays right; lateness only adds that much delay between a command and its effect. A real arm controller runs on a real-time OS (or PREEMPT_RT Linux) and is held to microseconds; that is out of scope here. Linux numbers come from the CI log ("Arm loop timing" step).
- Interview sentence: "My 1 kHz loop ran at 635 Hz on Windows 11 because the OS ignores timer-resolution requests from windowless processes; I measured each fix separately, and a high-resolution waitable timer brought it to 1000 Hz with the worst tick 1 ms late."

## 2026-11-18 · M3.4 · Kill the console, the arm holds: measured with two real processes
- Rule (REQ-SAF-001): the console sends a heartbeat every 10 ms. More than 3 periods without one while the arm is following, and the arm holds. Holding means every joint brakes at its acceleration limit and stays where it stopped. An instant stop would need infinite deceleration; driving "home" would move on its own with nobody watching. Commands are ignored while the console is not alive. When heartbeats return, the arm still holds until a new command arrives: it never resumes toward a target from before the silence.
- The watchdog lives in `arm_sim`, because it must work exactly when the console does not. Liveness is measured from arrival on the arm's own clock, never from the console's timestamp.
- Unit level (`ManualClock`, real loopback sockets): the hold comes on the first tick after 30 ms of silence (30–31 ms), and the braking stays within every joint's acceleration limit.
- End to end (`link_check`): starts `arm_sim` and a console process, waits until the arm is moving, kills the console with TerminateProcess / SIGKILL, and watches the telemetry. The first version reported 15–30 ms, which is impossible: if the last heartbeat left at most 10 ms before the kill, the hold comes 20–30 ms after it. The kill was timed *after* TerminateProcess had waited for the process to die, which takes a few ms. Timed from before the kill: 26.0–28.6 ms in 8 runs (limit 80 ms).
- With the real `console_app --arm-demo`, killed by `taskkill /F`: "console link lost: no heartbeat for 30.2 ms", hold, stop. The longest gap between two heartbeats that still counted as alive (a new figure in the report) was 11.1 ms, against a limit of 30. The console's comms thread holds a 1 ms timer resolution of its own: its waits are socket timeouts, which follow the OS tick (15.6 ms by default on Windows). Two late ticks in a row would be a false hold. The 11.1 ms measured may only have been possible because `arm_sim` on the same machine had raised the resolution already.
- Linux, for M3.3 (CI's "Arm loop timing" step, GitHub runner): 1000.3 Hz, no overruns, tick lateness p50 0.060 / p99 0.075 / max 0.27 ms, about four times tighter than Windows (the Windows runner: 0.27 / 0.95 / 0.98 ms).
- Interview sentence: "I killed the console process mid-motion and the arm held 26–29 ms later, which is the 30 ms heartbeat limit minus however long ago the last heartbeat had left; when my first measurement said 15 ms, the physics said that was impossible, and it was my timestamp, not the arm."

## 2026-11-18 · M4.1 · The teleop state machine as types
- States are types in a `std::variant` (`Idle`, `Engaged`, `Paused`, `Fault`), and each carries only the data that exists in it. Only `Engaged` has anchors and only `Fault` has a code and a time. "Paused, but with the anchors of an old engage" cannot be written down, so it cannot happen. With an enum plus member variables, the anchors would sit there in every state, waiting to be used by mistake.
- Transitions are one `std::visit` over (state, event) with an overload per decision. Two generic overloads are decisions too: a detected fault wins in every state, and anything not listed means nothing. Overload resolution picks the most specific one, so a new state or event is handled by an explicit decision or falls into "ignored", never into whatever an `else` happened to say.
- Motion: target = armAnchor + 0.5 × (hand − handAnchor). Engaging and releasing the clutch re-anchor, so taking over never makes the arm jump, and moving the hand while clutched is never replayed on the arm.
- Fault is latched (REQ-SAF-002). It keeps the first code (later faults are usually effects of it), ignores everything but a reset, refuses the reset while the cause is still present, and goes to Idle, not back to Engaged. The cause going away by itself changes nothing: a link that flickers back must not resume an arm in motion behind the operator's back.
- Sabotage: letting Disengage leave Fault (one extra overload) failed `REQ_SAF_002_FaultIsLatchedAndIgnoresEverythingButReset`.
- Interview sentence: "I modelled the teleop states as a std::variant so each state only has the data that is valid in it; transitions are one std::visit where every pair is an explicit decision, and the fault state is latched — even when the cause disappears, only an operator reset leaves it."

## 2026-11-18 · M4.2 · The teleop core: watchdogs, and why each limit is what it is
- The core is a 200 Hz thread. Each tick it reads its mailboxes (hand, operator requests, arm link status, video live flag, injected faults), works out which fault conditions are present, feeds the state machine, and tells the arm to move (Engaged) or hold (every other state). It never waits on anything. Nobody else reads the arm link's status: `LatestValue` has one reader, and before this step the GUI was it. The core forwards the link status in its own status for the GUI.
- `HoldCommand` (protocol type 4): while nobody drives the arm, the console says so every 10 ms. Before it, an idle or faulted console just stopped sending targets, and the arm kept going to the last one, possibly far away.
- Watchdog limits:
  - Hand input stale, 100 ms: at 200 Hz that is 20 missed readings, far beyond jitter. At the arm's top speed of 2.5 rad/s, 100 ms of motion on a frozen hand reading is at most 0.125 rad (×0.5 scale).
  - Commands not acknowledged, 100 ms: the arm echoes the sequence of the newest command it applied. A plain "echo == sent" check would fire on every tick, because one command is always in flight. The watchdog instead fires when the echo has not moved for 100 ms while we keep sending.
  - Arm holding on its own, 100 ms after taking over: right after engage or clutch release, the arm still reports Holding from our own hold commands until the first move arrives (about 10–20 ms).
  - Video frozen: as soon as the video watchdog says so (250 ms, REQ-VID-003). Moving an instrument you cannot see is the one thing never to do.
  - Arm link lost: the link's 30 ms. The arm has already held on its own by then.
- In Idle the same conditions only block engaging. In Engaged and Paused they fault. In Fault they block the reset.
- Tests: every watchdog with `ManualClock` and a fake `ArmPort`, including "just below the limit is no fault". Against the real arm controller over UDP, each piece on its own thread: the arm follows the hand at 0.5 (hand +0.4, arm +0.2 within 1e-4 rad); while clutched, with the hand moved by 0.8, the arm stays within 1e-4; after release it continues from where it is. An injected fault mid-move brakes the real arm to a stop short of the target, and it stays there.
- Interview sentence: "The console's watchdog is not the arm's: the arm protects itself when the console disappears, and the console stops tele-operation when anything it depends on (hand, video, acknowledgements) goes stale, and then tells the arm to hold rather than just falling silent."

## 2026-11-18 · M4.3 · The operator UI, and the newest datagrams UDP throws away
- The window drives the arm now. `TeleopModel` plays the hand controller: every 10 ms it sends the pad position and the clutch to the teleop core. The panel shows what the core decided (state, fault and what is still present, whether engage and reset would be accepted) and passes on requests. It decides nothing itself, so there is one place where the state lives and the tests of that place cover the UI too. Because the GUI thread is the hand device here, a GUI stall longer than 100 ms is a hand-input fault, as a frozen real hand controller would be.
- The video's live state feeds the core: not live (frozen, or no frame yet) blocks engaging and faults an engaged console. Under `QT_QPA_PLATFORM=offscreen` there is no GPU, so no video, and the panel says "Still present: video not live" with Engage disabled. This is the requirement working, seen in the first screenshot.
- Without a desktop (remote session), `--screenshot=FILE` with the offscreen platform shows the layout. The offscreen platform has no fonts until `QT_QPA_FONTDIR` points at some.
- A test from M3.4 failed under TSan on Linux, reading arm telemetry at 0.12 rad when the arm had long been at 0.5. The fake console had let a second's worth of telemetry (1000 datagrams) pile up unread. When a UDP receive buffer is full the kernel drops the new datagrams, so the buffer kept the oldest ~270, and "the newest in the buffer" was from the start of the move. That is the opposite of the newest-wins rule everywhere else in this console. `ArmLink` is safe because it drains the socket every 10 ms; the test now reads as it goes too. It passed on Windows and in earlier Linux runs only because of buffer sizes and timing.
- Interview sentence: "A full UDP receive buffer drops the newest datagrams, not the oldest, so a reader that falls behind ends up with the oldest data, exactly the wrong way round for control; the fix is to drain every period, which my link did and my test did not."

## 2026-11-18 · M4.4 · The fault injector found two safety holes the unit tests had not
- `safety_check` starts `arm_sim` as a real process, runs the console's arm link and teleop core on their own threads, and for every fault does the whole chain. It engages and moves, injects, and expects Fault with the right code. A reset while the fault is still injected must be refused. After clearing, the arm must be stopped and holding, the reset must be accepted and lead to Idle, and the arm must stay put.
- Detection times (3 runs, Windows, Debug):

| Injected | Detected as | After |
| --- | --- | --- |
| cable pulled (nothing in or out) | arm link lost | 33–43 ms (the arm held on its own at 30 ms) |
| one bit flipped in every packet to the arm | arm stopped on its own | 37–42 ms |
| hand controller frozen | hand controller not responding | 103–109 ms |
| video stopped | video not live | ~5 ms |
| generic | injected fault | ~5 ms |

- The slowest is the frozen hand (100 ms by design: a hand that is held still must not fault). The most dangerous is the one nobody would notice, commands not reaching the arm, and it is also the one that found the holes.
- Hole 1: the arm acknowledged commands it had ignored. `lastCommandSequence` was updated when a command was received, also when it was then ignored because the console was not alive. The telemetry claimed "applied" for a command that never moved anything. Now it is updated only when the command is applied (test `REQ_SAF_001_AnIgnoredCommandIsNotAcknowledged`).
- Hole 2: a reset went through while every command was still being refused. With corruption on, the console faulted at ~40 ms (the arm stopped on its own) and the scripted reset came at ~45 ms. No watchdog saw a cause at that moment:
  - "arm holding on its own" only means something while engaged;
  - the acknowledgement watchdog only ran while operating;
  - and the acknowledgement watchdog needs 100 ms anyway.

  The reset was accepted with the link still broken. Two changes:
  - acknowledgements are watched in every state, because the console always sends a move or a hold;
  - a reset needs fresh evidence of health, an acknowledgement that arrived after the fault, not just "no watchdog has fired yet".

  Unit test `REQ_SAF_002_ResetIsRefusedWhileTheArmStillDoesNotAcknowledge`.
- Both holes were in code with green unit tests, and both were about the gap between two timers or between "received" and "applied". Only running the real pieces together, with the fault actually happening, showed them.
- Also: `.clang-tidy`'s readability-redundant-member-init wanted the `{}` initializers removed that GCC's -Wmissing-field-initializers requires. GCC wins (it breaks the build), so the check is off, with the reason in the file.
- Interview sentence: "My fault injector found that a reset was accepted 45 ms into a fault while the arm was still refusing every command — no watchdog had fired yet — so I changed the rule from 'no fault visible' to 'fresh evidence of health since the fault', and found on the way that the arm was acknowledging commands it had ignored."

## 2026-11-18 · M5.1 · A history of poses: the second kind of mailbox
- The overlay's question is "where was the arm when this frame was captured?". A frame on screen is 15 to 100 ms old; the newest telemetry is under 1 ms old. Drawing the newest pose on an old picture puts the marker ahead of the instrument by speed × latency. So the console has to remember poses, by time.
- Every other hand-over in this console is "newest wins" (`LatestValue`): an old video frame or an old status is worth nothing. A history needs the opposite: every sample, in order. Hence `SpscQueue`: a ring and two counters, each written by one side only. The writer never waits; when the queue is full it is told (false) and carries on. Two mailboxes, two contracts: drop the old, or keep them all and refuse the new when there is no room.
- `ArmLink` now hands each accepted telemetry packet to a sink, on the link thread, with its arrival time on the console's clock. Why arrival time and not the arm's own timestamp: the arm's clock is another clock (another machine, another zero), and using it needs an estimate of the offset that can be wrong. Arrival time can only be late, by the time on the wire plus however late the link thread woke up. The arm's timestamp travels along in the sample, so the choice can be measured (M5.4) and changed in one place.
- `PoseHistory::at(T)`: binary search in a fixed ring, a straight line between the two samples around T. The rules for REQ-HUD-001 are all refusals:
  - T newer than the newest sample: not known. No holding the last pose, no continuing its motion.
  - T between two samples more than 30 ms apart: not known. Both ends of the hole are known, but the arm braked on its own in between.
  - T older than the history: not known.
- Why interpolation is allowed and extrapolation is not: between two samples the error is bounded by the arm's acceleration, a·h²/8 (6 rad/s² across a 30 ms gap: 0.0007 rad, under a pixel). Past the last sample the error grows with speed × time and nothing bounds what the arm did.
- Sabotage: returning the last pose for a newer time, and bridging any gap, failed three `REQ_HUD_001_*` tests. A queue that is never full failed the two-thread test (values out of order: the writer overwrote slots the reader had not taken).
- Interview sentence: "My video path drops old frames and my pose path keeps every sample, because a picture only needs the newest and a history needs them all; the lookup interpolates between samples but refuses to extrapolate, since inside two samples the error is bounded by acceleration and outside it is bounded by nothing."

## 2026-11-18 · M5.2 · The camera sees the arm, 83 ms late
- To judge an overlay you need something in the picture for it to sit on. The synthetic camera now draws the instrument: a shaft from its entry point to the tip, thicker at the near end, with an orange tip. `ToolCamera` is the geometry: the shaft pivots about the point where it enters the body (joint 1 swings it left/right, joint 2 up/down, joint 3 slides it in and out), and a pinhole camera projects it. The same function will place the overlay's marker in M5.3, so the picture and the overlay can only disagree about time, never about geometry.
- Why a delay: the pattern reaches the screen 3 ms after capture (15 ms with vsync in the worst case). At the demo's 200 px/s that is a 3 px error for an overlay that ignores time, hard to see and impossible to test. A real endoscope chain takes 50 to 100 ms. `--video-delay=80` holds every frame back; `captureTime` is not touched. Measured: capture to present p50 97.2 ms, p99 98.2 ms (5 ticks of 16.7 ms = 83.3 ms, plus the 14 ms the pipeline took before).
- Two timestamps, two users. The overlay needs `captureTime` (when was this picture true?). The frozen-picture watchdog also measures from capture, so a delayed chain eats into its 250 ms: with an 80 ms delay a frame is already ~100 ms old when it appears. That is why the option stops at 150 ms.
- First version of the delay line: "deliver every frame that is at least 80 ms old". 41 frames dropped in 6 s. 80 ms at 60 fps is 5 ticks = 83.3 ms, 3.3 ms to spare; when a tick came 4 ms late, the frame captured there missed its turn, and the next tick delivered two at once, of which one is shown. Now delivery goes by ticks: the frame captured N ticks ago, one per tick, whatever the clock says. A due frame is swapped with the mailbox's write slot, so its 3.7 MB are never copied.
- A limit of the simulation: the camera learns the arm's pose from the same telemetry as the console. With the link cut it keeps drawing the last pose it heard, where a real camera would keep seeing the real arm.
- Sabotage: stamping `captureTime` at delivery instead of capture failed `ADelayedFrameArrivesLaterButKeepsItsCaptureTime`.
- Interview sentence: "I gave the simulated camera a delay on purpose: with a 3 ms pipeline every timing mistake in the overlay is three pixels and invisible; at 83 ms the same mistake is 17 pixels, so it can be seen, and tested."

## 2026-11-18 · M5.2 · The demo kept faulting: a GUI thread stalled for 300 ms by the desktop
- The first screenshot of the new picture showed FAULT: "hand controller not responding". The scripted hand (`--hand-demo`) ran on the GUI thread's timer, and the GUI thread was stalling: gaps of 250 to 400 ms between two 10 ms timer ticks, every few seconds. The hand watchdog (100 ms) did its job.
- Not this step's code: with the M5.2 changes stashed, the same stalls (285, 309, 340 ms). And not the renderer: it presented 60.0 fps through them.
- Where the GUI thread was: a temporary watcher thread suspended it when a tick was 90 ms overdue and walked its stack. Three places: inside `NtUserPeekMessage`, inside `NtUserSetTimer` (calls into the Windows window manager that normally return at once), and in Qt's expose handling waiting for the render thread. So the time went in the window system, not in the application. On the same desktop `explorer.exe` was using 70 to 130 % of a core. That is the likely cause, not a proven one.
- What follows for the design. The pad has to stay on the GUI thread: where the mouse went while the GUI was not looking is not known, so a stalled GUI is a stalled hand, and the fault is right. But a scripted hand needs nothing from the window, so, like a real hand device, it now runs on its own thread (and is the core's only hand producer in that mode). After the change: 8 s engaged with no fault on the same desktop.
- Interview sentence: "My demo kept faulting on 'hand controller not responding'; a stack sample of the stalled GUI thread showed it waiting inside the window system for 300 ms at a time, so the watchdog was right and the design was wrong: a hand device must not live on the GUI thread, and the scripted one no longer does."

## 2026-11-18 · M5.3 · The overlay is drawn by the thread that draws the frame
- The ring around the instrument's tip is drawn in `VideoRenderer::render`, in the same render pass as the frame, straight after the video quad and in the same viewport. For the frame it is about to draw, the renderer asks `HudOverlay` where the arm was at that frame's `captureTime`, with the same projection the camera used.
- Why not a QML item on top of the video: its position would be set on the GUI thread, from a property. The render thread shows it with whatever frame it draws next, which the GUI thread cannot know; and when the GUI thread stalls (M5.2: 300 ms on this desktop) the marker would stand still on a moving picture. In the render pass, the marker and its frame reach the screen together or not at all.
- The render thread is the pose queue's one reader: each draw it empties the queue into its own `PoseHistory` (about 17 poses), then looks up one time. No lock anywhere between the arm link and the screen.
- REQ-HUD-001 on screen: no pose for the frame's time means no ring, and the side panel says why ("hidden: no arm data for the picture on screen"). A ring that vanished without a reason would look like a bug; one that stayed would look like knowledge.
- With the link cut (`--inject=link@1.8-6`): frames captured before the cut still get their ring, because the arm's pose at their moment is known. The first frame captured after the last pose gets none. With an 80 ms video delay the ring therefore stays about 100 ms after the cut, and every one of those rings is true.
- The answer for one frame is kept while that frame is on screen. A frozen picture keeps the ring that belongs to it, even after the history has forgotten that moment. "Not known" is not kept: the telemetry for the frame's moment may arrive one draw later.
- Naive mode (key H, or `--hud=naive`): the newest pose on whatever frame is on screen, in yellow so it is never taken for the real thing. With the 80 ms delay the ring visibly leads the instrument. Measured in M5.4.
- The ring itself: one quad and a fragment shader (distance from the centre, two `smoothstep`s), its matrix and colour in a 96-byte uniform buffer. Blended over the picture, with the alpha written kept at 1, because Qt Quick composes the item by that alpha.
- Sabotage: falling back to the newest pose when the frame's time is not known failed four tests, two of them `REQ_HUD_001_*`.
- Interview sentence: "The overlay is drawn in the video's own render pass from the frame's capture timestamp, so it cannot be a frame off; and when the pose for that timestamp is not known it draws nothing and says why, because a marker in the wrong place is worse than no marker."

## 2026-11-18 · M5.4 · Measured: 0.3 px synchronised, 7 px naive, gone 100 ms after the link
- `console_app --check-hud --arm-exe=.../arm_sim` starts a real `arm_sim`, drives it with the scripted hand, and measures from the pixels the renderer drew: the centre of the instrument's orange tip and the centre of the overlay's ring, in pixels of the 1280-wide frame. 30 captures synchronised, 30 naive, then the arm link is cut and restored. Video delay 80 ms. Eight runs on Windows (Release, D3D11):

| | median | worst capture |
| --- | --- | --- |
| synchronised: ring to tip | 0.24–0.41 px | 0.69–1.41 px |
| naive: ring to tip | 6.9–7.6 px | 9.8–13.5 px |
| naive, predicted (tip speed × frame age when drawn: ~77 px/s × ~88 ms) | 6.6–7.3 px | |
| naive, measured / predicted | 1.00–1.08 | |

- The naive error is exactly what the arithmetic says, which is the useful result: it is not "a bit off", it is speed × latency, and it grows with both. At the arm's top speed (up to about 1000 px/s in this picture) the same 88 ms would be 90 px: the tip would be outside its own ring.
- Arm link cut: the ring was gone 93–123 ms later, with the panel saying "no arm data for the picture on screen". That is the video latency, not a timeout: frames captured before the cut still get their ring, because the arm's pose at their moment is known. Back 97–102 ms after the link.
- What stamping poses with their arrival time costs (the M5.1 decision): over ~10 000 packets per run, none arrived more than 1 ms later than the quickest; worst 0.1–0.9 ms. At this tip speed that is under 0.1 px; at top speed, about 1 px. Arrival time stays. This is loopback on one machine: across a real network the figure would have to be measured again.
- Three things went wrong before the first number:
  - Grabbing the window (`QQuickWindow::grabWindow`) stalled the GUI thread for 1.00–1.16 s after every grab on D3D11, on every grab. A 60 fps overlay cannot be timed with one look per second. The check now asks the renderer to read back its own render target after the pass (`readBackTexture`), which costs nothing visible and also says which frame was drawn and when.
  - On a desktop with two screens at 100 % and 125 %, the window came up half on each, and most grabs did not match the window's geometry. (Reading back the item's own target does not care where the window is.)
  - One of the first three runs crashed at exit, after printing PASS. The renderer owned the read-back's result and its callback pointed at the renderer, while QRhi keeps a pointer to the result until the pixels arrive; the check always has one read-back under way when it quits. Not proven with a debugger, but the result now lives in the channel shared by the item and the renderer, which outlives the renderer, and the callback touches only the channel: 15 runs since, no crash.
- The check's own timer runs on the GUI thread, which stalls here (M5.2). So the time the ring disappeared is not "when the check looked" but the time of the first draw without it, noted by the renderer and carried in the capture.
- Sabotage, end to end: the synchronised overlay using the newest pose gave 11.6 px and failed three limits. Holding the last pose when the frame's time is not known failed "the ring stayed on screen after the arm link was cut".
- What the simulation does not cover: a real camera's timestamps (exposure time, rolling shutter, a driver that stamps late); the trip on a real network; two clocks drifting; and the camera here learns the pose from the same telemetry, so a wrong telemetry value would be wrong in the picture and in the overlay alike. The check proves the timing and the plumbing, not the kinematic calibration.
- Interview sentence: "I measured the overlay from the rendered pixels: synchronised to the frame's capture time it sits 0.3 px from the instrument, drawn from the newest pose it is 7 px off, which is the tip's speed times the frame's age to within 8 %; and when the arm link is cut the ring is gone after one video latency, because the frames captured before the cut are still telling the truth."
