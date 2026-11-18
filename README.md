# Mini Surgeon Console

A small, self-built simulator of the surgeon console of a surgical robot. It exists to learn,
hands-on, the problems such a console has to solve: low-latency endoscope video with an overlay,
tele-operation of a robot arm over a network, and a safety state machine that always fails safe.

**This is a learning project, not a medical device.**

## Architecture

Two processes, five threads in the console:

| Part | Runs on | Job |
| --- | --- | --- |
| Capture | own thread | Grabs frames, timestamps them, keeps only the newest one |
| Render | Qt Quick render thread | Uploads the frame as a texture, draws video + overlay + UI in one pass |
| GUI | main thread | QML menus, state, warnings; never touches video |
| Teleop core | own thread, fixed rate | Hand controller input, state machine, watchdog |
| Comms | own thread | UDP with sequence number, timestamp and CRC-32; heartbeat |
| `arm_sim` | separate process | Simulated joints, limits, holds position when the heartbeat is lost |

Rules: video never waits for the UI; the control loop never waits for video or the UI;
every fault ends in a safe state that only an operator reset can leave.

## Milestones

- [x] **M1** Skeleton: CMake, Qt 6 QML app, `arm_sim`, Google Test, CI on Windows and Linux, sanitizers
- [x] **M2** Video path: capture thread, latest-frame mailbox, `QQuickRhiItem`, latency measurement, webcam, frozen-picture warning
- [x] **M3** `arm_sim` at 1 kHz, UDP protocol, heartbeat in both directions
- [ ] **M4** Teleop state machine (`std::variant`), watchdog, fault injector, tests with a fake clock
- [ ] **M5** HUD overlay synchronised to the video by timestamp, hidden when stale
- [ ] **M6** Soak test, profiling, risk table, requirement IDs traced to tests

## Build

Requirements: CMake 3.25+, a C++20 compiler (MSVC 2022 or GCC 13+), Qt 6.8+ (Qt Quick).
Point `QT_ROOT_DIR` at your Qt kit.

Windows (PowerShell):

```powershell
$env:QT_ROOT_DIR = "C:\Qt\6.8.3\msvc2022_64"
cmake --preset windows-msvc
cmake --build --preset windows-msvc
ctest --preset windows-msvc
```

Linux:

```bash
export QT_ROOT_DIR=~/Qt/6.8.3/gcc_64
cmake --preset linux-gcc && cmake --build --preset linux-gcc && ctest --preset linux-gcc
# core libraries only, with AddressSanitizer + UndefinedBehaviorSanitizer:
cmake --preset linux-asan && cmake --build --preset linux-asan && ctest --preset linux-asan
```

`console_app --check-video` opens the real window, reads the frame number back from the
screen and fails if it is not the newest frame, at three window sizes (default, tall, wide),
checking each time that the picture is letterboxed, not stretched. It needs a GPU backend and a display, so
ctest registers it on Windows always and on Linux only when configured from a desktop
session. Pick a backend with `QSG_RHI_BACKEND=d3d11|d3d12|vulkan|opengl`.

`console_app --measure-latency[=SECONDS]` runs the console for a 2 s warm-up plus SECONDS
(default 10) and prints capture-to-present p50 / p99 / max, render and video FPS, and dropped
frames; add `--no-vsync` to present without waiting for the display. The same figures are on
the console's side panel while it runs.

`arm_sim` is the simulated arm: a 1 kHz joint loop that follows `JointCommand` packets within
each joint's position, velocity and acceleration limits and sends `JointTelemetry` back.
`arm_sim --listen=127.0.0.1:47001 --duration=5 --report` prints how evenly it ticked.

The arm link demo (ADR 0001): start `arm_sim` in a terminal, then `console_app --arm-demo` (the
console commands a slow back-and-forth motion; `--arm=A.B.C.D:PORT` if the arm is elsewhere).
The side panel shows "Arm link: OK" and the arm following. Kill the console (Task Manager, or
`taskkill /F /IM console_app.exe`): within 30 ms `arm_sim` prints "console link lost" and
"holding position", brakes and stays there. `link_check` tests exactly that with two real
processes.

`console_app --source camera` shows a webcam instead of the synthetic pattern (Qt Multimedia;
the console builds without it). `--camera=TEXT` picks one by name or id, `--list-cameras` shows
each camera's formats and the one used. Without a matching camera the console says so and shows
the labelled pattern. If frames stop (camera unplugged, driver stalled), a red "VIDEO FROZEN"
banner appears over the last picture; `console_app --check-freeze` tests that end to end.

To run the exe from Explorer, copy Qt next to it once:
`windeployqt --debug --qmldir console_app/qml build/windows-msvc/console_app/Debug/console_app.exe`
(`--release` for a Release build). The tests do not need this.

## Layout

```
common/        plain C++: CRC-32, clock, triple buffer / latest-value mailbox, latency percentiles
video/         plain C++: Frame, test pattern with a sequence barcode, SyntheticSource, letterbox,
               presentation timing, YUV to RGBA conversion, frozen-picture rule
comms/         plain C++: the arm link's wire protocol (session, sequence, timestamp, CRC-32), sequence filter,
               UDP socket (Winsock / BSD sockets)
arm_sim/       the simulated arm process (plain C++, no Qt): joint model, 1 kHz controller
console_app/   the Qt Quick console; VideoItem draws the video with QRhi (ADR 0003), CameraSource
cmake/         warnings, sanitizers, GoogleTest
docs/          requirements, risks, architecture decisions, dev journal
```
