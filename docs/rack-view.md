# StageHand — Rack View (UI v1 design, for review)

The first piece of StageHand's own UI: a main window showing the running rack,
one row per plugin instance, editable live. It uses the layout decided in
architecture.md ("Sequencer UI") and DGL (spike passed 2026-09-28). This doc
covers the models and APIs underneath the view. Widgets come after review.

## Scope

**In v1**
- Main window: toolbar, rack rows, status bar.
- Row input side: the source (All, a device, a virtual input), channels,
  groups (MIDI 2.0), key range, velocity range, transpose, and a MIDI activity
  light.
- Row middle: plugin name and vendor, an editor button, and the
  placeholder state for a missing plugin.
- Row output side: gain fader (dB), mute, a stereo meter with a clip latch,
  and the output pair.
- Row actions: enable/disable, remove. Add a plugin from a plain list of
  scanned plugins.
- Save (Ctrl-Shift-S) and quit (Ctrl-Q), with the existing save-by-default rule.
  The same keys work in the main window and in editor windows.

**Not in v1:** reorder, rename, plugin search, MIDI effects, sends and
inserts, exposed plugin parameters, key-range learn, the sources dock and
sequences, rack switching and setlists, undo.

## Layers

```
 DGL widgets (RackWindow, RowWidget, ...)     draw RackViewModel; emit intents
        │ intents                   ▲ RackViewModel (rebuilt per tick)
        ▼                           │
 RackController (App)                    main thread; no DGL types
        │ edits                     ▲ reads
        ▼                           │
 Session (edit API)  ──commit──▶  Engine  ◀──atomics──  SlotRuntime (shared)
```

- **Data flows one way.** The Session is the source of truth. Widgets never
  change state directly. They send an intent, the controller applies it, and
  the next view model reflects it.
- **RackController and RackViewModel have no DGL types**, so tests/ can
  exercise them headless with FakePlugin, the way SessionTests does. The DGL
  layer stays thin: layout, drawing, and turning mouse input into intents.
- **Everything runs on the main thread** inside MainLoop, as in the spike: a
  16 ms DGL idle timer, and a 33 ms tick for meters and coalesced commits.

## 1. Session edit API

Edits apply to the running session. Each one updates the slot's saved
settings (`Session::Slot::saved`, the RackSlot that capture() writes out) and
marks the session modified. Slots are addressed by `RackSlot::id`, which stays
stable once reorder arrives.

Edits come in two kinds, depending on how they reach the audio thread.

**Live (no graph commit):** gain and mute. These are what a performer moves
while notes sound. They're written to the slot's SlotRuntime (section 2) as a
target gain. The audio thread ramps toward it (section 3), so a fader drag
costs one atomic store and allocates nothing.

- `setGainDb(id, db)`
- `setMuted(id, muted)`: new rack field `muted`, default false. It's additive,
  so the format version doesn't change. **Later:** mute also releases the
  slot's sounding notes, so unmuting doesn't bring back notes (or stuck notes)
  that were held through the mute. The engine already knows them: its held-note
  table gives each note sent to that plugin, at the note number the plugin got.
  After the ramp reaches zero, the engine sends those note-offs, which is more
  precise than CC 123 and works for CLAP-dialect ports. Those held-note entries
  are then dropped for that plugin, so the keyboard's later note-offs go
  nowhere. Delivering it needs a main-to-audio command path (muting is an
  atomic today, with no event).

**Structural (graph commit):** everything that changes which plugins run or
what they hear. The Session rebuilds the EngineGraph from all its slots and
commits it. Plugins are reused (shared_ptr) and never reactivated. Edits are
coalesced: an edit marks the graph dirty, and `flush()` commits at most once
per controller tick. A key-range drag therefore commits at most ~30 times a
second, and only while it moves.

- `setSource(id, MidiSourceRef)`: resolves and opens a device if needed.
  Returns a problem string if the device isn't connected, and the slot hears
  nothing, as on load.
- `setFilter(id, MidiFilter)`: channels, groups, note and velocity ranges,
  transpose. The source fields are ignored, as in RackSlot.
- `setEnabled(id, bool)`: a disabled slot stays loaded and doesn't run. This is
  a setup action, not a performance one: removing a slot from the graph cuts its
  sound, and its tails, immediately. Mute is the live equivalent.
- `setFirstOutput(id, n)`
- `addSlot(PluginDescriptor, index) -> id`: create, activate, generate a unique
  id, add to the graph. Activation happens on the main thread and may take a
  moment. The UI shows it as loading.
- `removeSlot(id)`: the controller closes the slot's editor first. The plugin
  lives on in the old graph until the audio thread stops it and `collect()`
  destroys it on the main thread (the existing contract).

