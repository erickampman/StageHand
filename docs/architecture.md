# StageHand — Architecture & Technology Decisions

A CLAP (later VST3) plugin host / live rig for Linux, MIDI-2.0-native on the CLAP
path (VST3 is best-effort — see §3a). A MainStage-style performance host, not a
DAW. Sequencer UI is a future goal.

## Guiding principle: minimize third-party dependencies

Prefer the system/native API and header-only SDKs over frameworks. Every
dependency must earn its place: single-purpose, stable, and ideally
header-only or a system library already present. Write the glue ourselves
(host callbacks, ring buffers, the plugin abstraction) rather than pulling in
a framework that does it plus a lot we don't need.

"System library already on the box" (ALSA, the JACK API served by PipeWire) is
treated as a much lighter cost than a vendored/fetched third-party framework.

## Signal flow (the whole app in one line)

```
MIDI in (ALSA seq, UMP mode; MIDI 1.0 + 2.0)  →  UMP events, each keeping its
own protocol  →  per-plugin routing + dialect translation  →  hosted plugins
(CLAP note ports: MIDI / MIDI2)  →  process()  →  audio out (JACK client via
PipeWire)
```

Real-time model: the audio backend's process callback is the RT thread. MIDI is
read on a separate thread and handed to the RT thread over a lock-free SPSC
ring buffer, drained at the top of each process block and converted to the
plugin's event format. No allocation or locking on the RT path.

---

## 1. MIDI support

