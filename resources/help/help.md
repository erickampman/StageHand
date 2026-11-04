<div style="display: flex; align-items: center; gap: 0.75rem; margin: 1.5rem 0 0.5rem;">
  <a href="https://www.unlikelyware.com" target="_blank" rel="noopener noreferrer">
    <img src="Unlikelyware.png" alt="Unlikelyware" style="width: 56px; height: auto; border-radius: 10px; margin: 0; display: block;">
  </a>
  <h1 style="margin: 0;">StageHand</h1>
</div>

<p>
  <em>StageHand runs on macOS and Linux. Shortcuts are written with Cmd, as on the Mac; on Linux, use Ctrl. Where the Mac says Option, Linux says Alt.</em>
</p>

::: {.callout .callout-note .card}
## On This Page
[Overview](#overview) · [Getting Started](#getting-started) · [Nomenclature](#nomenclature) · [The Window](#window) · [Files and Saving](#files)

**The rack —** [Rack Rows](#rows) · [Adding Plugins](#adding) · [Inputs and Filters](#inputs) · [MIDI Effects](#midifx) · [Levels and Outputs](#levels) · [The Rig's Buses](#buses)

**Time and sources —** [Transport and Click](#transport) · [Count-in](#count-in) · [The Sources Dock](#dock) · [Sequences](#sequences) · [Loopers](#loopers)

**On stage —** [The Set and Its Songs](#setlists) · [The Perform View](#perform) · [Control Links](#control-links) · [Panic](#panic) · [DSP and Xruns](#dsp) · [Shortcuts](#shortcuts) · [Troubleshooting](#troubleshooting)
:::

::: {.callout .callout-note .card}
## Overview {#overview}
StageHand is a **live-performance host for CLAP plugins**. It holds a set of instruments ready to play from your keyboards and controllers, moves through the songs of a set without gaps, and keeps the band in time with a click, sequences and loopers.

**MIDI 1.0 and MIDI 2.0 are both first-class.** A MIDI 2.0 keyboard reaches a MIDI 2.0 plugin with its full resolution and per-note expression; MIDI 1.0 gear and plugins work as they always have, with StageHand translating between the two where they meet.

It's built for the stage first: large, high-contrast controls on a dark window; faders that move only when dragged, so a stray click changes nothing; song switches that let the last song's reverb ring out; and a Perform view with nothing on it to knock by accident.
:::

::: {.callout .callout-note .card}
## Getting Started {#getting-started}
1. **Add an instrument.** Right-click the empty space below the rows (or click the large **+** there) and choose an instrument. **+ Add plugin** in the toolbar lists every plugin.
2. **Choose what plays it.** The menu at the left of the row is its input: **All inputs**, one keyboard or controller, or StageHand's own inputs for other apps. Play, and the light beside it flickers.
3. **Set its level** with the fader on the right. Open the plugin's own window with **Editor**.
4. **Save** with Cmd-S. The first time, you name the set: it's saved as a `.setlist` package, with every song's rack and every plugin's settings in it.
5. **For more songs,** choose File → **New song (a copy of this one)**, or **Import a rack...** for one you made before (see [The Set and Its Songs](#setlists)).

Started with no arguments, StageHand reopens the last set at the song it was on, so after a crash or a restart you're back where you were. With no set saved before (or an untitled one last), it starts a new, untitled set of one empty song.
:::

::: {.callout .callout-note .card}
## Nomenclature {#nomenclature}
- **Set** (or setlist) — The one document StageHand opens and saves: its songs in order, each song's rack, the rig, and how to move between songs. Saved as a `.setlist` package (a folder that Finder shows as one file; on Linux, a folder).
- **Song** — One place in a set. It plays a rack of the set's own, and can give it a name of its own.
- **Rack** — Everything one song needs: its rows, sequences, tempo, time signature and click. It lives inside its set.
- **Row** — One plugin in a rack, with its input, filter, level, sends and output.
- **Rig** — What every song in a set shares: the two effect buses and the master level. It stays loaded through song switches, so effects never reload and their tails ring on.
- **Sequence** — A short loop of notes that plays into one or more rows, in time with the transport.
- **Looper** — Records what you play into rows and loops it back, driven by one footswitch.
- **Control Link** — A hardware knob, fader or pedal tied to a plugin parameter or a level.
:::

::: {.callout .callout-note .card}
## The Window {#window}
**At the top is the set's row** ([The Set and Its Songs](#setlists)): the **Unlikelyware logo** (it opens unlikelyware.com), the songs, the set's name, its MIDI button, and **?** for this page.

**The toolbar, below it, left to right:**
- **The rack's name**, with what Cmd-S would save under it: "unsaved changes", "3 songs unsaved", "unsaved rig changes" or "unsaved set changes".
- **Tempo**: drag up or down to change it (Shift for fine steps), use the wheel, or double-click for 120.
- **Tap**: tap the tempo in.
- **Start/Stop** (▶ / ■): the transport. Green while running.
- **Panic**: all notes off ([Panic](#panic)).
- **MIDI**: the transport's learned triggers, start on first note, and Panic's control.
- **Time signature**: 2/4 to 12/8.
- **Beat lights**: one per pulse of the bar; the downbeat in amber.
- **⋮**: what the lights count, the click, the count-in and the edge flash ([Transport and Click](#transport)).
- **Perform**, **+ Add plugin** and **File** (which also has **Help**, this page).

**Below the toolbar:** the rack's rows, then the rig's two buses, then the **Sources** dock (inputs, sequences and loopers), and the status bar at the bottom.

**The status bar** shows the sample rate and buffer size, the DSP load, xruns, the MIDI protocol (and "MIDI dropped" in amber if any MIDI was lost), where the set saves (or the latest notice), and the master fader and level.

Menus and panels open inside the window. A click outside one, or Escape, closes it.
:::

::: {.callout .callout-note .card}
## Files and Saving {#files}
**The set is the only document.** It holds its songs' racks and its rig inside it, so it never depends on files elsewhere. One set is open at a time.

The **File** menu, titled with the set's path ("Untitled set" until it's saved), has **New set** (Cmd-N), **Open...** (Cmd-O), **Save** (Cmd-S), **Save As...**, then **New song (a copy of this one)**, **Import a rack...**, **Check set**, **Set settings...** and **Help** ([The Set and Its Songs](#setlists)).
- **Cmd-S saves the whole set**: the song list, the rig, and every song's rack with changes, the current one's and those you switched away from. An untitled set asks for a name (Save As) instead. Shift-Cmd-S saves too.
- **Switching songs never saves.** A song you leave keeps its changes in memory until the set is saved; the toolbar counts the songs unsaved.
- **Save As** adds `.setlist` if you leave it out, and asks before replacing an existing set. The new package becomes the set's file.
- **New set** starts an untitled set of one empty song, with a copy of the rig in use.
- **Open** takes a set (`.setlist`). An older `.setlist` file, from before sets held their racks, or a lone `.rack` bundle opens as a new, untitled set made from it, and the old files are left alone. A set that doesn't load leaves the running one alone. On Linux, a set is a folder, and Open chooses a folder.
- **New, Open, Quit and closing the window ask first** when the set has unsaved changes: **Save** (or **Save As...** for an untitled set), **Discard the changes** (**Quit without saving**), or **Cancel**.

The status bar says where the set saves, or "Untitled set: File > Save As keeps it".

Started with `--no-save`, StageHand never saves: Save and Save As are disabled.
:::

::: {.callout .callout-note .card}
## Rack Rows {#rows}
Each row is one plugin, in three parts:
- **Input** (left): the input menu, the **MIDI FX** menu, a MIDI 1.0 / 2.0 badge, the key range bar, and the filter chip ([Inputs and Filters](#inputs), [MIDI Effects](#midifx)).
- **Plugin** (middle): the row's name, the plugin and its maker, its **Control Links** button (the MIDI plug), **Editor**, and its share of the DSP load.
- **Output** (right): **M** (mute), the two sends (**A** and **B**), the fader over a stereo meter, the clip light, the level, and the output pair ([Levels and Outputs](#levels)).

**Click the row's name** to type a new one (left empty, it's the plugin's name again). **Click the plugin's name** under it to replace the plugin: the row keeps its settings, MIDI effects and what plays into it, and the new plugin starts from its defaults. This song's Control Links to the old plugin's parameters go.

**Editor** opens the plugin's own window. For a plugin without one, it opens StageHand's **generic editor** instead: a slider, switch or menu for each of its parameters, and the value as the plugin writes it (click it to type a number). Double-click a control for its default; Shift-drag a slider for finer steps. It only opens when you click Editor.

Each row has its own colour, as a stripe down its left edge and a tint across it. A sequence or looper that plays into a row takes the row's colour, so you can see what plays what.

**Removing a row:** the red X at its right end asks first. **Shift-click** the X to remove it at once.

A row whose plugin can't be found is kept as a placeholder, with the reason, so the song still plays. Install the plugin and open the set again.
:::

::: {.callout .callout-note .card}
## Adding Plugins {#adding}
StageHand finds CLAP plugins in the usual places: `~/Library/Audio/Plug-Ins/CLAP` and `/Library/Audio/Plug-Ins/CLAP` on the Mac, `~/.clap` and `/usr/lib/clap` on Linux, and anywhere `CLAP_PATH` names.
- **+ Add plugin** lists the instruments, then the note effects and the audio effects, greyed: note effects go in a row's MIDI FX menu, audio effects on the rig's buses.
- **Right-click below the rows**, or click the **+** there, for the instruments alone.

The new row goes at the bottom. The first scan can take a moment; after that the list opens at once.

On Apple Silicon, a plugin built only for Intel can't be loaded: the scan skips it and says it's Intel only.
:::

::: {.callout .callout-note .card}
## Inputs and Filters {#inputs}
**The input menu** chooses what a row hears:
- **All inputs**: every keyboard and controller StageHand opened.
- **One device**, by name, with its protocol.
- **From other apps (MIDI 2.0)** and **From other apps (MIDI 1.0)**: StageHand's own inputs, which other apps (an on-screen keyboard, a DAW) can send to. Other apps see them as `MIDI 2.0 In` and `MIDI 1.0 In`; on Linux, under the clients `StageHand MIDI 2.0` and `StageHand` in the ALSA sequencer.
- **No live input**: for a row that only plays the sequences and loopers aimed at it.

A device that's unplugged shows in red, and the row hears it again when it's back.

**The key range bar** under the menu is the row's range of notes. Drag an end to move it, drag the middle to move the whole range, or click outside it to stretch the nearer end. Two rows with ranges side by side make a split; overlapping ranges make a layer.

**The filter chip** opens the filter panel: channels, groups (MIDI 2.0), the key and velocity ranges, and transpose, with Reset. Steppers take Shift for an octave or 10, and the wheel works too.

**An amber "!"** beside the badge means a MIDI 2.0 input feeds a MIDI 1.0 plugin, so per-note data is lost on the way.
:::

::: {.callout .callout-note .card}
## MIDI Effects {#midifx}
A row can run note effects (CLAP plugins that change MIDI, such as Retune) in front of its plugin. Everything the row hears goes through them, in order, then to the plugin. Their own audio isn't heard.

- **The MIDI FX menu**, beside the input menu, shows the first effect's name (and "+1" for each more), or "MIDI FX" when there are none. Its **Add a MIDI effect...** lists the note effects; a new one goes after the others.
- For each effect the menu has **Editor**, **Bypass** (the row hears its input directly), **Move up**, **Move down** and **Remove**. A bypassed effect's editor window says "(bypassed)" in its title, since the effect's own editor can't show it.
- With one effect, **E** beside the menu opens its editor.
- An effect that isn't installed shows in red. It's kept and saved with the rack, and the row's MIDI passes it by until it's back.
:::

::: {.callout .callout-note .card}
## Levels and Outputs {#levels}
- **The fader** moves with your drag, not to where you click. **Shift** is finer, **double-click** returns to 0 dB, and the wheel moves half a dB. It stops at 0 dB on the way past.
- **M** mutes the row: its notes are released, new notes don't reach it, and once it's quiet the plugin rests, costing no DSP. A muted row is dimmed, except its M.
- **The sends, A and B,** feed the rig's buses, after the fader. Drag up or down, use the wheel, or double-click to turn one on (0 dB) or off.
- **The clip light** latches red when the row's output clips. Click it to clear.
- **The output pair** ("out 1-2") is a menu when there are more than two outputs.
:::

::: {.callout .callout-note .card}
## The Rig's Buses {#buses}
The rig has **two effect buses**, A and B, between the rows and the dock. Each has two effect slots in series (a reverb, then an EQ, say), **M**, a return fader with its meter, and its output pair. Either slot can be empty, and passes the audio on.
- Choose an effect from a slot's menu; **Edit** opens its window, and the MIDI plug beside it its [Control Links](#control-links).
- Rows feed a bus with their **send** of that letter.
- The **master** fader, in the status bar, sets the level of everything on the main outputs. Double-click it, or a return fader, for 0 dB.

The rig is the same in every song of a set, so a song switch never reloads its effects. Each set keeps its own rig inside it; a new set starts with a copy of the rig in use.
:::

::: {.callout .callout-note .card}
## Transport and Click {#transport}
**Stopped**, the transport is at bar 1 and plugins see it stopped. **Running**, bar 1 beat 1 is the moment you pressed Start. Start and Stop from the toolbar, with **Space**, or from learned MIDI triggers.

**The MIDI button** beside Panic holds the transport's triggers (Learn Start/Stop, Learn Start, Learn Stop) and **Start on first note**: while stopped, the first note from the chosen input starts the transport, with beat 1 on that note.

**The ⋮ menu** after the beat lights:
- **What the lights count**: the beats (6/8 counts two, 7/8 seven), or every note.
- **The click**: off, or a level from -24 to 0 dB, and its output. The click has its own output, never mixed into the mains, so it can go to your in-ear mix while the PA gets the band. **Test the click** plays one blip, to check where it's heard.
- **The count-in** ([Count-in](#count-in)).
- **Flash the window's edge on each beat**: the whole border flashes, brighter on the downbeat, readable from across the stage.

The ⋮ turns amber when the click is on but its output isn't there, and the menu says why it's silent.
:::

::: {.callout .callout-note .card}
## Count-in {#count-in}
Each song can count in before bar 1: **off, half a bar, 1 bar or 2 bars**, chosen in the ⋮ menu and saved with the rack.
- Start begins the count-in; **bar 1 lands right after it**, with everything that starts with the song.
- The count-in always clicks, even in a song whose click is off.
- The tempo shows "Count-in 2 … 1", and the beat lights turn amber.
- No count-in when the music is already going: a start on first note, or a song switch with the transport running.
- Stop during the count-in cancels it.
:::

::: {.callout .callout-note .card}
## The Sources Dock {#dock}
The dock at the bottom holds what makes notes besides your hands:
- **Inputs**: one chip per open MIDI input, lit as its messages arrive. A device that left is amber until it's back.
- **+ Sequence**, **+ Looper** and **Stop all** (which stops every sequence).
- **A row per sequence and per looper.**

The dock grows with its rows, up to a little over half the window, then scrolls.
:::

::: {.callout .callout-note .card}
## Sequences {#sequences}
A sequence is a loop of notes that plays into rows you choose, on top of their live input.
- **+ Sequence** asks for **MIDI 2.0 (EDO pitches)** or **MIDI 1.0 (note numbers)**, then where it plays: an existing row, a new row, or none yet. Then type its name.
- **Its row:** Play/Stop, its name (click to rename), where it plays (click to choose rows), **Edit**, a mini-map of its notes, **At Start**, **Rate**, its length, **launch** (the next bar, the next beat, or at once), **Repeat**, and its MIDI triggers.
- **Play/Stop** waits for the launch point and blinks until then. Started while the transport is stopped, it starts the transport too, on beat 1.
- **At Start** sequences begin with the transport, however it's started: that's how a song's parts start together.
- **The MIDI button** learns a pad or pedal to start, stop or toggle it. One pad can drive several sequences.

Removing one asks first; **Shift-click** its X to remove it at once.
:::

<details class="disclosure">
<summary class="disclosure-title subtle">The sequence editor</summary>

::: disclosure-content
**Edit** (or a click on the mini-map) opens the sequence's own window. The strip on the left has Play/Stop, then sets the protocol (while it has no notes), the EDO and the note step 0 sounds at (MIDI 2.0), rate, length, launch, Repeat, the note-off offset, the **Swing** and **Slop** knobs, channel and group, and the length of new notes; then **Undo** and **Redo**, and a reminder of the gestures. The grid has one row per pitch, with the 12-tone black keys shaded so the familiar pattern shows at any EDO.

**Swing** delays every second unit of the loop: it is the share of a pair of units the first one takes, from 50% (straight) to 75%, and 67% is a triplet feel. **Slop** moves each note early or late at random, by up to 50 ms but never more than a quarter of a unit, and differently on every pass; the first note after Play is never moved. Neither changes the loop's length or the tempo. Drag a knob up or down (Shift: finer), or use the wheel; a double-click sets it back to 50% or 0 ms.

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
| Cmd-A, Escape | | Select every note, clear the selection |
| Cmd-Z, Shift-Cmd-Z (or Cmd-Y) | | Undo, redo |

Notes sound as you add, click and move them, through the rows the sequence plays into. **Space** plays or stops it. The wheel scrolls up and down; Shift-wheel, or a sideways swipe on a trackpad (or a tilt wheel), scrolls sideways; Cmd-wheel zooms. Cmd-Q quits and Shift-Cmd-S saves the set from this window too.
:::

</details>

::: {.callout .callout-note .card}
## Loopers {#loopers}
A looper records the notes you play from one input into its rows, and loops them, in time.
- **+ Looper** asks what it records (an input) and which row it plays into, then its name.
- **Its row:** the record/play button, its name, **Undo** (the last pass), **Clear**, **Keep** (makes it a sequence, so it's saved), **Input**, **Q** (quantize), start at the next **Bar** or **Beat**, its length (fixed, or ended by a press), and **Clean**, which skips the short wrong note a finger catches beside the right one.
- **Click a looper's row** to make it the current one: the one the footswitch drives.
- **What a looper records isn't saved**: each performance starts fresh. Keep it as a sequence to save it.
- Its red X removes it at once, without asking (nothing recorded is lost that would have been saved).
:::

<details class="disclosure">
<summary class="disclosure-title subtle">The looper footswitch</summary>

::: disclosure-content
One footswitch drives the current looper. Learn it from the looper row's MIDI button. A momentary switch (one that sends press and release) is needed for overdubs; a latching pedal gives taps only.

| State | Tap | Hold |
|---|---|---|
| Empty | arm: recording starts at the next beat or bar | |
| Armed | cancel | |
| Recording | end it (rounded up to the beat or bar) and play | |
| Playing | stop, at the next beat or bar | overdub while held |
| Stopped | play from the start, at the next beat or bar | |

A note played a little ahead of the downbeat is recorded on it. Stopping the transport stops every looper.
:::

</details>

::: {.callout .callout-note .card}
## The Set and Its Songs {#setlists}
A set puts racks in order, as songs, and moves between them without a gap. It always has at least one song: a new set starts with one empty song.
- **The set's row**, at the top of the window: the Unlikelyware logo, **|◀** and **▶|** for the previous and next song, where you are ("3/12"), the song's name (a menu of every song), "next: …" with a dot that's green once it's ready ("end of the set" after the last), the set's name, its MIDI button, and **?** for this page.
- **Adding songs:** File → **New song (a copy of this one)** adds a copy of the current song's rack after it, as a rack of its own ("So what 2"), and plays it, ready to change. **Import a rack...** copies a `.rack` you have into the set, after the current song.
- **The song menu** lists every song (a song with unsaved changes says so). Choose one to go to it. For the current song it also has: New song (a copy of this one), Add this rack again (a reprise: the same rack twice, so a change to one changes both), Move up, Move down, Rename in setlist..., and Remove from the set.... Removing asks first; the song's rack goes from the set when it's saved, unless another song uses it. The last song can't be removed.
- **Changes to the song list are part of the set**, saved with Cmd-S like everything else ([Files and Saving](#files)).
- **Set settings...** in the File menu chooses whether the transport keeps running across a switch (for medleys) or stops, whether a switch waits for the next bar or happens at once, and whether the old song rings out or is cut.
- **Check set** in the File menu reads and prepares every song in turn, without playing it, and lists what it finds (a plugin missing, a MIDI input not connected, a trigger that clashes) under the song in the song menu, in amber.

**Switches are instant.** The next and previous songs are prepared ahead, and the old song's reverb tails and held notes ring out. A note held across a switch keeps sounding until you let go.

**From MIDI:** the set's MIDI button learns Next, Previous and Program Change (program n goes to song n), so a foot controller can step through the set. They work in every song, and win over a song's own triggers on the same control.
:::

::: {.callout .callout-note .card}
## The Perform View {#perform}
**Perform** in the toolbar, or **Tab**, turns the window into what you want to see on stage, and Tab brings the rack back. Switching views never changes the sound.

It shows, large: the song and what's next, with big ◀ ▶ buttons; the beat, one box per pulse with the bar and beat in big digits; the count-in, very large, in amber; Start/Stop; the current looper; a large **Panic**; and the status bar. It hides the rows, the dock and everything that edits.

**Keys:** ← and → for the previous and next song, Space to start or stop, Tab (or the **Edit (Tab)** button) to go back. StageHand reopens in the view it was left in.
:::

::: {.callout .callout-note .card}
## Control Links {#control-links}
A Control Link ties a hardware control (a knob, a fader, an expression pedal) to a plugin parameter, a row's level, a send, a bus return or the master. One control with several targets is a macro.
- **Each row's Control Links button** (the MIDI plug beside Editor) opens its menu: learn a link from the list of the plugin's parameters (type to filter it), or learn the row's gain or sends; then move the hardware control. The status bar says what to do at each step.
- **Bus effects and bus returns** have a Control Links button of their own; **the bus return and master faders** also take a right-click.
- **Each link** has, per target, a range (drag B, the control's bottom, and T, its top, either way round; Shift for fine) and a curve, and is **pick-up** (it waits until the control reaches the parameter, so the sound never leaps) or **jump**. **+ Add target** gives one control more to drive. Choose a link in the menu to change it in the Control Link panel, which also has **Learn again** and **Remove**.
- **Scope:** **This song** (saved in the rack) or **All songs** (saved in the rig: the mod wheel can drive a plugin's tremolo in every song that has that plugin). A row's links start as this song's, the buses' and master's as all songs'.
- A control used by a link goes nowhere else in that song's rows, unless the link passes it on (**Pass on too**).

A level link works like a volume pedal after the fader: the fader doesn't move, and the two multiply.
:::

::: {.callout .callout-note .card}
## Panic {#panic}
**Panic** in the toolbar (and in the Perform view) stops stuck notes in every row: a note-off for every note StageHand knows is sounding, the pedals up, and All Notes Off on every channel.
- **The transport, the click, sequences and loopers keep running**: a stuck note is no reason to lose the band's time.
- **Effect tails ring on.** **Press again within 2 seconds** to cut the sound too (All Sound Off, and the buses' effects reset), for runaway feedback or a plugin stuck making noise.
- **Learn a MIDI control** for it from the transport's MIDI button (Learn Panic). It's kept in the rig, the same in every song. There's no key for it, so a stray keypress can't cut the sound.
:::

::: {.callout .callout-note .card}
## DSP and Xruns {#dsp}
- **"DSP 23% · peak 41%"** in the status bar is the share of each audio period StageHand spends: the average and the worst block of the last second. It turns amber from 70% and red from 90%. At 100% the audio is late, which is an xrun.
- **Each row and bus** shows its own share, amber when one takes more than half the period.
- **Xruns** are counted since StageHand started, red for a few seconds after a new one. Click the count to reset it (after a soundcheck, say).

When the load runs high: mute rows you don't need in a song (a muted plugin rests), choose a larger buffer, or use lighter plugin settings.
:::

::: {.callout .callout-note .card}
## Shortcuts {#shortcuts}
**In the rack window:**

| Key | Action |
|---|---|
| Space | Start or stop the transport |
| Tab | The Perform view, and back |
| Cmd-S, Shift-Cmd-S | Save the set (Save As for an untitled one) |
| Cmd-N | New set |
| Cmd-O | Open a set |
| Cmd-Q | Quit (asking first if the set has unsaved changes) |
| Escape | Close a menu or panel; stop MIDI learn |

**In the Perform view:** ← → previous and next song, Space start or stop, Tab back.

**In plugin editor windows:** Cmd-Q quits and Shift-Cmd-S saves the set. Quitting from here, or with Ctrl-C in the terminal, doesn't ask: a saved set with changes is saved, and an untitled set's changes are lost.

**In the sequence editor:** see [The sequence editor](#sequences).

**With the mouse:** Shift for finer drags and bigger steps; double-click a fader for 0 dB; Shift-click a red X to remove without asking; right-click below the rows to add an instrument.
:::

::: {.callout .callout-note .card}
## Troubleshooting {#troubleshooting}
- **No sound:** check the row's meter moves when you play. If it doesn't, check its input menu and the light beside it, its key range and filter, and M. If the meter moves, check the output pair and the master.
- **An app's MIDI doesn't arrive:** send it to **From other apps (MIDI 2.0)** or **(MIDI 1.0)**, and choose that input in the row (or All inputs). From the command line, `--midi all` opens software and app sources too.
- **The click is silent:** open the ⋮ menu; it says why (no output chosen, the output isn't connected, or the transport isn't running).
- **A plugin is missing** (a placeholder row): install it, and open the set again. Its settings were kept. **Check set** finds every song with a missing plugin before the show.
- **Clicks and dropouts:** watch DSP and xruns in the status bar ([DSP and Xruns](#dsp)).
- **A stuck note:** [Panic](#panic).
- **The command line:** `stagehand --help` lists every option: `--list-plugins`, `--list-midi`, `--list-audio` (macOS), `--tone` to check the audio path, `--monitor` to see incoming MIDI, `--set` and `--rack` to open one, and `--no-save`.
:::
