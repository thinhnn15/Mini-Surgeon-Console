# Requirements

Each requirement has an ID. A test that verifies a requirement carries the ID in its name,
for example `TEST(Crc32, REQ_COM_001_MatchesStandardCheckValue)`, so a script can list which
requirements are tested and which are not.

| ID | Requirement | Milestone | Verified by |
| --- | --- | --- | --- |
| REQ-VID-001 | The console always shows the newest video frame; older frames are dropped, never queued. | M2 | `LatestValue.REQ_VID_001_*` (mailbox; end-to-end in M2.3) |
| REQ-VID-002 | Glass-to-glass latency is measured and its p50 and p99 are reported. | M2 | |
| REQ-COM-001 | Every packet carries a sequence number, a monotonic timestamp and a CRC-32; corrupted or out-of-date packets are discarded. | M3 | `Crc32.REQ_COM_001_*` (CRC part) |
| REQ-SAF-001 | If `arm_sim` receives no heartbeat for 3 periods, it holds position. | M3 | |
| REQ-SAF-002 | Any fault moves the console to the Fault state; leaving Fault needs an explicit operator reset. | M4 | |
| REQ-SAF-003 | While Paused (clutch), hand motion does not move the arm. | M4 | |
| REQ-HUD-001 | Overlay data older than a threshold is hidden, never drawn at an out-of-date position. | M5 | |
