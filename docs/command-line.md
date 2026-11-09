# StageHand — Command line

How to build and run StageHand, and every option. `stagehand --help` prints
the same options in short form.

## Building

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

[BUILDING.md](../BUILDING.md) has the details.

The binary is `build/stagehand`. The tests run with `ctest` from `build/`.
`-DSTAGEHAND_APP=OFF` builds only the libraries and the tests (no JACK, ALSA,
X11 or DPF needed); that's how macOS builds, so far (docs/macos.md). CI runs
the tests on Linux and macOS for every pull request.
With `libxrandr-dev` installed (optional), the rack window opens sized for the
monitor it's on; without it, for the whole X screen (every monitor together).

## Audio: pw-jack

StageHand is a JACK client. On a PipeWire system, JACK is PipeWire's JACK
layer, reached by running under `pw-jack`. You don't have to type it:
Playing and `--tone` check for a JACK server first, and if there's none in
reach they restart themselves under `pw-jack` ("No JACK server in reach;
restarting under pw-jack."). So a launcher or file manager can start
StageHand directly.

- It restarts at most once: the restarted process has `STAGEHAND_PW_JACK=1`
  set, and never restarts again. If there's still no server, it stops with
  "jack_client_open failed".
- If `pw-jack` isn't installed, it says so and carries on, then stops with the
  same error.
- Typing `pw-jack` yourself still works; then no restart is needed.

The listing and monitoring modes (`--list-plugins`, `--list-midi`,
`--monitor`) don't use audio and never restart.

## Usage

```
stagehand [MODE] [options]
```

One mode per run. Without a mode, StageHand plays: `--play` is the default
(since 2026-10-01) and can be left out, so `stagehand --set X` plays X. With
no arguments at all, it reopens the last set at the song it was on
(docs/documents.md); with none, or an untitled one last, it plays a new,
untitled set (see "Untitled set").

| Mode | What it does |
|---|---|
| `--play` | The default: host plugins, MIDI in, audio out, with the rack window |
| `--list-plugins` | List installed CLAP plugins: name, vendor, version, kind (instrument, effect or other, from CLAP's features), id and file |
| `--list-midi` | List MIDI sources: id, protocol, and hardware, software or app |
| `--monitor` | Print incoming MIDI exactly as it arrives |
| `--tone [HZ]` | Play a test sine (default 440 Hz) to check the audio path |
| `-h`, `--help` | Show the help |

## Playing (--play)

What it plays:

| Option | Meaning |
|---|---|
| `--set PATH` | Play a set: a `.setlist` package (docs/documents.md), from its first song. An older `.setlist` file opens as a new, untitled set made from it. `--setlist` is the same. Can't be combined with `--rack` or `--plugin` |
| `--rack PATH` | Play a `.rack` bundle (docs/rack-format.md), as a new, untitled set of one song |
| `--plugin PATH\|ID` | Play one plugin, as a new, untitled set of one song: a `.clap` file, or an id or name from `--list-plugins`. It starts from the plugin's defaults |
| none of these | No arguments at all: the last set, at its song. Otherwise, or with none, a new, untitled set (see below) |

`--rack` and `--plugin` can't be used together: a rack already names its
plugins.

| Option | Meaning |
|---|---|
| `--plugin-id ID` | Which plugin, when the `.clap` file holds several |
| `--open-editors` | Open every plugin's editor at start. Otherwise the rack window's Editor buttons open them |
| `--no-ui` | No rack window, console only. Every editor opens at start |
| `--no-editor` | With `--no-ui`: don't open the editors either |

### Untitled set

`stagehand` with no `--set`, `--rack` or `--plugin` (and, with no arguments
at all, no last set to reopen) starts a new, untitled set with one empty
song named "Song", with the defaults: 120 BPM, 4/4, every hardware MIDI
input plus the virtual inputs, stereo out, and an empty rig. Build it in the
window as usual.

An untitled set is saved with **File > Save As** (or Ctrl-S), which names
it. With changes still unsaved, New, Open, Ctrl-Q and closing the window ask
first. Quitting from the terminal (Ctrl-C) doesn't ask; it prints "The
untitled set's changes weren't saved."

In the window, File also has New set, Open..., New song and Import a
rack...; see docs/documents.md. The file chooser is the desktop's own
(zenity, or kdialog on KDE; the system's panel on macOS).

### Saving

A set is saved whole: the song list, the rig, and every rack with edits
(docs/documents.md, "Saving"). Ctrl-S saves it; switching songs never does.
Quitting from the terminal (Ctrl-C) saves a titled set that has changes; the
rack window asks first.

| Option | Meaning |
|---|---|
| `--no-save` | Don't save anything |

Save As in the window never replaces something that isn't a set (an older
`.setlist` file included), and replaces another set only after asking.

## Untitled rack

`stagehand` with no `--setlist`, `--rack`, `--plugin` or `--save-rack` (and,
with no arguments at all, no last setlist or rack to reopen) starts a new, empty rack named "Untitled" with the defaults:
120 BPM, 4/4, every hardware MIDI input plus the virtual inputs, and stereo
out. Build it in the window as usual.

An untitled rack is saved with **File > Save As** (or Ctrl-S), which names
it. With changes still unsaved, New, Open, Ctrl-Q and closing the window ask
first. Quitting from the terminal (Ctrl-C) doesn't ask; it prints "The
untitled rack's changes weren't saved." A `--plugin` session without
`--save-rack` is untitled too.

In the window, File also has New rack and Open...; see docs/rack-view.md,
section 7. The file chooser is the desktop's own (zenity, or kdialog on KDE).

### Saving

A `--rack` is saved back to itself when StageHand quits, with each plugin's
current state and every edit made in the window (slots, sequences, triggers,
tempo).

| Option | Meaning |
|---|---|
| `--no-save-rack` | Don't save anything |
| `--save-rack PATH` | Save to PATH instead ("save as"). The way to keep a `--plugin` session |
| `--overwrite` | Let `--save-rack` replace an existing rack other than the one being played |

The save target is checked before any audio starts:
- A path that exists but isn't a rack is never replaced.
- An existing rack other than the one being played is replaced only with
  `--overwrite`.
- Saving back to the rack being played is always allowed.
- `--save-rack` with `--no-save-rack` is refused as a contradiction.

The same rules apply to Save As in the window, except that replacing another
rack asks instead of needing `--overwrite`.

## Audio options (playing, --tone)

| Option | Meaning |
|---|---|
| `--quantum N` | Force a PipeWire buffer size of N frames (4-8192) while StageHand runs, released when it exits. It affects all audio on the system. On macOS, this app's buffer on the device. Overrides the buffer size chosen in the status bar |
| `--channels N` | Output channels to register (default 2) |
| `--no-connect` | Don't connect to the system playback ports |

The buffer size and sample rate can also be chosen from the status bar (help page, "DSP and Xruns"). Both are kept per machine in `audio.json` in StageHand's state folder, beside the output device, as `bufferSize` (frames) and `sampleRate` (Hz; macOS only, from the next launch). `--quantum` wins over the saved buffer size for that run.

## MIDI options (playing, --monitor)

By default StageHand opens every hardware MIDI input, plus two virtual inputs
that other apps can connect to:

| Virtual input | ALSA client | Receives |
|---|---|---|
| `MIDI 2.0 In` | `StageHand MIDI 2.0` | MIDI 2.0 (UMP) |
| `MIDI 1.0 In` | `StageHand` | MIDI 1.0 |

The inputs opened, and where each landed, are printed at startup.

| Option | Meaning |
|---|---|
| `--midi ID` | Open only this source instead of every hardware input. Repeatable. ID is from `--list-midi`, a client:port like `14:0`, or `all` (hardware plus software and app sources) |
| `--midi-protocol 1\|2` | MIDI 2.0 protocol preference (default 2). With 1, every source is received as MIDI 1.0 |

Rack sources and sequence triggers name devices by their `--list-midi` id,
for example `KeyLab 88:KeyLab 88 MIDI 1`. A device that isn't there under
that id (a rack from another machine, or a port renamed by an update) is
looked for by name: the same device name first, then the same port name if
only one device has it (docs/macos.md, decision 5). The rack keeps its id.

### MIDI devices coming and going

StageHand follows the ALSA sequencer's announcements (added 2026-10-02), so
plugging and unplugging needs no restart:

- **A device plugged in while it runs** is opened as the ones there at start
  are: hardware by default, everything with `--midi all`, nothing new when
  `--midi` named the inputs. Rows on All inputs hear it; anything naming it
  that showed "(not connected)" (a row's input, a looper, a sequence or
  transport trigger, the setlist's triggers, the looper switch) connects.
- **A device unplugged** shows "(disconnected)" on its input chip, in amber,
  and what names it shows "(not connected)": it hears nothing, as before, but
  now it says so.
- **Plugged back in** (the same `--list-midi` id), it's subscribed again on
  the same input, and everything that used it hears it again.
- A notice says what changed. Other apps' new ports aren't opened (unless
  `--midi all`); pick one in a row's input menu, or connect it to the
  "From other apps" inputs, which take connections at any time.

## Keys

In the rack window and in plugin editor windows:

| Key | Action |
|---|---|
| Ctrl-Q | Quit. In the rack window, a set with unsaved changes asks first |
| Ctrl-Shift-S | Save the set now |

In the rack window only:

| Key | Action |
|---|---|
| Space | Start or stop the transport |
| Ctrl-S | Save (Save As for an untitled set) |
| Ctrl-N | New set |
| Ctrl-O | Open a set (or, on macOS, an older setlist or a rack) |
| Escape | Close a menu or panel, and cancel MIDI learn |

Closing the rack window quits the same way as Ctrl-Q. Closing a plugin editor
or a sequence editor closes just that window; StageHand keeps playing. In the
terminal, Ctrl-C quits the same way as Ctrl-Q.

## Exit status

| Code | Meaning |
|---|---|
| 0 | Success, including `--help` |
| 1 | A run failed (e.g. no audio server, or the save at quit failed) |
| 2 | Bad arguments |

## Examples

Open a new, untitled set:

```bash
./build/stagehand
```

Play a set, from its first song:

```bash
./build/stagehand --set ~/Sets/Friday.setlist
```

Play a rack as a new set of one song (Save As keeps it):

```bash
./build/stagehand --rack ~/Racks/Piano.rack
```

Play a gig set at a 128-frame buffer without changing it:

```bash
./build/stagehand --set ~/Sets/Friday.setlist --no-save --quantum 128
```

Try one plugin (Save As keeps the result as a set):

```bash
./build/stagehand --plugin com.unlikelyware.polygraphmodular
```

Listen only to one keyboard:

```bash
./build/stagehand --rack ~/Racks/Piano.rack --midi "KeyLab 88:KeyLab 88 MIDI 1"
```

See what a controller sends (useful before mapping a pad to a sequence):

```bash
./build/stagehand --monitor
```

Check the audio path:

```bash
./build/stagehand --tone 440
```
