# StageHand — Setlists and rack switching (design)

Proposed 2026-10-01; milestones 1 to 3 built (notes under "Proposed
milestones"). A setlist is an ordered list of racks
to step through on stage. It builds on decisions already recorded:
- domain-model.md, "Presets vs Racks": "A setlist is an ordered list of
  Racks. Leave room; don't build." This document is the build.
- rack-view.md, section 7: New / Open / Save As already switch racks, through
  `RackController::replaceRack()`.
- rack-view.md, `setMuted`: muting releases a slot's notes and suspends it
  once quiet. Draining an outgoing rack (section 3) reuses that machinery.
- sequences.md, section 4: "Later: ... launching a sequence with a rack
  switch."

## Domains: setlist, song, rack (and the app)

Each layer owns something different (decided 2026-10-01).

**Rack: the sound and the music of one piece.** A `.rack` bundle. It's
everything needed to play the piece, with or without a setlist.
- The rows: plugins and their state; each row's input (source, filter, key
  range, transpose), gain, mute and output pair.
- Sequences: their notes, settings, and the rows they play into.
- Time: tempo, time signature, transport triggers, At Start.
- The click: level and output.
- Its own unsaved changes.

**Song: one place in a setlist.** Not a file, and no settings of its own.
- Which rack: a path to a rack bundle.
- An optional display name, which overrides the rack's name in this setlist
  only ("Blue in green (ballad)").
- Its position in the set.
- Its status (missing, prepared, unsaved), computed, never saved.

Two songs can point to the same rack (a reprise), as can songs in different
setlists. Edits go to the rack, so every song using it changes.

**Setlist: the order of the set and how to move through it.** A `.setlist`
file.
- The songs, in order.
- The switch settings: the transport across a switch, when a switch happens,
  and whether the old rack rings out or is cut (section 5).
- Its MIDI triggers: Next, Previous, Program Change. They work in every song
  and win over a rack's own triggers.
- Which rig it uses.
- Nothing about the sound (no tempo, plugins or levels), so it can never
  contradict a rack.

**Rig: what every song shares** (added 2026-10-02; docs/rig.md). The 2
effect buses (their effects, return levels and outputs) and the master
level. Loaded once, kept across song switches. A setlist names its rig; a
rack opened on its own uses the default rig.

**App: this machine.** Saved in neither a rack nor a setlist.
- The audio device and its sample rate; which MIDI devices are connected.
- The last setlist and song, for reopening (section 1).
- How many racks to prepare ahead (section 2): it depends on this machine's
  memory, not on the set.

**The test for a new setting:** if it should travel with the piece into
another set, it's the rack's. If it's about the order of the set or the
moves between pieces, it's the setlist's. If it would change with a
different laptop, it's the app's. If every song shares it and it should stay
loaded through switches, it's the rig's. A song only ever adds a position and
a name.

## Where things stand

Opening a rack already switches racks, but it's built for setup, not for the
stage. `replaceRack()` calls `Session::load()`, which does everything at once
on the main thread:
- It saves the old rack (`saveForQuit()`).
- It stops the transport.
- It creates every plugin, loads its saved state (`loadState()`) and
  activates it.
- It commits the new graph.

Three problems make that unusable at a gig:

1. **Slow.** Creating and activating a plugin takes from milliseconds to
   seconds. The UI freezes for the whole load, and the old rack keeps playing
   until the new graph lands.
2. **Tails cut off.** At the swap, the old plugins leave the graph at once:
   reverb tails and held notes are chopped mid-sound.
3. **Saving gets in the way.** The save on switch costs a `saveState()` per
   plugin, and a failed write blocks the switch.

The design keeps the engine's commit / adopt / retire contract and adds three
things: a setlist, racks prepared ahead of time, and a switch that lets the
old rack ring out.

## 1. The setlist

- **Model:** `Session/Setlist.h` and `Session/SetlistFile.cpp`, alongside
  `Rack` and `RackFile`, with the same rules: a format name and version, and
  atomic writes.
- **Controller:** the current position, Next / Previous / go-to-song
  intents, and the editing intents below.
- **Command line:** `--setlist PATH` opens one. File → Open accepts either a
  `.rack` or a `.setlist`.
