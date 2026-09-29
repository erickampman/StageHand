# StageHand — Sequences (design)

Step sequences that play into rack rows, locked to the host's tempo. Builds on
decisions already recorded:
- domain-model.md, "Sequences (data model, draft)": protocol per sequence;
  MIDI 1.0 stores note numbers; MIDI 2.0 stores EDO steps and sends pitch 7.9;
  nearest-free note numbers.
- architecture.md, "Sequencer UI": sequences live in a sources dock below the
  rack, and the editor is its own window.
- rack-format.md, "Reserved: sequences": a top-level `sequences` array, and
  `"source": "sequence:<id>"` in a slot.

Reviewed 2026-09-29: every "proposed" point below was accepted, plus three
additions (note-off offset, Repeat, and re-enabling the protocol toggle when a
sequence is emptied).

## 1. Transport: the host tempo (prerequisite)

Today `TimeInfo` carries a fixed 120 BPM and `playing = false`, and nothing
advances a beat position. Sequences need a shared beat grid, and so do
tempo-synced plugins (LFOs, delays, arpeggiators).

- **The rack owns a tempo and time signature**, saved in `rack.json`
  (`"tempo": 120.0, "timeSignature": [4, 4]`).
- **Proposed: the transport free-runs.** The beat position starts at 0 when
  StageHand starts and always advances. There's no global Play button.
  Sequences start and stop individually, quantized to the grid (section 4).
  That's the MainStage/Ableton-launch model: nothing about the rack's sound
  waits for a transport. (The alternative is a global Play/Stop that also
  starts every sequence.)
- **The engine keeps the beat clock.** The main thread writes tempo and time
  signature atomically; the audio thread reads them once per block and advances
  `beatPosition += frames * tempo / 60 / sampleRate`. A tempo change takes
  effect at a block boundary (at most one quantum late, a few ms).
- **Plugins get it all.** CLAP's transport event gains `HAS_BEATS_TIMELINE`
  (`song_pos_beats`, `bar_start`, `bar_number`) and `IS_PLAYING`, alongside
  tempo and time signature. That's what makes a plugin's synced LFO or delay
  lock to the rack.
- **UI: a toolbar tempo control.** Drag or wheel the value (Shift for fine,
  0.1 BPM). DGL has no text entry, so there's no typing a value; double-click
  resets to 120. Plus a **Tap** button (average of the last 4 taps) and a beat
  light. Time signature is a small menu.
- **Later:** follow incoming MIDI clock, send MIDI clock out.

Implemented 2026-09-29 (milestone 1): `Engine::setTempo()` /
`setTimeSignature()` / `transport()`, per-chunk `TimeInfo` with beat position
and bar, CLAP's beats timeline, `tempo` and `timeSignature` in `rack.json`,
Session and controller edits (tap tempo averages the last four intervals, and
a 2 s gap starts over), and the toolbar control with beat lights. A new time
signature starts at the current bar's start, so bars stay contiguous.

## 2. The sequence model

```
Sequence
  id, name
  protocol         MIDI 1.0 | MIDI 2.0               (fixed at creation; decided)
  tuning           MIDI 2.0 only: edo (e.g. 31), referenceNote (MIDI note, default 60)
  channel, group   what it sends on (default 1, 1)
  rate             units per beat, a ratio: 1, 2, 3, 4, 8, 16 or 1/2, 1/3, 1/4, ...
  length           in units (the loop length)
  launch           quantize: next bar (default) | next beat | immediately
  repeat           on (default): loop | off: play once from the start, then stop
  noteOffOffsetMs  shifts every note-off from its note's end, in ms (default 0)
  notes[]          start (unit), length (units, >= 1), pitch, velocity
                     pitch: MIDI 1.0 = note number; MIDI 2.0 = EDO step
                     velocity: 7-bit (1.0) or 16-bit (2.0)
```

- **Proposed: whole units.** Notes start on a unit and last a whole number of
  units, exactly your rectangles: a 2-unit note is twice as long. Finer timing
  comes from a faster rate, not from sub-unit offsets. It keeps the grid, the
  drag rules and the file format simple. Swing and per-note offsets can come
  later as additions.