**Other queries and hooks**
- `bool modified()`, `markSaved()`: drive the toolbar's unsaved indicator.
  Plugin state changes count too: CLAP's `clap_host_state.mark_dirty`
  (currently a no-op in ClapPlugin) is forwarded through HostServices to the
  Session.
- `load()`, `capture()`, `unload()` are unchanged. `load()` resets
  `modified()`.

Held notes are already safe across all of these: the engine's held-note table
is keyed by plugin, so a note-off still reaches the plugin its note-on went to,
at the note number that plugin got, after any filter or transpose change.

## 2. SlotRuntime: the per-slot state shared across threads

```
struct SlotRuntime {                         // src/Engine/SlotRuntime.h; one per loaded slot
    std::atomic<float>         targetGain;   // main writes (gain, 0 when muted); audio reads
    LevelMeter                 meter;        // post-gain peaks per channel plus a clip latch:
                                             //   audio add()s, main take()s and clearClip()s
    std::atomic<std::uint32_t> midiEvents;   // audio: events delivered to this slot
    // audio thread only: currentGain and the ramp's target, step and frames left
};
```

- The Session creates one per slot and holds it as a `shared_ptr` alongside the
  plugin. `SlotConfig` gets a `runtime` pointer; `SlotConfig::gain` goes away.
- Every graph rebuilt for that slot points to the same SlotRuntime. Meters,
  activity counts, and the ramp position are therefore continuous across
  commits, and stay tied to the slot, not to its index in some graph.
- Lifetime follows the plugin rule: graphs are destroyed only on the main
  thread, so the last reference never drops on the audio thread.
- `Engine::takeOutputPeak()` is replaced by `Engine::outputMeter()`, a
  LevelMeter for the master outputs.
- Meter ballistics (decay, peak hold) happen in the view model, not on the
  audio thread.

## 3. Gain ramp (resolves the domain-model open question)

The audio thread moves `currentGain` toward `targetGain` linearly, reaching it
in 10 ms. The step is computed per chunk, and the gain is applied per sample
when summing. When the two are equal, it falls back to the current
constant-gain loop, so a slot at rest costs nothing extra. A new slot starts
with `currentGain = targetGain`, so it doesn't fade in. Tests: a step in
target gain produces a bounded per-sample change, and the ramp reaches the
target exactly.

## 4. RackViewModel

This is what the widgets draw: plain values rebuilt by the controller each
tick, with no pointers into the Session.

```
RackViewModel
  rackName, modified
  rows[]: RowModel
    id, name, pluginName, vendor
    state          running | disabled | loading | placeholder (+ reason)
    hasEditor, editorOpen
    sourceLabel    "All" | "KeyLab 88" | "MIDI 2.0 In" | "KeyLab 88 (not connected)"
    sourceProtocol MIDI 1.0 | 2.0 (for the badge)
    pluginNoteDialects             (to warn when a MIDI 2.0 source feeds a MIDI 1.0 plugin)
    filter         channels, groups, noteLow/High, velocityLow/High, transpose
    gainDb, muted, output pair
    meterDb[2], peakHoldDb[2], clipped, midiActive
  sourceChoices[]  All, virtual inputs, enumerated hardware (for the source menu)
  status           sample rate, buffer, xruns, MIDI drops, protocol preference
```

Intents from the widgets map one-to-one onto Session edits, plus
`openEditor(id)`, `closeEditor(id)`, `save()`, `quit()` and `clearClip(id)`.

## 5. RackController

It owns what main.cpp now does inline: the Session, the editor
windows (keyed by slot id), save-target logic, and the ticks. main.cpp shrinks
to parsing arguments, starting the backend and MIDI input, and building a
RackController plus the RackWindow. `--no-ui` keeps today's headless behavior,
which stays useful for tests and for a machine with no display.

Behavior on the main window:
- Closing the main window quits, saving by default.
- Closing an editor window only closes the editor.
- Editors open on request, not all at load (a change from M4's "open all by
  default", which doesn't scale to a real rack). `--open-editors` keeps the old
  behavior.

## 6. DGL layer (after this is reviewed)

`RackWindow` (a top-level NanoVG widget), then `RowWidget`s with three
sub-areas, a toolbar and a status bar. Constraints carried from the spike and
the DGL notes in architecture.md:
- Construct and destroy NanoVG widgets inside a
  `Window::ScopedGraphicsContext`. Close the window and quit the Application
  before it's destroyed.
- No text entry in v1. Values are edited by drag, scroll and
  double-click-to-reset. The source is a popup list.
- Stage-first drawing: large type, high contrast, and a UI scale factor
  (HiDPI and a "big" mode).

## Review decisions (2026-09-28)

1. Add and remove are in v1, with a plain list of scanned plugins.
2. Editors open on demand. `--open-editors` keeps the old behavior.
3. Mute is its own rack field, separate from `enabled`. Mute also sending
   note-offs is deferred (see `setMuted`).
4. The ramp time is fixed at 10 ms.
