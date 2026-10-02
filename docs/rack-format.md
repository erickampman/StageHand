# StageHand — Rack file format (v1)

A **Rack** is one complete setup: which plugins run, their saved state, and how
MIDI reaches each of them. It's the Session model's persistent form
(domain-model.md, "Session model" and "Save / restore"). Until StageHand has a
UI, rack files are also meant to be readable and hand-editable.

## Making a rack

In the rack window (docs/rack-view.md):

1. Start a new, empty rack: `stagehand --save-rack ~/Racks/Piano.rack`
   (or start from one plugin: `--plugin <id> --save-rack ...`).
2. **+ Add plugin** adds a row. Each row's input side picks its MIDI source (the
   source menu), drags its key range, and opens its filter panel (channels,
   groups, keys, velocity, transpose). Overlapping rows are layers; disjoint key
   ranges are splits. The row's **...** menu enables, disables, removes, and
   picks the output pair.
3. Pick patches in the plugins' editors (**Editor**), set levels, and quit
   (Ctrl-Q, or close the window): the rack is saved with everything. Play it
   again, and save back to it, with `stagehand --rack ~/Racks/Piano.rack`.

`rack.json` stays readable and hand-editable (the example below), for anything
the window doesn't do yet.

**Saving (default on):**
- A `--rack` is saved back to itself when StageHand quits, with each plugin's
  current state. **Ctrl-Shift-S** (or **Save**) in the rack window, or
  Ctrl-Shift-S in any editor window, saves right away without quitting.
- `--no-save-rack` turns saving off entirely, e.g. for a gig where accidental
  tweaks shouldn't stick.
- `--save-rack PATH` saves somewhere else instead ("save as"). It's also the only
  way to keep a `--plugin` session, which has no rack of its own.

Saving is guarded, and StageHand refuses before any audio starts in two cases:
- `--save-rack` names an existing rack *other* than the one being played, e.g.
  `--plugin X --save-rack Piano.rack` when `Piano.rack` already exists. That would
  replace the saved rack with a fresh one. Add `--overwrite` to do it on purpose.
- The save path exists but isn't a rack. That's always refused, even with
  `--overwrite`.

## Bundle layout

A rack is a directory ending in `.rack`:

```
Split Bass Pad.rack/
  rack.json          structure and host-owned settings (human-readable)
  state/
    bass.bin         one opaque plugin-state blob per slot (plugin-defined bytes)
    pad.bin
```

Plugin state stays out of the JSON because it's opaque, often binary, and can be
large. Base64 in JSON would bloat it and make diffs useless. A single-file (zip)
form can come later for sharing; a directory needs no extra dependency.

**Saving is safe.** StageHand writes a complete new bundle next to the old one
(`X.rack.tmp`), then swaps it into place. A crash mid-save leaves either the old
rack or the new one intact, never a mix.

## rack.json

```json
{
  "format": "stagehand-rack",
  "version": 2,
  "name": "Split: bass + pad",
  "tempo": 120.0,
  "timeSignature": [4, 4],
  "slots": [
    {
      "id": "bass",
      "name": "Bass",
      "plugin": {
        "format": "clap",
        "id": "com.unlikelyware.polygraphmodular",
        "path": "~/.clap/PolygraphModular.clap"
      },
      "state": "state/bass.bin",
      "muted": false,
      "gainDb": 0.0,
      "output": 1,
      "midi": {
        "source": "device:KeyLab 88:KeyLab 88 MIDI 1",
        "channels": "all",
        "groups": "all",
        "notes": [0, 59],
        "velocity": [0, 127],
        "transpose": 12,
        "notePort": 0
      }
    },
    {
      "id": "pad",
      "name": "Pad",
      "plugin": { "format": "clap", "id": "com.unlikelyware.polygraphmodular" },
      "state": "state/pad.bin",
      "midi": { "source": "all", "notes": [60, 127] }
    }
  ]
}
```

That example is a **split**: notes below middle C play the bass slot an octave
up, and the rest play the pad. Give two slots overlapping note ranges and you
get a **layer**.

### Top level

| Field | Required | Meaning |
|---|---|---|
| `format` | yes | Always `"stagehand-rack"` |
| `version` | yes | Format version. A newer version is refused, not guessed at |
| `name` | no | Display name; defaults to the bundle's name |
| `tempo` | no | The host tempo in BPM (quarter notes), 20-400; default 120. Plugins get it through CLAP's transport, and sequences follow it |
| `timeSignature` | no | `[numerator, denominator]`: 1-32 over 1, 2, 4, 8, 16 or 32; default `[4, 4]` |
| `slots` | yes | Plugin slots, in order |
| `sequences` | no | The rack's step sequences; see "Sequences" |
| `transport` | no | The transport's MIDI control; see "Transport" |
| `metronome` | no | What the beat lights and click count, and the click; see "Metronome" |

