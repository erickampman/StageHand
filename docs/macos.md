# StageHand — macOS (plan)

Proposed 2026-10-03, reviewed the same day (notes under "Review");
milestone 1 built (notes under "Milestones"). StageHand on macOS 15 (Sequoia) and later, keeping DGL for the UI
(no native Cocoa interface). Builds on:
- architecture.md, sections 1 to 3: MIDI, audio and plugins sit behind seams
  (`MidiInput`, `AudioBackend`, `PluginFactory`) so CoreMIDI and CoreAudio
  can be added without touching routing, the engine or the session.
- command-line.md, "MIDI devices coming and going": the hotplug design
  (`MidiInput::changes()` and `sync()`) a CoreMIDI input implements too.
- rig.md: the rig holds the click's output (2026-10-03), the same in every
  song.

## Where things stand (2026-10-03)

About 10,300 lines (the engine, sequencer and looper, Session, the rack, rig
and setlist files, the controller and the DGL widgets) are plain C++20 with
no Linux dependency. About 2,200 lines are Linux-specific, most of them
behind the seams already:

| Area | Linux | macOS | Size |
|---|---|---|---|
| Audio | `JackBackend` (JACK, through PipeWire) | `CoreAudioBackend` (a HAL IOProc, outputs only; a second device for the click) | Large |
| MIDI | `AlsaSeqInput` (sequencer, UMP mode, two clients) | `CoreMidiInput` (UMP, two input ports) | Medium |
| Plugin editors | `EditorWindow` (a raw X11 window) | A window holding the plugin's `NSView` (decision 11) | Medium |
| Main loop | `MainLoop` (`poll()`, `eventfd`) | DGL's (Cocoa's) loop drives; the timers as idle callbacks | Small to medium |
| CLAP loading | `dlopen` of `.clap` files; `~/.clap`, `/usr/lib/clap` | `.clap` bundles (CFBundle); `~/Library/Audio/Plug-Ins/CLAP`, `/Library/Audio/Plug-Ins/CLAP` | Small |
| File dialogs | zenity or kdialog, as a process | DPF's file browser, on both (decision 10) | Small |
| Monitor size | X11 / XRandR (`Ui/Monitor.cpp`) | CoreGraphics (plain C) | Tiny |
| Paths | XDG (`~/.local/share`, `~/.local/state`) | `~/Library/Application Support/StageHand` | Tiny |
| `main.cpp` | the pw-jack restart, `--quantum`, `/proc/self/exe` | left out; the buffer size and device set through CoreAudio | Small |

