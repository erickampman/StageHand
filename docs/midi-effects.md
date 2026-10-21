# StageHand — MIDI effects on a row (note effects)

A rack row can run CLAP note effects (MIDI in, MIDI out; Retune is the first)
in front of its instrument. They sit on the row's input side, after the source
and filter, as docs/architecture.md ("Sequencer UI") has always placed them.
Asked for 2026-10-21 for Retune → StruckSynth: Bitwig sends plugins no MIDI
2.0, so StageHand, which offers plugins MIDI | MIDI2, is the host where
Retune's microtonal (Pitch 7.9) path works.

Sections 1 and 2 are built. Section 3 (the rack file and the window) is a
**proposal for review**: nothing in the file format, the Session or the UI has
changed yet.

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

## 3. Proposal: the rack file and the window (for review)

### Rack file (additive; version stays 2)

A slot gains `midiEffects`, in order:

```json
"midiEffects": [
  {
    "id": "retune",
    "name": "Retune",
    "plugin": { "format": "clap", "id": "com.unlikelyware.retune", "path": "..." },
    "state": "state/pad.retune.bin",
    "bypassed": false
  }
]
```

- `id` is unique within the slot; the state file is `state/<slot>.<effect>.bin`,
  so it can't collide with a slot's `state/<slot>.bin` (slot ids have no `.`).
- `plugin` and `state` mean what they do for a slot. An effect that isn't
  installed is kept as a placeholder holding its descriptor and blob, and
  saved back unchanged, like a missing slot plugin.
- `bypassed`: the effect is left out of the graph, so the row hears its input
  directly. (Open question 3.)

Session: `Session::Slot` gets the effects' plugins (created, state loaded and
activated like the slot's plugin), `configOf()` fills
`SlotConfig::midiEffects`, `capture()` saves their state, the idle loop idles
them, and the edit API gains `addMidiEffect(slot, PluginDescriptor, index)`,
`removeMidiEffect`, `moveMidiEffect` and `setMidiEffectBypassed`, all
structural (a graph commit).

### Window (needs your decision)

Proposal:

- **The row's input side gets a "MIDI FX" control** after the filter: a small
  dropdown in the house style showing the first effect's name ("Retune", or
  "Retune +1" for two), or "MIDI FX" dimmed when there are none. A click opens
  a menu: each effect with *Editor*, *Bypass*, *Move up/down* and *Remove*,
  then *Add a MIDI effect*, which lists the "Other (note effects)" plugins.
- **Editor:** from that menu, per effect. With exactly one effect, the
  control also has a small editor button of its own, like the row's plugin.
- **"+ Add plugin" stops offering note effects as rows.** Placing one today
  makes a row that makes no sound. They'd be listed greyed under "Other (note
  effects): add one from a row's MIDI FX", as audio effects are listed for the
  rig's buses.
- The row's MIDI activity light stays on the input (what the row hears).

### Open questions

1. Is the "MIDI FX" dropdown on the input side where you want it, or would you
   rather have one chip per effect in the row (wider rows), or effects only in
   the row's "..." menu?
2. Should "+ Add plugin" drop note effects (greyed, as proposed), or offer
   "New row: note effect + instrument" (it asks for the instrument next)?
3. A missing or bypassed effect: pass the MIDI through (proposed; a missing
   Retune then plays untransposed, with the row's missing-plugin warning), or
   silence the row until it's found?