- **Rate is units per beat,** as a ratio. At 1/2, a unit lasts two beats; 3
  and 1/3 give triplets. A beat is the tempo's beat, and BPM counts quarter
  notes (the CLAP and DAW convention): at rate 4 a unit is always a sixteenth
  note, and a 6/8 bar is 3 beats long.
- **Note-off offset (per sequence).** Each note-off is sent `noteOffOffsetMs`
  after its note's end: positive for legato overlap, negative for staccato. In
  milliseconds, so it doesn't change with tempo or rate. A note always lasts at
  least 1 ms. If a delayed note-off would land after the next note-on on the
  same pitch, it's sent just before that note-on instead, so the two never
  overlap.
- **Repeat.** On, the sequence loops until stopped. Off, it plays once from
  its start (quantized like any launch) and stops itself at its length, with
  the note-offs still sent on time.
- **EDO pitch.** Step `s` sounds at `referenceNote + s * 12 / edo` semitones:
  that's the pitch 7.9 value sent, and the note number is the nearest free one
  (decided). Steps are relative to the reference and can be negative. A MIDI
  1.0 sequence is simply the 12-row-per-octave case, with note numbers.
- **Proposed: independent lengths.** Each sequence has its own length and rate.
  They stay aligned because every sequence is phase-locked to the same beat
  grid, and launch is quantized to it. Different lengths give polymeter
  (16 against 12) on purpose. Forcing one length would rule that out, and buy
  nothing a quantized launch doesn't already give.
- **In the rack.** `"sequences": [...]` holds them (reserved already), and a
  slot plays one with `"source": "sequence:<id>"`. Like a device, one sequence
  can feed several rows, and a row's filter (channel, key range, transpose)
  applies to it as to any source.

## 3. Playback (audio thread)

- **A sequence is a MIDI source.** It gets a MidiSourceHandle like a device, so
  routing, filters, per-plugin protocol translation, and the held-note table
  all work unchanged. Its events are generated per block rather than captured.
- **Sample-accurate by construction.** For the block's beat range, a playing
  sequence finds the note starts and ends that fall in it (wrapping at the loop
  end) and turns each into a sample offset from the beat clock. The block's
  generated events are merged with incoming MIDI in offset order before
  routing.
- **Content is published like graphs.** Each edit produces an immutable snapshot
  of the notes, committed with the same commit/adopt/retire rule. So an edit
  while playing is safe and allocation-free on the audio thread, and it doesn't
  rebuild the whole EngineGraph.
- **Note-offs are guaranteed.** A sounding note ends at its end, or at once
  when the sequence stops, when that note is deleted or shortened past the
  play position, or when the row is removed (the held-note table already covers
  the last).
- **Proposed: rate changes keep the loop phase.** Switching rate while playing
  keeps the same fraction of the way through the loop, so it stays on the grid.
- **Tempo changes** need nothing special: position is derived from the beat
  clock.

Implemented 2026-09-29 (milestone 2): `Core/Sequence.h` (the model),
`Engine/SequenceSet.h` (compiled snapshots, `SequenceRuntime`, per-block
generation), `Engine::commitSequences()` / `sendSequenceCommand()`, Session
sequence edits, and the rack-file format (rack-format.md, "Sequences").
Details settled in the code:
- Sequence sources have handles from `kSequenceSourceBase` up, never reused,
  and a slot listening to "all inputs" doesn't hear them.
- Changing a sequence's channel, group or protocol while it plays releases its
  sounding notes on the old ones first, so nothing sticks.
- At most 64 notes sound at once per sequence, and 1024 sequence events are
  generated per block. Beyond those, notes are skipped. Event overflow is
  counted in the engine's stats (`sequenceEventsFull`); the 64-note cap isn't.
- tests/SequenceTests.cpp checks exact sample positions for all of the above.

## 4. Starting and stopping (was TBD)

**Proposed:**
- **Each sequence has Play/Stop** in its dock row, quantized by its launch
  setting (default: the next bar). A pending start blinks until it begins.
- **MIDI-mappable.** A note or CC from a chosen source (a controller pad, a
  footswitch) starts, stops, or toggles it: "learn" by pressing the control
  after clicking the mapping button. Mappings are saved in the rack.