### Slot

| Field | Default | Meaning |
|---|---|---|
| `id` | required | Unique within the rack; letters, digits, `-`, `_`. Names the state file |
| `name` | `id` | Display name |
| `plugin.format` | required | `"clap"` (later `"vst3"`) |
| `plugin.id` | required | The plugin's own id (as `--list-plugins` shows it) |
| `plugin.path` | none | Where it was last found. A hint only: if it's wrong, StageHand scans for the id |
| `state` | none | Path of the state blob, relative to the bundle. Absent means the plugin's defaults |
| `muted` | `false` | Silent: muting and unmuting fade over 10 ms. Muting also ends the plugin's held notes. Once its output has gone quiet (or after 5 s), the plugin isn't run, so it costs no CPU. It's still loaded, so unmuting is instant. This replaced `enabled` on 2026-10-01: `"enabled": false` in an older file loads as muted, and `enabled` isn't written any more. The format version is unchanged |
| `gainDb` | `0.0` | Level when summed into the outputs |
| `sends` | both off | Levels sent to the rig's effect buses A and B, after the fader (docs/rig.md): `[dbA, dbB]` in dB, `null` for off. Added 2026-10-02; additive, so the format version is unchanged |
| `output` | `1` | First output channel (1-based) for the plugin's first channel. A mono plugin feeds two |
| `midi.source` | `"all"` | See "Sources" |
| `midi.channels` | `"all"` | `"all"` or a list of channels 1–16 |
| `midi.groups` | `"all"` | `"all"` or a list of UMP groups 1–16 |
| `midi.notes` | `[0, 127]` | Inclusive range of *played* keys (before transpose) |
| `midi.velocity` | `[0, 127]` | Inclusive note-on velocity range, on the familiar 0–127 scale (compared at MIDI 2.0 resolution) |
| `midi.transpose` | `0` | Semitones. MIDI 2.0 pitch 7.9 attributes move with the note |
| `midi.notePort` | `0` | Which of the plugin's note inputs receives it |

Unknown fields are ignored when loading and dropped when saving (v1 doesn't
round-trip them).

## Sources

`midi.source` names where a slot's MIDI comes from:

| Value | Meaning |
|---|---|
| `"all"` | Every open input: all hardware by default, plus the virtual inputs |
| `"device:<id>"` | One MIDI device, by its `--list-midi` id, e.g. `device:KeyLab 88:KeyLab 88 MIDI 1` |
| `"virtual:<name>"` | One of StageHand's virtual inputs: `virtual:MIDI 2.0 In`, `virtual:MIDI 1.0 In` |
| `"none"` | No live input: the slot hears only the sequences that play into it (see "Sequences") |

Sequences aren't a slot's source: a sequence lists the slots it plays into
(`targets`), and a slot hears those on top of its source. A slot set to
`"all"` never hears a sequence it isn't a target of. (Format 1 had
`"sequence:<id>"` here instead; such a slot loads as one of that sequence's
targets with source `"none"`, so it plays exactly as before.)

If a named device isn't connected when the rack loads, the slot still loads,
hears nothing from that source, and StageHand warns loudly at startup. It
deliberately does *not* fall back to "all": a split silently becoming a layer is
a worse surprise on stage than silence.

## Missing plugins

If a slot's plugin can't be found or fails to load, the slot is kept as a
**placeholder**: it doesn't run, StageHand warns, and saving the rack writes the
slot and its original state back unchanged. Installing the plugin later recovers
the slot exactly.

## Sequences

Step sequences the rack owns (docs/sequences.md). A sequence is a MIDI
*source*: it doesn't make sound, it plays into the slots listed in its
`targets`, on top of each slot's own source (so a slot keeps its keyboard). One
sequence can feed several slots, one slot can be fed by several sequences, and
each slot's filter applies to it as to any source. They're kept separate from
`slots` in the file, and live in a sources dock below the rack in the UI.

```json
"sequences": [
  {
    "id": "arp",
    "name": "Arp",
    "protocol": 2,
    "edo": 31,
    "referenceNote": 60,
    "channel": 1,
    "group": 1,
    "rate": "4",
    "length": 16,
    "launch": "bar",
    "repeat": true,
    "noteOffOffsetMs": 0,
    "notes": [[0, 2, 0, 50000], [4, 1, 10, 50000]]
  }
]
```

