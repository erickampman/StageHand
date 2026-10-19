<div style="display: flex; align-items: center; gap: 0.75rem; margin: 1.5rem 0 0.5rem;">
  <a href="https://www.unlikelyware.com" target="_blank" rel="noopener noreferrer">
    <img src="Unlikelyware.png" alt="Unlikelyware" style="width: 56px; height: auto; border-radius: 10px; margin: 0; display: block;">
  </a>
  <h1 style="margin: 0;">StageHand</h1>
</div>

<p>
  <em>StageHand runs on macOS and Linux. Shortcuts are written with Cmd, as on the Mac; on Linux, use Ctrl. Where the Mac says Option, Linux says Alt.</em>
</p>

~~~note [card]
## On This Page
[Overview](#overview) · [Getting Started](#getting-started) · [Nomenclature](#nomenclature) · [The Window](#window) · [Files and Saving](#files)

**The rack —** [Rack Rows](#rows) · [Adding Plugins](#adding) · [Inputs and Filters](#inputs) · [Levels and Outputs](#levels) · [The Rig's Buses](#buses)

**Time and sources —** [Transport and Click](#transport) · [Count-in](#count-in) · [The Sources Dock](#dock) · [Sequences](#sequences) · [Loopers](#loopers)

**On stage —** [Setlists](#setlists) · [The Perform View](#perform) · [Control Links](#control-links) · [Panic](#panic) · [DSP and Xruns](#dsp) · [Shortcuts](#shortcuts) · [Troubleshooting](#troubleshooting)
~~~
~~~note [card]
(#overview)
## Overview
StageHand is a **live-performance host for CLAP plugins**. It holds a set of instruments ready to play from your keyboards and controllers, moves through the songs of a set without gaps, and keeps the band in time with a click, sequences and loopers.

**MIDI 1.0 and MIDI 2.0 are both first-class.** A MIDI 2.0 keyboard reaches a MIDI 2.0 plugin with its full resolution and per-note expression; MIDI 1.0 gear and plugins work as they always have, with StageHand translating between the two where they meet.

It's built for the stage first: large, high-contrast controls on a dark window; faders that move only when dragged, so a stray click changes nothing; song switches that let the last song's reverb ring out; and a Perform view with nothing on it to knock by accident.
~~~
~~~note [card]
(#getting-started)
## Getting Started
1. **Add an instrument.** Right-click the empty space below the rows (or click the large **+** there) and choose an instrument. **+ Add plugin** in the toolbar lists every plugin.
2. **Choose what plays it.** The menu at the left of the row is its input: **All inputs**, one keyboard or controller, or StageHand's own inputs for other apps. Play, and the light beside it flickers.
3. **Set its level** with the fader on the right. Open the plugin's own window with **Editor**.
4. **Save** with Cmd-S. A rack is saved as a `.rack` bundle, with every plugin's settings in it.
5. **For a whole set,** choose File → **New setlist with this rack...**, then add a song for each piece (see [Setlists](#setlists)).

Started with no arguments, StageHand reopens the last setlist at the song it was on, so after a crash or a restart you're back where you were.
~~~
~~~note [card]
(#nomenclature)
## Nomenclature
- **Rack** — Everything one piece of music needs: its rows, sequences, tempo, time signature and click. Saved as a `.rack` bundle (a folder that Finder shows as one file).
- **Row** — One plugin in a rack, with its input, filter, level, sends and output.
- **Rig** — What every song shares: the two effect buses and the master level. It stays loaded through song switches, so effects never reload and their tails ring on.
- **Setlist** — The order of a set's songs and how to move between them. Saved as a `.setlist` file.
- **Song** — One place in a setlist. It points to a rack and can give it a name of its own for that set.
- **Sequence** — A short loop of notes that plays into one or more rows, in time with the transport.
- **Looper** — Records what you play into rows and loops it back, driven by one footswitch.
- **Control Link** — A hardware knob, fader or pedal tied to a plugin parameter or a level.
~~~
~~~note [card]
(#window)
## The Window
**The toolbar, left to right:**
- **Unlikelyware logo**: opens unlikelyware.com.
- **The rack's name**, with "unsaved changes" under it when there are some.
- **Tempo**: drag up or down to change it (Shift for fine steps), use the wheel, or double-click for 120.
- **Tap**: tap the tempo in.
- **Start/Stop** (▶ / ■): the transport. Green while running.
- **Panic**: all notes off ([Panic](#panic)).
- **MIDI**: the transport's learned triggers, start on first note, and Panic's control.
- **Time signature**: 2/4 to 12/8.
- **Beat lights**: one per pulse of the bar; the downbeat in amber.
- **⋮**: what the lights count, the click, the count-in and the edge flash ([Transport and Click](#transport)).
- **Perform**, **+ Add plugin** and **File**.

**Below the toolbar:** the rack's rows, then the rig's two buses, then the **Sources** dock (inputs, sequences and loopers), and the status bar at the bottom. With a setlist open, the setlist's row sits above the toolbar.

**The status bar** shows the sample rate and buffer size, the DSP load, xruns, the MIDI protocol, where the rack saves (or the latest notice), and the master level.

Menus and panels open inside the window. A click outside one, or Escape, closes it.
~~~
~~~note [card]
(#files)
## Files and Saving
The **File** menu has New rack, Open..., Save and Save As..., and the setlist items.
- **A titled rack saves itself** when you quit or switch to another rack. An untitled one asks first: Save As..., Discard, or Cancel.
- **Cmd-S saves everything with changes**: the current rack, any other unsaved racks in the set, and the rig.
- **Save As** adds `.rack` if you leave it out, and asks before replacing an existing rack.
- **Open** takes a `.rack` bundle or a `.setlist` file. One that doesn't load leaves the running rack alone.

Started with `--no-save-rack`, StageHand never saves: Save is disabled and switching racks discards changes.
~~~
~~~note [card]
(#rows)
## Rack Rows
Each row is one plugin, in three parts:
- **Input** (left): the input menu, a MIDI 1.0 / 2.0 badge, the key range bar, and the filter chip ([Inputs and Filters](#inputs)).
- **Plugin** (middle): the row's name, the plugin and its maker, its **Control Links** button (the MIDI plug), **Editor**, and its share of the DSP load.
- **Output** (right): **M** (mute), the two sends (**A** and **B**), the fader over a stereo meter, the clip light, the level, and the output pair ([Levels and Outputs](#levels)).

Each row has its own colour, as a stripe down its left edge and a tint across it. A sequence or looper that plays into a row takes the row's colour, so you can see what plays what.

**Removing a row:** the red X at its right end asks first. **Shift-click** the X to remove it at once.

A row whose plugin can't be found is kept as a placeholder, with the reason, so the rack still opens. Install the plugin and open the rack again.
~~~
~~~note [card]
(#adding)
## Adding Plugins
StageHand finds CLAP plugins in the usual places: `~/Library/Audio/Plug-Ins/CLAP` and `/Library/Audio/Plug-Ins/CLAP` on the Mac, `~/.clap` and `/usr/lib/clap` on Linux, and anywhere `CLAP_PATH` names.
- **+ Add plugin** lists the instruments, then other plugins (note effects), then the audio effects, greyed: those go on the rig's buses, not in rows.
- **Right-click below the rows**, or click the **+** there, for the instruments alone.

The new row goes at the bottom. The first scan can take a moment; after that the list opens at once.

On Apple Silicon, a plugin built only for Intel can't be loaded: the scan skips it and says it's Intel only.
~~~
~~~note [card]
(#inputs)
## Inputs and Filters
**The input menu** chooses what a row hears:
- **All inputs**: every keyboard and controller StageHand opened.
- **One device**, by name, with its protocol.
- **From other apps (MIDI 2.0)** and **From other apps (MIDI 1.0)**: StageHand's own inputs, which other apps (an on-screen keyboard, a DAW) can send to. On Linux they're `StageHand MIDI 2.0` and `StageHand` in the ALSA sequencer.
- **No live input**: for a row that only plays the sequences and loopers aimed at it.

A device that's unplugged shows in red, and the row hears it again when it's back.

**The key range bar** under the menu is the row's range of notes. Drag an end to move it, drag the middle to move the whole range, or click outside it to stretch the nearer end. Two rows with ranges side by side make a split; overlapping ranges make a layer.

**The filter chip** opens the filter panel: channels, groups (MIDI 2.0), the key and velocity ranges, and transpose, with Reset. Steppers take Shift for an octave or 10, and the wheel works too.

**An amber "!"** beside the badge means a MIDI 2.0 input feeds a MIDI 1.0 plugin, so per-note data is lost on the way.
~~~
~~~note [card]
(#levels)
## Levels and Outputs
- **The fader** moves with your drag, not to where you click. **Shift** is finer, **double-click** returns to 0 dB, and the wheel moves half a dB. It stops at 0 dB on the way past.
- **M** mutes the row: its notes are released, new notes don't reach it, and once it's quiet the plugin rests, costing no DSP. A muted row is dimmed, except its M.
- **The sends, A and B,** feed the rig's buses, after the fader. Drag up or down, use the wheel, or double-click to turn one on (0 dB) or off.
- **The clip light** latches red when the row's output clips. Click it to clear.
- **The output pair** ("out 1-2") is a menu when there are more than two outputs.
~~~
~~~note [card]
(#buses)
## The Rig's Buses
The rig has **two effect buses**, A and B, between the rows and the dock. Each has two effect slots in series (a reverb, then an EQ, say), a return fader with its meter, **M**, and its output pair. Either slot can be empty.
- Choose an effect from a slot's menu; **Edit** opens its window.
- Rows feed a bus with their **send** of that letter.
- The **master** fader, in the status bar, sets the level of everything on the main outputs.

The rig is the same in every song, so a song switch never reloads its effects. A setlist names its rig; a rack opened on its own uses the default rig.
~~~
~~~note [card]
(#transport)
## Transport and Click
**Stopped**, the transport is at bar 1 and plugins see it stopped. **Running**, bar 1 beat 1 is the moment you pressed Start. Start and Stop from the toolbar, with **Space**, or from learned MIDI triggers.

**The MIDI button** beside Panic holds the transport's triggers (Learn Start/Stop, Learn Start, Learn Stop) and **Start on first note**: while stopped, the first note from the chosen input starts the transport, with beat 1 on that note.

**The ⋮ menu** after the beat lights:
- **What the lights count**: the beats (6/8 counts two, 7/8 seven), or every note.
- **The click**: off, or a level from -24 to 0 dB, and its output. The click has its own output, never mixed into the mains, so it can go to your in-ear mix while the PA gets the band. **Test the click** plays one blip, to check where it's heard.
- **The count-in** ([Count-in](#count-in)).
- **Flash the window's edge on each beat**: the whole border flashes, brighter on the downbeat, readable from across the stage.

The ⋮ turns amber when the click is on but its output isn't there, and the menu says why it's silent.
~~~
~~~note [card]
(#count-in)
## Count-in
Each song can count in before bar 1: **off, half a bar, 1 bar or 2 bars**, chosen in the ⋮ menu and saved with the rack.
- Start begins the count-in; **bar 1 lands right after it**, with everything that starts with the song.
- The count-in always clicks, even in a song whose click is off.
- The tempo shows "Count-in 2 … 1", and the beat lights turn amber.
- No count-in when the music is already going: a start on first note, or a song switch with the transport running.
- Stop during the count-in cancels it.
~~~
~~~note [card]
(#dock)
## The Sources Dock
The dock at the bottom holds what makes notes besides your hands:
- **Inputs**: one chip per open MIDI input, lit as its messages arrive. A device that left is amber until it's back.
- **+ Sequence**, **+ Looper** and **Stop all** (which stops every sequence).
- **A row per sequence and per looper.**

The dock grows with its rows, up to a little over half the window, then scrolls.
~~~
~~~note [card]
(#sequences)
## Sequences
A sequence is a loop of notes that plays into rows you choose, on top of their live input.
- **+ Sequence** asks for **MIDI 2.0 (EDO pitches)** or **MIDI 1.0 (note numbers)**, then where it plays: an existing row, a new row, or none yet. Then type its name.
- **Its row:** Play/Stop, its name (click to rename), where it plays (click to choose rows), **Edit**, a mini-map of its notes, **At Start**, **Rate**, its length, **launch** (the next bar, the next beat, or at once), **Repeat**, and its MIDI triggers.
- **Play/Stop** waits for the launch point and blinks until then. Started while the transport is stopped, it starts the transport too, on beat 1.
- **At Start** sequences begin with the transport, however it's started: that's how a song's parts start together.
- **The MIDI button** learns a pad or pedal to start, stop or toggle it. One pad can drive several sequences.

Removing one asks first; **Shift-click** its X to remove it at once.
~~~
~~~disclosure The sequence editor [disclosure-title subtle]
**Edit** (or a click on the mini-map) opens the sequence's own window. The strip on the left sets the protocol (while it has no notes), EDO and the reference note, rate, length, launch, Repeat, the note-off offset, and channel and group. The grid has one row per pitch, with the 12-tone black keys shaded so the familiar pattern shows at any EDO.

| Gesture | Where | Result |
|---|---|---|
| Click | empty cell | A new note |
| Click and drag right | empty cell | A new note, as long as the drag |
| Drag | a note | Move it (Shift locks to one direction) |
| Drag | a note's right edge | Change its length |
| Option-drag | a note | Copy it |
| Cmd-drag up or down | a note | Velocity |
| Right-click or double-click | a note | Delete it |
| Shift-drag | empty area | Select a rectangle of notes |
| Click, Shift-click | a note | Select it, add it to the selection |
| Delete, arrows | a selection | Delete, move by a unit or a step (Shift: a beat or an octave) |
| Cmd-Z, Shift-Cmd-Z | | Undo, redo |

Notes sound as you add, click and move them, through the rows the sequence plays into. **Space** plays or stops it. The wheel scrolls, Shift-wheel scrolls sideways, and Cmd-wheel zooms.
~~~
~~~note [card]
(#loopers)
## Loopers
A looper records the notes you play from one input into its rows, and loops them, in time.
- **+ Looper** asks what it records (an input) and which row it plays into, then its name.
- **Its row:** the record/play button, its name, **Undo** (the last pass), **Clear**, **Keep** (makes it a sequence, so it's saved), **Input**, **Q** (quantize), start at the next **Bar** or **Beat**, its length (fixed, or ended by a press), and **Clean**, which skips the short wrong note a finger catches beside the right one.
- **Click a looper's row** to make it the current one: the one the footswitch drives.
- **What a looper records isn't saved**: each performance starts fresh. Keep it as a sequence to save it.
- Its red X removes it at once, without asking (nothing recorded is lost that would have been saved).
~~~
~~~disclosure The looper footswitch [disclosure-title subtle]
One footswitch drives the current looper. Learn it from the looper row's MIDI button. A momentary switch (one that sends press and release) is needed for overdubs; a latching pedal gives taps only.

| State | Tap | Hold |
|---|---|---|
| Empty | arm: recording starts at the next beat or bar | |
| Armed | cancel | |
| Recording | end it (rounded up to the beat or bar) and play | |
| Playing | stop, at the next beat or bar | overdub while held |
| Stopped | play from the start, at the next beat or bar | |

A note played a little ahead of the downbeat is recorded on it. Stopping the transport stops every looper.
~~~
~~~note [card]
(#setlists)
## Setlists
A setlist puts racks in order, as songs, and moves between them without a gap.
- **Making one:** File → **New setlist with this rack...** (the rack becomes song 1) or **New setlist...** (empty).
- **The setlist row** above the toolbar: **|◀** and **▶|** for the previous and next song, the song's number and name (a menu of every song), "next: …" with a dot that's green once it's ready, the setlist's name, and its MIDI button.
- **In a setlist, File's New and Open add to the set:** **New rack in this set...** (Cmd-N) starts a copy of the current song's rack, ready to change; **Add a rack to this set...** (Cmd-O) adds one you have.
- **The song menu** also has, for the current song: Add this rack again (a reprise), Move up, Move down, Rename in setlist..., and Remove from setlist. Removing a song never deletes its rack.
- **Setlist edits are saved at once.**
- **Setlist settings...** in the File menu chooses whether the transport keeps running across a switch, whether a switch waits for the next bar, and whether the old song rings out or is cut.
- **Close setlist** leaves the set, keeping the current rack open.

**Switches are instant.** The next and previous songs are prepared ahead, and the old song's reverb tails and held notes ring out. A note held across a switch keeps sounding until you let go.

**From MIDI:** the setlist's MIDI button learns Next, Previous and Program Change, so a foot controller can step through the set. They work in every song.
~~~
~~~note [card]
(#perform)
## The Perform View
**Perform** in the toolbar, or **Tab**, turns the window into what you want to see on stage, and Tab brings the rack back. Switching views never changes the sound.

It shows, large: the song and what's next, with big ◀ ▶ buttons; the beat, one box per pulse with the bar and beat in big digits; the count-in, very large, in amber; Start/Stop; the current looper; a large **Panic**; and the status bar. It hides the rows, the dock and everything that edits.

**Keys:** ← and → for the previous and next song, Space to start or stop, Tab to go back. StageHand reopens in the view it was left in.
~~~
~~~note [card]
(#control-links)
## Control Links
A Control Link ties a hardware control (a knob, a fader, an expression pedal) to a plugin parameter, a row's level, a send, a bus return or the master. One control with several targets is a macro.
- **Each row's Control Links button** (the MIDI plug beside Editor) opens its menu: learn a link from the list of the plugin's parameters (type to filter it), or learn the row's gain or sends; then move the hardware control. The status bar says what to do at each step.
- **Bus effects** have the same in their menus; **the bus return and master faders** take a right-click.
- **Each link** has, per target, a range (either way round) and a curve, and is **pick-up** (it waits until the control reaches the parameter, so the sound never leaps) or **jump**. Choose a link in the menu to change it in the Control Link panel.
- **Scope:** **This song** (saved in the rack) or **All songs** (saved in the rig: the mod wheel can drive a plugin's tremolo in every song that has that plugin).
- A control used by a link goes nowhere else in that song's rows, unless the link passes it on.

A level link works like a volume pedal after the fader: the fader doesn't move, and the two multiply.
~~~
~~~note [card]
(#panic)
## Panic
**Panic** in the toolbar (and in the Perform view) stops stuck notes in every row: a note-off for every note StageHand knows is sounding, the pedals up, and All Notes Off on every channel.
- **The transport, the click, sequences and loopers keep running**: a stuck note is no reason to lose the band's time.
- **Effect tails ring on.** **Press again within 2 seconds** to cut the sound too (All Sound Off, and the buses' effects reset), for runaway feedback or a plugin stuck making noise.
- **Learn a MIDI control** for it from the transport's MIDI button (Learn Panic). It's kept in the rig, the same in every song. There's no key for it, so a stray keypress can't cut the sound.
~~~
~~~note [card]
(#dsp)
## DSP and Xruns
- **"DSP 23% · peak 41%"** in the status bar is the share of each audio period StageHand spends: the average and the worst block of the last second. It turns amber from 70% and red from 90%. At 100% the audio is late, which is an xrun.
- **Each row and bus** shows its own share, amber when one takes more than half the period.
- **Xruns** are counted since StageHand started, red for a few seconds after a new one. Click the count to reset it (after a soundcheck, say).

When the load runs high: mute rows you don't need in a song (a muted plugin rests), choose a larger buffer, or use lighter plugin settings.
~~~
~~~note [card]
(#shortcuts)
## Shortcuts
**In the rack window:**

| Key | Action |
|---|---|
| Space | Start or stop the transport |
| Tab | The Perform view, and back |
| Cmd-S | Save everything with changes (Save As for an untitled rack) |
| Cmd-N | New rack (in a setlist: a new rack in the set) |
| Cmd-O | Open a rack (in a setlist: add a rack to the set) |
| Cmd-Q | Quit |
| Escape | Close a menu or panel; stop MIDI learn |

**In the Perform view:** ← → previous and next song, Space start or stop, Tab back.

**In plugin editor windows:** Cmd-Q quits and Shift-Cmd-S saves the rack.

**With the mouse:** Shift for finer drags and bigger steps; double-click a fader for 0 dB; Shift-click a red X to remove without asking; right-click below the rows to add an instrument.
~~~
~~~note [card]
(#troubleshooting)
## Troubleshooting
- **No sound:** check the row's meter moves when you play. If it doesn't, check its input menu and the light beside it, its key range and filter, and M. If the meter moves, check the output pair and the master.
- **An app's MIDI doesn't arrive:** send it to **From other apps (MIDI 2.0)** or **(MIDI 1.0)**, and choose that input in the row (or All inputs). From the command line, `--midi all` opens software and app sources too.
- **The click is silent:** open the ⋮ menu; it says why (no output chosen, the output isn't connected, or the transport isn't running).
- **A plugin is missing** (a placeholder row): install it, and open the rack again. Its settings were kept.
- **Clicks and dropouts:** watch DSP and xruns in the status bar ([DSP and Xruns](#dsp)).
- **A stuck note:** [Panic](#panic).
- **The command line:** `stagehand --help` lists every option: `--list-plugins`, `--list-midi`, `--list-audio` (macOS), `--tone` to check the audio path, and `--monitor` to see incoming MIDI.
~~~
