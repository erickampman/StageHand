# StageHand — TODO

- **The looper, later:** controllers (the sustain pedal first) and pitch bend
  in loops, with the sequence model extended (docs/looper.md, milestone 4).

- **Planned features (2026-10-03), designed and built one at a time, in this
  order:**
  1. Count-in per song, and the Perform view with a large beat display
     (docs/perform.md).
  2. Panic (all notes off and pedals up on every row, a button and a learned
     trigger), and CPU load and xruns shown in the window (docs/panic.md).
  3. MIDI controllers to plugin parameters (knobs, faders, an expression
     pedal), learned per rack or for all songs (docs/controls.md).
  4. Velocity curves per row, or a fixed velocity.
  5. MIDI out: program changes to hardware at a song switch, MIDI clock out,
     the click as MIDI notes.
- **Right-click in the empty space below the rack rows** opens the same menu
  as "+ Add plugin" (added 2026-10-03), so a row can be added where rows go.
- **The generic editor** (planned 2026-10-06, docs/generic-editor.md): a
  window of sliders and menus for a plugin with no editor of its own, never
  in place of one that has an editor.
- **The house look and feel** (asked for 2026-10-18): everything should look
  like the plugins and KeyboardEDO where it can. That's PlainOldSynth's glowing knobs,
  group boxes, captioned dropdowns and value boxes, a linear slider in the
  same style, and DejaVu Sans, in StageHand's own colours (KeyboardEDO's
  `docs/decisions.md`, "Look and feel"). StageHand draws with DGL and NanoVG,
  as the plugins do, so PlainOldSynth's `src/UI/Knob.hpp`, `GroupBox.hpp` and
  `Dropdown.hpp` can be used nearly as they are; the slider needs porting to
  NanoVG from KeyboardEDO's `src/app/Widgets.cpp` (ImGui). The rack view
  first; the generic editor should be built in it from the start. Plugin
  editors keep their own look.
- **Later:** backing tracks (an audio file per song, in time with the
  transport); song notes in the Perform view; undo for rack edits.
