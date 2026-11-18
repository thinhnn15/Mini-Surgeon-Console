# ADR 0003: Draw the video with a QQuickRhiItem, uploading straight from the frame

Status: accepted (M2.3)

## Context

Frames arrive in a `TripleBuffer<Frame>` (ADR 0002). Something in the Qt Quick window has
to put the newest one on screen, scaled to the video area, every display refresh. Later the
HUD overlay (M5) must be drawn in the same pass, from data matched to the same frame, so
video and overlay can never be shown out of step.

Rules from the README: video never waits for the UI, and the GUI thread never touches video.

## Options

1. **QML `Image` + `QQuickImageProvider`**: wrap each frame in a `QImage` and let QML show
   it. Runs through the GUI thread (property change, image request), copies the frame, and
   QML may cache. Breaks "the GUI thread never touches video".
2. **`QQuickItem::updatePaintNode` + `QSGSimpleTextureNode`**: create a `QSGTexture` from a
   `QImage` in the sync step. No custom shaders, but the texture is built from a `QImage`
   copy on every frame, and there is nowhere natural to draw an overlay with our own
   shaders later.
3. **`QQuickRhiItem`** (Qt 6.7+): our own renderer on the render thread with full QRhi
   access: our texture, our shaders, our draw calls. Qt Quick gives it an offscreen colour
   buffer and composites that into the scene.
4. **`QSGRenderNode`**: like 3 but draws straight into the window's render pass, without
   the offscreen buffer. Saves one full-size blit, but must cooperate with the scene graph's
   state (scissor, stencil, clip, opacity) and is harder to get right.

## Decision

Option 3. The renderer runs on the render thread and is the source's one reader: one
`acquireLatest()` per rendered frame, then the texture upload reads `Frame::pixels` in
place through `QByteArray::fromRawData`. No copy on our side, no allocation per frame.
The GUI thread only hands over the `shared_ptr<FrameSource>` in `synchronize()`.

Shaders are written once in Vulkan-style GLSL and compiled at build time by `qsb`
(`qt_add_shaders`), so the same item runs on D3D11, D3D12, Vulkan, Metal and OpenGL.
Checked on this PC: D3D11, D3D12, Vulkan and OpenGL all pass the end-to-end check.

The picture is letterboxed (`msc::video::letterbox`), never stretched: a surgeon judges
distances and angles from the image.

## The one subtle rule

`fromRawData` means Qt does not own the pixels. D3D11 and OpenGL defer the actual upload to
the end of the Qt Quick frame, after `render()` has returned. That is safe because the
frame stays ours until the next `acquireLatest()`, and that only happens in the next
`render()`, after this frame has ended. Anything that breaks that order (a second acquire
per frame, rendering the item twice per frame, acquiring outside `render()`) would let the
writer overwrite pixels the GPU has not read yet. If that ever becomes necessary, use
the copying `QRhiTextureSubresourceUploadDescription(const void*, quint32)` constructor
instead (about 130 µs per 720p frame, ADR 0002).

## Consequences

- One extra full-size blit per frame (the item's offscreen buffer into the window). Small
  at 720p; M2.4 measures the whole latency, and option 4 is the fallback if it matters.
- The item renders on every vsync while it has a source, even if no new frame arrived.
  That keeps the newest frame on screen at most one refresh late.
- The software backend cannot draw a `QQuickRhiItem`. Decision: the video area then shows a
  notice ("No video: the software renderer is active…", driven by `GraphicsInfo.api` in
  QML) instead of staying black, because a black video area looks like a dead camera. The
  CI smoke test (software, offscreen) only checks start-up; the end-to-end check needs a
  real GPU backend and fails at once, with that reason, on the software one.
- The M5 overlay draws in this same renderer, after the video quad, from data looked up
  by the frame's `captureTime`.

## What would change this decision

- The blit shows up in the latency budget (M2.4), or 4K / stereo makes it expensive:
  move to `QSGRenderNode`.
- A camera delivers NV12 / YUYV (M2.5): upload the planes as they are and convert to RGB in
  the fragment shader, rather than converting on the CPU.
