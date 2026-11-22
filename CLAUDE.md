# CLAUDE.md

Qt 6.8+ (Qt Quick, QRhi), C++20. See README.md for architecture and milestones.

## Build and check

```bash
cmake --preset linux-gcc && cmake --build --preset linux-gcc && ctest --preset linux-gcc
cmake --build build/linux-gcc --target all_qmllint        # must be clean for touched QML
build/linux-gcc/arm_sim/arm_sim &                          # then:
build/linux-gcc/console_app/console_app                    # or --screenshot=FILE to check visuals
```

A UI change is not done until it has been seen: run the app, or take `--screenshot` (works with
`QT_QPA_PLATFORM=offscreen`), and look at the result.

## UI architecture

The UI shows what C++ decided and passes requests on; it decides nothing.

### 1. Split the UI into reusable components

- One component per file under `console_app/qml/components/` (e.g. `PanelButton.qml`,
  `StatusChip.qml`, `HandPad.qml`), listed in `QML_FILES` of `qt_add_qml_module` in
  `console_app/CMakeLists.txt`. `Main.qml` only lays out and wires components together.
- A component takes its data through `property` (`required property` when it has no sensible
  default) and reports through `signal`. It never reaches for an id outside its own file
  (`teleop`, `video`, `faults`): the parent passes in what it needs, so the component works
  anywhere and can be loaded on its own.
- Anything drawn more than once, or a self-contained panel (status bar, teleop panel, video
  panel, fault chips), is its own component. Inline `component X: ...` only for a helper used in
  that one file.
- Colours, sizes, spacing and fonts come from one shared `Theme` singleton
  (`pragma Singleton` QML file, or a `QML_SINGLETON` C++ class), not hex literals repeated
  across files.
- A component sizes itself through `implicitWidth`/`implicitHeight`; the parent decides the
  actual size. No fixed window-relative sizes inside a component.

### 2. No heavy logic in QML

Allowed in QML: property bindings, layout, anchors, states/transitions, animations, `qsTr()` and
formatting a value C++ already computed, forwarding an input event to one C++ call.

Not allowed in QML; do it in C++:

- Deciding state: `switch`/`if` chains on a state, comparing state strings
  (`teleop.state === "fault"`), working out what the next mode is (e.g. the `H` key cycling
  `hudMode` belongs in a C++ `cycleHudMode()`).
- Calculations: geometry, timing, statistics, unit conversion, clamping, filtering.
- `Timer`s or `Connections` that drive behaviour (as opposed to a purely visual animation).
- A JavaScript function longer than a few lines, or one that a test would need to cover.

Rule of thumb: if it could be wrong in a way that matters, it belongs in C++ where Google Test
can cover it.

### 3. Logic lives in C++

- Keep the decision itself in a Qt-free library (`teleop`, `video`, `hud`, ...), tested with
  Google Test. The QML-facing class is a thin adapter that turns it into properties and calls.
- Give QML ready-to-show values (a `severity` enum, a status text, an `...Allowed` flag) instead
  of raw data it has to interpret.

## Writing QML

- Start every file with `pragma ComponentBehavior: Bound`. In delegates, declare
  `required property int index` / `required property var modelData` (or the model's roles)
  instead of using them unqualified.
- Qualify every access: `root.label`, `button.active`, never a bare name from an outer scope or
  `parent.someProperty` reached through several levels. The root item's id is `root`.
- Type everything: `property real`, `property string`, `property list<...>`, enum types from C++;
  `var` only when there is no better type. JS functions get type annotations
  (`function format(ms: real): string`). Typed code is compiled ahead of time by qmlcachegen;
  untyped code falls back to the interpreter.
- Do not break bindings by assigning in JS (`x = 5` replaces the binding `x: a + b` for good).
  Change the source of the binding instead, usually a C++ property.
- Signal handlers use arrow functions with named parameters: `onPressed: event => { ... }`.
- Order inside an object: `id`, property declarations, signals, functions, property bindings,
  attached properties/handlers, child objects, states/transitions.
- All user-visible text through `qsTr()`.
- Files are PascalCase (`HandPad.qml`), ids and properties camelCase. Format with `qmlformat`.

## Performance (60 fps render, latency matters)

- Video and per-frame data never travel through QML properties: video stays in `VideoItem`
  (`QQuickRhiItem`), the HUD in its render pass. QML only gets throttled summaries, as
  `VideoItem::statsTimer_` does. Do not emit a NOTIFY signal at the 1 kHz control rate.
- Bindings must be cheap: no loops, allocations or string building over lists in a binding.
- Hide with `visible: false`, not `opacity: 0`. Use a `Loader` (`active:`) for panels that are
  rarely shown.
- No `layer.enabled`, `ShaderEffect`, `clip: true` or `DropShadow`-style effects unless needed:
  each one can add an offscreen pass in the same frame as the video.
- Lists that change at run time use a C++ `QAbstractListModel` with roles, not a `QVariantList`
  that QML has to rebuild whole on every change.

## C++ ↔ QML interface

- Register types only through `qt_add_qml_module`: `QML_ELEMENT`, `QML_SINGLETON`,
  `QML_UNCREATABLE("reason")`, `QML_NAMED_ELEMENT`. No `qmlRegisterType`, no
  `setContextProperty`.
- `Q_PROPERTY(T name READ name [WRITE setName] NOTIFY nameChanged FINAL)`. Emit the NOTIFY only
  when the value actually changed. Group signals (`changed`) only when the values always change
  together.
- States and modes are `enum class` + `Q_ENUM`, not strings.
- Requests from QML are `Q_INVOKABLE` (or public slots) returning nothing or a cheap value; the
  C++ side validates every request again. A button being disabled in QML is a convenience, never
  the safety check.
- New C++ objects reach QML as `QML_SINGLETON`s or through `setInitialProperties` /
  `required property`, not by `findChild` on an `objectName`.
- Objects returned to QML have a QObject parent (or are explicitly owned), so the JS garbage
  collector never deletes them.

## Threads

- A QObject exposed to QML lives on the GUI thread; its properties are read and written there
  only.
- Other threads (capture, teleop, comms, the render thread) talk to it through queued signals,
  `QMetaObject::invokeMethod(..., Qt::QueuedConnection)`, atomics or the lock-free mailbox
  `msc::LatestValue` (`common/include/msc/common/latest_value.hpp`). No mutex the GUI thread
  waits on in the normal path (the check-only capture hand-over in `VideoStatsChannel` is the
  exception).
- The GUI thread never blocks: no sleeps, no blocking I/O, no waiting on the teleop or video
  threads.
- Render-thread code (`QQuickRhiItemRenderer`) never touches QML objects. State crosses either in
  `synchronize()` or through a shared channel of atomics / `LatestValue` like
  `VideoStatsChannel`. Do not rely on `synchronize()` for render → GUI data: a renderer that keeps
  itself going with `update()` does not get `synchronize()` calls (see `video_item.hpp`).

## Safety in the UI

- Faults and warnings come from C++ and are always visible: nothing may cover the fault panel or
  the "VIDEO FROZEN" banner, and no animation may delay showing them.
- Every control's enabled state is bound to a C++ `...Allowed` property, never computed in QML.

## Existing code

`console_app/qml/Main.qml` predates these rules (inline components, `switch` on `teleop.state`,
string compares, the `H`-key cycle, no `ComponentBehavior: Bound`, two `qmllint` "unqualified
access" warnings on `faults`) and `main.cpp` finds objects with `findChild`. When a change touches
one of those parts, bring it in line with these rules as part of that change.
