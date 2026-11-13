# Risk table (mini FMEA)

Modelled loosely on ISO 14971: hazard, how it happens, the harm, the control, and how the control
is verified. Severity and probability are rough (1 = low, 5 = high).

| ID | Hazard | Sequence of events | Harm | Sev | Prob | Control | Verified by | Requirement |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| RISK-001 | Arm keeps moving on an old command | Network stalls, `arm_sim` keeps applying the last command | Tissue injury | 5 | 3 | Heartbeat watchdog in `arm_sim`, hold position after 3 missed periods | | REQ-SAF-001 |
| RISK-002 | Overlay shows the instrument in the wrong place | Telemetry arrives late, overlay drawn with old data | Wrong judgement of position | 4 | 3 | Timestamp every sample, hide overlay when stale | | REQ-HUD-001 |
| RISK-003 | Video lags behind the hands | Frames queue up between capture and display | Imprecise motion | 4 | 3 | Latest-frame mailbox, latency measured in CI | | REQ-VID-001 |
