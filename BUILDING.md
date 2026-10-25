# Building StageHand

How to build StageHand and run its tests on Linux and macOS. The CI workflow
(`.github/workflows/ci.yml`) runs the same steps on both platforms, so if this page and the
workflow ever disagree, the workflow is right.

StageHand is C++20, built with CMake and Ninja. The code it shares with Umpire (UMP, the clock and
queues, the plugin seam and CLAP host, the JACK/ALSA and CoreAudio/CoreMIDI backends) is
**uw-core**, a git submodule (`external/uw-core`, namespace `uw`), which brings the CLAP SDK as its
own submodule. Its tests run in uw-core. DPF's DGL, for the UI, is the DPF fork (`erickampman/DPF`,
a submodule at `external/dpf`), which pins the pugl fork (`erickampman/pugl`) as its own submodule:
one copy, with every local patch, shared by StageHand, Umpire and the plugins
(`external/dpf/README.fork.md`). nlohmann/json is vendored (`external/nlohmann`). Everything else
is a system library. While the repos are private, cloning needs access to uw-core, DPF and pugl.

| Platform | What builds today |
|---|---|
| Linux | everything: the `stagehand` app (rack window, plugin hosting), its libraries, the test suites |
| macOS | the libraries, the test suites, and the `stagehand` app over CoreAudio, CoreMIDI and CLAP bundles: it plays racks with the rack window and the plugins' editors, as `StageHand.app` (not yet signed: [docs/macos.md](docs/macos.md)) |

## What you need

**macOS:** macOS 26 or later on Apple Silicon (arm64). Intel Macs aren't supported.

- The Xcode Command Line Tools: `xcode-select --install`. They bring clang, the macOS SDK, git and
  Python 3.
- [Homebrew](https://brew.sh), then `brew install cmake ninja`.
- Full Xcode is optional: Xcode 26 or later compiles the app icon. Without it the app builds
  with no icon, and CMake says so.

**Linux:** a C++20 compiler (GCC 11+ or Clang 14+), CMake 3.21+, Ninja, Python 3 and git. On Debian
or Ubuntu this installs them and every library the build needs:

```bash
sudo apt install build-essential cmake ninja-build pkg-config python3 git \
  libgl1-mesa-dev libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxinerama-dev \
  libxcb1-dev libx11-xcb-dev libxcb-util-dev libxkbcommon-dev libxkbcommon-x11-dev \
  libfontconfig1-dev libasound2-dev libjack-jackd2-dev \
  libxfixes-dev libxi-dev libxss-dev libxtst-dev libwayland-dev wayland-protocols \
  libegl1-mesa-dev libgles2-mesa-dev libdrm-dev libgbm-dev libdecor-0-dev libudev-dev \
  libdbus-1-dev libibus-1.0-dev
```

It's one list for every Unlikelyware project (the same one CI installs), so it covers more than
StageHand needs. On other distros, install the same libraries under their own names.

On Linux StageHand needs alsa-lib 1.2.10 or later (Ubuntu 24.04, Debian 13 or newer) for the ALSA
sequencer in UMP mode; with an older one CMake stops at `pkg_check_modules(ALSA ...)`. On macOS it
uses only system frameworks (CoreAudio, CoreMIDI, CoreFoundation, CoreGraphics, AppKit and OpenGL).

At runtime on Linux StageHand also uses:
- `pipewire-jack`: audio goes through PipeWire's JACK layer (`pw-jack`).
- `zenity`, or `kdialog` on KDE: the Open and Save dialogs.

## Get the source

```bash
git clone https://github.com/erickampman/StageHand.git
cd StageHand
tools/fetch_deps.sh
```

`tools/fetch_deps.sh` checks out the git submodules at their pinned commits: uw-core (with the CLAP
SDK inside it) and DPF (with pugl inside it). Run it again after a `git pull` that moves, adds or
re-points a submodule: a plain pull leaves them where they were.

If the update refuses because `external/dpf` isn't empty (files left from an old vendored copy,
such as build output), remove the directory (`rm -rf external/dpf`) and run the script again.

