# StageHand — The rig: effect buses, sends and the master (design)

Proposed 2026-10-02; nothing built yet. Shared effects (a reverb, a delay)
that every row can feed, plus one master level for the whole output. Builds
on:
- setlists.md, "Domains": the rack owns a piece's sound, the setlist owns
  only order and switching. The rig is a new layer between them (below).
- architecture.md, "Sequencer UI": "The output side holds level, meter,
  sends and insert effects."
- rack-view.md, sections 2 and 3: SlotRuntime and the gain ramp, which
  sends and the master reuse.

## Decisions (2026-10-02)

- **Effect buses and sends.** Each row has a send level per bus. The buses
  hold shared effects, and their returns are mixed with the dry rows. (This
  is the classic aux send/return layout, not a compressor's sidechain key:
  "sidechain" is kept for that.)
- **The buses belong to a rig, not to the setlist or the rack.** A rig is
  loaded once and stays loaded across songs, so switching songs never
  reloads the effects and their tails ring on through a switch. A setlist
  names its rig; several setlists can share one; a rack opened on its own
  uses the default rig.
- **2 buses** (A and B).
- **Returns go to outputs 1-2** by default.
- **Sends are taken after the row's fader** (so after its gain and mute): a
  muted row stops feeding the buses, while what's already in them rings out.
  Sends before the fader can be an option later.
- **A master level** on the final mix (dry rows plus both returns), and no
  separate dry level: each row's own fader is its dry level, and each bus has
  its return level, as on a mixing desk or in MainStage's concert-level
  master. The master scales every main output.
- **Each bus has two effect slots, in series** (say, a reverb, then an EQ).
  Either can be empty.
- **Ctrl-S saves everything with changes:** the rack (every unsaved rack in
  the set, setlists.md section 6) and the rig. The setlist itself is always
  saved at once.
- **A new song starts as a copy of the current one:** File → "New rack in
  this set..." copies the current song's rack (its rows, plugin states,
  sends, sequences and settings) to the new bundle, rather than starting
  empty. With no song playing, it starts empty.
- **No latency compensation** for effects that look ahead: their delay isn't
  compensated.
- **Deferred:** sidechain ducking (a row or sequence driving a compressor's
  key input). It fits later as a bus whose output feeds a key input instead
  of being heard.
- **Not planned:** insert effects on rows. Many instruments have effects of
  their own.

## Domains, with the rig

| Layer | Owns |
|---|---|
| Rack (a song) | rows, their gain, mute, output pair, and **their send levels**; sequences; tempo and transport |
| Rig | the 2 buses (each one's two effect slots and their state, return level, mute, output pair) and the master level |
| Setlist | the order of songs, switch settings, its MIDI triggers, and **which rig it uses** |
| App | the machine: audio device, MIDI devices, last position, the default rig |

The test from setlists.md gains a line: if it's shared by every song and
should stay loaded through switches, it's the rig's.

## Signal flow

```
row 1 ─ gain/mute ─┬─────────────────────────────▶ its output pair (dry)
                   ├─ send A ─▶ bus A input
                   └─ send B ─▶ bus B input
row 2 ...
bus A input ─ effect 1 ─ effect 2 ─ return level/mute ─▶ outputs 1-2
bus B input ─ effect 1 ─ effect 2 ─ return level/mute ─▶ outputs 1-2
main outputs (dry + returns) ─ master level ─▶ the backend
```

- **Engine:** per block, each row's output is summed dry into its output
  pair as today, and also into each bus's input buffer, times its send level.
  Send levels ramp like gain (`Engine::kGainRampSeconds`), so a send move
  doesn't click. Then each bus runs its first effect with that buffer as
  the effect's main audio input (`ProcessContext::audioIn`, filled for the
  first time), its second effect on the first one's output (an empty slot
  passes audio through), and adds the result to its output pair times the
  return level.
  Last, the master level (ramped) scales the main outputs. The click's aux
  outputs aren't touched by the master.
- **The graph:** the rig's buses go into every EngineGraph, with their own
  runtimes (send, return and master gains), so a song switch commits a new
  graph whose bus plugins are the same instances: nothing reloads, and
  nothing is cut.
- **Effects get no MIDI** at first.

## The rig on disk

A bundle like a rack: `<name>.rig/` with `rig.json` and `state/` for the
effects' saved state. A setlist refers to its rig by path, relative to the
setlist like its racks. The default rig's path is an app setting
(`$XDG_STATE_HOME/stagehand/`, with the last position), created empty on
first use. A rack's slots gain `"sends": [dbA, dbB]` (off when absent), an
additive change to the rack format.

## UI (proposed)

- **Rows:** two small send controls (A, B) in the output column, beside the
  gain fader: a drag or the wheel, a double-click for off.
- **Bus strips:** two rows, between the rack's rows and the sources dock:
  the bus's two effect slots (each a plugin menu listing effects, with its
  Editor), return fader and meter, M, and its output pair.
- **Master:** a fader beside the master meter in the status bar.
- **Choosing an effect:** plugins are classified by CLAP's features
  (`PluginDescriptor::kind`, done 2026-10-02): "+ Add plugin" lists
  instruments, then other plugins (note effects), then the audio effects
  greyed out under "Effects (for the rig's buses: not in rows yet)". The bus
  menu will list the effects.

## Proposed milestones

1. **Engine:** audio into plugins, the 2 buses with two effect slots each,
   sends, returns and the master, all ramped. Tests with a fake effect: send
   levels, post-fader sends (a muted row feeds nothing), two slots in series,
   returns, the master, bus plugins kept across graph commits.
2. **The rig model and file,** the Session owning it, sends in the rack
   format, the default rig, and a setlist naming its rig.
3. **UI:** send controls, bus strips, the master fader, effect
   classification, and Ctrl-S saving the rig too.
4. **New song as a copy:** "New rack in this set..." copies the current
   song's rack. Independent of 1-3; it can come at any time.

## Open questions

None at the moment (answered 2026-10-02: a new song copies the current one;
one master on the final mix; two effect slots per bus; Ctrl-S saves
everything).
