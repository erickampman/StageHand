# StageHand — Rack file format (v1)

A **Rack** is one complete setup: which plugins run, their saved state, and how
MIDI reaches each of them. It's the Session model's persistent form
(domain-model.md, "Session model" and "Save / restore"). Until StageHand has a
UI, rack files are also meant to be readable and hand-editable.

## Making a rack (until there's a UI)

1. Start from one plugin and save it as a rack. Pick a patch in the plugin's
   editor, then quit (Ctrl-Q in the editor window, or Ctrl-C in the terminal);
   the rack is saved with that state:
   `stagehand --play --plugin <id> --save-rack ~/Racks/Piano.rack`
2. Edit `rack.json` by hand to add slots, splits, and layers (the example below).
   A new slot can reuse another's state file, or omit `state` to start from the
   plugin's defaults.
3. Play it and adjust the plugins in their editors. The rack is saved back when
   you quit: `stagehand --play --rack ~/Racks/Piano.rack`

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
  "version": 1,
  "name": "Split: bass + pad",
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
      "enabled": true,
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
| `slots` | yes | Plugin slots, in order |
| `sequences` | reserved | See "Reserved: sequences" |

### Slot

| Field | Default | Meaning |
|---|---|---|
| `id` | required | Unique within the rack; letters, digits, `-`, `_`. Names the state file |
| `name` | `id` | Display name |
| `plugin.format` | required | `"clap"` (later `"vst3"`) |
| `plugin.id` | required | The plugin's own id (as `--list-plugins` shows it) |
| `plugin.path` | none | Where it was last found. A hint only: if it's wrong, StageHand scans for the id |
| `state` | none | Path of the state blob, relative to the bundle. Absent means the plugin's defaults |
| `enabled` | `true` | A disabled slot is loaded but not run (quick to re-enable) |
| `muted` | `false` | A muted slot runs but is silent. Muting and unmuting fade over 10 ms. It's the live-safe way to silence a slot, since disabling cuts it off, tails included |
| `gainDb` | `0.0` | Level when summed into the outputs |
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
| `"sequence:<id>"` | **Reserved** for sequences (see below) |

If a named device isn't connected when the rack loads, the slot still loads,
hears nothing from that source, and StageHand warns loudly at startup. It
deliberately does *not* fall back to "all": a split silently becoming a layer is
a worse surprise on stage than silence.

## Missing plugins

If a slot's plugin can't be found or fails to load, the slot is kept as a
**placeholder**: it doesn't run, StageHand warns, and saving the rack writes the
slot and its original state back unchanged. Installing the plugin later recovers
the slot exactly.

## Reserved: sequences

Sequences aren't designed yet, but the format leaves room for them:

- A future top-level `sequences` array holds them, **separate from `slots`**. A
  sequence is a MIDI *source* the rack owns. It doesn't make sound; it feeds
  slots.
- A slot selects one as its input with `"source": "sequence:<id>"`, just as it
  selects a device.
- That separation is a data-model choice. The UI matches it: sequences live in
  a sources dock below the rack, not as rack rows, and slots show them as
  their input (architecture.md, "Sequencer UI").

## Versioning

`version` counts incompatible changes. A newer rack is refused with a clear
message, and older versions are migrated on load. Plugin state blobs are
versioned by the plugins themselves and passed through untouched.
