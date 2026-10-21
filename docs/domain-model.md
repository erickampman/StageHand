# StageHand — Domain Model (conceptual)

Conceptual models to be fleshed out into interfaces later. No code yet — these
are the entities, responsibilities, relationships, and threading contracts.
Companion to `architecture.md` (technology choices).

Principle carried over: everything **above the platform seams speaks only the
value models below**. A backend (JACK/CoreAudio) or format (CLAP/VST3) is hidden
entirely inside one seam implementation; the engine never knows what's beneath.

## Why backend-agnostic works: the pull model

Every target audio backend (JACK, PortAudio, CoreAudio, ALSA) is a *pull* model:
the backend owns the real-time thread and calls us once per block. The generic
contract is therefore:

> `engine.process(frames, time, out_buffers)` — the backend calls this; the
> engine never asks what is underneath.

## Three layers

```
Session layer   Rack / Patch — what's loaded, routing, saved state   (main thread, persistent)
Engine layer    Graph of nodes; process() contract; RT plumbing      (real-time thread)
Platform seams  AudioBackend | MidiInput | HostedPlugin              (each hides one dependency)
```

## Value models (data that flows — the "nouns")

- **TimeInfo** — per-block transport snapshot: sample rate, sample position,
  tempo, time signature, playing/stopped, loop points. Plugins expect it now
  (CLAP transport); the future sequencer is "the thing that advances TimeInfo".
- **AudioBuffer** — non-interleaved float, `channels × frames`. Backend-neutral;
  JACK per-port pointers and CoreAudio AudioBufferList both adapt into it.
- **MidiEvent** — the canonical MIDI unit, in **UMP format**: block sample-offset
  + up to 4 UMP words + word count. UMP carries both protocols, and **each message
  keeps its original one**: MIDI 1.0 as MT `0x2` (a lossless wrap of the bytes),
  MIDI 2.0 as MT `0x4`. One type, one codepath, no forced conversion. See "MIDI
  protocol handling & per-plugin routing".