**Requirement:** MIDI input from hardware and other apps, with **MIDI 1.0 and
MIDI 2.0 both first-class**. MIDI 2.0 is the differentiator (DAWs handle it
poorly), but most hardware, most plugins, and all non-UMP data sources are MIDI
1.0 and must work exactly, not via a lossy round trip. Several plugins are active
at once and each gets the MIDI appropriate to it (see domain-model.md, "MIDI
protocol handling & per-plugin routing").

**Key distinction: UMP the *format* ≠ MIDI 2.0 the *protocol*.** MIDI 1.0 bytes
wrap losslessly into UMP message type `0x2` (and SysEx into `0x3`) — a wrap, not
an upconversion, and reversible byte-for-byte. So the internal canonical event
is UMP, and **every message keeps its original protocol**: MIDI 1.0 stays MT
`0x2`, MIDI 2.0 stays MT `0x4`. Translation between protocols happens only at a
plugin boundary, and only when that plugin can't accept the message's own
protocol.

**Options considered**
- **ALSA sequencer in UMP mode (kernel ≥ 6.5)** — the sequencer is where legacy
  and UMP sources meet: a seq client declares its protocol
  (`snd_seq_set_client_midi_version`) and the kernel converts events from any
  source to it. Covers hardware, virtual ports, and app-to-app connections (e.g.
  a KeyEDO port or a test tool) in one API. System lib, no third-party dep.
- **ALSA rawmidi UMP** — `/dev/snd/umpC*D*` exists **only for UMP-native (USB
  MIDI 2.0) hardware**; legacy devices remain legacy rawmidi and are *not*
  presented as UMP. (An earlier revision of this doc claimed otherwise — wrong.)
  Too narrow to be the primary path.
- **libremidi** — modern C++ MIDI lib with UMP support and CoreMIDI; one small
  dep, buys the Mac later. Third-party.
- **RtMidi** — rejected: MIDI 1.0 only.
- **JACK MIDI** — a MIDI 1.0 byte stream. Not the primary path, but a legitimate
  *non-UMP source* the byte-stream parser (below) can accept.

**Decision (initial): ALSA sequencer, UMP mode, with two clients.**
- A **MIDI 1.0 client** (`SND_SEQ_CLIENT_UMP_MIDI_1_0`): legacy sources connect
  here and arrive as MT `0x2`, exact.
- A **MIDI 2.0 client** (`SND_SEQ_CLIENT_UMP_MIDI_2_0`): UMP-native sources
  connect here and arrive as MT `0x4`, untouched.
- The `MidiInput` impl picks the client per source from the source's own
  protocol, so the kernel never has to convert anything: no upconversion of
  1.0 sources, no downconversion of 2.0 sources.
- **Non-UMP byte streams** (JACK MIDI, legacy rawmidi, files, network) go
  through a small MIDI 1.0 byte parser (running status, real-time interleave,
  SysEx chunking) that also wraps to MT `0x2`/`0x3`, never upconverts.
- **Verified in M2** (kernel 7.0, alsa-lib 1.2.15; `stagehand --monitor` with
  `aseqsend` as legacy, UMP 1.0 and UMP 2.0 sender):
  - Legacy or UMP 1.0 → MIDI 1.0 client: MT `0x2`, values untouched. One
    normalization: **a note-on with velocity 0 arrives as a note-off** (same
    meaning, different bytes).
  - UMP 2.0 → MIDI 2.0 client: MT `0x4` untouched, pitch 7.9 attribute intact
    (the PMU test path).
  - UMP 2.0 → MIDI 1.0 client: downconverted, attributes dropped (as expected;
    this is what the global preference *off* does on purpose).
  - Legacy → MIDI 2.0 client: upconverted with spec min-center-max scaling
    (velocity 100 → `0xC924`). **The kernel's stateful RPN/NRPN translation emits
    only when Data Entry LSB (CC 38) arrives. MSB-only data entry (CC 6 alone,
    common on MIDI 1.0 gear, e.g. pitch-bend range) is silently dropped**, even
    when later messages follow. This is the concrete reason legacy sources must
    land on the MIDI 1.0 client, and a requirement for our own upconverter (M5).
  - A **virtual input** can't know the connecting app's protocol, so a MIDI 1.0
    app connected to "MIDI 2.0 In" gets the kernel upconversion above. Apps
    should connect to the virtual input matching their protocol.
  - The global preference works: with it *off*, "MIDI 2.0 In" is created on the
    MIDI 1.0 client and UMP 2.0 input arrives downconverted.
- Not yet verified: `open()` of a *MIDI 2.0 source* (no UMP-native source with a
  subscribable port was available; needs USB MIDI 2.0 hardware or the KeyEDO
  port). The per-port `snd_seq_port_info_set_ump_is_midi1` flag might allow one
  client instead of two; untested.

**Global MIDI 2.0 protocol preference** (Logic-style, app-wide, default **on**):
- **On:** UMP-native sources are received in MIDI 2.0 (MIDI 2.0 client); legacy
  sources stay MIDI 1.0 (MIDI 1.0 client). Mixed protocols coexist, per message.
- **Off:** every source connects to the MIDI 1.0 client (the kernel downconverts
  UMP-native devices), and plugins only ever receive MIDI 1.0. A compatibility
  safety valve for a misbehaving device or plugin.
- It's a policy switch, not a correctness requirement: because each message
  keeps its protocol, mixed 1.0/2.0 works without it.

**Default inputs: every hardware MIDI source** (ALSA ports flagged
`SND_SEQ_PORT_TYPE_HARDWARE`; software ports like `Midi Through` are left out),
as a live host should. MainStage listens to all inputs unless told otherwise,
and a keyboard that silently isn't listened to is a gig-killer. `--midi`
narrows the set, and each slot's MidiFilter chooses among what's open. Not yet
handled: a device plugged in after startup (needs ALSA's announce port).

All MIDI I/O sits behind the `MidiInput` seam, so libremidi/CoreMIDI can slot in
for Mac without touching routing or translation (both live in the engine).

**MPE: pass-through only (decided 2026-10-11).** StageHand neither generates nor
translates MPE. The policy across these projects is to support no MPE feature
that MIDI 2.0 duplicates: synths accept MPE as a compatibility input, but the
host (StageHand), the DAW (Umpire) and the controller (KeyboardEDO) don't
originate it. A host needs no MPE code for MPE to work: MPE is ordinary MIDI 1.0
spread over several channels, and the delivery policy (domain-model.md) already
hands MIDI 1.0 to a plugin byte-for-byte, so an MPE controller reaches an
MPE-capable plugin intact. Two things keep it intact: a slot's channel filter
must include every channel the controller's zone uses (normally `"all"`), and
nothing may move a note to another channel. MIDI 2.0 per-note data sent to a
MIDI-1.0-only plugin is downconverted to channel messages, never rotated across
channels.

**Open questions**
- SysEx transport to plugins: a message spans many UMP packets and must be
  reassembled into a contiguous buffer (CLAP `clap_event_midi_sysex`). Leaning:
  reassemble on the MIDI thread into a preallocated pool; the event carries a
  reference. Not needed for the first PMU test.
- MIDI 2.0 per-note data → CLAP note expressions, for plugins whose note port is
  native CLAP only. Deferred. (Not MPE: MIDI 2.0 → MPE channel rotation is not
  planned; see "MPE" above.)
- Per-slot protocol override ("force MIDI 1.0 for this plugin"). Deferred.

---

## 2. Audio support

**Requirement:** low-latency audio output for live performance; coexist with
the desktop (PipeWire is running). Audio input later.

**Options considered**
- **JACK client API (served by PipeWire)** — the pro-audio low-latency
  standard; on this system PipeWire *implements* JACK, so no separate server to
  run. Thin, stable API. Graph routing for free (output can go anywhere).
- **PipeWire native API** — most "modern-correct," already running, but a
  heavier API surface than JACK for the same result right now.
- **ALSA PCM directly** — lowest latency, no dep, but grabs the device
  exclusively and fights PipeWire (this is exactly the REAPER "error opening
  the audio hardware" failure mode). Antisocial on a desktop.
- **PortAudio / RtAudio** — cross-platform wrapper; a dep, but gives CoreAudio
  on Mac. Candidate for the eventual cross-platform seam, not needed now.

**Decision (initial): be a JACK client, via the JACK API that pipewire-jack
provides.** Low-latency, coexists with the desktop, minimal API, routable.
Abstract behind an audio-backend seam so CoreAudio/PortAudio can be added for
Mac.

**Which JACK server is chosen per invocation, deliberately.** StageHand links
the ordinary `libjack` (jackd2's). Run under `pw-jack`, it uses PipeWire's;
without it, it talks to a real jackd server if one is running. PipeWire's
library path is *not* baked into the binary (no rpath, no system-wide
`ld.so.conf.d` switch): that would silently rule out real jackd, and other
backends (exclusive ALSA, CoreAudio) belong behind the `AudioBackend` seam, not
in link-time choices.

### Latency / buffer size (resolved; verified in M1 against PipeWire 1.6.2)

**How PipeWire decides.** A *driver* node (the sound card, or the internal
Dummy-Driver when nothing is connected) runs the cycle; each cycle every node
linked to it processes one **quantum** — the buffer size a JACK client sees.
There is one quantum per driver's graph, so once connected to a card StageHand
shares it with every app on that card. Nodes *request* a latency (`node.latency`,
or `PIPEWIRE_LATENCY` — "only a suggestion"); the graph runs at the smallest
request, clamped to `clock.min-quantum`..`clock.max-quantum` (32..2048 here),
defaulting to `clock.quantum` (1024). JACK clients default to
`node.lock-quantum = true`, so the first JACK client running holds the quantum
and later *requests* lose (observed: a 128 request forced up to 1024).

**Forcing mechanisms:**

| Mechanism | Scope | Lifetime |
|---|---|---|
| `clock.force-quantum` (`pw-metadata -n settings 0 …`) | every graph: all apps, all devices | until reset to 0 or PipeWire restarts |
| `node.force-quantum` (node property) | at least the graph that node is in; possibly every driver (see caveats) | only while that node is active |
| `default.clock.*` in `pipewire.conf.d` | defaults and limits | permanent |

**Decision.** StageHand **never sets `clock.force-quantum`**: it's global, and a
crash would leave the whole system forced until someone resets it. Instead,
`--quantum N` (`AudioConfig::bufferSize`) makes JackBackend set
**`node.force-quantum` on its own JACK client**, through `PIPEWIRE_PROPS` at
`jack_client_open`. It uses `PIPEWIRE_PROPS` rather than `PIPEWIRE_QUANTUM`
because the latter forces the sample rate too. A `PIPEWIRE_PROPS` the user set
themselves wins, with a warning. A real jackd ignores the variable.

**Verified behavior** (first on the Dummy-Driver with the card suspended, then
on the card itself):
- A client with no connections joins whichever driver is **already running**
  (the card, if anything is playing) and falls back to the Dummy-Driver only when
  nothing runs. So `--no-connect` does *not* isolate StageHand: with silence
  playing on the card, an unconnected `--quantum 256` StageHand pulled the card
  from 2048 to 256.
- On real hardware (user test), the force applied while StageHand ran and was
  released on exit. A driver keeps showing its last quantum in `pw-top` for a
  few seconds, until WirePlumber suspends it as idle.
- A per-node force overrides another client's lock (a locked client at 1024 was
  pulled to 256).
- It lasts only while StageHand's node is active: that client returned to 1024
  when the forcing client exited. By the same mechanism a crash releases it.
- Forcing bypasses `clock.min-quantum` (a forced 16 took effect under a minimum
  of 32). The hard bounds are the documented `quantum-floor` (4) and
  `quantum-limit` (8192), which `--quantum` enforces.
- The force applies at **activation**, so `open()` still reports the old size;
  `config()` and the buffer-size callback report the change. Nothing downstream
  may treat `open()`'s buffer size as final.

**Caveats.** At minimum every app on the same device runs at the forced size.
**Possibly all audio on the system:** during a user test with StageHand connected
to the card, the Dummy-Driver was also seen at 256, and PipeWire's wording
("force a quantum in the graph… the last node to be activated wins") reads as
system-wide. Not isolated yet (it needs StageHand in a separate graph while the
card runs), so **treat `--quantum` as system-wide**. The per-node lifetime
(automatic release) holds either way, and that's the property that makes it
safe. Very small quanta on real hardware risk xruns. Untested: how a per-node
force interacts with a global `clock.force-quantum`.

**Engine consequence (M3).** The buffer size can change in both directions while
running. Plugins can be activated with a maximum of 8192 (PipeWire's documented
largest graph buffer), with block splitting kept as a safety net for other
backends and configurations.

**Open questions**
- Client model (join the PipeWire/JACK graph) vs owning a device — client model
  chosen for flexibility; revisit if a dedicated-interface live rig wants
  exclusive access.
- How the user picks the device (today: auto-connect to the system playback
  ports).

---

## 3. Plugin support

**Requirement:** load and host CLAP plugins now; VST3 later. Minimize deps.

**Options considered**
- **CLAP, hosted against the header-only CLAP SDK** — `clap/clap.h` is
  header-only, MIT, tiny (already vendored in the PMU project). Hosting means
  implementing the host side ourselves: load the `.clap` shared object, get the
  entry point, walk the plugin factory, instantiate, and provide host callbacks
  (audio-ports, note-ports, params, etc.). No framework weight. **CLAP carries
  both protocols natively**: `CLAP_NOTE_DIALECT_MIDI` (`clap_event_midi`, 3
  bytes) and `CLAP_NOTE_DIALECT_MIDI2` (`clap_event_midi2`, UMP words), declared
  per note port along with a preferred dialect. MIDI-1.0-only CLAP plugins are
  fully supported: MT `0x2` events go to them as exact bytes, and MT `0x4`
  events are downconverted for them. Plugins that accept MIDI2 get MT `0x4`
  untouched. Per-message rules are in domain-model.md.
- **clap-helpers** — optional header-only C++ helpers for the host side. May
  adopt to reduce boilerplate; still header-only, MIT.
- **VST3 SDK** — heavier C++/COM-like framework (MIT since SDK 3.8, 2025).
  Hosting is more involved. **Deferred** until the CLAP path works end to end;
  vendor the SDK then (as PMU does).

**Decision:** Start with **CLAP hosting on the vendored header-only SDK**.
Define an internal **format-agnostic plugin interface** (a `HostedPlugin`
abstraction) from day one so VST3 becomes an additive backend later, never a
rewrite. Defer the VST3 SDK entirely for now.

**Open questions**
- Adopt clap-helpers, or hand-roll the host callbacks for full control /
  minimal deps? Lean hand-rolled first, adopt selectively.
- Parameter model / state save-restore for the host (per-plugin) — needed once
  more than one plugin can be loaded.

---

## 3a. VST3 impedance & translation notes

Evaluated the domain model against VST3's architecture before committing to it.
**Result: no model rework needed** — the seam + opaque-state-blob + canonical
internal-event choices absorb VST3 as *translation work inside the VST3
`HostedPlugin` impl*, never a redesign. But VST3 differs from CLAP in ways that
must be planned, not discovered mid-implementation. Recorded here so the future
VST3 pass is scoped up front.

### MIDI 2.0 fidelity is a CLAP-only guarantee (scopes the product premise)

**VST3 has no MIDI 2.0 / UMP concept at all.** Our UMP `MidiEvent` decodes cleanly
to CLAP (`clap_event_midi2`), but for VST3 it must be *decoded* into VST3 note
events + parameter changes, and MIDI-2.0's higher resolution (32-bit velocity,
per-note controllers/pitch) only partially maps — some to VST3 **Note
Expression**, the rest downcast to MIDI-1.0 semantics.

So the "MIDI-2.0-native" premise is exact **only on the CLAP path**:
- **CLAP:** end-to-end MIDI 2.0.
- **VST3:** best-effort / downcast; lossy by VST3's own limitation, not ours.

This is a reason CLAP is the **first-class** format, not co-equal with VST3.

### The canonical `EventBlock` is CLAP-shaped; VST3 must demultiplex it

CLAP consumes exactly our model: one time-sorted list interleaving notes + params
+ MIDI. VST3 splits these apart, so the VST3 impl must fan the flat `EventBlock`
out each block:
- notes → `IEventList`
- param events → `IParameterChanges`, **grouped by param id into per-param
  `(sampleOffset, value)` queues** (not a flat list)
- MIDI CC → parameter changes via the plugin's declared
  `IMidiMapping::getMidiControllerAssignment` (CCs are *not* events in VST3). This
  is a **second, plugin-declared mapping layer**, separate from the user's macro
  CC→param mapping, and mandatory or CCs never reach the plugin.

Sample-accuracy survives the transform. Treat `EventBlock` as an internal
canonical form, not "the CLAP format".

### Processor/controller split: mirror-image param feedback + dual state

VST3 is two objects — `IComponent` (processor, audio thread) + `IEditController`
(GUI, main thread). Two consequences the model already absorbs, with per-format
semantics worth stating:
- **Dual state:** `saveState()` for VST3 wraps *both* component and controller
  state into the one opaque blob. This is exactly why "the host never parses the
  blob" was the right call — it survives VST3's split unchanged.
- **Param feedback runs mirror-image to CLAP.** A plugin's own-GUI edit:
  - **CLAP:** plugin self-applies, then *emits* the change → host observes via the
    **RT→Main** queue (sync/persist only).
  - **VST3:** controller calls `IComponentHandler::performEdit` on the **main
    thread**; the host **must forward it to the processor** via the **Main→RT**
    queue next block, or the knob changes nothing. Load-bearing, not just sync.

  The existing three-queue design covers both; the directions just differ by
  format.

### Minor / mechanical (per-format, handled at the seam)

- **Param value space:** VST3 params are normalized `[0,1]`; CLAP uses plain
  range. Canonical `ParameterValue` stores **plain** (CLAP-natural); the VST3 seam
  normalizes at the boundary.
- **Bus/arrangement negotiation:** VST3 `activate` also does `setBusArrangements`
  (host picks channel layout; plugin may reject). `activate(sampleRate, maxBlock)`
  encompasses it internally. `PortLayout.dialect` is CLAP-only; a VST3 event bus
  has none.
- **Descriptor id:** VST3 class IDs are 128-bit TUIDs (32-hex); keep
  `PluginDescriptor.id` a format-tagged opaque string wide enough for both.
- **TimeInfo ↔ VST3 `ProcessContext`:** a clean match, no friction.
- **Linux VST3 GUI needs `IRunLoop`** (host registers the plugin's fds/timers into
  its event loop) — more involved than CLAP embed. Deferred with UI; flagged.

---

## 3b. Scaling to professional hardware

A high-end audio/MIDI interface (RME, MOTU, Focusrite, native USB-MIDI-2.0
devices) is supported by this design, and the MIDI-2.0 case is *better* than
supported — it's the case the MIDI model was built for. Captured so the
non-hardcoding disciplines aren't lost.

**Audio — reachable as-is via the JACK/PipeWire client:**
- **Multichannel:** the model is already N-channel (`AudioBuffer` = channels ×
  frames, `PortLayout` = N ports). Discipline: keep channel count **dynamic**
  end to end — the `OutputNode` exposes the card's full port set and routing
  targets specific hardware outs. **Never hardcode stereo.**
- **High sample rates:** the backend *learns* the rate at `open()`; everything
  downstream is rate-agnostic. 96k/192k just work.
- **Low latency:** as a JACK/PipeWire client we inherit its RT scheduling
  (SCHED_FIFO via rtkit) — the thing that actually delivers pro latency — without
  acquiring real-time priority ourselves.

**The seam is the growth path for the demanding case.** The convenient
JACK-via-PipeWire path carries slight overhead vs bare metal. Users who want
**exclusive, lowest-latency** access to a pro card get it via a *new*
`AudioBackend` implementation (bare ALSA on the device, or a dedicated JACK
server) slotted beside `JackBackend` — engine, graph, and plugins never know.
This is the additive-backend move the seam exists for; not needed now, but the
door is open.

**MIDI — where it shines:**
- Multiple DIN/USB ports appear as ALSA sequencer ports; `MidiInput` enumerates
  and opens as many as wanted, and each Rack slot's MIDI filter chooses which it
  hears.
- A genuine **USB-MIDI-2.0 interface** exposes real UMP endpoints through ALSA,
  and since our canonical event *is* UMP, that hardware is first-class, not
  adapted.

**Honest caveats (none are blockers):**
- **Device/port selection** is a Session/global-config concern only sketched so
  far; needs fleshing when config is built.
- **Two separate audio cards at once** is the one real edge case — a single RT
  graph needs one master clock. PipeWire can aggregate (with resampling); bare
  JACK wants one master. **Initial stance: one audio device at a time.**
- Keep stereo assumptions out of the *implementation* — the model is N-channel;
  the code must stay that way.

---

## 4. Plugin editors (M4, not StageHand's own UI)

StageHand's own UI is still deferred. Hosting a *plugin's* editor is a separate,
smaller job, and it's needed now: without it a plugin plays only its default
state.

- **Embedding, X11 only.** CLAP embeds editors on Linux only through X11
  (`CLAP_WINDOW_API_X11`); its Wayland API is floating-only. So StageHand opens a
  plain top-level X11 window (Xlib; `libX11` is a system library and already a
  PMU build dependency) and passes it as the editor's parent. On a Wayland
  session both windows run through XWayland, as REAPER's do. Floating editors
  aren't supported.
- **The host owns the event loop.** Plugin editors on Linux have no event loop
  of their own. They ask the host for periodic timers (`timer-support`; PMU
  registers a ~60 fps timer to drive pugl/NanoVG) and file-descriptor watches
  (`posix-fd-support`; common in other frameworks). So the app's main thread
  becomes a `poll()` loop (`App/MainLoop`) that runs timers, watched fds (the
  plugins' and the editor window's X11 connection), and StageHand's
  housekeeping. `HostServices` exposes timers and fd watches to the plugin seam
  in format-neutral terms.
- **Per-editor requests** (resize, show/hide, closed, resize hints) go to an
  `EditorHost` the host passes to `HostedPlugin::openEditor()`, not to the shared
  `HostServices`, because they concern one window.
- **Closing an editor window closes that editor only**; audio keeps running.
  **Ctrl-Q in an editor window quits StageHand** (the same shutdown as Ctrl-C in
  the terminal, so the rack is saved unless `--no-save-rack`), and
  **Ctrl-Shift-S saves the rack right away**. Both are passive key grabs on the
  host window, so they work while the plugin's editor has focus. Everything
  else, including Ctrl-C and Ctrl-S, still goes to the plugin: inside an editor
  those belong to the plugin (copy, its own save), and in a window, unlike a
  terminal, Ctrl-C is no signal.
- **Lifetime:** the editor closes before its plugin is destroyed, and a plugin's
  timers and fd watches are removed with it even if the plugin forgot to.
- **Scale:** StageHand doesn't call CLAP `set_scale` on X11, so plugins pick
  their own DPI. Revisit if editors come up the wrong size on a HiDPI screen.

## Dependency summary

| Area   | Choice (initial)              | Cost           | Cross-platform later |
|--------|-------------------------------|----------------|----------------------|
| MIDI   | ALSA sequencer, UMP mode (+ MIDI 1.0 byte parser) | system lib | libremidi / CoreMIDI |
| Audio  | JACK API (via pipewire-jack)  | system lib/API | PortAudio / CoreAudio|
| Plugin | CLAP SDK (header-only, vendored) | headers only | + VST3 SDK (vendored)|
| Editor windows | Xlib (M4) | system lib | Cocoa |
| Rack files | nlohmann/json 3.11.3 (single header, MIT, vendored; same copy as PMU) | header only | same |
| Glue   | hand-written (ring buffer, host callbacks, plugin abstraction) | none | — |

Net third-party footprint at the start: **effectively just the CLAP headers**,
plus two system libraries (ALSA, JACK-API) that are already on the machine.

## Deferred (not now)

- **UI** — not built yet. **Leaning: DPF's DGL (pugl + NanoVG over OpenGL)**,
  the stack PMU already uses. It reuses PMU's widget work (`NodeView`, knobs,
  param panels); it's vendored and permissively licensed (ISC/zlib), with no
  install beyond what PMU needs; it's cross-platform the way PMU is (X11 now,
  Cocoa later, via pugl); and custom drawing suits a stage UI (large,
  high-contrast). Cost: DGL targets plugin UIs, so desktop pieces are thin
  (minimal text entry, no native menus, multi-window needs care). DPF's file
  dialog covers Load/Save. Alternative: Dear ImGui (MIT; fastest for tool-style
  UI, can run on pugl + OpenGL; developer-tool look unless styled, shares
  nothing with PMU). **Ruled out:** Qt (user decision), GTK 4 (heavy, weak on
  macOS, dropped foreign-X11 embedding), JUCE (weight, licensing), wxWidgets
  (GTK underneath), web UIs (weight). **Decided 2026-09-28: DGL** (the spike
  ran a DGL window next to PMU's editor on StageHand's MainLoop, both
  responsive, with 0 xruns). The v1 rack view design is in docs/rack-view.md.