- **Stop all** in the dock, which sends note-offs everywhere a sequence was
  playing.
- **Later:** keyboard-follow modes (the sequence plays only while keys are
  held, or is transposed by the lowest held key), and launching a sequence with
  a rack switch.

Implemented 2026-09-29 (milestone 3), the sources dock at the bottom of the
main window:
- **Header:** "+ Sequence" (MIDI 2.0 with EDO pitches, or MIDI 1.0) and Stop all.
- **Inputs row:** a chip per open MIDI input, lit by its messages. The engine
  now counts messages per input (`Engine::sourceActivity()`).
- **Sequence rows:**
  - Play/Stop: a triangle or a square, blinking amber while a quantized start
    or stop is pending.
  - Name, protocol and EDO, note count, and the rows that play it (amber when
    none do).
  - A mini-map with beat lines, note starts and the playhead.
  - A rate menu (1/4 to 16) and a length stepper. The stepper won't go below
    the last note's end and says why; notes are deleted in the editor, never
    by the length control.
  - A launch menu, a Repeat toggle, and a "•••" menu with Remove (confirmed).
- **Rows' source menus** list the sequences after the devices.
- **Size:** the dock grows with its sequences up to a little over half the
  window, then scrolls with the wheel.
- **Not yet:** mapping Play/Stop to MIDI (milestone 5), and setting the note-off
  offset or renaming (the editor, milestone 4).

Implemented 2026-09-29 (milestone 5), MIDI-mapped launch:
- **Triggers:** each sequence has a list of triggers, saved in the rack
  (rack-format.md, "Triggers"). A trigger is an input, a channel and group,
  and a note or a controller number, with an action: Play/Stop (toggle),
  Start, or Stop. Start and Stop are quantized by the sequence's launch
  setting, just like the dock's button.
  - A note acts on its note-on. A controller acts when its value rises to 64
    or more (a MIDI 2.0 controller: the top half of its range), once per
    press, so a pedal or a pad that sends 127 then 0 acts once.
  - Messages that match a trigger go nowhere else: no row hears the pad,
    note-offs included.
  - One control can drive several sequences (a pad that starts the drums and
    the bass together).
- **On the audio thread:** the engine now takes a block's input MIDI before
  the sequences generate, so a trigger acts at its own frame. A trigger just
  before a bar line catches that bar, and "immediately" means at the pad's
  sample.
- **Learn:** the dock row's **MIDI** button opens a menu: Learn Play/Stop,
  Learn Start, Learn Stop, and Remove for each trigger. While learning, the
  button blinks amber, and the next note-on or controller press from any input
  is captured (and not played). Escape or "Stop learning" cancels. A learned
  trigger replaces the sequence's trigger for that action and any other use
  of the same control on that sequence.
- **Inputs:** a trigger remembers the input it was learned from. If that
  device isn't connected when the rack loads, the trigger is kept, does
  nothing, is shown as "(not connected)", and the load warns.
- tests/SequenceTests.cpp checks trigger timing (including the bar-line case),
  controller edges, consuming, and learn; the Session, rack-file and
  controller tests cover the rest.

## 5. The editor window

A second DGL window (the Application supports several), opened from the dock
row, one per sequence.

- **Left strip:** the MIDI 1.0 / 2.0 toggle, EDO and reference (2.0), rate,
  length, launch quantize, Repeat, note-off offset, channel and group, and step
  labels for the visible rows. The protocol toggle works only while the
  sequence has no notes (changing it with notes would be a conversion), so
  it's enabled again when the sequence is cleared.
