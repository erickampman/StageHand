# StageHand — Controls: MIDI controllers to parameters (design)

Proposed 2026-10-03; its questions answered the same day. A knob, fader or
expression pedal on the performer's hardware drives a plugin parameter, or
one of StageHand's own levels (a row's gain, a send, a bus return, the
master). Each such pairing is a **control link** ("Control Link" in the UI):
a source (a learned control), one or more targets, a range and a curve, and
a scope. Builds on:
- MIDI learn, as triggers, the looper switch and Panic use it (the next
  note-on or controller press from any input is captured).
- `HostedPlugin::parameters()` (names, ranges, stepped or not) and the
  `ParamValue` event, which the engine can already put in a slot's events
  (Core/Event.h) but doesn't yet.
- The plugin editor windows, and CLAP's parameter events out of `process()`
  (`ProcessContext::eventsOut`, which the engine passes as null today).
- The rig (docs/rig.md): what's the same in every song.

## What a control link is

1. **Source:** a controller (CC 0-127, or a MIDI 2.0 controller at its full
   resolution) on a channel and group, from one input or any, as triggers
   are written. Notes aren't sources; pitch bend and aftertouch, later
   perhaps; 14-bit CC pairs, not planned (MIDI 2.0 is the way to fine
   control).
2. **Targets:** one or more of
   - a plugin parameter in a row (by the parameter's stable id), or in a bus
     effect;
   - a row's gain, a row's send to bus A or B, a bus's return, the master.
     The expression pedal as a volume pedal is the most common use there is.
   One control with several targets is a **macro** (a fader that opens the
   filter and raises the reverb send). Each target has its own range and
   curve.
3. **Range and curve, per target:** the parameter's value at the control's
   bottom and at its top (any part of its range; top below bottom reverses
   it, heel-down = full); a curve: linear, slow start, or fast start. A
   stepped parameter (a switch, a waveform choice) takes the nearest step.
   StageHand's own levels are in dB, with the fader's scale.
4. **Jump or pick-up, per link, pick-up by default** (decided 2026-10-03):
   - **Pick-up:** after the parameter changes some other way (a song switch,
     a preset, the plugin's own GUI, another link, Undo), the control does
     nothing until it reaches the parameter's value (crosses it, or comes
     within 2% of it); from then on it drives it. No leap in the sound.
   - **Jump:** the first move sets the parameter to where the control is.
   - The pick-up point **follows the parameter** (decided 2026-10-03): move
     the knob in the plugin's GUI, and the hardware knob has to find the new
     value.
5. **Consumed by default** (decided 2026-10-03): a controller that is a
   link's source goes nowhere else in its scope's rows, so a synth that
   already answers CC 11 doesn't answer it twice. A link can be set to pass
   the controller on as well.
6. **Order of matching:** the setlist's triggers, the looper switch, Panic,
   then the rack's triggers, then control links. A controller that's a
   trigger isn't a link's source too (refused when learned, with a notice).

## Scope

7. **Each link chooses its scope** (decided 2026-10-03):
   - **This song:** saved in the rack, per row (`"controls"` in the slot,
     rack-format.md). Its targets are that rack's rows, or the rig's levels
     and bus effects while that song plays.
   - **All songs:** saved in the rig (`"controls"` in rig.json). Targets:
     the rig's own (bus effects, returns, master), always there; and **a
     plugin's parameter wherever that plugin is a row** (by plugin id and
     parameter id): "the mod wheel always drives the Rhodes' tremolo depth,
     in every song that has the Rhodes". A song with that plugin in two rows:
     both. Row gain and sends can't be All songs (rows differ from song to
     song).
   - A This song link on the same control **overrides** an All songs one for
     that song (as a song's own setting overrides a default), and the
     Control Link dialog says so.
   - Changing a link's scope moves it between the rack and the rig, marking
     both modified.

## Where, in the window

The rack window is crowded (decided 2026-10-03: show as much as there's room
for, no more), so control links live behind one button per row and per bus
effect, and in a dialog, not on the rows themselves.

8. **A Control Links button on each row** (the MIDI icon, beside Editor; its
   menu titled "Control Links"): lit when the row has links (either scope),
   amber if a link's input isn't connected, blinking while learning. Its menu:
   - **Learn a Control Link from the editor…** (the editor opens if it
     isn't): move a knob in the plugin's window, then the hardware control.
     The status bar says what's happening at each step ("Move a knob in
     Prophet's editor…", "Now move the hardware control…", "Pedal CC 11 →
     Filter cutoff").
   - **Learn from the list…**: a searchable list of the plugin's parameters
     (names, filtered as you type), then the hardware control.
   - **Learn the row's gain / send A / send B**, then the control.
   - One line per link touching this row, with as much as fits: "KeyLab CC
     74 → Cutoff 30-80%, pick-up" (amber "waiting for pick-up" while it is);
     choosing one opens it in the dialog. **Remove …** lines, red.
9. **The same on each bus effect** (its chip's menu) and on the master and
   bus return faders (right-click), for the rig's levels.
10. **The Control Link dialog**, one link at a time, a panel over the rack
    as the sequence settings are: the source (with Learn again), the scope (This
    song / All songs), jump or pick-up, pass on or not; then each target as
    a line: its name, a range bar with two handles (bottom and top, either
    way round), the curve, a live marker showing the parameter now (and the
    control's position, in amber while waiting for pick-up); + Add target
    (from the editor, or the list), and Remove. Changes apply at once.
11. **On a row**, nothing new beyond the lit button. (If room is found later,
    a small "2 controls" under the plugin's name.)

## In the engine

12. **Compiled with the sequences** (a `ControlLink` table in the
    `SequenceSet`, beside the triggers): sources resolved to input handles,
    targets to slot indices and parameter ids (or a level), ranges, curves,
    modes. Swapped atomically with the rest at a commit or a switch.
13. **On the audio thread**, in `takeMidi()` after the triggers: a match
    transforms the controller's value (7-bit, or MIDI 2.0's 32-bit) per
    target and, unless picking up, puts a `ParamValue` event in that slot's
    events at the message's offset (bus effects get theirs in their stage's
    events, new), or sets a level's target (it ramps as the faders do). The
    message is then consumed, or routed as usual if the link passes it on.
14. **Each parameter's value, known on the audio thread** for pick-up: the
    last value a link sent, and the plugin's own changes, which the engine
    reads from `eventsOut` (a sink per slot, new; CLAP plugins report GUI
    moves and preset changes there while processing). At compile time the
    main thread fills in each target's current value (`paramValue()`).
    A song switch re-arms pick-up for every link.
15. **A row that isn't processing** (suspended after a mute) gets its
    parameter changes at its next run, or through `flushParams()` when
    the plugin asks (CLAP's `request_flush`).
16. **Learn from the editor** needs the plugin's "a parameter was touched"
    on the main thread: the engine forwards `ParamGestureBegin` (and the
    first `ParamValue` of a gesture) from `eventsOut` through a small queue,
    audio -> main, as it does learned controls.
17. **Saving:** parameters a link moved are saved in the plugin's state as
    any change is (a song saved with the pedal heel-down keeps that cutoff).
    The links themselves are rack or rig data, separate from plugin state.

## Milestones

1. **Links, engine:** the compiled table, matching after the triggers,
   consumed or passed on, range, reverse, curve, steps; jump and pick-up;
   `ParamValue` events to rows, and to bus effects (their own events); the
   levels as targets; `eventsOut` read per slot, so pick-up follows the
   plugin. Tests with the fake plugin: values at the right offsets, the
   curves, pick-up (a value crossed, from either side; re-armed by a plugin
   change and by a switch), jump, a macro, consumed and passed on.
2. **Links, saved and compiled:** `"controls"` in the rack's slots and in
   the rig, the Session compiling both scopes (This song overriding All
   songs; All songs found by plugin id in every song's rows), the controller:
   learn from the list and of a level, edit, remove, change scope. Tests:
   the files, the session (scope, override), the controller.
3. **Links, UI:** the Control Links button and menu on rows, bus effects and
   faders; the Control Link dialog with the range bars and the live markers.
4. **Learn from the editor:** gestures audio -> main, the learn sequence
   with its status messages. Tests: the engine forwarding a gesture, the
   controller's sequence.

## Open questions

None at the moment. Answered 2026-10-03:
1. Scope as proposed in 7, including an All songs link driving every row
   that has its plugin (two rows of the same plugin in a song: both).
2. **"Control Link"** is the name in the UI too, where there's room: the
   menu's title and items ("Learn a Control Link from the editor…"), the
   dialog's title, the status messages. The row's button stays the MIDI
   icon.
3. **No 14-bit controller pairs** (CC 1/33 and so on), later if ever: fine
   control is what MIDI 2.0 is for, and StageHand leans that way (its
   controllers already arrive at 32 bits from a MIDI 2.0 input).
4. **Pitch bend and aftertouch as sources: later.** Synth patches usually
   know what to do with them already.
5. **The Perform view:** maybe something informational later (a Control
   Link waiting for pick-up, say); on-screen controls that drive links
   maybe in the future. Neither is in these milestones.
