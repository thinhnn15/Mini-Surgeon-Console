# Requirements

Each requirement has an ID. A test that verifies a requirement carries the ID in its name,
for example `TEST(Crc32, REQ_COM_001_MatchesStandardCheckValue)`, so a script can list which
requirements are tested and which are not. That script is `trace_check` (a CTest): "Verified by"
must list every test that carries the ID and nothing else, and the result is
[traceability.md](traceability.md).

| ID | Requirement | Milestone | Verified by |
| --- | --- | --- | --- |
| REQ-VID-001 | The console always shows the newest video frame; older frames are dropped, never queued. | M2 | `LatestValue.REQ_VID_001_*` (mailbox); `console_app.REQ_VID_001_NewestFrameReachesTheScreen` (end to end, needs a GPU) |
| REQ-VID-002 | Glass-to-glass latency is measured and its p50 and p99 are reported. | M2 | `LatencyStats.REQ_VID_002_*`, `PresentationTracker.REQ_VID_002_*`; `console_app.REQ_VID_002_LatencyIsMeasuredAndReported` (capture-to-present, needs a GPU; the glass-to-glass remainder needs a camera pointed at the screen, not done yet) |
| REQ-VID-003 | A picture that is not live is never shown as live: if no new frame reaches the screen for 250 ms, the view is marked frozen within one more watchdog period. | M2 | `VideoSignal.REQ_VID_003_*`; `console_app.REQ_VID_003_FrozenVideoIsFlagged` (stops the source, needs a GPU) |
| REQ-COM-001 | Every packet carries a sequence number, a monotonic timestamp and a CRC-32; corrupted or out-of-date packets are discarded. | M3 | `Crc32.REQ_COM_001_*`, `Protocol.REQ_COM_001_*` (layout, CRC, malformed and corrupted packets), `SequenceFilter.REQ_COM_001_*` (old, duplicate, restarted sender), `ArmController.REQ_COM_001_*` (the arm ignores an old command and a corrupted datagram) |
| REQ-SAF-001 | If `arm_sim` receives no heartbeat for 3 periods, it holds position. | M3 | `HeartbeatMonitor.REQ_SAF_001_*`, `ArmController.REQ_SAF_001_*` (holds within 3 periods, brakes within the limits, waits for a new command), `ArmLink.REQ_SAF_001_*`; `arm_sim.REQ_SAF_001_HoldsWhenTheConsoleDies` (two real processes, the console killed) |
| REQ-SAF-002 | Any fault moves the console to the Fault state; leaving Fault needs an explicit operator reset. | M4 | `StateMachine.REQ_SAF_002_*` (logic), `Fixture.REQ_SAF_002_*` (teleop core: each watchdog, reset refused / accepted), `TeleopWithRealArm.REQ_SAF_002_*` (a fault stops the real arm over UDP); `console.REQ_SAF_002_EveryFaultEndsInASafeState` (every injected fault, arm_sim as a real process) |
| REQ-SAF-003 | While Paused (clutch), hand motion does not move the arm. | M4 | `StateMachine.REQ_SAF_003_*` (logic), `Fixture.REQ_SAF_003_*` (teleop core), `TeleopWithRealArm.REQ_SAF_003_*` (the real arm stays put while clutched) |
| REQ-HUD-001 | Overlay data older than a threshold is hidden, never drawn at an out-of-date position. The overlay for a video frame uses the arm's pose at that frame's capture time; if that pose is not known (no telemetry at or after that time, or a gap of more than 30 ms around it), nothing is drawn. | M5 | `PoseHistory.REQ_HUD_001_*` (no extrapolation, no bridging of gaps), `HudOverlay.REQ_HUD_001_*` (which frames get a marker when the link goes quiet); `console_app.REQ_HUD_001_OverlayIsOnTheInstrumentAndHiddenWhenStale` (from the pixels the renderer drew, with a real `arm_sim`: on the instrument when synchronised, gone after the arm link is cut; needs a GPU, so it is skipped on the Windows CI runner) |
