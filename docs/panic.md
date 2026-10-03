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
   (open question 1): a stuck note is no reason to lose the band's time, and
   a sequence's next note sounds as usual.
2. **Effect tails ring on** after one press: reverbs and delays fade as
   usual. **A second press within 2 seconds also cuts the sound** (open
   question 2): All Sound Off (CC 120) to every row, and a reset of every
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
   - **no key** (open question 3): a stray key shouldn't cut the sound.
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
   period (open question 4). It's measured per slot per chunk (two clock
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

## Milestones (proposed)

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

## Open questions

1. **Does Panic stop the transport?** Proposed: no; it ends notes, and
   sequences, loopers and the click keep going. The alternative is a full
   stop as well (Stop, then the notes off).
2. **Cutting tails:** proposed as a second press within 2 seconds. The
   alternatives: always cut (one press, harder), or never (notes only).
3. **A key for Panic?** Proposed: none. Escape is the obvious one, but it
   also closes menus, and a stray press would cut notes mid-song.
4. **Per-row load:** proposed for milestone 3. It finds the plugin that's
   too heavy, at two clock reads per plugin call. It could wait for later.
