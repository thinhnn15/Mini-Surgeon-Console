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
- [ ] **M2** Video path: capture thread, latest-frame mailbox, `QQuickRhiItem`, latency measurement
- [ ] **M3** `arm_sim` at 1 kHz, UDP protocol, heartbeat in both directions
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

## Layout

```
common/        plain C++: CRC-32, clock, triple buffer / latest-value mailbox (later: protocol)
video/         plain C++: Frame, test pattern with a sequence barcode, SyntheticSource
arm_sim/       the simulated arm process (plain C++, no Qt)
console_app/   the Qt Quick console
cmake/         warnings, sanitizers, GoogleTest
docs/          requirements, risks, architecture decisions, dev journal
```