- **Canvas:** one row per EDO step (or per note number), one column per unit.
  - **Black-key bars:** a row is drawn dark when its pitch rounds to a 12-TET
    black key (C#, D#, F#, G#, A#). In 31-EDO, steps 2-3, 7-9, 15-16, 20-21 and
    25-27 of each octave are dark, so the familiar 2+3 pattern appears as
    bands.
  - Bar and beat lines vertically, from the rate and time signature.
  - A playhead while it plays.
  - Vertical scroll over the pitch range, horizontal zoom.
- **Notes** are rectangles, their brightness showing velocity.

Implemented 2026-09-29 (milestone 4):
- **Where it lives:** `src/App/SequenceEditor` is the editing logic, tested
  headless: working notes, selection, gestures, undo and the trimming rule.
  `src/Ui/SequenceEditorWidget` is the window.
- **Opening:** one window per sequence, from the dock (a click on the
  sequence's name, or "Edit notes..."). Opening an open one brings it forward.
- **Closing:** closing an editor closes just that window. Closing the rack
  window quits even while editors are open (DGL alone would wait for every
  window).
- **Notes and settings:** notes belong to the editor. Every other setting is
  read fresh from the Session, so the dock and the editor never overwrite
  each other. Edits are heard as you make them.
- **Gestures:** each drag is one gesture, recomputed from its start on every
  move, and one undo step. The right-edge zone is 6 px. Ctrl-drag changes
  velocity over 200 px for the full range.
- **Keys:**
  - Arrows move the selection; Shift moves by a beat, or by an octave (one EDO
    period) vertically.
  - Space plays or stops the sequence.
  - The wheel scrolls, Shift-wheel scrolls sideways, and Ctrl-wheel zooms. The
    zoom fits the loop to the window until you zoom yourself.
- **The strip also sets:** "Step 0 at" (the reference note), and the length
  of new notes.

### Drag and click rules (accepted)

| Gesture | Where | Result |
|---|---|---|
| Click | empty cell | New note, 1 unit (or the last length used), default velocity |
| Click and drag right | empty cell | New note, length follows the drag (whole units) |
| Drag | note body | Move it: x in units, y in steps. Shift locks to one axis |
| Drag | note's right edge (last 6 px) | Change its length (at least 1 unit) |
| Alt-drag | note body | Copy it |
| Ctrl-drag vertically | note | Velocity |
| Right-click or double-click | note | Delete |
| Shift-drag | empty area | Select a rectangle of notes |
| Click | note | Select it (Shift-click adds) |
| Delete / arrows | with a selection | Delete / move by a unit or a step |
| Ctrl-Z, Ctrl-Shift-Z | | Undo, redo |

- **Same-row overlaps:** a note dropped or stretched onto another in the same
  row trims the earlier one to end where the new one starts (the usual piano
  roll rule). Two notes on one pitch never overlap, which for MIDI 1.0 would be
  a retrigger.
- **Loop end:** a note can't extend past the sequence's length. Lengthening the
  sequence reveals space; shortening it asks before deleting notes past the new
  end.
- **Undo** is per sequence, in the editor (snapshots, cheap at this size).

## 6. Things easily forgotten

- **Output:** a sequence sends on one channel and group; per-note channel is
  out of scope for v1.
- **Default velocity** and a velocity lane (a strip of bars under the grid) are
  worth having early; the Ctrl-drag above is the minimum.
- **Recording** (step input or live from a keyboard) is later.
- **Copy/paste** between sequences is later; within one, Alt-drag covers it.
- **KeyEDO:** reuse its tuning model when it's ported, so a sequence and a live
  KeyEDO keyboard agree on what "step 7 in 31-EDO" means. Worth checking
  before the tuning fields are fixed in the file format.
- **Panic:** Stop all, plus a Panic that sends all-notes-off to every row.

## Proposed milestones

1. **Transport:** rack tempo and time signature, the engine's beat clock, CLAP
   transport with the beats timeline, toolbar tempo control with Tap. Tests.
2. **Sequence model and playback, headless:** model, rack-file support, the
   engine's per-block generation and merge, launch quantize, note-off
   guarantees. Tests with the fake plugin, which checks exact sample offsets.
3. **Sources dock:** sequence rows with Play/Stop, rate, length; adding
   sequences; choosing one as a row's source.
4. **Editor window:** the grid, black-key bars, drag rules, undo.
5. **MIDI-mapped launch** (learn).

## Decisions (2026-09-29)

All accepted as proposed: free-running transport with per-sequence quantized
launch; whole-unit notes; independent lengths; launch by dock button and MIDI
mapping, next bar by default; the drag and click rules with same-row trimming;
rate as units per beat. Added: note-off offset in ms (default 0), Repeat
(default on), and the protocol toggle re-enabled when the sequence is empty.
