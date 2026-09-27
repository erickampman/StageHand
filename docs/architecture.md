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
MIDI in (ALSA UMP)  →  UMP/MIDI-2.0 events  →  hosted plugin (CLAP note ports,
MIDI2 dialect)  →  process()  →  audio out (JACK client, served by PipeWire)
```

Real-time model: the audio backend's process callback is the RT thread. MIDI is
read on a separate thread and handed to the RT thread over a lock-free SPSC
ring buffer, drained at the top of each process block and converted to the
plugin's event format. No allocation or locking on the RT path.

---

## 1. MIDI support

**Requirement:** MIDI input from hardware, MIDI 2.0 as a first-class citizen
(the project's differentiator — DAWs handle MIDI 2.0 poorly).

**Options considered**
- **ALSA rawmidi UMP (kernel ≥ 6.5)** — native MIDI 2.0. The kernel exposes UMP
  endpoints and, crucially, presents *legacy MIDI 1.0 devices as UMP too*, so
  coding to the UMP API handles both old and new hardware from one path. Zero
  third-party dependency (libasound is a system lib). Linux-only.
- **libremidi** — modern C++ MIDI lib (RtMidi successor) with UMP/MIDI-2.0
  support and multiple backends incl. CoreMIDI. One small, single-purpose dep;
  buys cross-platform (Mac) later. Third-party.
- **RtMidi** — rejected: MIDI 1.0 only, so it defeats the whole premise.
- **JACK MIDI / PipeWire MIDI** — JACK MIDI is a 1.0 byte stream (no UMP).
  PipeWire's own MIDI API is heavier and less direct for our needs.

**Decision (initial): ALSA rawmidi UMP directly.** Best MIDI 2.0 fidelity,
no third-party dep, Linux-first matches where we are. Kernel 7.0 on this box
already supports it. Put all MIDI I/O behind an internal seam so libremidi (or
CoreMIDI) can slot in for Mac later without touching the rest of the app.

**Open questions**
- Do we also want ALSA *seq* (UMP-capable) for virtual ports / connecting to
  other apps, vs rawmidi for direct hardware? Likely both eventually.
- MIDI 2.0 hardware is still rare; validate the legacy-device→UMP conversion
  path early with an ordinary MIDI 1.0 controller.

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

**Open questions**
- Client model (join the PipeWire/JACK graph) vs owning a device — client model
  chosen for flexibility; revisit if a dedicated-interface live rig wants
  exclusive access.
- Buffer size / latency defaults and how the user picks the device.

---

## 3. Plugin support

**Requirement:** load and host CLAP plugins now; VST3 later. Minimize deps.

**Options considered**
- **CLAP, hosted against the header-only CLAP SDK** — `clap/clap.h` is
  header-only, MIT, tiny (already vendored in the PMU project). Hosting means
  implementing the host side ourselves: load the `.clap` shared object, get the
  entry point, walk the plugin factory, instantiate, and provide host callbacks
  (audio-ports, note-ports, params, etc.). No framework weight. **CLAP natively
  carries MIDI 2.0** via note-ports `CLAP_NOTE_DIALECT_MIDI2` /
  `clap_event_midi2`, so our ALSA-UMP input can pass straight through to
  plugins that support it, with a MIDI-1.0 fallback dialect for those that
  don't. Perfect alignment with the MIDI-2.0 goal.
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
- Multiple DIN/USB ports appear as ALSA rawmidi devices; `MidiInput` enumerates
  and opens as many as wanted, assigned per Rack slot.
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

## Dependency summary

| Area   | Choice (initial)              | Cost           | Cross-platform later |
|--------|-------------------------------|----------------|----------------------|
| MIDI   | ALSA rawmidi UMP              | system lib     | libremidi / CoreMIDI |
| Audio  | JACK API (via pipewire-jack)  | system lib/API | PortAudio / CoreAudio|
| Plugin | CLAP SDK (header-only, vendored) | headers only | + VST3 SDK (vendored)|
| Glue   | hand-written (ring buffer, host callbacks, plugin abstraction) | none | — |

Net third-party footprint at the start: **effectively just the CLAP headers**,
plus two system libraries (ALSA, JACK-API) that are already on the machine.

## Deferred (not now)

- **UI** — deliberately unexplored for now.
- **Sequencer UI** — a later goal; the graph/routing model should leave room.
- **VST3 hosting** — after CLAP works end to end.
- **Cross-platform (macOS)** — design the seams for it, don't build it yet.

## Cross-cutting: the seams that make deferral safe

Three internal interfaces isolate the platform/format choices so today's
minimal picks never block tomorrow's:
1. **MIDI input seam** — ALSA-UMP today, libremidi/CoreMIDI later.
2. **Audio backend seam** — JACK today, PortAudio/CoreAudio later.
3. **Hosted-plugin seam** — CLAP today, VST3 later.
