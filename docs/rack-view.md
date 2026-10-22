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
  so the format version doesn't change. **Since 2026-10-01 mute also covers
  what `enabled` did, and `enabled` is gone** (`SlotRuntime::muted`, read
  per block):
  - At the block where the engine sees the mute, it sends the plugin a
    note-off for every note it holds, at the note number it got, from the
    held-note table. It then drops the plugin from those entries, so the
    keyboard's later note-offs go nowhere. The note-offs go at once rather
    than after the fade, because a release under the fade is inaudible anyway
    and it frees voices sooner.
  - While muted, note-ons don't reach the plugin. Other MIDI does, so
    controller state such as the sustain pedal stays right. Unmuting
    mid-chord brings back nothing: only notes played after unmuting sound.
  - Once the gain reaches 0 and the plugin's own output stays under -100 dBFS
    for 0.2 s, or after 5 s muted whatever it outputs (a drone, noise), it's
    suspended. It isn't processed (`stopProcessing()`), except for a block
    that brings it events. It then runs that block unheard and stops again.
    Unmuting resumes it at the next block. The plugin stays loaded, so its
    settings and state are kept, and no graph commit is needed.

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
- (`setEnabled(id, bool)` was removed 2026-10-01: mute does its job; see
  `setMuted`.)
- `setFirstOutput(id, n)`
- `addSlot(PluginDescriptor, index) -> id`: create, activate, generate a unique
  id, add to the graph. Activation happens on the main thread, synchronously;
  a slow plugin briefly holds up the UI (not audio).
- `removeSlot(id)`: the controller closes the slot's editor first. The plugin
  lives on in the old graph until the audio thread stops it and `collect()`
  destroys it on the main thread (the existing contract).
- `replacePlugin(id, PluginDescriptor)` (2026-10-22): swaps the row's plugin
  for another and keeps the row. The id stays, so sequences, loopers and links
  that name the row still find it. Its settings, MIDI effects and colour
  stay too. The new plugin starts from its defaults. A row still named after
  its old plugin takes the new one's name; one named otherwise keeps its
  name. This song's Control Link targets on the old plugin's parameters are
  dropped, and a link left with no targets goes with them. Its level and send
  targets stay. The rig's links follow the plugin id, so they stop or start
  applying on their own. The note port goes back to 0 if the new plugin
  doesn't have it. The old plugin fades out as a mute does (its notes end at
  once, a 10 ms fade): it's added to the draining slots, muted and without
  the row's MIDI effects, and goes once quiet. If the new plugin can't be
  created, nothing changes. A placeholder can be replaced too, and then its
  MIDI effects load. There's no confirmation: the saved rack keeps the old
  plugin and its state until the next save. The controller closes the row's
  editor first.

**Other queries and hooks**
- `bool modified()`, `markSaved()`: drive the toolbar's unsaved indicator.
  Plugin state changes count too: CLAP's `clap_host_state.mark_dirty` goes
  through `HostServices::stateChanged()` to the app, which marks the Session
  modified.
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
tick, with no pointers into the Session. Implemented in
`src/App/RackViewModel.h`.

```
RackViewModel
  rackName, modified
  rows[]: RowModel
    id, name, pluginName, vendor
    state          running | placeholder (+ reason)   (disabled: gone 2026-10-01, see setMuted)
    hasEditor, editorOpen
    sourceLabel    "All" | "KeyLab 88" | "MIDI 2.0 In" | "KeyLab 88 (not connected)"
    sourceProtocol mixed | MIDI 1.0 | 2.0 (for the badge)
    pluginDialects, protocolWarning (a MIDI 2.0 source feeds a MIDI 1.0 plugin)
    filter         channels, groups, noteLow/High, velocityLow/High, transpose
    gainDb, muted, output pair
    meterDb[2], peakHoldDb[2], clipped, midiActive
  sourceChoices[]  All, virtual inputs, enumerated hardware (for the source menu)
  status           sample rate, buffer, xruns, MIDI drops, protocol preference,
                   save target, master meter and clip
```

Intents from the widgets map one-to-one onto Session edits, plus
`openEditor(id)`, `closeEditor(id)`, `save()`, `quit()`, `clearClip(id)`,
`clearOutputClip()`, `addPlugin()` (from `availablePlugins()`, scanned once)
and `refreshSources()` (when the source menu opens). Meter ballistics: the
display falls at 24 dB/s to a -60 dB floor, and peaks hold for 1.5 s. The MIDI
light stays on for 150 ms after activity.

## 5. RackController

