# StageHand — Panic, and CPU load and xruns (design)

Proposed 2026-10-03. Two things for when something goes wrong on stage: a
**Panic** that silences stuck notes at once, and a **CPU load and xrun
indicator** that shows trouble coming before it's heard. Builds on:
- Engine.h: the held-note table (a note-off goes wherever its note-on went)
  and the held pedals (sent to a new song's rows at a switch).
- looper.md and rig.md: the looper switch, a learned control in the rig,
  the same in every song. Panic's control lives beside it.
- perform.md: the Perform view, where Panic needs a big button.
- The status bar, which already shows the xrun count (in amber once there's
  one).

## Panic

1. **What it does, at the next block's start, in every row of the current
   song:**
   - a note-off for every note the engine knows is sounding (the held-note
     table), at the note number each plugin got;
   - the pedals up (CC 64, 66 and 67 to 0) and All Notes Off (CC 123) on all
     16 channels, for notes the engine doesn't know about (a plugin's own
     arpeggiator or latch);
   - the held pedals forgotten, so the next song's rows don't get a sustain
     that's no longer down.

   Sequences, loopers and auditions end the notes they're sounding the same
   way. **The transport, the click, sequences and loopers keep running**
   (decided 2026-10-03): a stuck note is no reason to lose the band's time, and
   a sequence's next note sounds as usual.
2. **Effect tails ring on** after one press: reverbs and delays fade as
   usual. **A second press within 2 seconds also cuts the sound** (decided
   2026-10-03): All Sound Off (CC 120) to every row, and a reset of every
   bus's effects (CLAP's `reset()`, which clears a reverb's or delay's
   memory), for runaway feedback or a plugin stuck making noise. Rows that
   ignore CC 120 are reset too.
3. **Rows and channels:** every row, whatever its input or filter (Panic goes
   straight to the plugins, not through the routing), in the row's protocol:
   MIDI 1.0 messages to MIDI 1.0 ports, their MIDI 2.0 form to MIDI 2.0
   ports, as today's translation does. A muted or draining row gets it too.
4. **Where:**
   - a **Panic** button in the toolbar, red text on the panel colour, beside
     Start/Stop;
   - a large Panic button in the Perform view, under the transport;
   - a **learned MIDI control** (a note or a controller press, learned like
     the looper switch, with the same ⋮/MIDI button), held in **the rig**
     (`"panic"` in rig.json): it's the performer's, the same in every song.
     Matched with the setlist's triggers, before the rack's; what matches
     goes nowhere else;
   - **no key** (decided 2026-10-03): a stray key shouldn't cut the sound.
5. **Seen:** the button lights red for a moment, and the status bar says
   "Panic: all notes off" (or "Panic: sound cut" after a second press) for a
   few seconds.

### In the engine

- `Engine::panic()` (main thread; any thread for the learned control's path,
  which is already on the audio thread): a flag the audio thread takes at the
  next block. Then, before the block's MIDI is routed: a note-off per entry
  in the held-note table, straight into the slot that holds that plugin; the
  table and the held pedals cleared; the sequences' and loopers' sounding
  notes ended (their tables cleared, as Stop does without stopping); the
  audition voices ended; the CCs to every slot, at offset 0, in order after
  the note-offs.
- The second press adds CC 120 and asks each bus effect and row to reset
  (`HostedPlugin::reset()`, a new call on the audio thread; for CLAP,
  `clap_plugin::reset`).
- Room: the CCs are 16 channels × 5 messages per row, well inside a slot's
  per-block event buffer. If the buffer is full anyway, the rest go at the
  next block (not dropped).

## CPU load and xruns

6. **DSP load:** the share of each period StageHand spends computing it:
   the time `Engine::process()` takes, divided by the period (frames / rate).
   It's StageHand's own, measured the same way on JACK and later CoreAudio,
   and it's what a heavier plugin or one more row changes. At 100% the
   period isn't finished in time, and that's an xrun.
7. **Shown in the status bar,** replacing "xruns N":
   **"DSP 23% · peak 41%"**: the average and the worst block of the last
   second. Dim under 70%, amber from 70%, red from 90% (the colour follows
   the peak: one late block is a click).