- **EventBlock** — per-block, time-ordered list interleaving **MidiEvents and
  ParamEvents**, offsets in `[0, frames)`. Built once at block top, handed to
  plugins (matches CLAP's single sorted input event list).
- **PluginDescriptor** — format-neutral identity: id, name, vendor, format tag
  (CLAP/VST3), path. What a browser lists and a saved Rack references.
- **PortLayout** — a plugin's shape: audio in/out port & channel counts; note
  ports with the dialects each supports (CLAP's four: native CLAP, MIDI, MIDI
  MPE, MIDI2) plus its preferred dialect. Drives wiring and per-message delivery.
- **MidiFilter** — per-slot routing: which source(s), UMP groups, channels, note
  range, velocity range, and transpose a plugin slot receives. Overlaps = layers,
  disjoint note ranges = splits.
- **ParameterInfo / ParameterValue** — id, name, range, default, flags, and the
  CLAP cookie (opaque fast-lookup handle the plugin gives us); plus a value
  carrier for get/set and automation.

## Seam roles (the swappable boundaries — the "verbs")

Abstract roles with defined lifecycles; concrete impls (JackBackend,
AlsaSeqInput, ClapPlugin) are the only things touching a dependency.

- **AudioBackend** — owns the device connection and the RT thread; negotiates
  sample rate & block size; drives the engine process callback; presents output
  (later input) as AudioBuffers; **exposes each block's start time / frame clock**
  (needed for sample-accurate MIDI mapping — see that section).
  Lifecycle: `open(config) → start → [process…] → stop → close`. Owns the RT
  thread — the clock source for everything.
- **MidiInput** — enumerate sources (each with its native protocol), open
  selected ones, receive messages, **timestamp each message at capture**, and
  deliver them as `(source, timestamp, UMP)`. It **wraps but never translates**:
  MIDI 1.0 arrives as MT `0x2`/`0x1`/`0x3`, MIDI 2.0 as MT `0x4`. It applies the
  global MIDI 2.0 protocol preference and can expose virtual input ports for
  other apps. Routing and translation are the engine's job, not the seam's.
  Lifecycle: `setSink → open(source) → [events on its own thread] → close`.
  Owns a non-RT MIDI thread; the engine's sink pushes to a lock-free queue.
- **HostedPlugin** — load/instantiate; report PortLayout & Parameters;
  `activate(sampleRate, maxBlock)`; `process(AudioBuffers, EventBlock, TimeInfo)`;
  produce output events; `stopProcessing()` (audio thread, when it leaves the
  graph); `idle()` (main thread, services plugin requests such as CLAP
  `request_callback`); `saveState() → opaque bytes` / `loadState(bytes)` (see
  Save / restore); optional embedded editor (`openEditor` into a native parent
  window with an `EditorHost` for its requests, `closeEditor`, resizing;
  architecture.md §4).
  Lifecycle: `load → activate → [process…] → deactivate → unload`. Called on the
  RT thread, configured from the main thread.

## Engine model — the graph

(Interfaces: `src/Engine/Engine.h`, `src/Engine/EngineGraph.h`.)

**v1 topology is MainStage-style, not a chain.** Incoming MIDI fans out to every
plugin slot whose MidiFilter accepts it, and each slot's main output is summed
into the backend outputs. That's exactly what layers and splits need (M5), and a
single plugin is just the one-slot case. **The one chain is a row's MIDI
effects** (added 2026-10-21, docs/midi-effects.md): note effects run in order in
front of the slot's plugin in the same block, each one's output the next one's
input; routing, the held-note table and Panic deliver to the first of them. Insert effects, general routing, and the
sequencer (a MIDI source like any other) extend the graph type later. The
long-term vocabulary still applies: sources, plugin nodes, outputs, and typed
connections between them.

**EngineGraph** is immutable once built. It holds the slots (plugin plus
host-owned settings) and the scratch memory the audio thread needs: per-slot
event buffers and audio buffers, all allocated on the main thread.

**Commit / adopt / retire.** The main thread builds a graph and `commit()`s it
(an atomic pointer). At the start of its next block, the audio thread *adopts*
it and pushes the old graph onto a retire queue. The main thread's `collect()`
destroys retired graphs. Graphs are never destroyed on the audio thread, and
adoption waits if the retire queue is full.

**Plugin lifetime and CLAP's thread rules.** Slots hold plugins by `shared_ptr`.
The main thread creates, loads state, and activates. The audio thread starts
processing implicitly on the first `process()`, and calls `stopProcessing()` when
an adopted graph no longer contains the plugin. Because the last reference can
only drop when the main thread destroys a retired graph, a removed plugin is
always stopped (audio thread) before it's deactivated and destroyed (main
thread).

**Shutdown:** commit an empty graph, wait until it's adopted, then stop the
backend.

**Block size:** plugins are activated with a maximum of 8192 frames (PipeWire's
quantum-limit), and larger backend blocks are split into chunks.

## Sample-accurate MIDI ↔ audio alignment

The requirement: a note-on that physically arrived 173 samples into a 256-frame
block must be applied at frame 173, not at the block boundary — otherwise timing
quantizes to the block size (~5.8 ms at 256/44.1k), audible as sloppy timing.
This is a first-class contract, not an optimization.

How the models deliver it:

1. **Timestamp at capture.** MidiInput tags each message with a monotonic
   timestamp on its own thread (ALSA can return a hardware/queue timestamp;
   otherwise `clock_gettime` at read). The lock-free queue carries
   `(timestamp, UMP words)`, never bare bytes with no time.
2. **Map to a frame offset, one period behind.** MIDI drained at the top of cycle
   *k* was captured during cycle *k−1*, so it is always "late" relative to this
   block's start. Mapping it against the block start would collapse every event
   to offset 0. Instead, map the **previous cycle's window** onto this block:
   `offset = round((event_ts − (block_start_ts − frames/sampleRate)) × sampleRate)`,
   clamped to `[0, frames)`. That costs a constant one-period latency and keeps
   relative timing sample-exact (the technique JACK MIDI bridges use). Events
   older than the window (after a stalled cycle) clamp to 0; events stamped at or
   after `block_start_ts` belong to the next block and stay queued.
