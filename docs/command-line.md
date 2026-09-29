# StageHand — Command line

How to build and run StageHand, and every option. `stagehand --help` prints
the same options in short form.

## Building

```bash
cmake -S . -B build -G Ninja
cmake --build build -j16
```

The binary is `build/stagehand`. The tests run with `ctest` from `build/`.

## Audio: pw-jack

StageHand is a JACK client. On a PipeWire system, JACK is PipeWire's JACK
layer, reached by running under `pw-jack`. You don't have to type it:
`--play` and `--tone` check for a JACK server first, and if there's none in
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

One mode per run. With no arguments at all, StageHand plays a new, empty
scratch rack (see "Scratch rack").

| Mode | What it does |
|---|---|
| `--play` | Host plugins: MIDI in, audio out, with the rack window |
| `--list-plugins` | List installed CLAP plugins (id, name, note dialects) |
| `--list-midi` | List MIDI sources: id, protocol, and hardware, software or app |
| `--monitor` | Print incoming MIDI exactly as it arrives |
| `--tone [HZ]` | Play a test sine (default 440 Hz) to check the audio path |
| `-h`, `--help` | Show the help |

## --play

What `--play` plays:

| Option | Meaning |
|---|---|
| `--rack PATH` | Play a rack: a `.rack` bundle (docs/rack-format.md) |
| `--plugin PATH\|ID` | Play one plugin: a `.clap` file, or an id or name from `--list-plugins`. It starts from the plugin's defaults |
| `--save-rack PATH` alone | Start a new, empty rack, built in the rack window and saved to PATH |
| none of these | A scratch rack (see below) |

`--rack` and `--plugin` can't be used together: a rack already names its
plugins.

| Option | Meaning |
|---|---|
| `--plugin-id ID` | Which plugin, when the `.clap` file holds several |
| `--open-editors` | Open every plugin's editor at start. Otherwise the rack window's Editor buttons open them |
| `--no-ui` | No rack window, console only. Every editor opens at start |
| `--no-editor` | With `--no-ui`: don't open the editors either |

### Scratch rack

`stagehand` with no arguments, or `--play` with no `--rack`, `--plugin` or
`--save-rack`, starts a new, empty rack named "Scratch" with the defaults:
120 BPM, 4/4, every hardware MIDI input plus the virtual inputs, and stereo
out. Build it in the window as usual.

A scratch rack is **never saved**: Ctrl-Shift-S says so, and quitting with
changes prints "The scratch rack's changes weren't saved." To keep a new
rack, start with `--save-rack PATH` instead. The rack window can't choose a
path, because it has no text entry.

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

A `--plugin` session without `--save-rack` has nowhere to save; Ctrl-Shift-S
then says so.

## Audio options (--play, --tone)

| Option | Meaning |
|---|---|
| `--quantum N` | Force a PipeWire buffer size of N frames (4-8192) while StageHand runs, released when it exits. It affects all audio on the system |
| `--channels N` | Output channels to register (default 2) |
| `--no-connect` | Don't connect to the system playback ports |

## MIDI options (--play, --monitor)

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
for example `KeyLab 88:KeyLab 88 MIDI 1`.

## Keys

In the rack window and in plugin editor windows:

| Key | Action |
|---|---|
| Ctrl-Q | Quit (saving, unless `--no-save-rack`) |
| Ctrl-Shift-S | Save the rack now |
| Escape | Rack window only: close a menu or panel, and cancel MIDI learn |

Closing the rack window quits the same way as Ctrl-Q. Closing a plugin editor
or a sequence editor closes just that window; StageHand keeps playing. In the
terminal, Ctrl-C quits the same way as Ctrl-Q.

## Exit status

| Code | Meaning |
|---|---|
| 0 | Success, including `--help` |
| 1 | A run failed (e.g. no audio server, or the save at quit failed) |
| 2 | Bad arguments, or a refused save target |

## Examples

Open a scratch rack to try things out:

```bash
./build/stagehand
```

Play a rack, saving it back at quit:

```bash
./build/stagehand --play --rack ~/Racks/Piano.rack
```

Start a new rack and build it in the window:

```bash
./build/stagehand --play --save-rack ~/Racks/New.rack
```

Play a gig rack at a 128-frame buffer without changing it:

```bash
./build/stagehand --play --rack ~/Racks/Gig.rack --no-save-rack --quantum 128
```

Try one plugin and keep the result as a rack:

```bash
./build/stagehand --play --plugin com.unlikelyware.polygraphmodular --save-rack ~/Racks/New.rack
```

Listen only to one keyboard:

```bash
./build/stagehand --play --rack ~/Racks/Piano.rack --midi "KeyLab 88:KeyLab 88 MIDI 1"
```

See what a controller sends (useful before mapping a pad to a sequence):

```bash
./build/stagehand --monitor
```

Check the audio path:

```bash
./build/stagehand --tone 440
```
