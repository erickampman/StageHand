# StageHand — MIDI effects on a row (note effects)

A rack row can run CLAP note effects (MIDI in, MIDI out; Retune is the first)
in front of its instrument. They sit on the row's input side, after the source
and filter, as docs/architecture.md ("Sequencer UI") has always placed them.
Asked for 2026-10-21 for Retune → StruckSynth: Bitwig sends plugins no MIDI
2.0, so StageHand, which offers plugins MIDI | MIDI2, is the host where
Retune's microtonal (Pitch 7.9) path works.

All three sections are built: the engine first, then (once the author chose
among the options) the rack file, Session and window.

## 1. Plugin output events (uw-core, built)

`ClapPlugin` used to drop everything a plugin sent except parameter changes.
Now its notes and MIDI reach the engine's `EventSink` as `Midi` events, with
their sample offsets and port. MIDI keeps its protocol, the same rule as
input:

| Plugin sends | Engine gets |
|---|---|
| `CLAP_EVENT_MIDI` | MT `0x2` (channel voice) or `0x1` (system), group 1 (`ump::fromMidi1Bytes`) |
| `CLAP_EVENT_MIDI2` | its UMP words, unchanged |
| `CLAP_EVENT_NOTE_ON` / `NOTE_OFF` | a MIDI 2.0 note on/off in group 1, 16-bit velocity |
| wildcard notes (key or channel -1), `NOTE_END`, `NOTE_CHOKE`, SysEx, note expressions | nothing, for now |

A CLAP note becomes MIDI 2.0 rather than 1.0 so its velocity keeps its
resolution; an instrument that takes only MIDI 1.0 gets it downconverted like
any other MIDI 2.0 message. Note expressions (Retune's planned CLAP-dialect
path, its PLAN.md) would map to MIDI 2.0 per-note messages; not needed while
Retune sends MIDI 2.0 to a host that takes it.

## 2. The chain in the engine (built)

`SlotConfig::midiEffects`: the row's MIDI effects, in order, before its plugin.
Per block (per chunk, when a block is split):

- **Routing is unchanged, and happens at the head of the row.** The filter, the
  held-note table, Panic, mute and pedal hand-over all work as before. They
  deliver to the first MIDI effect's note port 0, translated for it, instead of
  to the plugin. The held-note table still names the row by its plugin, so a
  note-off goes through the same effects its note-on did. A note effect must
  treat it the way it treated the note-on (Retune latches each note's output
  key for this).
- **Each effect's output is the next one's input**, and the last one's is the
  plugin's, in the same chunk at the same offsets. The engine's `ChainSink`
  translates each message for the next note port the way `deliver()` does: its
  own protocol if that port takes it, otherwise downconverted, or upconverted
  with a converter state kept per effect output stream. Output out of time
  order is sorted, and an offset past the chunk is clamped into it.
- **No audio is heard from an effect.** One with an audio output gets a scratch
  buffer of that size (none, for Retune), never mixed.
- **Lifetime is the plugin's.** An effect that leaves the graph is stopped by
  the audio thread (`EngineGraph::contains` includes them). A muted row that
  suspends stops its effects too. A second Panic resets them.
- Limits: each effect's output shares the slot's per-block event capacity
  (1024); overflow is counted in `slotEventsFull`, as for the slot.

Tests: `test-engine` (`testMidiEffects`, with `FakeNoteEffect` in
`tests/FakePlugin.h`): two effects in a row, translation at each port, a Pitch
7.9 attribute reaching a MIDI 2.0 plugin, sorting and clamping, split blocks,
Panic, and an effect leaving the graph. `test-note-effects` runs the real
Retune (31-EDO, one step up) into StruckSynth through the engine, into memory,
and checks StruckSynth gets note 60 with Pitch 7.9 = 60 + 198/512 and sounds.
It skips unless both are installed.

## 3. The rack file and the window (built; decided 2026-10-21)

The author chose the proposal's recommendations: a MIDI FX dropdown on the
input side, note effects greyed in "+ Add plugin", and a missing or bypassed
effect passed by.

### Rack file (additive; version stays 2)

A slot gains `midiEffects`, in order (docs/rack-format.md, "Slot"):

```json
"midiEffects": [
  {
    "id": "retune",
    "name": "Retune",
    "plugin": { "format": "clap", "id": "com.unlikelyware.retune", "path": "~/Library/Audio/Plug-Ins/CLAP/Retune.clap" },
    "state": "state/pad.retune.bin",
    "bypassed": false
  }
]
```

- `id` is unique within the slot, by the slot id's rule; the state file is
  `state/<slot>.<effect>.bin`, which can't collide with a slot's
  `state/<slot>.bin` (ids have no `.`). `name` is written only when it differs
  from `id`.
- An effect that isn't installed, or whose row's plugin isn't, is kept with its
  descriptor and state, saved back unchanged, and passed by. The load reports
  it, and so does Check setlist (`runningProblems()`).
- `bypassed`: left out of the graph; the row hears its input directly.

### Session and controller

`Session::Slot::midiEffects` holds each effect's plugin (or why it didn't
load), parallel to `saved.midiEffects`. They're created, restored and activated
with their row (`prepareStep()`), saved by `capture()`, idled with the rest, and
put into `SlotConfig::midiEffects` unless bypassed. Edits, all structural:
`addMidiEffect` (refused for a placeholder row, or a plugin without a note input
and output), `removeMidiEffect`, `moveMidiEffect`, `setMidiEffectBypassed`.
`RackController` adds the same intents, and an effect's editor window, kept in
the editors map as `"<slot>/<effect>"` (closed with the effect or its row).
`RowModel::midiEffects` carries each one's name, problem, bypass and editor
state. `stagehand`'s rack listing prints them.

### Window

- **The MIDI FX dropdown** sits on the row's first line, after the input menu
  (which gives up the room): the first effect's name, "+N" for more, or
  "MIDI FX" dimmed when there are none; red if one isn't loaded, dim if all are
  bypassed. Its menu lists each effect (numbered, in order) with *Editor*,
  *Bypass*, *Move up*, *Move down* and *Remove*, then *Add a MIDI effect...*,
  the note effects (CLAP "note-effect"), added after the others.
- **E**, beside the dropdown, opens the editor when the row has exactly one
  effect and it has one.
- **"+ Add plugin"** and the other row-making plugin lists show note effects
  greyed, under "Other (note effects): add one from a row's MIDI FX".
- The row's MIDI light stays on the input: what the row hears.
- The help page has a "MIDI Effects" section.

Tests: `test-rackfile` (`testMidiEffects`: round trip, state files, bypass, a
missing state file, duplicate ids), `test-session` (`testMidiEffects`: load with
state, a missing effect and a placeholder row kept, bypass left out of the
graph, capture, add/move/bypass/remove live, a removed effect stopped and
destroyed), `test-rackcontroller` (`testMidiEffects`: the view model, refusals
with notices, editors, idle).