| Field | Default | Meaning |
|---|---|---|
| `id` | required | Unique among the sequences; letters, digits, `-`, `_` |
| `name` | `id` | Display name |
| `protocol` | `2` | `1` (notes are note numbers) or `2` (notes are EDO steps, sent with pitch 7.9) |
| `edo`, `referenceNote` | `12`, `60` | MIDI 2.0: steps per octave, and the MIDI note step 0 sounds at |
| `channel`, `group` | `1`, `1` | What it sends on (1-16) |
| `rate` | `"4"` | Units per beat: `"4"` is sixteenths, `"1/2"` a unit every two beats, `"3"` triplet eighths |
| `length` | `16` | Loop length in units (1-4096) |
| `launch` | `"bar"` | Where Play and Stop take effect: `"bar"`, `"beat"` or `"immediate"` |
| `repeat` | `true` | Off: plays once from its start, then stops |
| `noteOffOffsetMs` | `0` | Moves every note-off by this much (-1000 to 1000): positive for legato, negative for staccato |
| `notes` | `[]` | `[start, length, pitch, velocity]`: start and length in units, within `length`; pitch is a note number (1.0) or an EDO step relative to `referenceNote` (2.0, may be negative); velocity 1-127 (1.0) or 1-65535 (2.0) |
| `atStart` | `false` | Starts on beat 1 whenever the transport starts |
| `triggers` | `[]` | MIDI controls that start or stop it; see "Triggers" |
| `targets` | `[]` | The slot ids it plays into (format 2). Empty: it plays nowhere yet |

A target naming a slot the rack doesn't have is an error, not a silent gap:
the file is inconsistent. In the UI, removing a slot takes it out of every
sequence's targets, and removing a sequence simply stops its slots hearing it.

**Format versions.** 2 added `targets` and the `"none"` source. StageHand
reads format 1 (converting `"sequence:<id>"` sources, above) and writes 2; an
older StageHand refuses a format-2 rack ("made by a newer StageHand") rather
than loading it with its sequences silently playing nowhere.

### Triggers

A note or controller that starts or stops a sequence (docs/sequences.md,
section 4). Usually made with MIDI learn in the dock.

```json
"triggers": [
  {"action": "toggle", "source": "device:Pads:Pads MIDI 1", "note": 36, "channel": 10, "group": 1},
  {"action": "stop", "source": "all", "controller": 64, "channel": 1, "group": 1}
]
```

| Field | Default | Meaning |
|---|---|---|
| `action` | `"toggle"` | `"toggle"` (Play/Stop), `"start"` or `"stop"`; quantized by the sequence's `launch` |
| `source` | `"all"` | The input: `"all"`, `"device:<id>"` or `"virtual:<name>"`, as for a slot. Not a sequence |
| `note` or `controller` | required, one of them | 0-127. A note acts on its note-on; a controller when its value rises to 64 or more |
| `channel`, `group` | `1`, `1` | 1-16 |

Messages that match a trigger go only to it. A trigger whose device isn't
connected is kept and does nothing, with a warning at load.

## Transport

The transport starts stopped; Start puts bar 1 at that moment, Stop ends every
sequence and returns to bar 1 (docs/sequences.md, section 7). Its MIDI
control:

```json
"transport": {
  "triggers": [{"action": "toggle", "source": "device:Pads:Pads MIDI 1", "note": 40, "channel": 10}],
  "startOnNote": "off"
}
```

| Field | Default | Meaning |
|---|---|---|
| `triggers` | `[]` | As a sequence's triggers ("Triggers"); the action starts or stops the transport |
| `startOnNote` | `"off"` | While stopped, the first note-on from this input starts it: `"off"`, `"all"` (any input), `"device:<id>"` or `"virtual:<name>"`. The note still plays |

Written only when it differs from the defaults.

## Metronome

```json
"metronome": {"count": "beats", "click": true, "clickLevel": -12, "clickOutput": "Built-in Audio Analog Stereo"}
```

| Field | Default | Meaning |
|---|---|---|
| `count` | `"beats"` | `"beats"`: the counted beats (6/8: 2, 7/8: 7); `"notes"`: every note (6/8: 6) |
| `click` | `false` | The click, on its own outputs |
| `clickLevel` | `-12` | dB, -60 to 0 |
| `clickOutput` | `""` | The playback device it's connected to, by the name PipeWire shows. Missing: see docs/sequences.md, section 7 (never the main outputs) |

Written only when it differs from the defaults.

## Versioning

`version` counts incompatible changes. A newer rack is refused with a clear
message, and older versions are migrated on load. Plugin state blobs are
versioned by the plugins themselves and passed through untouched.
