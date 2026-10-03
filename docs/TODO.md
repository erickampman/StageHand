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
     pedal), learned per rack.
  4. Velocity curves per row, or a fixed velocity.
  5. MIDI out: program changes to hardware at a song switch, MIDI clock out,
     the click as MIDI notes.
- **Right-click in the empty space below the rack rows** opens the same menu
  as "+ Add plugin" (added 2026-10-03), so a row can be added where rows go.
- **Later:** backing tracks (an audio file per song, in time with the
  transport); song notes in the Perform view; undo for rack edits.
