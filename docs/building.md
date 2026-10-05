# StageHand — Building

How to build StageHand and run its tests on Linux and macOS. The CI workflow
(`.github/workflows/ci.yml`) runs exactly these steps on both platforms, so
if this page and the workflow ever disagree, the workflow is right.

StageHand is C++20, built with CMake (3.21+) and Ninja. Third-party code is
vendored: the CLAP SDK as a git submodule (`external/clap`, pinned to 1.2.10),
DPF's DGL for the UI (`external/dpf`) and nlohmann/json (`external/nlohmann`).
Everything else is a system library.

| Platform | What builds today |
|---|---|
| Linux | everything: the `stagehand` app (rack window, plugin hosting), its libraries, the test suites |
| macOS | the platform-free libraries (core, engine, session, app) and the test suites; no app until the CoreMIDI and CoreAudio backends exist ([macos.md](macos.md), milestones 3 onward) |

## Getting the source

The CLAP submodule has to be initialized in every checkout:

```bash
git clone --recurse-submodules https://github.com/erickampman/StageHand.git
```

or, in an existing clone:

```bash
git submodule update --init
```

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
for clangd. `-DSTAGEHAND_APP=OFF` builds what macOS builds: the libraries and
tests, with none of the Linux dependencies above.

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
decisions 9 and 13). Only the libraries and the test suites build so far.

### Dependencies

Xcode Command Line Tools (AppleClang, C++20):

```bash
xcode-select --install
```

CMake and Ninja, for example from Homebrew:

```bash
brew install cmake ninja
```

No JACK, ALSA, X11 or OpenGL packages. The macOS backends will use only
system frameworks (CoreAudio, CoreMIDI, AppKit).

### Build and test

`-DSTAGEHAND_APP=OFF` is required: without it CMake stops with "The app
builds on Linux only, so far".

```bash
cmake -B build -G Ninja -DSTAGEHAND_APP=OFF
```

```bash
cmake --build build
```

```bash
ctest --test-dir build --output-on-failure
```

## Other platforms

Not supported. Anything other than Linux builds only with
`-DSTAGEHAND_APP=OFF`, and only macOS is tested (by CI).