3. **Deliver a time-sorted EventBlock.** Events are sorted ascending by offset
   (CLAP requires monotonically non-decreasing event times). The plugin receives
   the whole list with offsets and does its **own** sample-accurate splitting
   internally — the host does NOT slice the audio block at each event. This is
   CLAP's model and it keeps the host simple.

**Accuracy bound:** alignment is only as good as the correlation between the MIDI
timestamp clock and the audio-device clock. With ALSA timestamps + the backend's
per-block frame time this is sample-accurate in practice; the residual is input
jitter from the MIDI transport itself, which no host can remove.

**Corollary for the seams (now part of their contracts, not optional):**
AudioBackend must expose the block's start time / frame clock; MidiInput must
timestamp every event at capture.

## MIDI protocol handling & per-plugin routing

Requirements: MIDI 1.0 and 2.0 are both first-class; non-UMP sources are
supported; MIDI-1.0-only CLAP plugins are fully supported; several plugins are
active at once and each receives the MIDI appropriate to it.

### One rule: preserve the protocol, translate only at the plugin boundary

UMP is a *format*; MIDI 1.0 and MIDI 2.0 are *protocols* carried inside it. The
input side only **wraps**:
- MIDI 1.0 (the seq MIDI 1.0 client, or the byte parser for non-UMP streams) →
  MT `0x2` channel voice, MT `0x1` system, MT `0x3` SysEx7. Lossless, reversible.
- UMP-native MIDI 2.0 → MT `0x4` (and `0x5`), untouched.

Nothing is up- or downconverted at input (the one exception is the global
preference set to *off*, which deliberately makes everything MIDI 1.0). So inside
the engine every message keeps its origin protocol, and two plugins listening to
the same source can each receive what suits them.

### Delivery policy: per message, per plugin

**If the plugin's note port accepts the message's own protocol, deliver it
unchanged. Otherwise translate to a dialect the port accepts (its preferred one,
if several).**

| Message | Plugin port accepts | Delivered as | Fidelity |
|---|---|---|---|
| MIDI 1.0 (MT `0x2`) | MIDI (or MPE) | `clap_event_midi`, original bytes | exact (ALSA delivers note-on vel 0 as note-off) |
| MIDI 1.0 (MT `0x2`) | MIDI2 only | upconverted → `clap_event_midi2` | exact per spec; stateful |
| MIDI 2.0 (MT `0x4`) | MIDI2 | `clap_event_midi2`, untouched | exact |
| MIDI 2.0 (MT `0x4`) | MIDI only | downconverted → `clap_event_midi` | lossy: 2.0-only data dropped |
| System / SysEx7 (MT `0x1`, `0x3`) | any | whichever dialect the port accepts | exact |
| any | native CLAP only | translated to CLAP note events | deferred |

- A plugin like PMU (`CLAP | MIDI | MIDI2`) hits rows 1 and 3: MIDI 1.0 arrives
  as bytes and MIDI 2.0 as UMP, with nothing converted. Exactness beats
  "preferred dialect"; the preference only picks the target when translation is
  unavoidable.
- Upconversion state (RPN/NRPN selection, bank select) belongs to the *incoming
  stream*, so it's kept **per source** (by group/channel), not per plugin. Two
  sources on one channel can't corrupt each other's RPN sequences, and each
  upconverted message is computed once and shared by every MIDI2-only plugin.
- **MSB-only Data Entry must not be lost.** Measured in M2: the kernel's own
  upconverter emits an RPN/NRPN only when CC 38 (LSB) arrives, so CC 6 alone (the
  common case on MIDI 1.0 gear) vanishes. Ours emits on CC 6 with LSB 0, then
  re-emits if a CC 38 follows.
- Downconversion is stateless, but one MT `0x4` message can expand into several
  MIDI 1.0 messages (a registered controller → CC 101/100/6/38). They share the
  original sample offset, in order.
