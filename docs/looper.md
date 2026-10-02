# StageHand — The looper (design)

Proposed 2026-10-02; milestones 1 to 3 built. A looper records what's played into a
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
   beats), or off (the played timing kept, for loops with feel), set when
   it's made. Applied as the loop plays (revised 2026-10-02): the notes keep
   their played timing, so changing it re-quantizes the loop from its next
   pass, and off brings the played timing back. Notes played just before the
   start (7a) are on the start, whatever the setting. A change mid-pass acts at
   once, every note still exactly once a pass (added 2026-10-02): one already
   played this pass isn't played again where quantize moved it, and one moved
   just behind the playhead (a grid step at most) plays at once.
6. **It always stays in time.** Recording starts at the next beat or the next
   bar after the start press (a setting); a length ended by a press is rounded
   up to the next beat or bar the same way.
7. **A note held across the loop's end is cut there.**
7a. **Notes played just early count** (added 2026-10-02). Players hit the
    downbeat slightly ahead, so a note-on up to a sixteenth (a quarter of a
    beat) before recording starts is recorded on the start, not lost.
7b. **No automatic stop** (2026-10-02): a loop plays until it's stopped
    (no "stop after N bars or repeats").
7c. **Fat fingers cleaned up** (added 2026-10-02). A finger that catches the
    key beside the one it meant leaves a short wrong note with the right one:
    two notes starting within a tenth of a beat, a semitone or two apart, in
    the same pass, the shorter at most half as long as the other. The shorter
    isn't played, nor kept as a sequence. It's still recorded (the setting
    can be turned off, and it plays again): Clean, in the looper's row, on
    by default, saved with the rack. A chord of equal notes, a note alone and
    an overdub's notes against the loop's are never touched.
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
    loopers stop and what they recorded is discarded; the new song brings its
    own loopers, empty. Back on a song, its loopers are empty again: each
    performance starts fresh. (Convert a loop to a sequence to keep it.)
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
    Loopers have names (added 2026-10-02): a click on its name renames it, in
    place, as a sequence's; a new looper's name field opens as it's made
    ("Loop N" until changed, N a running tab in the rack: one past the highest
    "Loop N" used, so a deleted looper's number isn't used again). The name is saved with the rack, and Keep gives
    it to the sequence.
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
| Playing | stop (on release), at the next beat or bar | overdub while held; on release it plays on, the pass kept |
| Stopped | play from the start, at the next beat or bar | - |

- **A tap and a hold while playing:** as the switch goes down, an overdub
  starts recording, provisionally. Released within about 0.4 s, it was a tap:
  the overdub is dropped and the loop stops. Held longer, it's an overdub that
  began exactly at the press, so nothing is late. Only stop acts on release.
- **Starting and stopping keep time:** arming, playing from stopped and
  stopping wait for the next beat or bar (the looper's setting, the same for
  all three), so a stopped loop doesn't cut off mid-phrase. Ending a recording
  acts at the press (rounded up).
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
- **Stop is at the next beat or bar,** as start is (revised 2026-10-02; was
  immediate). The transport stopping still stops loopers at once.
