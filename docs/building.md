# StageHand — Building

How to build StageHand and run its tests on Linux and macOS. The CI workflow
(`.github/workflows/ci.yml`) runs exactly these steps on both platforms, so
if this page and the workflow ever disagree, the workflow is right.

StageHand is C++20, built with CMake (3.21+) and Ninja. The code it shares
with Umpire (UMP, the clock and queues, the plugin seam and CLAP host, the
JACK/ALSA and CoreAudio/CoreMIDI backends) is **uw-core**, a private git
submodule (`external/uw-core`, namespace `uw`), which brings the CLAP SDK
(1.2.10) as its own submodule. Its tests run in uw-core. Third-party code is
vendored: DPF's DGL for the UI (`external/dpf`) and nlohmann/json
(`external/nlohmann`). Everything else is a system library.

| Platform | What builds today |
|---|---|
| Linux | everything: the `stagehand` app (rack window, plugin hosting), its libraries, the test suites |
| macOS | the libraries, the test suites, and the `stagehand` app over CoreAudio, CoreMIDI and CLAP bundles: it plays racks with the rack window and the plugins' editors, as `StageHand.app` (not yet signed or with an icon: [macos.md](macos.md), milestones 6 and 7) |

## Getting the source

The submodules (uw-core, and the CLAP SDK inside it) have to be initialized
in every checkout. uw-core is private, so the clone needs access to it:

```bash
git clone --recurse-submodules https://github.com/erickampman/StageHand.git
```

or, in an existing clone:

```bash
git submodule update --init --recursive
```

When StageHand's change needs a uw-core change, commit and push uw-core
first, then commit StageHand's new pin. Otherwise the other machine can't
fetch the pin. To have git refuse such a push:

```bash
git push --recurse-submodules=check             # this push only
git config push.recurseSubmodules check         # the default for this clone
```

The `git config` line is per clone: run it in every clone that has
submodules (StageHand, Umpire, each plugin), on the Linux PC and on the Mac.

Build from `rack-view`: it's where current work is, and `main` is far behind.

## Linux

### Dependencies

| Package (Debian/Ubuntu) | Why |
|---|---|
| `g++`, `cmake`, `ninja-build`, `pkg-config` | toolchain |
| `libjack-jackd2-dev` | JACK API (audio) |
| `libasound2-dev`, **alsa-lib 1.2.10 or newer** | ALSA sequencer in UMP mode (MIDI 1.0 and 2.0 input) |
| `libx11-dev` | the rack window and plugin editor windows |
| `libgl-dev` | OpenGL, for DGL (the rack window) |
| `libxrandr-dev` (optional) | a new window fits its monitor; without it, the whole X screen |

```bash
sudo apt install g++ cmake ninja-build pkg-config libjack-jackd2-dev libasound2-dev libx11-dev libgl-dev libxrandr-dev
```

At runtime StageHand also uses:
- `pipewire-jack`: audio goes through PipeWire's JACK layer (`pw-jack`).
- `zenity`, or `kdialog` on KDE: the Open and Save dialogs.

alsa-lib 1.2.10 means Ubuntu 24.04, Debian 13 or newer; with an older one
CMake stops at `pkg_check_modules(ALSA ...)`. On other distros, install the
same libraries under their own names (`alsa-lib`, `jack2` or `pipewire-jack`,
`libX11`, `libXrandr`, `mesa`/`libglvnd`, with their `-devel` packages where
the distro splits them).

### Build and test

```bash
cmake -B build -G Ninja
```

```bash
cmake --build build
```

```bash
ctest --test-dir build --output-on-failure
```

The default build type is `RelWithDebInfo`; add `-DCMAKE_BUILD_TYPE=Debug` to
the first command for a debug build. `build/compile_commands.json` is there
for clangd. `-DSTAGEHAND_APP=OFF` builds the libraries and tests only, with
none of the Linux dependencies above.

The test suites use fakes (`tests/FakePlugin.h`, `tests/SessionFakes.h`), so
they need no audio, MIDI, plugins or display.

### Run

```bash
./build/stagehand
```

With no arguments it reopens the last setlist at the song it was on, or opens
a new, empty rack. `./build/stagehand --help` lists everything else. To run it
as plain `stagehand`, link it into a directory on your `PATH`:

```bash
ln -s "$PWD/build/stagehand" ~/.local/bin/stagehand
```

That link points at whatever was last built in `build/`. If you build another
branch there (`main` is older, and with no arguments only prints the help),
`stagehand` changes with it. Use a separate build directory for other
branches, such as `cmake -B build-main -G Ninja`.

**Audio.** You don't need to type `pw-jack`: started without it and with no
JACK server to reach, StageHand restarts itself under `pw-jack`
([command-line.md](command-line.md), "Audio: pw-jack"). A real jackd, if one is
running, is used instead.

**Real-time priority.** When it starts, StageHand prints `MIDI thread:
SCHED_FIFO` or `normal priority`. Normal priority means you have no real-time
scheduling permission, and MIDI timestamps jitter by the thread's wake-up
time. On Debian/Ubuntu, answer yes to real-time priority when installing
`jackd2` (or run `sudo dpkg-reconfigure jackd2`), add yourself to the `audio`
group, and log in again.

## macOS

Apple Silicon, macOS 15 or later (the build sets `CMAKE_OSX_ARCHITECTURES`
to `arm64` and the deployment target to 15.0; [macos.md](macos.md),
decisions 9 and 13). The libraries, the test suites and the app build; the
app plays racks and plugins with their editors (no rack window yet).

### Dependencies

Xcode Command Line Tools (AppleClang, C++20):

```bash
xcode-select --install
```

CMake and Ninja, for example from Homebrew:

```bash
brew install cmake ninja
```

No JACK, ALSA, X11 or OpenGL packages. The macOS backends use only system
frameworks (CoreAudio, CoreMIDI, CoreFoundation, CoreGraphics, AppKit and
OpenGL; DGL is vendored).

### Build and test

```bash
cmake -B build -G Ninja
```

```bash
cmake --build build
```

```bash
ctest --test-dir build --output-on-failure
```

Then, to hear the audio path and see MIDI arrive (Ctrl-C stops each):

```bash
./build/stagehand --tone
```

```bash
./build/stagehand --monitor
```

The build makes `build/StageHand.app`. `./build/stagehand` is a small script
that runs the app's binary by its real path, so it runs as the app (its name
in the menu bar, its bundle id) with the terminal's output; a symbolic link
to the binary wouldn't. To start it as Finder does:

```bash
open build/StageHand.app
```

Launched that way, its output goes nowhere; Cmd-Q, or Quit in the Dock,
quits and saves as Ctrl-C does.

## Other platforms

Not supported. Anything other than Linux and macOS builds only with
`-DSTAGEHAND_APP=OFF`, and isn't tested.