- The engine does routing and translation. The plugin seam only maps the type
  already chosen (MT `0x2`/`0x1` → `clap_event_midi`, MT `0x4` →
  `clap_event_midi2`). A VST3 impl decodes both into VST3's own event model
  (architecture.md §3a); which dialect it advertises is its choice.

### Routing: which plugin hears what

Each PluginSlot carries a **MidiFilter** (host-owned per-slot state, persisted in
the Rack):
- **source**: all open sources, or one specific source
- **UMP group mask** and **channel mask**
- **note range** and **velocity range** (note messages only)
- **transpose** in semitones

Overlapping filters on several slots give **layers**; disjoint note ranges give
**splits** (MainStage-style keyboard zones). Non-note channel messages (CC, bend,
pressure) pass whenever source, group, and channel match.

Two correctness rules the router enforces:
- **A note-off follows its note-on.** The router records, per (source, group,
  channel, note), which slots received the note-on, and sends the note-off to
  exactly those slots even if a filter has changed in between (a split moved, a
  Rack switched). Otherwise notes hang. The same table is what will let held
  notes ring through a Rack switch later.
- **Transforms are protocol-aware.** A MIDI 2.0 note-on can carry a **pitch 7.9
  attribute** (absolute pitch; PMU uses it for microtonal/EDO pitch). Transposing
  the note number without moving the attribute breaks the tuning, so transpose
  shifts both.

Filters live in the immutable graph snapshot the engine runs, so a filter edit
on the main thread takes the usual Main→RT snapshot swap. On the RT side, the
per-slot event buffers, held-note table, and upconversion state are all
preallocated and fixed-size. If a slot's event buffer overflows in a block, the
excess is dropped and counted rather than allocated.

### Global MIDI 2.0 protocol preference

App-wide (Logic-style), default **on**. It chooses which of the input seam's two
protocol paths UMP-native sources take (architecture.md §1): **on** = MIDI 2.0
sources arrive as MIDI 2.0; **off** = everything arrives as MIDI 1.0 and no
plugin ever receives `clap_event_midi2`. It's a compatibility safety valve, not
a correctness requirement.

### Testing MIDI 2.0 end to end (PMU)

A MIDI 2.0 test source (the planned KeyEDO port, or a small test tool) registers
as an ALSA seq **UMP MIDI 2.0** client and sends MT `0x4` note-ons carrying pitch
7.9 attributes. StageHand receives them on the MIDI 2.0 path, and PMU (preferred
MIDI2) gets them untouched. A MIDI 1.0 keyboard can't exercise the attribute
path; it has no attribute to send.

## Sequences (data model, draft)

