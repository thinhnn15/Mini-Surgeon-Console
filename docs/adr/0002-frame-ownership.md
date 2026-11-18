# ADR 0002: Hand video frames over in place, through a triple buffer

Status: accepted (M2.2)

## Context

The capture thread produces frames; the render thread shows the newest one (REQ-VID-001).
A 1280x720 RGBA8 frame is 3,686,400 bytes, 60 times a second. Who owns a frame, and how
does it get from one thread to the other?

Measured on the dev PC (Release, MSVC, 600 runs each, scratch benchmark):

| Operation on one 3.7 MB frame | Time |
| --- | --- |
| `memcpy` into an existing buffer | ~130 µs |
| allocate a new zeroed buffer (`make_shared<vector>`) | ~530 µs |

The frame period at 60 fps is 16,667 µs.

## Options

1. **Copy through `LatestValue<Frame>`**: copy in on publish, copy out on read. About
   260 µs per frame, 1.6 % of the frame period. Simple. (`LatestValue` would also need to
   accept a non-trivially-copyable `Frame`.)
2. **`shared_ptr<const Frame>`**: the writer allocates a new frame each time and passes the
   pointer. Easy to reason about (immutable frames, freed when the last user lets go), but
   it allocates 3.7 MB per frame, which costs about four times a copy, and the cost varies
   with the allocator's mood. On Linux, blocks that big are mapped from the OS and given
   back on free, so every frame also page-faults.
3. **Triple buffer used in place** (`TripleBuffer<Frame>`): three frames are allocated
   once. The writer renders straight into its slot and publishes; the reader takes the
   newest slot and uploads straight from it. Only a slot index changes hands.

## Decision

Option 3.

The measurement says copying is not expensive here: option 1 would be acceptable on this
PC. The reasons for option 3 are not raw speed:

- **No allocation and no copy on the video path after start-up**, so the time per frame
  does not depend on the allocator or on page faults. Latency that is low *and steady*
  matters more for a surgeon's view than a good average.
- **Headroom.** A 4K frame is 33 MB (copy about 1.2 ms, twice per frame), and a surgical
  console shows stereo video (two streams). Option 1 grows with that; option 3 does not.
- It reuses the triple buffer already built and tested for REQ-VID-001, rather than adding
  a second mechanism.

## Consequences

- The writer must overwrite everything that matters in its slot: the slot still holds a
  frame from a few publishes ago.
- The reader's frame stays valid only until its next `acquireLatest()`. The renderer must
  finish uploading before it asks for the next one (it does: one acquire per render).
- Changing the resolution means reallocating the slots, which is only safe when the
  source is stopped. Fine for now; a camera that changes format at run time will need a
  "reconfigure" step (M2.5).

## What would change this decision

- Frames small enough (for example 320x240, about 300 KB) that a copy is noise: option 1
  is simpler to read and review.
- A reader that must keep a frame longer than one render (recording, a freeze-frame
  button): option 2's shared ownership fits that better.