- **Reopening (decided 2026-10-01):** started with no arguments, StageHand
  reopens the last setlist at the song it was on. The setlist's path and the
  song are kept in `$XDG_STATE_HOME/stagehand/last.json` (default
  `~/.local/state`), written at each switch (a few bytes, atomic), so a crash
  reopens where it was too. If that setlist is gone, or a rack was opened on
  its own last, it starts as today (an untitled rack). A path on the command
  line wins.

### On disk

A setlist is one file that refers to racks; it doesn't contain them.

```
~/StageHand/
  Friday gig.setlist
  Sunday brunch.setlist
  Racks/
    Autumn leaves.rack/      rack.json, state/*.bin   (as today)
    Blue in green.rack/
    So what.rack/
```

```json
{
  "format": "stagehand-setlist", "version": 1,
  "songs": [
    { "rack": "Racks/Autumn leaves.rack" },
    { "rack": "Racks/Blue in green.rack", "name": "Blue in green (ballad)" },
    { "rack": "Racks/So what.rack" }
  ],
  "switching": { "transport": "keep", "at": "now", "oldRack": "ringOut" },
  "triggers": [ { "action": "next", "input": "FCB1010", "control": "cc",
                  "channel": 1, "number": 80 } ]
}
```

- **Paths are relative to the setlist file**, so copying the folder to
  another machine keeps everything working. A rack on another filesystem
  root is stored as an absolute path (`~` abbreviated, as in rack.json).
- **Shared racks:** a rack used in two setlists is the same rack, so a fix
  applies to both. A setlist that contained its racks would make copies that
  drift apart.
- **A song is a rack plus an optional `name`**, which overrides the rack's
  name in the setlist only. Everything else about a song (tempo, time
  signature, sequences, transport triggers) is the rack's. The setlist adds
  only how switching behaves (section 5) and its MIDI triggers (section 4).
- **A missing rack doesn't remove the song.** It stays in the list, marked
  missing, and Next skips it. This is the same rule as missing plugins: never
  silently drop something from a set.
- **Racks don't know they're in a setlist.** A `.rack` bundle is unchanged.
- **Not self-contained.** It can't be emailed as one file. Later, an "Export
  setlist..." command could copy the setlist and its racks into one folder,
  rewriting the paths.

### The setlist row

The toolbar is already full: from left to right it holds the rack name, the
transport (tempo, Tap, Start/Stop, MIDI, time signature, beat lights, click),
then "+ Add plugin" and File. So the setlist gets a row of its own, above the
toolbar, following the hierarchy: set, then song (rack), then the rack's
rows.

- **Only when a setlist is open.** A rack opened on its own looks as it does
  today.
- **Contents:** |◀ 3/12 *Song name* ▾ ▶|, then "next: So what" with a dot
  showing whether it's prepared (section 2). On the right: the setlist's
  name, and its MIDI button (learn, section 4). The arrows are large, for the
  stage, and drawn as skip shapes (|◀ ▶|) so they can't be taken for the
  transport's Start just below. An arrow with nowhere to go is greyed out.
- **No song playing:** after the playing song is removed from the set, the
  row shows "-/12" and "Not a song in this set: <rack>" in amber until a song
  is chosen.
- **The song list:** a click on the song name opens every song as a menu,
  each with its number, its prepared or missing state, and the current one
  marked. A click on a song goes there. The editing items are at the bottom
  (below).
- **The rack name:** in a setlist, the song name in the setlist row says
  which rack is loaded. The toolbar's name area then only needs the unsaved
  marker, which gives the transport some room.
- **Later, compacting the toolbar:** "+ Add plugin" could move into File, or
  below the last rack row. The click's settings could fold into the beat
  lights' menu.

### Making and editing a setlist

All of this is in the app; a setlist never has to be written by hand.

