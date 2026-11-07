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
     A link drives a **link level** in series with the fader (decided while
     building milestone 1): the fader doesn't move, the saved gain isn't
     the pedal's position, and the two multiply, as a volume pedal after a
     mixer channel would. 0 dB until a link moves it.
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
   - **This song:** saved in the rack (`"controls"`, a list at the top of
     rack.json, rack-format.md: a macro can span rows, so not per row). Its
     targets are that rack's rows, or the rig's levels and bus effects while
     that song plays.
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

**Milestone 1 implemented (2026-10-03):** `CompiledControlLink`
(Engine/ControlLink.h): a source (input, group, channel, controller), jump or
pick-up, consumed or passed on, and targets: `Parameter` (keyed by the
`HostedPlugin`, a row's or a bus effect's, and the parameter id), `Gain`,
`Send`, `Return` (keyed by the `SlotRuntime`) and `Master`, each with bottom,
top, a curve (linear, slow start = squared, fast start = square root) and
steps. They're published in `TransportControl::links`, with the sequences.
`takeMidi()` matches them after every trigger and marks the message linked,
and consumed unless a matching link passes it on (a consumed controller isn't
recorded by loopers either); `routeMidi()` applies them at the message's
place, so a `ParamValue` event lands in offset order among the row's events.
Bus effects now get events (`BusState::Stage::events`, 256 a block, split
into chunks as a row's are). Levels are `SlotRuntime::linkGainDb` and
`linkSendDb[]`, and the engine's `linkMasterDb`, multiplied into each
ramp's target (-60 dB and below is silence). Pick-up: within 2% of the
target's range, or crossing it since the control's last move;
`Target::moved()` re-arms it when another link moves the same target, and
when the plugin reports a change of its own through `eventsOut` (the
engine's `ParamFeedback` sink, now passed to rows and bus effects), unless
it's the value a link just sent (an echo). `SequenceSet::keepControlLinks()`
carries pick-up state over an edit; a song switch doesn't, so the new song's
links start from the values compiled (`Target::initial`). Tests:
`testControlLinks` in the engine tests (a jump macro: reversed with a fast
start, a stepped bus effect parameter at the message's frame, a row's level
to silence and back; pick-up from below and above, re-armed by the plugin's
own change but not its echo, and by another link; passed on to the row and
to the master's level; kept by an edit, re-armed by a switch).

**Milestone 2 implemented (2026-10-03):** `ControlLinkSetup` and
`ControlLinkTarget` (Session/ControlLinks.h) as saved: `Rack::controls` and
`Rig::controls`, read and written by Session/ControlLinkJson.h
(rack-format.md, "Control links"). A song's targets name rows (a missing row
fails the load); the rig's name plugins, and can't be a row's gain or send.
`Session::setControlLinks()` and `controlLinks()` per `ControlScope` (marking
`modified()` or `rigModified()`), inputs resolved as triggers' are (again on
MIDI changes; `controlLinkProblem()`), `capture()` keeps the song's, and
removing a row drops its targets (and a link left with none).
`compileControlLinks()` builds the engine's links in `buildSequences()`: the
song's, then the rig's unless the song has a link on the same control;
parameter targets found in the running rows (by row, or by plugin id in every
row) and bus effects, with the parameter's steps, its range (bottom and top
clamped to it) and its value now (`paramValue()`) as where pick-up starts. A
target that isn't there is left out, and a link with none is too. Links are
compiled again whenever the graph is (they name its plugins).
`RackController`: `rowParameters()`, `effectParameters()` (automatable ones),
`learnControlLink(target, scope)` (the parameter's whole range or -60 to 0
dB; a link on the same control in that scope gets another target),
`setControlLink()`, `removeControlLink()`, `setControlLinkScope()` (for all
songs a row's parameter becomes its plugin's; to this song, a plugin's
becomes each row's that has it). Refused, with a notice: a key, a control a
trigger answers (the transport's, a sequence's, the setlist's, the looper
switch, Panic), a row's gain or send for all songs.
`RackViewModel::controlLinks` (control, connected, mode, overridden, each
target's text and whether it's there) and `controlLinkLearning`. Tests: the rack file (every target kind,
both modes, failures), the rig file (a plugin target; a gain and a row
refused), the session (`testControlLinks`: a row's parameter, all songs' in
both rows, overridden by the song's, a bus effect's stepped parameter, an
unknown parameter and an absent input left out, a row removed), the
controller (`testControlLinks`: learn, a macro, the refusals, scope moved,
overridden, saved, removed).

**Milestone 3 implemented (2026-10-03):** the Control Links button (the MIDI
icon) beside each row's Editor, beside each loaded bus effect's Edit, and
on each bus strip where a row has its sends (for the return); the return
and master faders also take a right-click, and "master" in the status bar is
lit while a link drives it. Lit when links act there, amber if one's input
isn't connected, blinking while one is learned there. Its menu ("Control
Links: Keys"): Learn a Control Link from the list... (the parameters,
filtered as you type: menus can now be searchable, `UiWidget::Menu::search`),
Learn the row's gain / send A / send B, or Learn a Control Link for a
return or the master; a line per link acting there ("All inputs, CC 7, ch 1
→ gain -20.0 dB to +3.0 dB + 1 more, jump", amber with "waiting for
pick-up" or "not connected"), which opens it in the dialog; and Remove lines,
which remove the targets acting there (a macro keeps its others). A link
learned from a row is this song's; from a bus effect, a return or the
master, all songs' (they're the rig's); the dialog changes it. While
learning, the status bar says "Control Link: now move a knob, fader or pedal
(Esc stops)". The Control Link dialog, a panel over the rack: the control
(Learn again, Remove, Done), This song / All songs, Pick-up / Jump, Pass on
too; the control's position; then each target: its name ("waiting for
pick-up" in amber), "now", B and T as values (a parameter's percent of its
range, a stepped one's step, a level's dB), a range bar with B and T handles
(drag either, either way round; Shift: fine), a white line where the target
is now, a dot where the control would put it (amber while waiting), the
curve (Linear, Slow start, Fast start, in turn), a red X; + Add target (a
row's parameter from its list, gain, sends; a bus effect's parameter; a
return; the master). Changes apply at once. Live state comes from the
engine: each compiled target carries an `origin` (Session's scope, link and
target, `linkOrigin()`), and `Engine::linkStates()` publishes each one's
value, the control's position and waiting, at the end of every block (the
first 64 targets). `RackController`: `relearnControlLink()` (in place;
refused if another link in its scope has that control),
`addControlLinkTarget()`, and `setControlLinkScope()` returns where the link
went. `ControlLinkModel` has the control as saved (`setup()`), the
control's position, and Learn again; each target, the rows it acts on, its
scale, where it is now and waiting; `controlLinkLearnTarget`. Tests: the
engine (`linkStates()`: not moved, waiting, picked up, a level), the
controller (the model's control, rows, scale, live value and waiting; Learn
again and its refusal; a target added, reset, refused; the scope's new
index; a gain refused for all songs). Learn from the editor is milestone 4.

