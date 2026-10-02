# StageHand — The looper (design)

Proposed 2026-10-02; nothing built yet. A looper records what's played into a
loop, live, and plays it back in time, with overdubs. It's a cousin of a
sequence (docs/sequences.md) but it isn't one: what it records is made on
stage and isn't saved, and it's driven by a footswitch. Builds on:
- sequences.md: a sequence is a MIDI source the rack owns that plays into its
  target rows, locked to the beat clock. A loop plays back the same way.
- sequences.md, section 4 and setlists.md, section 4: MIDI learn and triggers.
- setlists.md, section 3: what a song switch does.

## Decisions (2026-10-02)

1. **MIDI, not audio.**
2. **From where:** chosen when the looper is made: an input (and its
   channels), as a trigger's input is chosen.
3. **Into which rows:** chosen when it's made, like a sequence's targets.
4. **Notes only at first.** Controllers (the sustain pedal first), pitch bend
   and MIDI 2.0 per-note data later; they need the sequence model extended.
5. **Quantize:** to the beat times a division or multiple (1/4, 1/2, 1, 2...
   beats), set when it's made. Applied as notes are recorded.
6. **It always stays in time.** Recording starts at the next beat or the next
   bar after the start press (a setting); a length ended by a press is rounded
   up to the next beat or bar the same way.
7. **A note held across the loop's end is cut there.**
8. **Transport stopped:** the start press starts it, unless the transport has
   a start of its own (a learned trigger, or start on first note); then it
   waits for that.
9. **Length:** in beats or bars (the user's choice): fixed (N), or ended by a
   press.
10. **Overdub:** more passes layered on the loop, and undo of the last pass.
11. **One footswitch** (below, "Controls"): tap to record, play, stop; hold
    to overdub.
12. **As many loopers as you like.** One is the current looper: the one the
    footswitch drives, shown in its row.
13. **What's played still sounds** as usual while it's recorded.
14. **At a song switch** (revised 2026-10-02, with 15): the old song's
    loopers stop, and their recordings stay in memory with that rack until
    quit, one copy per rack path, as its unsaved edits do (setlists.md,
    section 6): back on that song, the loop is still there. The new song
    brings its own loopers.
15. **A looper is saved in its rack; what it records isn't** (revised
    2026-10-02). Its existence and its settings (input, targets, quantize,
    start at beat or bar, length) are saved in rack.json, since a performer
    knows in advance which songs loop; its notes never are. Converting to a
    sequence is one click; nothing warns at a switch or at quit (at most a
    flash).
16. **Convert to sequence** (a button or menu item in its row) makes a normal
    sequence in the current rack, unsaved until Ctrl-S, and the normal rules
    apply from then on.
17. **UI:** one row per looper in the Sources dock, under the sequences.
18. **A size cap,** preallocated, and shown.

## Controls (revised 2026-10-02)

As simple as possible, for real time: **one footswitch** (a learned note or
controller) drives **the current looper** (chosen by clicking its row, and
shown there; a new looper is current). No per-looper triggers, and no double
presses.

| State | Press (a tap) | Hold |
|---|---|---|
| Empty | arm: recording starts at the next beat or bar | - |
| Armed (waiting) | cancel | - |
| Recording | end (the length rounded up to the beat or bar); play from the start | - |
| Playing | stop (on release) | overdub while held; on release it plays on, the pass kept |
| Stopped | play from the start, at the next beat or bar | - |

- **A tap and a hold while playing:** as the switch goes down, an overdub
  starts recording, provisionally. Released within about 0.4 s, it was a tap:
  the overdub is dropped and the loop stops. Held longer, it's an overdub that
  began exactly at the press, so nothing is late. Only stop acts on release.
- **Starting is never immediate:** arming and playing from stopped wait for the
  next beat or bar. Ending a recording acts at the press (rounded up).
- **Clear and undo** are buttons in the looper's row (and optional learned
  triggers, for a second switch). After stopping, a new loop needs a clear, or
  another looper.
- **Hold needs a momentary switch** (press and release). A latching pedal
  gives taps only: no overdub.
- **The transport stopping** stops every looper and returns it to its start.
- **Its targets are its own rack's rows,** so a song switch never leaves a
  looper pointing at rows that aren't there. A row removed leaves its
  targets, as for a sequence.

Also decided (2026-10-02):
- **A fixed length (N)** goes to play at N; overdubs are always a hold.
- **Undo** goes back pass by pass, to the first recording (a stack).
- **An overdub crossing the loop's end** carries on around the loop; a key
  still held at release is cut there.
- **Stop is immediate,** ending the loop's notes, not quantized.
- **Deleting a looper:** the red X at the right of its row, without asking
  (what it recorded isn't saved).

## Engine

- A loop is a MIDI source with its own handle, in the sequences' range, so it
  plays into its targets through their filters like a sequence, and the held
  note table ends its notes.
- Recording happens on the audio thread: the recorded input's note-ons and
  note-offs, at their beat positions, quantized, into a buffer preallocated
  when the looper is made. A pass is a range of that buffer, so undo drops the
  last range.
- **The cap:** 4096 notes per looper (about 128 KB). Notes past it are dropped
  and counted; the row shows how full the loop is once it's past 75%.
- The main thread reads a looper's state, length and fill through atomics for
  the row, and copies the notes out to convert it.
- The footswitch is matched on the audio thread, like the setlist's triggers,
  so a press acts within the block; its release ends a hold.

## In the rack file

An additive `"loopers"` list in rack.json (rack-format.md), its settings only:

```json
"loopers": [
  { "id": "loop", "name": "Loop 1", "source": "device:<id>", "channels": [1],
    "targets": ["keys"], "quantize": "1/2", "start": "bar",
    "length": { "bars": 4 } }
]
```

`"length"`: `{ "bars": N }` or `{ "beats": N }` for a fixed length; absent:
ended by a press (rounded up to the bar, or the beat, as `"start"`).

## Milestones (proposed)

1. **Engine:** record into a loop (start at beat or bar, end by press or N,
   quantize, cut at the end), play it, overdub and undo, the cap. Tests with
   the fake plugin: in time, rounding, quantize, overdub and undo, the cap.
2. **Controller and UI:** making a looper (input, targets, quantize, start at
   beat or bar, length) and saving it in the rack (its settings, not its
   notes), its recordings kept in memory per rack across song switches, its row (state, length, progress, fill), the current
   looper, the footswitch learned with its tap and hold, clear and undo, and
   transport and song-switch behaviour.
3. **Convert to sequence.**
4. **Later:** controllers and pitch bend in loops (with sequences).

## Open questions

None at the moment (answered 2026-10-02: one button, tap to record, play,
stop, hold to overdub; loopers saved in the rack without their notes).
