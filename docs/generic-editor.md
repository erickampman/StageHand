# StageHand — The generic editor

Proposed 2026-10-06; the first version built 2026-10-21 (see "Built"). A
window StageHand draws itself, listing a plugin's parameters as sliders,
switches and menus, **for a plugin with no editor of its own**. Builds on:
- `HostedPlugin::parameters()` and `paramValue()` (Plugin/HostedPlugin.h):
  each parameter's id, name, range, default, and whether it's stepped or
  automatable, from CLAP's params extension; `flushParams()` for changes
  while a plugin isn't processing.
- controls.md: control links already send `ParamValue` events into a
  plugin's block (`Engine::setLinkTarget()`), and the plugin's own parameter
  output and gestures are read back.
- sequences.md and `RackUi`: the sequence editor's window, a DGL window per
  item opened from the rack, the pattern this one follows.
- rack-format.md: a plugin's state holds its parameter values, so nothing
  new is saved.

## Decisions (proposed 2026-10-06)

1. **Only for a plugin with no editor of its own** (decided 2026-10-06).
   Unlike REAPER, which offers its generic panel for every plugin, StageHand
   shows it only where `hasEditor()` is false: no CLAP GUI extension, or none
   for this platform's embedded window (X11 on Linux, Cocoa on macOS). A
   plugin with an editor never shows the generic one, not even as a choice:
   one editor per plugin, the plugin's own when there is one.
2. **The row's Editor button opens it.** Today the button is greyed for a
   plugin without an editor; it becomes enabled and opens the generic
   window. A bus effect's Edit button the same. A plugin with no parameters
   to show (none, or all hidden) keeps a greyed button.
3. **Which parameters:** every parameter except the hidden ones
   (`CLAP_PARAM_IS_HIDDEN`). Read-only ones are shown but can't be moved
   (`CLAP_PARAM_IS_READONLY`); not only the automatable ones, which is what
   control links offer.
4. **DGL, both platforms,** like the sequence editor: no platform code.
5. **Its window is the editor window** for everything else that counts
   editors: `openAllEditors()`, `--editor`, the open-editor count, closing
   at a song switch.

## What it shows

- **A row per parameter:** its name; a slider for a continuous value; a
  switch for one stepped between two values; a menu for one with more steps
  (an enum: its value names, from the plugin's value text); the value as
  the plugin writes it ("440 Hz", "Saw"), which can be typed into.
  Double-click resets to the default.
- **Groups** by the parameter's `module` path (CLAP: `"Osc 1/Filter"`),
  collapsible; none if the plugin gives no paths.
- **A search field** over the names, for plugins with hundreds of
  parameters.
- **The plugin's own changes** (its parameter output, or a value it changed
  itself) move the controls, except one being dragged.

## What's needed

| Piece | Work | Size |
|---|---|---|
| Engine | a main-thread to audio-thread queue for the window's changes (the SPSC queue exists), gesture begin and end around a drag | ~100 lines |
| CLAP | value text (`value_to_text`) and typed values (`text_to_value`); the hidden, read-only and enum flags; `module`; a real `rescan()` (an empty stub now), so a plugin changing its parameters refreshes the list | ~150 |
| Controller | a `ParameterEditor` model, like `SequenceEditor`: the visible parameters, search, groups, cached value text, set and gesture, the rack marked modified; refreshed for the visible rows only, on the 33 ms tick | ~300 |
| Widget and window | the rows, scrolling, the search field, groups; opened and closed by `RackUi` | ~550-700 |
| Wiring | the Editor and Edit buttons enabled for a plugin without an editor (decision 2) | ~80 |
| Tests | the model with the controller tests' fake plugins: ranges, steps, text both ways, gestures, rescan | ~250 |

About 1,300 to 1,600 lines, two sessions. The cost is in plugins with many
parameters: asking a plugin for value text is a call into it, too slow to
make for hundreds of rows every frame, so the text is cached and asked again
only for rows on screen whose value changed.

## Milestones (proposed)

1. **Plain:** sliders and stepped menus with raw values (the parameter's
   plain range), names, double-click to default; the queue, gestures, the
   buttons and the window. No value text, search or groups. About 700
   lines; enough for a small plugin.
2. **Full:** value text and typed values, enum names in the menus, groups,
   search, read-only and hidden, `rescan()`.

## Built (2026-10-21)

Asked for so Retune (a note effect with no window) can be set from StageHand.
The author added: **it opens only when chosen** (an Editor or Edit button, or
a MIDI effect's E or Editor), never by itself, so `openAllEditors()` and
`--editors` open the plugins' own editors only. That narrows decision 5.

Milestone 1 and part of 2:
- `App/ParameterEditor`: the rows (every parameter but hidden ones), each a
  switch (stepped, two values), a menu (stepped, up to 24 values, each named
  by the plugin's value text) or a slider (snapping when stepped); values read
  back from the plugin each tick, except a row being dragged or set in the
  last three ticks; set() clamps and snaps; reset() to the default.
- `Ui/ParameterEditorWidget` and `RackUi::openParameterEditor()`: a DGL
  window per plugin, 560 wide. A row is the name, its control and the value
  as the plugin writes it (CLAP `value_to_text`), which can be clicked to type
  a number. Sliders move with the drag (Shift: finer); the wheel moves a step
  or a hundredth; double-click resets. Read-only rows are dimmed. It scrolls.
- `RackController::openAnyEditor()`: the plugin's own editor, else (through
  `Config::openGenericEditor`, set by the app once the rack window exists) the
  generic one, for rows, MIDI effects and bus effects. `editable()` drives each
  button. `setParameter()` sends a change through `Engine::setParameter()`
  (a main-to-audio queue, delivered as a ParamValue at the start of the next
  block to whichever plugin it is, control links' pick-up told) and marks the
  rack modified, or the rig for a bus effect.
- uw-core: `ParameterInfo::isHidden`, `isReadOnly`, `module`;
  `HostedPlugin::paramValueText()`.
- A bypassed MIDI effect now stays in the graph without MIDI, so it still
  takes its parameter changes (CLAP's flush isn't allowed while it's active).

Not yet: groups by `module`, search, typing text the plugin parses
(`text_to_value`), `rescan()`, gestures. Tests: `test-rackcontroller`
(`testGenericEditor`), `test-engine` (parameter changes in a row with MIDI
effects, a bypassed effect).

## Open questions

- A plugin whose editor exists but fails to open (`create()` or
  `set_parent()` failing): the generic editor in its place, or only the
  failure said? Decision 1 counts it as having an editor.
- A plugin whose GUI only floats (CLAP's floating windows, no embedded
  window for this platform): StageHand doesn't open floating editors today,
  so decision 1 gives it the generic one. Worth supporting floating editors
  instead, if one turns up?