`src/App/RackController.h`. It owns the editor windows (keyed by slot id),
saving, and the periodic work: `service()` every 5 ms (engine collect, plugin
idle) and `tick()` about 30 times a second (flush edits, rebuild the model).
It sees editors only as `EditorHandle`s and audio only as an `AudioStatus`
function, both supplied by the app. That keeps it free of X11 and JACK and
tested headless (tests/RackControllerTests.cpp). main.cpp builds it and keeps
the console reporting, which is the only UI until step 4. In step 4, `--no-ui`
will keep that headless behavior, which stays useful for tests and for a
machine with no display.

Behavior on the main window:
- Closing the main window quits, saving by default.
- Closing an editor window only closes the editor.
- Editors open on request, not all at load (a change from M4's "open all by
  default", which doesn't scale to a real rack). `--open-editors` keeps the old
  behavior. Until the rack window exists (step 4), main.cpp still opens them all
  at start, since nothing else could.

## 6. DGL layer

`src/Ui/RackUi.h` is the only DGL-facing class. Its header has no DGL types, so
main.cpp and the tests never include DGL. It owns the DGL Application and the
Window, and a single top-level NanoVG widget that draws the whole view model
and hit-tests it, rather than one widget per control. The MainLoop drives it
(`idle()` every 16 ms, `refresh()` after each controller tick), and closing
the window quits, saving by default. `--no-ui` keeps the console-only mode.

Implemented in step 4: toolbar (rack name, unsaved-changes tag, Save), rows
(input side displayed, not yet editable; slot and plugin names; Editor button;
mute; fader; stereo meters with peak hold; clip light, click to clear), and
status bar (audio, xruns, MIDI drops, protocol, save target or the latest
notice, master meter). Faders move with the drag, not to the click, so a stray
click on stage changes nothing. Shift gives fine control, double-click resets
to 0 dB, and the wheel moves 0.5 dB. Fader and meters share one -60..+6 dB
scale.

Implemented in step 5:
- Input side: the source label opens a menu of sources (All, the virtual
  inputs, then hardware and software devices, each with its protocol). The key
  bar is dragged: an end moves that end, the middle moves the range, and a
  press outside it extends the nearer end. The filter chip opens the filter
  panel: channels and groups (from "all", a click picks just that one, and the
  last one stays on), key and velocity ranges, and transpose, as steppers
  (Shift for an octave or 10, and the wheel works too), plus Reset.
- Each row: the "out 1-2" text opens the output-pair menu when there are more
  than two outputs, and a red X at the row's right end removes the row, after
  a Remove / Cancel confirmation. (Until 2026-10-01 a "..." menu held
  Enabled, the output pair and Remove. Enabled is gone: Mute does its job.)
  Sequence rows end with the same red X. Shift-click on the X removes it at
  once, without asking (2026-10-19; the confirmation says so). There's no
  undo for it yet.
- A muted row is dimmed like a placeholder (since 2026-10-01), except its M
  button, which stays bright.
- "+ Add plugin" lists the scanned plugins (scanned once, on first use) and
  adds the choice at the bottom. A right-click below the rows, or a click on
  the + there, lists only the instruments (docs/look.md, 2026-10-19).
  `--save-rack PATH` alone starts a new,
  empty rack. With the window, a rack with nothing runnable no longer exits.
- Placeholder rows show no output controls.
- The plugin line under a row's name (its plugin and vendor, or "not loaded:
  ..." on a placeholder) ends with a menu arrow. Clicking it lists the
  instruments with the current one ticked, and choosing another replaces the
  row's plugin (`replacePlugin`, section 1; 2026-10-22).
- A click on a row's name types a new one in its place, as a sequence's does
  (Enter keeps it, Escape doesn't; 2026-10-22). The plugin line below still
  says what plays. Names are trimmed and at most 64 bytes, and an empty one
  gives the row its plugin's name again. The id doesn't change, so sequences,
  loopers and links still find the row (`Session::setSlotName`). A row whose
  name is still its plugin's counts as not renamed, so Replace names it after
  the new plugin.
- Menus and the panel are overlays inside the window (DGL has no native
  menus). A click outside or Escape closes them. Input goes to regions recorded
  during the last draw, topmost first.
- Edits patch the controller's model right away, so input that arrives before
  the next tick (several wheel notches) builds on them.

Constraints carried from the spike and the DGL notes in architecture.md:
- Construct and destroy NanoVG widgets inside a
  `Window::ScopedGraphicsContext`. Close the window and quit the Application
  before it's destroyed.
- No text entry in v1 (since then, the names of sequences, loopers and rows are typed in place). Values are edited by drag, scroll and
  double-click-to-reset. The source is a popup list.
- Stage-first drawing: large type, high contrast, and a UI scale factor
  (HiDPI and a "big" mode).
- **Row colours** (2026-10-03): each rack row has a colour by its place (eight
  hues, then again, so neighbours differ): a stripe at its left edge and a
  tint shading from it across the row. A sequence's or looper's row takes the
  colour of the rows it plays into (its stripe split between them when there
  are several; the tint from the first), and none when it plays into none.
- **The window's size** (2026-10-02) is kept as it closes, in
  `$XDG_STATE_HOME/stagehand/window.json` (default `~/.local/state/...`; on
  macOS in `~/Library/Application Support/StageHand`; this machine's, not a rack's or the rig's), and reopened at. The first time,
  it's 1100 wide and tall enough for the rack (`RackWidget::wantedHeight()`:
  its rows, the buses, the dock). Either way it's fitted to the monitor under
  the pointer (XRandR; the whole X screen without it), at most 92% of it, so
  a size kept on a big monitor shrinks on a small one. Its position isn't
  kept (the window manager places it).

## 7. Files: New, Open, Save, Save As (2026-09-30)

The rack window's **File** button opens a menu: New rack, Open..., Save and
Save As.... Keys: Ctrl-N, Ctrl-O, Ctrl-S (Ctrl-Shift-S also saves, as in
plugin editor windows) and Ctrl-Q.

- **Titled and untitled.** A rack has a path (its bundle) or is untitled: the
  no-argument start, a `--plugin` session, or after New. Save writes to the
  path; for an untitled rack it's Save As. The status bar says which.
- **Switching racks** (New, Open) saves a titled rack first, just as quitting
  does, then loads the new one while audio keeps running (Session::load's
  rules). Plugin and sequence editor windows close, and MIDI learn is
  cancelled. If the save fails, nothing changes.
- **Untitled changes are asked about.** New, Open, Ctrl-Q and closing the window
  ask "Keep the changes to Untitled?": Save As... (then carry on), Discard or
  Quit without saving, or Cancel. Ctrl-C in the terminal and Ctrl-Q in a
  plugin editor window don't ask.
- **Save As** adds `.rack` if it's missing, and the rack takes the file's name.
  An existing rack there is replaced only after "Replace the existing rack
  X?"; anything else there is never replaced. The same rules back
  `--save-rack` (Session/RackFile.h, `classifySaveTarget`).
- **Open** takes the `.rack` folder (or its rack.json). A file that isn't a
  rack, or doesn't load, leaves the running rack alone, with a notice.
- **`--no-save-rack`:** Save and Save As are disabled, and switching racks
  discards changes without asking (saving was turned off on purpose).
- **The chooser** is the desktop's own, run as a separate process
  (App/FileDialog: zenity, or kdialog on KDE), so the main loop never waits
  on it. DPF's built-in browser was the first plan, but it can't choose a
  folder or suggest a name when saving, and its X11 fallback can't save. It
  starts beside the current rack, else in `~/Racks`, else in your home
  folder. Without zenity or kdialog, the status bar says what to install.

- **Setlists (2026-10-01):** the File menu also has New setlist with this
  rack..., New setlist..., Open setlist... and, while one is open, Close
  setlist. See docs/setlists.md.

- **Help (2026-10-19):** a "?" button at the toolbar's right end, after
  File (with a setlist open, at the setlist row's right end, after its MIDI
  button: docs/look.md), and the File menu's last item open the help page, `help/help.html`,
  in the default browser (`open` on macOS, `xdg-open` on Linux), as
  KeyboardEDO's Help does. The build copies it, with the logo it shows as
  `Unlikelyware.png`, to `build/help` beside the executable (Linux) and into
  `StageHand.app/Contents/Resources/help` (macOS). It is exported from
  `resources/help/help.md` by the author in Markdown Web Writer. If the page
  is missing from the install, Help says so in the status bar. The Perform
  view has no Help: a stray click there shouldn't put a browser over the
  stage, and Edit (Tab) is one key away.

## Review decisions (2026-09-28)

1. Add and remove are in v1, with a plain list of scanned plugins.
2. Editors open on demand. `--open-editors` keeps the old behavior.
3. Mute is its own rack field, separate from `enabled`. Mute also sending
   note-offs is deferred (see `setMuted`). (Superseded 2026-10-01: mute
   releases notes, and replaced `enabled`.)
4. The ramp time is fixed at 10 ms.