A sequence is a MIDI source the rack owns (rack-format.md, "Reserved:
sequences"). Its UI is the sources dock plus an editor window
(architecture.md, "Sequencer UI"). Decided 2026-09-28:

- **A sequence has a protocol, like every source**, chosen when it's created. A
  MIDI 1.0 / 2.0 toggle sits to the left of the sequence canvas in the editor.
  Changing it later is an explicit conversion. Going from 2.0 to 1.0 is lossy
  and needs a rule for EDO steps that don't land on a note.
- **Notes are stored in the form the protocol sends.**
  - **MIDI 1.0:** each note stores a note number (0–127) and a 7-bit velocity.
    The grid has one row per note number. For a plugin that does its own
    keyboard-mapped tuning (a Scala `.kbm`, for example), the editor can label
    rows as EDO steps. That labeling is display only.
  - **MIDI 2.0:** each note stores an EDO step and a 16-bit velocity. The
    tuning (EDO size and reference pitch) belongs to the sequence, so changing
    the tuning doesn't rewrite the notes, and two sequences can play in
    different EDOs at once. Playback sends MT `0x4` note-ons with the pitch 7.9
    attribute.
- **Delivery needs nothing new.** The engine treats a sequence like a device of
  the same protocol and applies the per-plugin delivery policy above. A MIDI 2.0
  sequence driving a MIDI 1.0 plugin loses pitch 7.9 on downconversion. The row
  could warn when that happens.
- **Note-number assignment for MIDI 2.0 (EDO) notes: nearest free.** With a
  pitch attribute, the note number is the note's *identity*: note-off matching,
  per-note controllers and per-note pitch bend, and the host's held-note table
  all key on it. For EDOs above 12, several steps round to the same 12-TET note.
  If they sound together, they would collide: the second note-on would retrigger
  the first note's voice, and either note-off would end both. So each note-on
  gets the nearest note number (to the step's 12-TET pitch) not already sounding
  in that sequence and channel, and keeps it until its note-off. That's done at
  playback, on the audio thread, with a fixed-size table. A plugin that ignores
  the attribute is off by at most a semitone or two in dense chords. Rejected:
  note number = step index (a plugin that ignores the attribute plays wildly
  wrong pitches, and it covers only ~4 octaves of 31-EDO), and spreading
  collisions across channels (it fights the row's channel filter).

## Parameter changes & automation timing

Core realization: **a parameter change is just another timed event in the same
EventBlock as MIDI.** CLAP's `process()` takes one input event list that
interleaves note/MIDI events with param events (`PARAM_VALUE`, `PARAM_MOD`,
`PARAM_GESTURE_BEGIN/END`), all sorted by sample time. So automation reuses the
sample-offset machinery above rather than inventing a parallel path.

**ParamEvent** value model (joins MidiEvent in the EventBlock):
target node/plugin, param id (+ CLAP cookie for fast lookup), kind
(value / modulation / gesture-begin / gesture-end), the value (or mod amount),
and a **sample offset** in `[0, frames)`.

**Inbound sources → their timing** (all become ParamEvents in the sorted block):
- **Host UI edit** (main thread) — user moves a knob in StageHand's own UI.
  Pushed via a lock-free Main→RT queue; drained at block start and given
  offset 0 (UI gestures aren't sub-block scheduled). *Never* call a plugin
  setter from the UI thread — RT-safety and ordering both require it go through
  the event list.
- **MIDI-mapped control** — a CC / MIDI-2.0 controller mapped to a param. It is
  *derived from a timestamped MidiEvent*, so it inherits that event's sample
  offset → genuinely sample-accurate, for free.
- **Sequencer / automation lane** (future) — a SourceNode emits ParamEvents at
  scheduled offsets → sample-accurate by construction.

**Outbound (plugin → host):** a plugin's `process()` emits output events,
including param-value changes driven by its *own* editor GUI or internal
modulation. The host drains that output list after each plugin and forwards the
changes over an **RT→Main** queue to update its ParameterValue state, its UI,
and what a saved Rack will persist. This keeps host state in sync with reality.

**Stopped / flush path:** when audio isn't running, the plugin's GUI can still
change params. CLAP's `params.flush` handles this off the RT thread; the host
must support it so edits made while stopped aren't lost.

**Gestures:** begin/end brackets around a user touch — needed later for
automation *recording*. Modeled as event kinds now so recording isn't a retrofit.

## Session model — what persists

- **Rack (Patch)** — user-facing, savable: an ordered set of **PluginSlots**
  (PluginDescriptor + opaque plugin state blob + host-owned per-slot settings,
  including its MidiFilter) plus graph connections. A saved filter refers to its
  source by stable source *id*, which is resolved to a live handle when the Rack
  loads or the device appears. A future "setlist" is a
  list of these. Lives on the main thread, serialized to disk (see Save /
  restore); the engine runs an immutable snapshot. A **PluginSlot** may also be a
  *missing-plugin placeholder* that retains the descriptor + blob when the plugin
  isn't installed.

## Save / restore (Rack persistence)

Saving a Rack means capturing three layers, only one of which the host
understands:

1. **Plugin-owned state** — an *opaque byte blob* the plugin produces
   (CLAP `clap.state.save`; VST3 `getState` later). It already contains the
   plugin's parameters and internal data. **The host stores it verbatim and never
   parses it** — that's what keeps StageHand format- and plugin-agnostic.
2. **Host-owned per-slot state** — what StageHand manages, not the plugin:
   bypass/enabled, custom slot name, gain, MIDI source & channel assignment,
   CC→param maps, routing. Fully host-defined and human-readable.
3. **Graph + Rack metadata** — node connections, MIDI source assignments, and
   Rack header (name, format version, app version).

### Seam contract (added to HostedPlugin)

`saveState() → opaque bytes` and `loadState(bytes)`. Format-neutral: a CLAP
stream ↔ blob today, VST3 state later, same signatures. **`loadState` is applied
before the plugin joins the live graph** (see sequencing).

Source of truth for parameters is the **plugin blob**, not a re-serialization of
the host's ParameterValue cache — saving both invites divergence. The host cache
exists only for the UI.

### Container format

Recommendation: a **bundle** — a `.rack` directory (or a zip of it for a single
file) with a human-readable `rack.json` manifest for structure (layers 2 & 3)
plus **one binary file per plugin state blob** for layer 1. This avoids
base64-bloating large plugin states inside JSON and keeps the manifest diff-able.
JSON here would reuse the same header-only `nlohmann/json` PMU already uses; a zip
wrapper (single-file `.rack`) would be the one added dependency to weigh — a
directory bundle needs none. (Exact choice tracked in open questions.)

### Threading & sequencing (all main-thread, off the RT path)

- **Save:** iterate slots → `saveState()` each → gather host-owned settings →
  write the bundle. The plugin may stay active during save (CLAP `state.save` is
  main-thread and callable while active). Edits made while the transport is
  *stopped* are already reflected via the `params.flush` path (see automation
  section), so a Rack saved while stopped is still current.
- **Restore:** parse manifest → for each slot, re-instantiate the plugin from its
  PluginDescriptor → `loadState(blob)` → apply host-owned settings → rebuild
  connections → hand the engine the finished **immutable graph snapshot via the
  Main→RT pointer swap**. Because state is loaded *before* the plugin enters the
  live graph, the RT thread never sees a half-restored plugin.

### Robustness (this is a live-performance tool)

- **Missing/uninstalled plugin on restore** (moved machine, uninstalled plugin):
  keep a **"missing plugin" placeholder** slot holding the PluginDescriptor + the
  raw blob, so re-installing the plugin recovers the patch. Never silently drop a
  slot — losing a plugin mid-setlist is a gig-killer.
- **Versioning:** the manifest carries a format version (refuse a newer major,
  migrate a newer minor); plugin blobs are versioned by the plugin itself and
  passed through opaquely.

### Presets vs Racks (future, same machinery)

- A single-plugin **preset** is just one slot's `saveState`/`loadState` — reuses
  the seam, no new mechanism.
- A **setlist** is an ordered list of Racks. Leave room; don't build.
  (Designed 2026-10-01 in docs/setlists.md.)

## Threading & ownership

| Thread | Owns | Rule |
|---|---|---|
| RT / audio (backend-owned) | Graph.process, plugin process() | no locks / alloc / syscalls |
| MIDI (input-owned) | reading devices → UMP events | push to RT via lock-free SPSC queue |
| Main / control | Rack, loading, param edits, save/load | talks to RT only via queues / atomic snapshot swap |

Three crossings that matter: **MIDI→RT** (event queue), **Main→RT** (param edits
+ new-graph-snapshot pointer swap), and **RT→Main** (param feedback from plugin
GUIs / internal modulation, for UI + state sync). All are SPSC ring buffers; no
third-party lib needed. A plugin's own-GUI edits while transport is *stopped*
take the non-RT `params.flush` path instead.

## Two sequences that pin the contracts

- **Startup:** load Rack → instantiate HostedPlugins → open AudioBackend (learn
  sampleRate/block) → activate plugins → open MidiInput sources → start backend
  → callbacks begin.
- **Per block (RT):** backend calls `process(frames, time)` → drain the MIDI
  queue, mapping each timestamp to a sample offset → **route each message through
  the slot filters (note-offs via the held-note table), apply transforms, and
  translate per slot dialect** → drain the param-input queue → merge into each
  slot's time-sorted EventBlock (MIDI + param events) → walk graph nodes → after
  each plugin, drain its output events (param feedback) onto the RT→Main queue →
  OutputNode fills buffers → return.

## Open questions (to resolve before turning these into interfaces)

- ~~Graph generality in v1~~ Resolved: fan-out / sum topology (MainStage-style)
  behind the commit/adopt/retire contract; see "Engine model".
- Multi-plugin routing model detail (serial chain vs arbitrary graph) — deferred
  with the sequencer, but the Connection model should not preclude it.
- ~~Parameter automation source (host UI vs sequencer) and how changes cross to
  RT.~~ Resolved: unified as ParamEvents in the EventBlock; see "Parameter changes
  & automation timing".
- ~~Container format~~ Resolved: a `.rack` directory bundle (`rack.json` plus
  `state/*.bin`) with a safe swap-in save; a zip form for sharing can come later.
  Specified in `docs/rack-format.md`, including the room reserved for sequences.
- Whether to adopt CLAP `state-context` (distinguishing save-for-preset vs
  duplicate vs project) or ignore it for v1. Leaning ignore until presets land.
- Plugin state blob is opaque/plugin-versioned; only the manifest schema needs
  host-side version handling (resolved in Save / restore).
- SysEx delivery: reassembly of multi-packet SysEx into a contiguous buffer for
  plugins. Leaning: MIDI thread reassembles into a preallocated pool, the event
  carries a reference (architecture.md §1).
- Translation for native-CLAP-only plugins (MIDI 2.0 per-note data → CLAP note
  expressions). Deferred. MPE channel rotation for MPE-only plugins is not
  planned: StageHand passes MPE through but never generates it (architecture.md
  §1, "MPE").
- Rack switch while notes are held: sustain on the old Rack (MainStage-style) or
  all-notes-off. The held-note table supports either; policy TBD.
- ~~Host gain smoothing~~ Resolved in docs/rack-view.md, sections 2 and 3:
  gain moves to a per-slot SlotRuntime shared across graphs, with a fixed 10 ms
  linear ramp on the audio thread. Original note: slot gain
  is applied as a per-block constant, so every committed change is a step at a
  block boundary. Dragging a gain knob in the DGL spike (2026-09-28) produced
  audible zipper noise. A faster UI commit rate only shrinks the steps; it does
  not remove them. Any host gain that changes while sound plays needs a per-sample
  ramp on the audio thread (~5–20 ms toward the target). That covers pedal/CC
  mappings (7-bit MIDI 1.0 pedals give 128 steps), slot faders, mute/unmute and
  crossfades on rack switches. Leaning: the engine keeps an RT-owned current gain
  per slot, keyed by plugin like the held-note table, so it survives graph
  adoption. `SlotConfig::gain` becomes the target. Build it with the first live
  gain control, not separately.
- **Tempo control (requested 2026-09-28).** The path to plugins already
  exists: each block's TimeInfo becomes CLAP's transport event (tempo, time
  signature, playing), which ClapPlugin sends with every process() call. What's
  missing is a source for it. Sketch: a rack-level tempo (and time signature)
  saved in rack.json and set from a toolbar control (typed or dragged value, tap
  tempo). The main thread writes it to the engine atomically, and the audio
  thread advances the beat position per block. Later: follow incoming MIDI
  clock, and send it out. The future sequencer is "the thing that advances
  TimeInfo", so this is its first piece.
- **MIDI 1.0 / 2.0 in the row's input filter (requested 2026-09-28).** Today
  protocol is global (the preference), per source (the row's badge), and per
  plugin (the engine delivers what each note port takes). There's no per-row
  choice. Clarify what's wanted before designing. It could be "receive as"
  (Auto = the plugin's preference, or force MIDI 1.0 / MIDI 2.0 translation for
  this row), or accepting only sources of one protocol.
