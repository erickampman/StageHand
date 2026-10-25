# StageHand

A live-performance host for CLAP plugins, with MIDI 1.0 and MIDI 2.0 both
first-class: racks of plugins, setlists, a rig with effect buses, a sequencer
and a looper. Linux today (JACK through PipeWire, the ALSA sequencer, a DGL
rack window); a macOS port is under way.

- [Building on Linux and macOS](BUILDING.md)
- [Command line](docs/command-line.md), or `stagehand --help`
- Design: [architecture](docs/architecture.md),
  [domain model](docs/domain-model.md), [rack format](docs/rack-format.md),
  [setlists](docs/setlists.md), [rig](docs/rig.md),
  [MIDI effects](docs/midi-effects.md),
  [macOS port](docs/macos.md)

## Installing

**macOS:** drag `StageHand.app` into Applications. The 0.9 previews aren't notarized, so the first
time macOS refuses to open it: open System Settings ▸ Privacy & Security and click Open Anyway. Or
clear the quarantine flag yourself:

```bash
xattr -dr com.apple.quarantine /Applications/StageHand.app
```

**Linux:** unpack the download anywhere you'll keep it, such as `~/.local/lib/stagehand`, and keep
`stagehand` and its `help` folder together (Help looks for `help/` beside the executable). To run
it as plain `stagehand`, link it into a directory on your `PATH`:

```bash
ln -sf ~/.local/lib/stagehand/stagehand ~/.local/bin/stagehand
```

Plugins go in the CLAP folder, where StageHand looks for them: `~/.clap` on Linux,
`~/Library/Audio/Plug-Ins/CLAP` on macOS.

## Building from source

```bash
git clone https://github.com/erickampman/StageHand.git
cd StageHand
tools/fetch_deps.sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

[BUILDING.md](BUILDING.md) has what to install first on Linux and macOS, the tests, and running it.

MIT licensed; see [LICENSE](LICENSE).