- **Sequencer UI** — a later goal. Sequences are sources in the Rack model
  (docs/rack-format.md, "Reserved: sequences"). The graph/routing model already
  leaves room. Layout decisions (2026-09-28):
  - **Rack rows are plugin instances, nothing else.** Each row has three parts.
    The input side holds sources, the filter (MIDI version, channel, group,
    key range, …) and MIDI effects. The middle holds the plugin, its editor
    button and, later, a few exposed parameters. The output side holds level,
    meter, sends and insert effects. Sequences are not rack rows.
  - **Placement rule:** anything that belongs to one row lives in that row.
    Anything that can feed several rows is a source.
  - **Sources dock** at the bottom of the main window. One row holds hardware
    inputs. One or more rows hold sequences, each with its own transport
    controls (MIDI-mappable). A plugin row connects to a source through its input
    side, by picking it or by dragging the source onto the row.
  - **The sequence editor is its own window**, like plugin editors. It's a grid
    with one row per note (one per EDO step for EDO sequences) that is too tall
    for a dock pane. The sequence data model (MIDI 1.0 and MIDI 2.0 / pitch 7.9
    EDO sequences) is still open.
- **VST3 hosting** — after CLAP works end to end.
- **Cross-platform (macOS)** — design the seams for it, don't build it yet.

## Cross-cutting: the seams that make deferral safe

Three internal interfaces isolate the platform/format choices so today's
minimal picks never block tomorrow's:
1. **MIDI input seam** — ALSA sequencer (UMP mode) today, libremidi/CoreMIDI
   later. Routing and protocol translation live in the engine, not the seam.
2. **Audio backend seam** — JACK today, PortAudio/CoreAudio later.
3. **Hosted-plugin seam** — CLAP today, VST3 later.
