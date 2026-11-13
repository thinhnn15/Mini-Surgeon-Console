# ADR 0001: Run the simulated arm as a separate process

Status: accepted (M1)

## Context

The console mixes a lot of complex code: Qt Quick, GPU rendering, video capture. Any of it can crash
or hang. The arm must stay safe when that happens.

## Options

1. One process, the arm simulation on its own thread.
2. A separate process, talking to the console over UDP.

## Decision

Option 2. A crash in the console cannot corrupt the arm's memory, and the arm can detect the
console's death through the missing heartbeat and hold position. It also mirrors real systems,
where the console and the robot controller are different computers.

## Consequences

- A wire protocol is needed (sequence number, timestamp, CRC), with its own tests.
- Clocks of the two processes are not shared; timestamps need care (M3, M5).
- Demo for the interview: kill `console_app` and show that `arm_sim` holds position.