8. **Xruns:**
   - the count since StageHand started, as today, but **"xruns 3 · 12 s
     ago"**;
   - **red for 5 seconds** after a new one, then amber while the count isn't
     zero;
   - a click on it resets the count (after a soundcheck, say);
   - written to stderr with the song and the bar ("xrun: song 4 (So what),
     bar 17"), so a gig's xruns can be found later in the log.
9. **Per row:** each row shows its plugin's share of the period, small,
   beside its meter ("4%"), amber when one row takes more than half of the
   period (decided 2026-10-03). It's measured per slot per chunk (two clock
   reads per plugin call), and the bus effects get the same on their strips.
10. **In the Perform view:** the status bar, as in the edit view, so DSP and
    xruns are visible on stage.

### In the engine and the backend

- `Engine::process()` reads `nowNanoseconds()` at its start and end (the vDSO
  clock: no system call) and keeps, per block, `used / period`. Published:
  the peak since the main thread last took it, and a running average
  (exponential, about a second). Per slot, the same, kept in its runtime
  (`SlotRuntime`).
- `RackController` takes the peak each tick and keeps the worst of the last
  second for display, and the xrun count from the backend, with the time the
  count last changed.

## Milestones

1. **Panic, engine and button:** `Engine::panic()` (one press and two), the
   held tables, sequences, loopers and auditions; `HostedPlugin::reset()`;
   the toolbar and Perform view buttons, the status message. Tests with the
   fake plugin: the note-offs at the right note numbers (after a transpose),
   the CCs in each protocol, a muted row, the pedals not resent at a
   switch, sequences still running and sounding their next note, the second
   press's CC 120 and reset.
2. **Panic, learned control:** `"panic"` in rig.json, learned and cleared
   from a MIDI button beside Panic, matched on the audio thread. Tests: the
   rig file, the controller (learn, remove), the engine (it fires, and goes
   nowhere else).
3. **DSP load and xruns:** the engine's measure (whole and per slot), the
   status bar, the per-row figure, the xrun line and its log. Tests: the
   load published and its peak taken, the controller's last-second peak and
   xrun time (with a fake audio status).

**Milestone 1 implemented (2026-10-03):** `Engine::panic()` sets a flag the
audio thread takes at the next block's start (`takeMidi()`); `routeMidi()`
calls `Engine::silence()` before the first staged message or sequence event
at or after the Panic's frame, so a sequence's next note sounds as usual.
`silence()` sends a note-off (MIDI 2.0, translated by `deliver()`) for each
held-note entry to each plugin holding it, at the note it got; then, to every
row with a note port, muted and draining ones included, on all 16 channels of
group 1: CC 64, 66, 67 at 0 and CC 123 (MIDI 1.0, upconverted for
MIDI-2.0-only ports). It clears the held notes, the held pedals and the
audition voices, and `SequenceSet::forgetSounding()` makes sequences and
loopers forget their sounding notes, so no stray note-off follows. Within
`kPanicCutSeconds` (2 s, on the block clock) of the last Panic, CC 120 comes
first on each channel and every row's plugin and bus effect is reset
(`HostedPlugin::reset()`, a new audio-thread call, default no-op;
`ClapPlugin::reset()` calls CLAP's `reset()` while active). With no graph it
still counts. `Engine::panics()` publishes the count and whether the last cut
the sound. A slot's event buffer (1024 a block) holds a Panic's 64 to 80 CCs
with room to spare, so nothing is carried to the next block; if one is full,
the excess counts as dropped, as any message would. UI: **Panic** in the
toolbar between Start/Stop and the transport's MIDI button (red text on the
panel; red with dark text for 0.5 s after a Panic, as the controller sees
`panics()` change), and a large Panic **beside** the Perform view's ▶/■
(beside rather than under, so the current looper keeps its place on a short
window); the notice "Panic: all notes off" or "Panic: sound cut" in the
status bar. Tests: `testPanic` in the engine tests (note-offs at the
transposed number, the CCs in order in each protocol, a muted row, the pedal
not sent to a new row, the sequence's next note with no stray note-off, the
second Panic's CC 120 and resets, one after 2 s, no graph).

**Milestone 2 implemented (2026-10-03):** `Rig::panic`, `"panic"` in rig.json
written as `"looperSwitch"` is (rig.md). `Session::setPanicControl()` resolves
its input and compiles it into `TransportControl::panic` beside the looper
switch; the engine matches it after the setlist's triggers and the looper
switch, before the rack's (`SequenceSet::matchPanic()`: a note-on, or a
controller's rise to 64), and it goes nowhere else. The Panic placed at its
own frame. `RackController::panic()`, `learnPanic()`, `removePanic()`; it's
learned and removed from the transport's MIDI menu ("Learn Panic (all
songs)", "Remove Panic: …"), not from a MIDI button of its own: the toolbar
has no room for one, and the transport's button sits beside Panic. That
button lights for Panic's control too, and is amber when its input isn't
connected. Tests: the rig file (round trip, absent by default), the
controller (`testPanic`: the button, the light and notice, learning a
controller press, the second press cutting the sound, saved in the rig,
removed).