**Milestones 4 and 5, implemented (2026-11-06): made and edited on screen, and
learned from the editor.** Before the 0.9.0 preview, a link no longer needs
the hardware to be made or changed, and every link can be seen in one place.
- **Learn from the editor** (milestone 4, as sections 8 and 16 planned): the
  row's or bus effect's menu, "Learn a Control Link from the editor...". Its
  editor opens (the generic one if it has none) and the status bar says
  "touch a knob or slider in Keys's editor"; the first parameter touched
  there that can be linked is the target (one that can't is said once and
  skipped), then the hardware control is learned as from the list. The
  engine passes touches audio -> main only while armed
  (`Engine::armTouch()`, `takeTouched()`, a queue of 32): a plugin's
  `ParamGestureBegin`, a `ParamValue` it sends itself (some plugins never
  send gestures), from a row, a MIDI effect or a bus effect, and the generic
  editor's own changes. Esc, Stop learning or any other learn disarms it.
- **Made, choosing the controller** (milestone 5): "Make a Control Link,
  choosing its controller..." in each menu: what it drives (a parameter, or
  the row's gain or sends), then the controller from a list, filtered as you
  type: CC 1 to 119 with their usual names, without bank select, data entry,
  the (N)RPN numbers and the channel mode messages; one already in use is
  marked amber with what uses it (a trigger, or another link in that scope).
  It starts on All inputs, group 1, channel 1, with the parameter's whole
  range, and the dialog opens on it. `RackController::makeControlLink()`
  refuses what learning refuses; on a control a link in that scope has, it
  becomes another target, as learning does.
- **Listens to**, in the dialog: the input, group, channel and controller as
  dropdowns. `setControlLinkControl()` is what Learn again does once the
  control has moved, with its refusals (a trigger's control, another link's
  in the same scope); Learn again now goes through it.
- **All Control Links** (Cmd/Ctrl-L, the File menu, the last line of every
  Control Links menu): a panel listing every link, this song's then all
  songs', with its control, its targets and their ranges, and its state
  (pick-up or jump, passed on; not connected, overridden, or waiting for
  pick-up in amber). A line opens the dialog, whose Done comes back to the
  list; a red X removes the link (asking first); "+ New Control Link" offers
  every target there is (as + Add target does), then "Move it now (learn)"
  or "Choose it from a list...". The wheel scrolls it.
- Tests: the controller (`testMadeControlLinks`: made, a macro, refused for
  a parameter that can't be linked, a row's gain for all songs and a
  trigger's control; `controlUse()`; the control changed and refused; learned
  from the editor through a gesture, a plugin's own value and the generic
  editor's, a parameter that can't be linked skipped, and disarmed by Esc and
  by another learn).

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