Build from `main`.

## Build and test

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

The same three commands on both platforms. Everything goes in `build/` (ignored by git).

Without `-DCMAKE_BUILD_TYPE` the build type is `RelWithDebInfo`; `-DCMAKE_BUILD_TYPE=Debug` gives a
debug build. `build/compile_commands.json` is there for clangd. `-DSTAGEHAND_APP=OFF` builds the
libraries and tests only, with none of the libraries above.

The test suites use fakes (`tests/FakePlugin.h`, `tests/SessionFakes.h`), so they need no audio,
MIDI, plugins or display.

## Running it on Linux

```bash
./build/stagehand
```

With no arguments it reopens the last setlist at the song it was on, or opens a new, empty rack.
`./build/stagehand --help` lists everything else. To run it as plain `stagehand`, link it into a
directory on your `PATH`:

```bash
ln -s "$PWD/build/stagehand" ~/.local/bin/stagehand
```

That link points at whatever was last built in `build/`. Use a separate build directory for other
branches, such as `cmake -B build-main -G Ninja -DCMAKE_BUILD_TYPE=Release`.

**In the dock and app grid.** StageHand's windows carry its icon, so the dock shows it however
StageHand was started. To launch it from the app grid and keep it in the dock, install a launcher
and the icons for your user (in `~/.local/share`; it runs this checkout's `build/stagehand`):

```bash
tools/install-desktop-entry.sh
```

**Keeping it without the clone.** The link and the launcher run this clone's `build/stagehand`. To
keep StageHand after deleting the clone, copy it with its help page (Help looks for `help/` beside
the executable) and point both at the copy:

```bash
mkdir -p ~/.local/lib/stagehand ~/.local/bin
rm -rf ~/.local/lib/stagehand/stagehand ~/.local/lib/stagehand/help
cp -R build/stagehand build/help ~/.local/lib/stagehand/
ln -sf ~/.local/lib/stagehand/stagehand ~/.local/bin/stagehand
tools/install-desktop-entry.sh ~/.local/lib/stagehand/stagehand
```

**Audio.** You don't need to type `pw-jack`: started without it and with no JACK server to reach,
StageHand restarts itself under `pw-jack` ([docs/command-line.md](docs/command-line.md), "Audio:
pw-jack"). A real jackd, if one is running, is used instead.

**Real-time priority.** When it starts, StageHand prints `MIDI thread: SCHED_FIFO` or `normal
priority`. Normal priority means you have no real-time scheduling permission, and MIDI timestamps
jitter by the thread's wake-up time. On Debian/Ubuntu, answer yes to real-time priority when
installing `jackd2` (or run `sudo dpkg-reconfigure jackd2`), add yourself to the `audio` group, and
log in again.

## Running it on macOS

To hear the audio path and see MIDI arrive (Ctrl-C stops each):

```bash
./build/stagehand --tone
```

```bash
./build/stagehand --monitor
```

The build makes `build/StageHand.app`. `./build/stagehand` is a small script that runs the app's
binary by its real path, so it runs as the app (its name in the menu bar, its bundle id) with the
terminal's output; a symbolic link to the binary wouldn't. To start it as Finder does:

```bash
open build/StageHand.app
```

Launched that way, its output goes nowhere; Cmd-Q, or Quit in the Dock, quits and saves as Ctrl-C
does.

To keep it in Applications (and after deleting the clone), copy the app there. An app you built
yourself has no quarantine flag, so it opens without the steps a downloaded one needs:

```bash
rm -rf /Applications/StageHand.app
cp -R build/StageHand.app /Applications/
```

## Other platforms

Not supported. Anything other than Linux and macOS builds only with `-DSTAGEHAND_APP=OFF`, and
isn't tested.