**Milestone 3 implemented (2026-10-03):** the engine times `process()` with
`nowNanoseconds()` and publishes the share of the period it took: a running
average (`kLoadAverageSeconds`, 1 s) and the worst block since the last
`Engine::takeDspLoad()`. Each slot's `process()` calls and each bus's effects
are timed per block into a `LoadMeter` in their `SlotRuntime` (`load`). The
controller shows the worst of each second for the next second
(`LoadWindow`), so the figures hold still: `StatusModel::dspPercent`,
`dspPeakPercent`, `RowModel::loadPercent`, `BusModel::loadPercent`. Xruns:
the count since start or since `RackController::resetXruns()` (a click on it
in the status bar), `xrunSecondsAgo`, `xrunRecent` (5 s); each new one is
written to stderr as "xrun: song 4 (So what), bar 17" (the rack's name
without a setlist, "stopped" when stopped). UI: the status bar reads "48000 Hz
1024 frames   DSP 23% · peak 41%   xruns 3 · 12 s ago   MIDI 2.0", the DSP
dim, amber from 70%, red from 90% (following the peak), the xruns red for 5 s
after one, amber while not zero. Each row shows "DSP 4%" under its Editor
button (amber over 50%), each bus strip after its name. Tests: `testDspLoad`
in the engine tests (a fake plugin busy 3 ms of a 10 ms period), the
controller's `testDspAndXruns` (the row's and the peak's percent after a
second, the xrun count, age, red, reset).

## Retuned keys

Added 2026-11-07 (ReleaseUMP R2-23). A MIDI 2.0 Pitch 7.25 (registered
per-note controller 3, as KeyboardEDO sends) retunes a key until it's reset,
so after a Panic the key's next note would still sound retuned. The engine
keeps, per row, a bit per group, channel and note for each key its plugin was
given a Pitch 7.25 on, after transpose and MIDI effects, as the plugin got it
(`SlotRuntime::retuned`, `Engine::trackRetuned()`); a Per-Note Management
with S set clears it. A Panic (`silence()`) asks each row with retuned keys
for resets at its frame; `runSlots()` adds them to the plugin's input after
its MIDI effects, after the chunk's events up to that frame (the Panic's
note-offs), as Per-Note Management with D and S set: a voice ringing out keeps
its pitch, the key's next note starts in tune (`Engine::addRetuneResets()`).
They go to the plugin directly, not through its MIDI effects, which would
remap them. At most `EngineGraph::kMaxRetuneResets` (128) a chunk; the rest
follow in the next chunks. MIDI 1.0 plugins can't be sent a Pitch 7.25, so
they get none. Test: `testPanicRetunedKeys` in the engine tests.

## Open questions

None at the moment (answered 2026-10-03, each as proposed: Panic keeps the
transport running; a second press within 2 seconds cuts the sound; no key;
the per-row load in milestone 3).
