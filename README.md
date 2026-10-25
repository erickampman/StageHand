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