**The toolchain is checked (2026-10-03):** the libraries and every test suite
build with clang and with libc++ (Apple's standard library) without a single
warning, and all 12 suites pass, on Linux. Apple's clang is close to it; no
`<format>`, ranges or `jthread` is used, and `std::atomic<double>` is
lock-free on arm64.

## Decisions (proposed; 3, 9 and 10 decided 2026-10-03)

1. **DGL stays** for the rack window and the sequence editor (pugl's Cocoa
   backend). No native menu bar or controls.
2. **Outputs only.** StageHand has no audio inputs (`AudioConfig::
   inputChannels` is 0 and the engine never reads input; the rig's "audio
   into plugins" is rows feeding bus effects, inside StageHand). So no
   microphone permission, and the backend opens an output device only.
3. **The click can go to another interface** (decided 2026-10-03: headphones
   on a second device, say), as it can on Linux today, where PipeWire
   adapts between devices. On macOS StageHand makes a private aggregate
   device (`AudioHardwareCreateAggregateDevice` with
   `kAudioAggregateDeviceIsPrivateKey`: not shown to other apps, gone when
   StageHand quits) of the main device and the click's. The main device is
   the clock; the click's device is set to the main device's sample rate
   first and gets drift compensation (`kAudioSubDeviceDriftCompensationKey`),
   so CoreAudio keeps the two in step and the engine still sees one device
   and one callback. The main outputs and the bus pairs are channels of the
   main device; the click's pair is any two channels of either.
   `AuxRouting` keeps its shape: its playback devices are the system's
   output devices, and connecting the click to one rebuilds the aggregate (a
   short gap in the sound, so it's a setup action, not a stage one). With
   the click on the main device, no aggregate is made. The click's output is
   the rig's (rig.md, 2026-10-03), the same in every song, so a song switch
   never rebuilds the aggregate; a rack only says whether it clicks, and how
   loud. The aggregate may add a little latency (the larger of the two
   devices' safety offsets): measure it.
4. **One clock, one convention.** MIDI timestamps and audio block times share
   a clock: `CLOCK_MONOTONIC` on Linux, host time (`mach_absolute_time`, in
   nanoseconds through `mach_timebase_info`) on macOS, where CoreMIDI and
   CoreAudio both report it; a `nowNanoseconds()` in Core, used by every
   backend. And a block's time means the same on both: **when its period
   began**, as JACK's `jack_get_cycle_times()` gives it, since the engine
   places each captured message in the block by it. CoreAudio's output time
   is later (the buffer plus the device's latency): the backend passes the
   IOProc's "now" instead (see "CoreAudio backend"). An engine test states
   the convention, so neither backend drifts from it.
5. **Devices by name across platforms.** A rack names MIDI devices by id;
   ALSA's are `"<client>:<port>"` (`"KeyLab 88:KeyLab 88 MIDI 1"`), and
   CoreMIDI's are made the same shape, `"<device>:<endpoint>"`, so most
   match on both. A device that isn't found by id is looked for by name (the
   device's, then the endpoint's or port's), on both platforms; a saved rack
   keeps its id until the device is chosen again.
6. **The rack bundle is a package.** `.rack` declared as a package type in
   the app's `Info.plist`, so Finder (and the file dialogs) treat a rack as
   one file. The folder format doesn't change.
7. **Cmd, not Ctrl.** Ctrl-S, Ctrl-N, Ctrl-O, Ctrl-Q become Cmd (DGL's
   `kModifierSuper`) on macOS, and the menus say so. Cmd-Q is handled in the
   window (a pugl app has no menu bar).
8. **OpenGL** (DPF's NanoVG backend) is deprecated on macOS but works,
   Apple Silicon included. Kept until it doesn't.
9. **macOS 15 or later** (decided 2026-10-03). CoreMIDI's UMP API, audio
   workgroups and everything else here are there; one version to test.
10. **DPF's file browser on Linux too** (decided 2026-10-03), so both
    platforms share one file-dialog path and `FileDialog`'s zenity and
    kdialog processes go. On Linux DPF uses the desktop's portal
    (xdg-desktop-portal, over D-Bus: `libdbus-1-dev` at build, else DPF's
    own plainer browser). To check when it's built: choosing a folder (a
    rack bundle on Linux), the setlist filter (`*.setlist`), and a suggested
    name when saving. DPF is vendored (`external/dpf`), not a submodule, so
    what's missing is patched there.
11. **Plugin editors in a plain window.** The plugin draws into an `NSView`
    the host gives it; the host's window only holds it. First try a DGL
    (pugl) window and hand the plugin its view; if DGL's OpenGL view gets in
    the way (it may keep drawing under the plugin's), a small Objective-C++
    helper (`.mm`, about 80 lines: an `NSWindow`, its content view given to
    the plugin, resized when the plugin asks). That helper isn't StageHand
    UI, so it doesn't go against decision 1. Not SwiftUI: SwiftUI would wrap
    the same `NSView` (`NSViewRepresentable`) and add a second language and
    toolchain for a window with nothing of its own; Swift with AppKit works
    but costs the same for 80 lines. On Linux the editor window becomes a DGL
    window too, so the raw X11 code goes.
12. **The main output device:** deferred to milestone 4 (decided
    2026-10-03); see "Open questions".
13. **arm64 only.** An arm64 process can't load x86_64 plugins (Rosetta
    doesn't mix architectures in one process), so a universal StageHand
    wouldn't help its plugins. The scan lists a plugin built only for Intel
    as such ("Intel only: not loadable"), not as broken.
14. **Developer ID, not the App Store.** Hosting plugins and opening racks
    anywhere rule out the App Sandbox, which the App Store requires.

## Engine and backends

### CoreAudio backend
- **A HAL IOProc** (`AudioDeviceCreateIOProcID`) on the device (or the
  aggregate), not AUHAL: it's given the time now and the output's time, so it
  passes now as the block's start (decision 4), and it suits the aggregate.
- **Denormals flushed** in the callback (FTZ: the FPCR's FZ bit on arm64),
  so a reverb's dying tail can't cost CPU spikes. JACK's callback gets the
  same (FTZ/DAZ on x86, FZ on arm64): nothing sets it today.
- **The buffer size** through `kAudioDevicePropertyBufferFrameSize` (what
  `--quantum` does on Linux, without touching other apps), the sample rate
  through the device's nominal rate.
- **Property listeners:** the sample rate changing (prepared racks dropped
  and remade, as now), the device list (hotplug), the device going away
  (unplugged, or the Mac sleeping). The aggregate is rebuilt when either of
  its devices comes back, and the click waits silent until its device is
  there, as on Linux.
- **Output channels:** the main pair and the buses' pairs as channel numbers
  on the main device; the click's pair on the main device or, through the
  aggregate, on another (decision 3).
- **Real-time:** CoreAudio's I/O thread. Plugins that start threads should
  join its workgroup (`os_workgroup`); CLAP's thread pool extension runs on
  the host's threads, so this is the host's job when it's added.

### CoreMIDI input
- **Two input ports,** one per protocol (`MIDIInputPortCreateWithProtocol`
  fixes the protocol per port, not per connection), as the ALSA input has
  two clients. Each source is connected to the port of its own protocol
  (`kMIDIPropertyProtocolID`, with the global preference), and the
  connection's reference names its handle. UMP arrives through
  `MIDIEventList`, wrapped as now: MIDI 1.0 as MT 0x2, MIDI 2.0 as MT 0x4.
- **"From other apps":** `MIDIDestinationCreateWithProtocol`, one per
  protocol, named as now.
- **Ids** in the ALSA shape, `"<device>:<endpoint>"` (decision 5).
- **Timestamps:** the packet's host time (decision 4), or now if it's 0.
- **Hotplug:** the client's notifications (objects added and removed) bump
  `changes()`; `sync()` reconnects a source that came back (by its unique
  id, then its name). They arrive on the run loop of the thread that made
  the client, so the main loop must run Cocoa's loop first (milestone 2).
- **Threads:** CoreMIDI calls back on its own high-priority thread: no
  thread of ours, no SCHED_FIFO.

### Plugins
- **Bundles:** `.clap` on macOS is a bundle, loaded through CFBundle, the
  binary inside found by the bundle, `clap_entry->init()` given the bundle's
  path. The scan looks in `~/Library/Audio/Plug-Ins/CLAP`,
  `/Library/Audio/Plug-Ins/CLAP` and `CLAP_PATH`, for bundles (folders), not
  files, and checks each binary's architectures (decision 13).
- **Editors:** `CLAP_WINDOW_API_COCOA`, parented to an `NSView` (decision
  11). On macOS a CLAP editor's size is in points, not pixels (`set_scale`
  doesn't apply): `Plugin/Editor.h`'s "physical pixels" is X11's. Plugins on
  macOS use the host's timer support; `posix-fd-support` stays Linux's.

## The app

- **Main loop:** Cocoa delivers events, CoreMIDI's notifications included,
  only while its run loop runs, and runs its own loop while a window is
  resized or a menu tracked, so a `poll()` loop would stall then (and
  CoreMIDI's notifications would never come). On macOS DGL drives:
  `Application::exec()` runs Cocoa's loop, and `MainLoop`'s timers (the
  controller's service and tick, the UI's idle) are DGL idle callbacks. With
  no window (`--no-ui`, `--monitor`), a bare `CFRunLoopRun()` with the
  timers on it. Watched file descriptors (Linux plugins' `posix-fd-support`)
  aren't needed on macOS.
- **File dialogs:** DPF's file browser on both platforms (decision 10): the
  native open and save panels on macOS, the portal (or DPF's own browser)
  on Linux; `FileDialog` keeps its start-then-poll shape over it. Racks as
  packages (decision 6) make choosing one a file choice on macOS.
- **The monitor** a window opens on: `CGGetDisplaysWithPoint()` at the
  pointer, `CGDisplayBounds()`, through CoreGraphics' C API.
- **Paths:** the default rig, the last position and the window size under
  `~/Library/Application Support/StageHand` (and `~/Library/Preferences` is
  left alone: these aren't preferences).
- **`main.cpp`:** the pw-jack restart and the PipeWire quantum are Linux
  only; `--quantum` sets the CoreAudio buffer size instead; choosing the
  main device is decision 12's; `--list-midi` and `--monitor` work as they
  do.
- **Files in protected folders:** StageHand isn't sandboxed, so racks in
  `~/Documents` or on the Desktop get macOS's one-time "allow access"
  prompt, as any such app does.

## Building and shipping

- **No Xcode needed.** The Command Line Tools (`xcode-select --install`)
  have clang, the SDK, `codesign` and `lldb`; CMake and Ninja build C++ and
  Objective-C++ (`.mm`) with them. Xcode is for Instruments (profiling),
  and maybe notarization: whether `notarytool` comes with the Command Line
  Tools alone is to check. GitHub's macOS runners have Xcode anyway.
- **CMake:** the Linux dependencies (JACK, ALSA, X11, XRandR) and sources
  inside `if(LINUX)`; CoreAudio, AudioToolbox, CoreMIDI, CoreFoundation and
  CoreGraphics on macOS; DPF's Cocoa build. `CMAKE_OSX_DEPLOYMENT_TARGET`
  15.0, `CMAKE_OSX_ARCHITECTURES` arm64 (decision 13).
- **CI:** there is none yet. A GitHub Actions workflow runs the test suites
  on Linux and macOS on every pull request. They use fakes (no audio, MIDI or
  display), so they run anywhere.
- **The app bundle:** `StageHand.app`, generated by CMake (`MACOSX_BUNDLE`),
  with an `Info.plist`: HiDPI (`NSHighResolutionCapable`), the `.rack`
  package type, an icon. No microphone usage description (decision 2).
- **Signing:** a Developer ID and notarization (decision 14), for running it
  on other Macs without a Gatekeeper override. With the hardened runtime,
  loading other developers' plugins needs the
  `com.apple.security.cs.disable-library-validation` entitlement, or every
  CLAP plugin signed by someone else fails to load.

## Milestones (proposed)

1. **Builds and tests on macOS:** the libraries and the test suites only
   (not the app, which needs the backends), CMake's Linux parts inside
   `if(LINUX)`, and a CI workflow running the suites on Linux and macOS.
   Linux builds the same targets with the same dependencies as before; it
   gains CI. Needs no Mac.
2. **Clock, paths and the main loop:** `nowNanoseconds()` and the block-time
   convention with its engine test (decision 4), the paths, and the main
   loop with DGL driving (Cocoa's loop on macOS; Linux's loop unchanged), on
   both platforms. Before CoreMIDI, whose notifications need Cocoa's loop.
3. **CoreMIDI input,** two ports, with hotplug, ids in the ALSA shape and
   devices by name (decision 5, both platforms). Tests: the name fallback in
   the Session tests; the backend on a Mac.
4. **CoreAudio backend:** a HAL IOProc on the main device (decision 12,
   settled first), denormals
   flushed (and in JACK's callback), the buses' channel pairs, then the click
   on its own pair or another device through the private aggregate
   (decision 3), the buffer size, sample-rate changes, sleep and unplugging.
5. **Plugins and editors:** the CLAP bundle loader, the scan paths and the
   architecture check (decision 13), and the editor window (decision 11:
   DGL, or the Objective-C++ helper), replacing the X11 one on Linux too.
6. **The app:** DPF's file dialogs on both platforms (decision 10; zenity
   and kdialog go, with `test-filedialog`), the monitor, Cmd shortcuts, the
   app bundle.
7. **Shipping:** signing, notarization, the entitlement, a release build.

Milestones 3 to 5 are the real work, each about the size of a setlist
milestone. Everything past milestone 1 needs a Mac to try: CI can build and
run the tests, but not play audio or MIDI.

**Milestone 1 implemented (2026-10-03):** `STAGEHAND_APP` (on by default on
Linux, off elsewhere, and refused off Linux) builds the Linux seams, DGL, the
UI and the app; without it only the platform-free libraries (core, engine,
session, app) and the 12 test suites build, with no JACK, ALSA, X11 or DPF.
`CMAKE_OSX_DEPLOYMENT_TARGET` 15.0 and `CMAKE_OSX_ARCHITECTURES` arm64.
`FileDialog` uses `pipe()` and `FD_CLOEXEC` (macOS has no `pipe2()`). CI
(`.github/workflows/ci.yml`): Linux (Ubuntu 24.04) builds everything and runs
the suites; macOS 15 (Apple Silicon) builds the libraries and runs the
suites, on every pull request and on pushes to main and rack-view. Linux's
build is otherwise unchanged: the same targets, with the same dependencies.

## Review (2026-10-03)

The first draft, read again against the code and Apple's APIs:
- **Corrected:** CoreMIDI fixes the protocol per input port, not per
  connection, so it needs two ports (as ALSA has two clients); and an AUHAL
  render callback's host time is the output's, later than the period's
  start that the engine expects, so the backend is a HAL IOProc passing
  "now" (decision 4).
- **Added:** the main output device and how it's chosen (decision 12);
  Cocoa's loop driving the main loop, moved before CoreMIDI (its hotplug
  notifications need it); arm64 only, with Intel-only plugins named as such
  (decision 13); no App Store (decision 14); the aggregate's sample rate,
  clock and latency; denormals; CoreMIDI ids in the ALSA shape; DPF's
  portal needing D-Bus, and patching the vendored DPF where it falls short;
  the plain-window fallback for plugin editors, and why not SwiftUI
  (decision 11); building without Xcode; milestone 1 building the
  libraries and tests only, and adding CI for Linux too.
- **Checked:** the libraries and tests with clang and libc++ (no warnings,
  every suite passing).

## Open questions

- **The main output device** (decision 12), before milestone 4: which device
  by default (proposed: the system's default output at start, not followed
  afterwards, so a headset connecting mid-set can't move the PA), how it's
  chosen (proposed: `--device NAME`, and a choice in the window), and where
  it's kept (the rig, as the click's output, or per machine). Deferred
  2026-10-03: nothing needs it until the CoreAudio backend.

Answered 2026-10-03: macOS 15 or later; DPF's file dialogs on Linux too; the
click can go to another interface, through a private aggregate device on
macOS.
