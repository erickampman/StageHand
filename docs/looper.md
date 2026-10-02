# StageHand — The looper (design)

Proposed 2026-10-02; nothing built yet. A looper records what's played into a
loop, live, and plays it back in time, with overdubs. It's a cousin of a
sequence (docs/sequences.md) but it isn't one: it's made on stage, isn't saved,
and is driven by a footswitch. Builds on:
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
11. **One footswitch** (below, "Controls").
12. **As many loopers as you like.** One is the current looper: the one the
    footswitch drives, shown in its row.
13. **What's played still sounds** as usual while it's recorded.
14. **At a song switch** loopers are kept, stopped.
15. **Not saved** in the rack or the setlist. Converting to a sequence is one
    click; nothing warns at a switch or at quit (at most a flash).
16. **Convert to sequence** (a button or menu item in its row) makes a normal
    sequence in the current rack, unsaved until Ctrl-S, and the normal rules
    apply from then on.
17. **UI:** one row per looper in the Sources dock, under the sequences.
18. **A size cap,** preallocated, and shown.

## Controls

One footswitch (a learned note or controller, as for a trigger) drives the
current looper:

| State | Press | Double press | Hold |
|---|---|---|---|
| Empty | arm: record from the next beat/bar | - | - |
| Armed (waiting) | cancel | - | - |
| Recording, length by press | end (rounded up), play | stop | clear |
| Recording, fixed N | end now (rounded up), play | stop | clear |
| Playing | overdub | stop | undo the last pass |
| Overdubbing | play (the pass is kept) | stop | undo this pass |
| Stopped | play, from the loop's start, at the next beat/bar | - | clear |

With a fixed length, recording runs into overdub when it reaches N; the next
press goes to play, and presses then alternate play and overdub.

Proposed for the edge cases (to confirm):
- **Every press acts at once.** Waiting to tell a press from a double press
  (about 300 ms) would put the end of a recording out of time. So a double
  press is a press, then a second one within the window that undoes the first
  and stops: from playing, press starts an overdub, and the second press
  discards that overdub and stops.
- **A hold starts as a press,** and after about a second becomes the hold's
  action (clear, or undo), which supersedes the press.
- **Hold and double press need a momentary switch** (press and release
  messages). A latching pedal only gives presses.
- **The current looper** is chosen by clicking its row, or by an optional
  learned "next looper" trigger. A looper can also have its own triggers,
  which drive it whether it's current or not.
- **The transport stopping** stops every looper; starting it again leaves them
  stopped until pressed.
- **After a song switch,** a stopped looper keeps its targets by row id: they
  work again if the new rack has rows with those ids, and otherwise it plays
  nowhere until its targets are changed. Converting it then makes a sequence
  in the current rack.

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

## Milestones (proposed)

1. **Engine:** record into a loop (start at beat or bar, end by press or N,
   quantize, cut at the end), play it, overdub and undo, the cap. Tests with
   the fake plugin: in time, rounding, quantize, overdub and undo, the cap.
2. **Controller and UI:** making a looper (input, targets, quantize, start at
   beat or bar, length), its row (state, length, progress, fill), the current
   looper, the footswitch learned with its press / double / hold, and
   transport and song-switch behaviour.
3. **Convert to sequence.**
4. **Later:** controllers and pitch bend in loops (with sequences).

## Open questions

- The press / double / hold proposals above.
- Do loopers stop with the transport, or keep their place for a restart?
- One footswitch for all loopers (the current one), per-looper triggers, or both
  (proposed: both)?