- **Deleting a looper:** the red X at the right of its row, without asking
  (what it recorded isn't saved).

## Engine

- A loop is a MIDI source with its own handle, in the sequences' range, so it
  plays into its targets through their filters like a sequence, and the held
  note table ends its notes.
- Recording happens on the audio thread: the recorded input's note-ons and
  note-offs, at their beat positions, quantized (or not), into a buffer
  preallocated
  when the looper is made. A pass is a range of that buffer, so undo drops the
  last range. While armed, the input's note-ons in the last sixteenth before
  the start are held back, and recorded on the start if recording begins.
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

`"quantize"`: a number of beats (`"1/4"`, `"1/2"`, `"1"`, `"2"`...) or `"off"`.
`"start"`: `"beat"` or `"bar"`, for starting, ending and stopping alike.
`"length"`: `{ "bars": N }` or `{ "beats": N }` for a fixed length; absent:
ended by a press (rounded up to the bar, or the beat, as `"start"`).

## Milestones (proposed)

1. **Engine:** record into a loop (start at beat or bar, early notes counted,
   end by press or N, quantize or off, cut at the end), play it, stop at beat
   or bar, overdub and undo, the cap. Tests with the fake plugin: in time,
   early notes, rounding, quantize, overdub and undo, the cap.
2. **Controller and UI:** making a looper (input, targets, quantize, start at
   beat or bar, length) and saving it in the rack (the container: its
   settings, never its notes), its notes discarded at a song switch, its row (state, length, progress, fill), the current
   looper, the footswitch learned with its tap and hold, clear and undo, and
   transport and song-switch behaviour.
3. **Convert to sequence.**
4. **Later:** controllers and pitch bend in loops (with sequences).

**Milestone 1 implemented (2026-10-02):** `Engine/Looper.h` (`LooperRuntime`:
the notes, preallocated to the cap, the pass stack and what the row will read;
`CompiledLooper`: its settings) and `Engine/Looper.cpp`, as `SequenceSet`
members: loopers ride in the sequence snapshot (`SequenceSet`'s third
argument) and play into their targets as a sequence does, through the held
note table. The engine records each block's live input at its own beat before
the loops play (`Engine::recordLoops()`), and takes the footswitch and the
buttons through `Engine::sendLooperCommand()` (Press, Release, Stop, Clear,
Undo). A press arming a looper while the transport is stopped starts it,
unless the transport has a start of its own. A note-off ends a loop's note
after the length it was played, from its quantized start. Tests:
`tests/LooperTests.cpp`. Not yet: making loopers, saving them and the
footswitch's MIDI (milestone 2).

**Milestone 2 implemented (2026-10-02):** `LooperSetup` in the rack (rack.json's
`"loopers"`, docs/rack-format.md) and the Session's looper entries, MIDI
sources the rack owns like sequences, built empty on every load (a song switch
included) and published with the sequences. The looper switch is in the rig
(rig.json's `"looperSwitch"`; decided while building it, since it's the
performer's footswitch, the same in every song), learned like a trigger and
matched on the audio thread after the setlist's triggers; its press and
release go to the current looper (`Engine::setCurrentLooper()`). The
controller's intents make and edit loopers (input, targets, quantize, beat or
bar, length), choose the current one (a new one is current; a rack's first,
when there's none), and tap, clear and undo. UI: + Looper (an input, then a
row), one row per looper under the sequences (its button is the switch's tap;
state, length, overdubs, input and targets, its place in the loop, how full it
is past 75%; Undo, Clear, Input, Q, Bar/Beat, Length, the red X), the current
looper highlighted, and the Looper switch button in the dock's header (moved
2026-10-02 to a MIDI button in each looper's row: the one switch, driving the
current looper). Tests:
`testLoopers` (controller), loopers in the rack file tests, the switch in the
rig file tests.

**Milestone 3 implemented (2026-10-02):** Keep, in a looper's row: the loop as
a normal sequence in the rack (`RackController::keepLooperAsSequence()`),
named and aimed as the looper, stopped, unsaved until Ctrl-S; the looper plays
on. The audio thread copies the loop (`LooperCommand::Snapshot` into
`LooperRuntime::snapshot`, preallocated), and `loopToSequence()` makes the
sequence: on the grid of its quantize (64ths when off, coarser past 4096
units), in the protocol, channel and group of its first note (one channel per
sequence: notes on others join it), MIDI 2.0 as 12-EDO from middle C, launched
at the bar or beat as the looper starts, repeating. Tests:
`testLoopToSequence` (looper), `testKeepLoop` (controller).

**Exactly once and fat fingers (2026-10-02):** each note remembers the cycle
it last played in (`LooperRuntime::Note::playedCycle`), and a quantize change
plays at the block's start the notes it pulled just behind the playhead
(`LooperRuntime::lastQuantize`). A note is marked a fat finger's slip
(`Note::fatFinger`) as it, or its neighbour, ends; `CompiledLooper::cleanup`
(`LooperSetup::cleanup`, rack.json's `"cleanup"`) leaves the marked out, and
Keep leaves them out of the sequence when it's on. Tests: `testExactlyOnce`,
`testFatFingers` (looper), the setting in `testLoopers` (controller) and the
rack file tests.

## Open questions

None at the moment (answered 2026-10-02: one button, tap to record, play,
stop, hold to overdub; a looper saved in the rack as a container, its notes
never saved, and discarded at a song switch).