- **Starting one from a rack:** File → "New setlist with this rack...". A
  Save dialog (zenity / kdialog, as for racks) asks where the `.setlist`
  goes. The setlist opens with the current rack as song 1, and the setlist
  row appears. An untitled rack is saved first, because a song must refer to
  a rack on disk; that dialog says so ("First, save this rack: it becomes
  song 1"), so it isn't taken for the setlist's own name.
- **Starting an empty one:** File → "New setlist...". The row shows "No
  songs yet" until one is added.
- **In a setlist, a rack is a song in it** (decided 2026-10-01). File's New
  and Open add to the set rather than leave it, so a rack never escapes the
  set by accident:
  - File → "New rack in this set..." (Ctrl-N) asks where to save a new
    rack (the setlist's `Racks/` folder by default), adds it after the
    current song, and plays it. Decided 2026-10-02 (rig.md): the new rack
    starts as a copy of the current song's rack, not empty; with no song
    playing, it starts empty. Not built yet: today it starts empty. It's the way to build a set from scratch:
    add, set up, add the next.
  - File → "Add a rack to this set..." (Ctrl-O) adds an existing rack after
    the current song, without switching to it.
  - Save and Save As... work on the current song's rack as always (Save As
    points the song at the new bundle).
- **Changing songs** (the song list's menu, under "This song": the current
  song's own edits; reordering is Move up / Move down for now, drag and drop
  is deferred):
  - "Add this rack again", for a song that recurs in a set (a reprise),
    added after it.
  - "Move up" and "Move down".
  - "Rename in setlist..." (the inline text field, as for sequences). It
    sets the song's `name`; the rack keeps its own name.
  - "Remove from setlist", after a confirmation. The rack bundle stays on
    disk: removing a song never deletes a rack.
- **A missing song** (its rack moved or deleted) can't be played, but a
  click on it in the song list opens its own menu: "Find its rack..." points
  the song at a rack you choose, and "Remove from setlist..." removes it.
- **Saving the setlist:** setlist edits are saved at once (it's a small file,
  written atomically), so a setlist has no unsaved state of its own. A rack's
  own edits are saved as they are today (section 6 covers saving during a
  set).
- **A rack outside the set:** File → "New rack outside the set" (added
  2026-10-02) closes the setlist and starts an untitled rack in one step.
- **Closing it:** File → "Close setlist" leaves the set. It
  keeps the current rack open on its own, and the setlist row goes away.
  Those two are the only ways out.
  Opening another setlist replaces this one.

## 2. Preparing racks ahead of time

This removes the slow part from the switch.

- **Split `Session::load()`** into a prepare step and an activate step:
  - **Prepare** builds a `PreparedRack`: the parsed Rack, its plugins created
    with their saved state loaded and activated at the running sample rate,
    and its sequence entries built. Nothing is committed to the engine. A
    prepared plugin is activated but never processed, so it costs memory, not
    CPU.
  - **Activate** makes a prepared rack the Session's rack (section 3).
- **Incremental.** CLAP requires creating and activating a plugin on the main
  thread, so preparing does one plugin per UI tick. The UI stays responsive,
  and the status bar shows progress ("Preparing Song 4: 2/5 plugins").
- **What to keep ready:**
  - By default, the next and previous racks.
  - As an option, the whole setlist (MainStage's concert model): every switch
    is instant, at the cost of memory.
  - This is an app setting, not a setlist one (see "Domains"): it depends on
    the machine's memory, not on the set. It's saved with the app's other
    state in `$XDG_STATE_HOME/stagehand/` (or a config file, once the app
    has one).
- **Jumping to a rack that isn't ready** (going to entry 9 from entry 2, say)
  prepares it with progress shown. The current rack keeps playing until it's
  ready, then the switch happens. Nothing goes silent while waiting.
- **Invalidation:** if the audio backend restarts at a different sample rate,
  prepared racks are discarded and prepared again.
- **Not in v1:** reusing a plugin instance when the same plugin is in both
  racks. CLAP wants state loaded before activation, so reuse means deactivate,
  load state, then activate again. That's an optimisation, and a riskier one
  with misbehaving plugins.

## 3. The switch: instant, with the old rack ringing out

- **One commit.** The switch commits a single new graph: the new rack's
  slots, plus the old rack's slots marked as draining. It's an ordinary
  `commit()`, adopted at the next block, so the switch takes one audio period.
- **A draining slot:**
  - New note-ons don't reach it. Everything else from its inputs still
    does, as for a muted slot, so a sustain pedal released after the switch
    releases the old patch's notes too (see "Edge cases", 1).
  - Note-offs reach it through the held-note table, so a key held across
    the switch keeps sounding on the old patch until it's released. That's
    the "held notes carry over" behaviour players expect.
  - Its gain stays where it was, so tails ring at full level.
  - It finishes once its output has been quiet for a moment, using the
    silence detection that mute uses (`Engine::kQuietLevel`,
    `kQuietSeconds`), or after a cap of about 8 s.
  - The main thread sees the finished flag on its `SlotRuntime` and commits a
    graph without it. The plugin is then destroyed through the existing
    `collect()` path.

  This is close to mute: no new note-ons but the rest of the MIDI, and
  removal once quiet. The differences are that a draining slot keeps its gain
  and doesn't release held notes itself.
- **Setlist setting: cut on switch** (section 5). The old rack's slots are muted
  instead (its notes released, then faded), for when a song should end in
  silence.
- **Sequences:** the old rack's sequences stop at the switch with note-offs
  (`SequenceCommand::StopNow`). The new rack's sequences arrive in the same
  block's sequence snapshot.
- **Shared state stays open.** MIDI devices aren't reopened: the Session's
  `openSources` already outlives a rack. Nor is the click output, and nothing
  re-enumerates devices on stage.
- **Session shape:** the Session changes from owning the rack to owning the
  active rack plus a list of draining slots. Draining slots have no rack
  identity: just their plugin, runtime and filter.

## 4. Driving it from MIDI

- **Setlist triggers:** Next and Previous are actions like the transport's
  Start and Stop. They reuse `CompiledSequence::Trigger` and MIDI learn, so a
  pedal or a pad can step through the set.
- **Program Change:** a new trigger control. Program Change n on a chosen
  input and channel goes to entry n, or to the entry assigned that number.
  It's what most foot controllers send.
- **Setlist-level, not rack-level:** these triggers belong to the setlist, so
  they work the same in every song. Like today's triggers, they're matched in
  `Engine::takeMidi()` before routing and go no further.
- **The audio thread only requests.** It can't load or build a graph, so it
  posts "go to entry n" to the main thread, which commits the prepared graph.
  The delay is one UI tick plus one audio period: a few milliseconds when the
  rack is prepared.

## 5. What a switch applies, and the switch settings

**The rack owns the song.** Its tempo and time signature, its sequences
(including At Start), its transport triggers and its click settings all come
with it and apply at the switch. A setlist has no per-song options: a reprise
at another tempo is another rack. (Decided 2026-10-01.)

- **Tempo and time signature:** the new rack's apply at the switch.
- **At Start sequences:** if the transport is running after the switch, the
  new rack's At Start sequences start at the next bar, as if the transport had
  just started there. This is sequences.md's "launching a sequence with a rack
  switch".

**Switch settings** are about how switching behaves, so they belong to the
whole setlist (`"switching"` in the file, section 1), set from File →
"Setlist settings...":

| Setting | Values | Default |
|---|---|---|
| Transport across a switch | keep running (medleys), stop | keep running |
| When a switch happens | at once, at the next bar | at once |
| The old rack | ring out (section 3), cut | ring out |

"At the next bar" applies only while the transport runs; stopped, a switch is
always at once.

## 6. Saving while in a setlist

The setlist itself never needs saving: its edits are written at once
(section 1). Saving is about the racks.

- **Never save as part of a live switch.** The old rack's edits are kept in
  memory, and it's marked unsaved. Each plugin's state is captured at the
  switch with `saveState()` (quick, in memory), and the next rack loads at
  once. A set can therefore hold several unsaved racks.
- **Save (Ctrl-S) in a setlist saves every unsaved rack in the set**, not
  just the one showing. Saving only the current rack would quietly leave the
  others unsaved.
  - The unsaved marker counts them ("2 songs unsaved"), and the song list
    marks which songs they are.
  - Quit and File → "Close setlist" offer to save the same set of racks.
- **No background saving at first.** Unsaved racks are written at the next
  Save or at quit. Writing to disk on its own after a switch is possible
  later, but it means disk activity mid-gig, so it waits until explicit Save
  has been tried in practice.
- **Never block a switch** on a dialog or a failed write. A failure shows as a
  notice, and the rack stays unsaved.
- **A shared rack changes everywhere.** Saving "So what" changes it in every
  setlist that uses it. That's the point of sharing racks rather than
  copying them (section 1).
- **A rack used twice in a set** (a reprise) has one prepared copy per
  appearance (section 2). Editing it on song 3 leaves song 9's prepared copy
  with the old settings. So when an edited rack is saved, or switched away
  from, every other prepared appearance of it is prepared again from the
  edited rack. Song 9 then plays what was just set up.
- Today's `replaceRack()` keeps its save-first behaviour for Open and New,
  which aren't live actions.

## Edge cases

Settled early because they shape milestones 1 to 3 (items 1 to 6 touch the
data model or the engine); the rest can come with later milestones.

### Held notes and pedals across a switch

1. **A sustain pedal held through the switch.** If a draining slot heard only
   note-offs, the pedal-up would never reach it, and the old patch would ring
   until the 8 s cap. So a draining slot hears everything except note-ons, as
   a muted slot does (section 3).
2. **The new rack doesn't know the pedal is down.** Switch with the pedal
   held, and the new song's notes aren't sustained until the pedal is pressed
   again. The engine tracks each input's sustain, sostenuto and soft pedal
   (CC 64, 66 and 67) per channel, and sends the held ones to the new rack's
   slots at the switch.
3. **Rapid switching** (a pedal double-tap, ▶▶). Several racks draining at
   once is a CPU spike, so at most one rack drains: a further switch fades the
   older draining rack out quickly, through the mute path. A jump to a song
   that isn't prepared waits for it (section 2), and only the last request
   counts: pressing ▶ three times goes three songs on, once.

### Racks shared across songs and setlists

4. **Edits belong to the rack, not the song.** With a reprise, two
   appearances could otherwise hold two different unsaved versions of one
   rack. There's one in-memory copy per rack path, which every appearance
   shares (section 6 prepares the others again from it).
5. **Save As, or a rack moved, in a setlist.** Save As on song 3's rack
   points song 3 at the new bundle; otherwise the setlist would keep playing
   the old file. A rack moved or renamed outside the app leaves its song in
   the list, marked missing (section 1), never silently dropped.

### Settings that clash between rack and setlist

6. **MIDI mappings.** Each rack has its own transport triggers. If a rack
   maps a pedal to Start that the setlist maps to Next, the setlist wins: its
   triggers are matched first and go no further. A rack-level conflict is
   shown as a warning when the rack is prepared.
7. **The click** (level and output) is saved per rack, so it changes from
   song to song if racks differ. That follows the rule that the rack owns
   the song (section 5); a set that wants one click sets its racks alike.
8. **Switching in time.** With the transport running, a switch lands
   mid-bar, and tempo-synced delays and arpeggiators jump. The switch
   setting "at the next bar" (section 5) quantizes it like a sequence's
   launch, so medleys flow.

### Before the gig

9. **Check the set.** Problems (a missing plugin, an unplugged device, a
   trigger conflict) otherwise only appear on reaching the song. File →
   "Check setlist" prepares each song once and lists the problems, and the
   song list shows those songs in amber all evening.
10. **Heavy sample libraries.** Preparing the next song can mean a sampler
    loading gigabytes from disk while you play, competing with disk
    streaming and risking dropouts. Preparing the whole set at the start
    avoids it, at the cost of memory. Measure early with the heaviest
    plugins.
11. **Plugins that aren't idle while prepared.** Some run their own threads
    even when not processed. "Prepared costs only memory" (section 2) has to
    be checked against the plugins actually used.

### Stage failures

12. **The foot controller disconnects.** Its triggers can't switch songs
    until it's back. The setlist row's large ◀ ▶ buttons are the fallback,
    and an unplugged trigger device shows clearly in the row.
13. **Open windows at a switch.** Plugin editors, sequence editors and MIDI
    learn belong to the old rack. They close, or are cancelled, at the
    switch, as Open does today.

## Proposed milestones

1. **Setlist basics:** the file and model, the setlist row, `--setlist`,
   making and editing a setlist in the app (section 1, with Save As
   re-pointing the song: edge case 5), and Next / Previous through today's
   `replaceRack()`. Slow, but it works end to end and is
   enough for rehearsal. Tests: the setlist file round trip (relative paths,
   a missing rack kept), and the controller's position and editing logic.
2. **Prepared racks:** split `Session::load()`, prepare incrementally, keep
   the next and previous racks ready, and switch with one commit (no tails
   yet). One in-memory copy per rack path (edge case 4), and windows closed
   at the switch (13). Tests: preparing across several ticks, then a switch that commits
   once.
3. **Draining:** the old rack's slots ring out and are then removed, with
   pedal state sent to the new rack and at most one rack draining (edge
   cases 1 to 3). Engine tests with the fake plugin, like the mute tests:
   note-offs and the pedal follow held keys, no new note-ons reach it, it's
   removed once quiet, and a quick second switch fades the first rack out.
4. **MIDI control:** setlist triggers, Program Change, and learn, with the
   setlist's triggers taking precedence over a rack's (edge case 6).
5. **Switch settings:** the rack's tempo and At Start sequences applied at
   the switch, and the setlist's three switch settings (section 5; edge case
   8).
6. **Saving:** edits kept in memory across switches, Save writes every
   unsaved rack in the set, and a reprise's other prepared copies are
   prepared again. Tests: edit, switch, edit, then one Save writes both racks;
   a reprise picks up the edit.

7. **Check setlist,** and showing problems in the song list (edge case 9).

Milestones 2 and 3 are what make it safe for a gig.

**Milestone 1 implemented (2026-10-01):** the model and file
(`Session/Setlist.h`, `Session/SetlistFile.cpp`), the controller's setlist
state and intents, `--setlist` and reopening the last setlist (playing is now
the default mode), the setlist row with Previous / Next and the song list, its
editing items, File's New and Open adding to the set while one is open, and
File > New setlist with this rack..., New setlist...,
Open setlist... (a file chooser showing `*.setlist`) and Close setlist.
Switching asks first if an untitled rack's changes would be lost, as New and
Open do. Tests: `tests/SetlistFileTests.cpp`, `testSetlist` in
`tests/RackControllerTests.cpp`, and the file chooser's arguments.

**Milestone 2 implemented (2026-10-02):** `Session::load()` is now
`prepare()` (a `PreparedRack`: the rack and its sequence entries),
`prepareStep()` (one plugin created, restored and activated) and `activate()`
(sources and triggers resolved, one graph commit with the rig's buses, so the
rig's effects stay loaded). `RackController` keeps the next and previous
songs' racks prepared, one plugin per tick, keyed by rack bundle so a reprise
shares one copy (edge case 4); Next and Previous then switch at once, closing
editors, MIDI learn and sequence windows (13). The rack left is prepared again
from its in-memory capture, edits included. A jump to a rack that isn't ready
waits for it while the current one plays (the last request wins); going to
another appearance of the playing rack only moves the position. Opening a
setlist, Open and New still prepare at once, and a switch still saves first
(until milestone 6). The status bar shows the progress, and the setlist row a
dot when the next song is prepared. Prepared racks are dropped if the sample
rate changes. Tests: `testPrepare` (Session), `testPreparedRacks`
(controller), with `Engine::graphCommits()` counting the commits.

**Milestone 3 implemented (2026-10-02):** `Session::activate()` keeps the old
rack's slots in the switch's graph as draining ones (`SlotConfig::draining`;
the Session's `drainingSlots()`, with no rack identity). The engine sends a
draining slot everything but note-ons (a held key's note-off still comes
through the held-note table), keeps its gain and its sends to the rig, and
sets `SlotRuntime::drained` once its output has been quiet for
`kQuietSeconds`, or after `Engine::kMaxDrainSeconds` (8 s); `Session::flush()`
then commits a graph without it. A further switch mutes the rack still
draining, so at most one rings out. The engine tracks the sustain, sostenuto
and soft pedals held on each live input, and sends them to every slot new in
an adopted graph, before anything else in that block. The old rack's
sequences end with note-offs when the new sequence set is adopted. Open and
New ring out too. `OldRack::Cut` (mute instead of draining) is there for the
switch settings; the controller uses ring out until milestone 5. Tests:
`testDraining` in the engine and Session tests.

## Open questions

None at the moment. Decided 2026-10-01: editing stays in the song list's
menu (drag and drop deferred); StageHand reopens the last setlist at its song;
there are no per-song options (the rack owns the song, section 5).

## Later

- **Perform view:** a mode of the main window (not another window: on stage
  it's one screen) with the song name large, the next song, the beat lights,
  and big ◀ ▶ buttons.
- **Drag and drop** reordering, in a separate "Edit setlist" panel, so the
  stage menu can't reorder by accident.
- **Export setlist...** into one self-contained folder (section 1).
- **Background saving** of racks after a switch (section 6).
