# StageHand — macOS (plan)

Proposed 2026-10-03, reviewed the same day (notes under "Review");
milestones 1 to 5 built, 6 under way (notes under "Milestones"). StageHand on macOS 26 (Tahoe) and later, keeping DGL for the UI
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
9. **macOS 26 or later** (decided 2026-10-03 as macOS 15; raised to 26 on
   2026-10-22). CoreMIDI's UMP API, audio workgroups and everything else here
   are there; one version to test. Raised because the app icon, an Icon
   Composer document, compiles only with Xcode 26 running on macOS 26 (on
   macOS 15, Xcode 16's actool writes nothing and Xcode 26's crashes), so
   CI's macOS job runs on macOS 26 and 15 would go untested.
10. **DPF's file browser on Linux too** (decided 2026-10-03), so both
    platforms share one file-dialog path and `FileDialog`'s zenity and
    kdialog processes go. On Linux DPF uses the desktop's portal
    (xdg-desktop-portal, over D-Bus: `libdbus-1-dev` at build, else DPF's
    own plainer browser). To check when it's built: choosing a folder (a
    rack bundle on Linux), the setlist filter (`*.setlist`), and a suggested
    name when saving. DPF is the author's fork (`external/dpf`, a submodule
    since 2026-10-22), so what's missing is patched in the fork.
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
12. **The main output device** (decided 2026-10-05). By default, the
    system's default output **as it is when StageHand starts, not followed
    afterwards**: macOS moves its default to a headset that connects, and a
    rig that followed would put the PA mix in the headset mid-set. Chosen
    with `--device NAME` (a device's name or UID, or a unique part of its
    name, from `--list-audio`), later also in the window (milestone 6).
    Kept **per machine, in the app's state** (`appStateDir()`, beside the
    window size and the last position), not in the rig: rig.md puts the audio
    device in the App layer, and a rig or setlist copied to another Mac (or
    to Linux) mustn't carry this machine's interface. A saved device that's
    missing at start: the system default, said plainly, and back to the
    saved one when it returns. Linux has no counterpart (PipeWire routes).
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
- **Denormals flushed** (done 2026-10-03, below): `Engine::process()` holds
  a `ScopedFlushDenormals` for the whole block, so this backend gets it as
  JACK's does, plugins included.
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
- **Timestamps:** `nowNanoseconds()` as each message reaches the input
  (changed 2026-10-05 from the packet's host time: see milestone 5's notes).
- **Hotplug:** the client's notifications (objects added and removed) bump
  `changes()`; `sync()` reconnects a source that came back (by its unique
  id, then its name). They arrive on the run loop of the thread that made
  the client, so the client is made on a small thread of the input's own
  that runs a `CFRunLoop` and only counts them (changed in milestone 3: see
  its notes). The input then works under any main loop, or none.
- **Threads:** CoreMIDI calls back on its own high-priority thread: no
  MIDI thread of ours, no SCHED_FIFO (the notification thread above carries
  no MIDI).

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
  CLAP plugin signed by someone else fails to load. It's in
  `cmake/StageHand.entitlements`, which the release signing passes to
  `codesign` (ReleaseUMP R4-16); the build's own ad-hoc signature doesn't
  need it.

## Milestones (proposed)

1. **Builds and tests on macOS:** the libraries and the test suites only
   (not the app, which needs the backends), CMake's Linux parts inside
   `if(LINUX)`, and a CI workflow running the suites on Linux and macOS.
   Linux builds the same targets with the same dependencies as before; it
   gains CI. Needs no Mac.
2. **Clock and paths:** `nowNanoseconds()` and the block-time convention
   with its engine test (decision 4), and the paths, on both platforms.
   (The main loop with Cocoa's loop driving was planned here; it moved to
   milestone 3, 2026-10-03: it's macOS code in the app, which doesn't build
   on macOS until there's a MIDI input to run, and Linux's loop doesn't
   change.)
3. **CoreMIDI input and the app on macOS:** the app built on macOS with the
   modes that need no audio (`--list-midi`, `--monitor`), the main loop with
   Cocoa's loop driving (CoreMIDI's notifications need it), and the CoreMIDI
   input: two ports, with hotplug, ids in the ALSA shape and devices by name
   (decision 5, both platforms). Tests: the name fallback in the Session
   tests; the backend on a Mac. (Cocoa's loop moved to milestone 4 when
   built: see the notes.)
4. **CoreAudio backend:** the main loop with Cocoa's loop driving (moved
   from milestone 3), a HAL IOProc on the main device (decision 12,
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

**Milestone 2 implemented (2026-10-03):** `Core/Clock.h`: `nowNanoseconds()`
(`CLOCK_MONOTONIC` on Linux, `mach_absolute_time` in nanoseconds on macOS,
with `hostTimeToNanoseconds()` for CoreMIDI's and CoreAudio's host times),
used by `JackBackend` and `AlsaSeqInput` in place of their own copies.
`AudioBackend.h` states the block-time convention (a block's time is when
its period began; CoreAudio passes the IOProc's "now"), and
`testCaptureToBlock` (engine) holds every backend to it: MIDI captured
during a period plays in the next block at the same offset, a constant one
period later (it fails with a backend passing the period's end, checked).
`Session/AppPaths.h`: `appDataDir()` and `appStateDir()` (XDG on Linux,
`~/Library/Application Support/StageHand` on macOS) for the default rig, the
last position and the window size. Tests: `tests/ClockTests.cpp` (the
clock, and host times on macOS), `testAppPaths` (setlist file tests: both
platforms' paths, each checked on its own CI), the rig and setlist path
checks per platform.

**Done ahead, from Linux (2026-10-03):** two parts of later milestones that
need no Mac.
- From milestone 3, **devices by name** (decision 5):
  `findDeviceByName()` (Session) and `Session::resolve()`. A device a rack
  names that won't open by its id is looked for among the devices listed:
  by its device name first (the port of the same name, else the device's
  first port), then by its port name if exactly one device has a port of
  that name. Never by an address (`"14:0"`), never a guess between two. The
  rack keeps the id it was saved with, and a device already open isn't
  opened again. Tests: `testDevicesByName` (Session).
- From milestone 4, **denormals:** `Core/Denormals.h`, `ScopedFlushDenormals`
  (x86: the MXCSR's FTZ and DAZ; ARM64: the FPCR's FZ), held by
  `Engine::process()` for each block and restored after, so every backend
  and every plugin the engine calls gets it. Tests: `tests/DenormalTests.cpp`
  (a denormal result and a denormal input are 0 under it, normal values
  untouched, the mode restored, nesting), run on x86 Linux and ARM64 macOS
  by CI.

**Milestone 3 implemented (2026-10-05):** `Midi/CoreMidiInput` (UMP API, two
input ports `"StageHand"` and `"StageHand MIDI 2.0"`, each source connected
to the port of its own protocol; virtual inputs through
`MIDIDestinationCreateWithProtocol`; each connection's refCon a `Receiver`
holding its handle, never freed before the input, so a late callback for a
closed handle finds it cleared). Ids `"<device>:<endpoint>"`; a source with
no device (another app's virtual source) is just `"<endpoint>"`. Hardware
means a device on any driver but the IAC bus and network sessions. Offline
endpoints (a known USB device unplugged) aren't listed. Timestamps were the
packet's host time, clamped to now (changed in milestone 5: see there). CMake builds
the app on macOS by default with `src/Midi/CoreMidiInput.cpp`,
`main.cpp` and `MidiMonitor.cpp`; playing, `--tone` and `--list-plugins` are
behind `STAGEHAND_PLAY_MODES` (Linux only for now) and say so on macOS.
`Midi/PlatformMidiInput.h` picks the input for `main.cpp`; both inputs have
`threadDescription()`. CI builds the app on macOS too.

- **Changed: the notification thread, and Cocoa's loop to milestone 4.**
  The plan had the main loop run Cocoa's loop so CoreMIDI's notifications
  could come. But nothing on macOS runs a main loop yet (`--monitor` and
  `--list-midi` don't use one; `--play` needs audio), and tying the input's
  hotplug to whichever loop the app runs would make it depend on DGL. So
  the input makes its client on its own thread, with a `CFRunLoop` that
  only counts notifications. Cocoa's loop driving `MainLoop` (with the
  timers as DGL idle callbacks) belongs with the first macOS window, in
  milestone 4 or 5, and no longer has CoreMIDI to wait for.
- **Checked on a Mac** (macOS 15, Apple Silicon), with a test sender
  publishing a MIDI 1.0 and a MIDI 2.0 source: the 1.0 source arrives as
  MT 0x2 and the 2.0 source as MT 0x4, each on its own port; both virtual
  inputs receive; note-on to note-off spacing is right; a source leaving
  moves `changes()` and `sync()` reports it gone, and coming back (a new
  endpoint of the same name) reconnects it under the same handle. KeyEDO
  lists as a MIDI 2.0 app source.
- **MSB-only Data Entry survives on macOS.** A MIDI 1.0 app sending to the
  `"MIDI 2.0 In"` destination: RPN 0/0 with CC 6 only arrives as `RPN 0:0
  value 0x04000000` (the MSB, scaled), at once, without waiting for an LSB,
  unlike ALSA's converter (architecture.md, section 1). CoreMIDI also passes
  CC 101 and CC 100 through, as MIDI 2.0 controller messages, before the
  RPN.
- **Checked since:** a Launchkey Mini (MIDI 1.0, USB) and KeyEDO's
  published source (MIDI 2.0) in `--monitor`. Not yet: a UMP-native USB
  device.

**Milestone 4, first part (2026-10-05): the CoreAudio backend and `--tone`.**
`Audio/CoreAudioBackend`: a HAL IOProc on one output device, chosen as
decision 12 says (`--device`: a name, a UID or a unique part of a name;
default the system's output at start, not followed). The engine renders into
planar buffers allocated at open for the device's largest buffer; the IOProc
interleaves them into the device's streams (Float32, the HAL's virtual
format), the main outputs on its first channels in stream order. The block's
time is the IOProc's "now" in host time (decision 4). `--quantum` sets this
process's `kAudioDevicePropertyBufferFrameSize` (clamped to the device's
range), and `close()` puts back what was there (the setting outlives an
open in the same process otherwise: found by the test). Listeners on the
HAL's own thread (`kAudioHardwarePropertyRunLoop` set to none): buffer size
and nominal rate changes (reported by `config()`), overloads (counted as
xruns), the device going away. `--tone` and `--list-audio` run on macOS;
`--device` is macOS's.

- **Order changed:** Cocoa's loop driving `MainLoop` waits for `--play`
  on macOS (milestone 5's plugin loader): until then nothing on macOS runs
  a main loop, so it couldn't be tried.
- **Checked on a Mac** (MacBook Pro speakers, 48 kHz), silently, with a
  processor recording each block: at 64, 128, 512 and 1024 frames every
  block's time advances by exactly its frames (within 30 µs), `process()`
  starts 10 to 60 µs after the block's time (so it is the period's start),
  no overloads; output latency 18 ms at 64 frames, 27 ms at 512 (the
  built-in speakers' safety offset is large). `--device` refuses an
  unknown or ambiguous name, naming the matches.
- **Done since** (second part, below): the saved device per machine, the
  click through the aggregate, devices going and coming, the rate held. The
  buses' pairs needed nothing: they're `--channels` outputs, onto the
  device's channels in order.

**Milestone 5, first part (2026-10-05): the CLAP loader, and playing without
windows.** `ClapModule` loads a bundle through CFBundle
(`CFBundleLoadExecutableAndReturnError`, `clap_entry` by
`CFBundleGetDataPointerForName`, `init()` given the bundle's path) and leaves
the binary loaded at the end (unloading code a plugin may still have threads
in is the riskier choice). `ClapModule::isLoadableHere()` reads the bundle's
architectures without loading it: no arm64 binary is "Intel only: not
loadable on Apple Silicon" (decision 13), printed by the scan and skipped.
The scan looks in `~/Library/Audio/Plug-Ins/CLAP`,
`/Library/Audio/Plug-Ins/CLAP` and `CLAP_PATH`, takes folders ending `.clap`
as plugins and doesn't look inside them. `ClapPlugin` asks for
`CLAP_WINDOW_API_COCOA` (an `NSView*` parent) on macOS, and doesn't offer
`posix-fd-support` there. `MainLoop` wakes through a self-pipe (macOS has no
`eventfd`); with no window, its `poll()` loop serves a headless `--play`
(CoreMIDI's hotplug has its own thread, so needs no Cocoa loop). `main.cpp`
plays on both platforms: `STAGEHAND_WINDOWS` (Linux) has the rack window and
the editor windows; without it, stand-ins that never open, so `--play` runs
console only and says so. The click isn't routed on macOS yet
(`clickRouting` null, which the controller already allows).

- **Checked on a Mac:** `--list-plugins` finds and loads all six installed
  CLAP plugins (arm64 bundles: StruckSynth, PlainOldSynth, Polygraph MU,
  IFSynth, PMDGLCanvas, GtkClapMin). `--play --plugin
  com.unlikelyware.plainoldsynth` loads it, sees its MIDI 2.0 note port,
  runs on CoreAudio, and quits cleanly on Ctrl-C.
- **Found: DPF plugins' Objective-C classes collide.** DPF names its Cocoa
  classes `<DGL_NAMESPACE>_<compiler build>_<class>` (`PuglWindow`,
  `PuglOpenGLView`, ...), so every DPF plugin built with the default
  namespace (`DGL`) and the same clang registers the same classes; loading
  two in one process makes the Objective-C runtime warn ("implemented in
  both ... may cause ... mysterious crashes"), and which class wins is
  arbitrary. It's the plugins' to fix (each its own `DGL_NAMESPACE`, e.g.
  `-DDGL_NAMESPACE=StruckSynthDGL`), but StageHand's own DGL window
  (milestone 6) must set one too (`StageHandDGL`), or it collides with
  every such plugin it hosts.
- **Found: "late MIDI" with a USB keyboard, and fixed.** At 64 frames the
  Launchkey made `late MIDI message(s)` notices. CoreMIDI's packet time is
  when the driver got the bytes, earlier than the message reaches the
  engine's queue; a block starting in between missed it, and the next found
  it more than a period old (placed at the block's start: up to a period of
  jitter). Messages are now stamped as they arrive, like ALSA's read-time
  stamps, so capture time never precedes queue time. KeyEDO stamps 0, so it
  was never affected.

**Milestone 5, second part (2026-10-05): editor windows, and Cocoa's loop.**
- `App/EditorWindowMac.mm`: decision 11's plain window, an `NSWindow` whose
  content view is the plugin's parent `NSView` (`CLAP_WINDOW_API_COCOA`),
  with the X11 window's interface, so `main.cpp` takes either. The plain
  window was chosen over a DGL one: DGL isn't built on macOS yet, and DPF's
  shared Objective-C class names (above) make a window with no DGL in it the
  safer host. Its one Objective-C class has its own name
  (`StageHandEditorWindowDelegate`). Fixed-size editors pin the content's
  min = max; a plugin's `request_resize` resizes the window and is confirmed
  with `set_size` after the request returns (`dispatch_async`), as X11 does
  when its resize lands. The close button closes that editor only; the
  window and its delegate are let go after the event (they're released from
  inside the window's own close handling otherwise). Cmd-Q quits and
  Cmd-Shift-S saves from any editor window (decision 7), through a local key
  monitor before the plugin sees them. The process becomes a regular app
  (Dock icon, key windows) when the first editor opens.
- `App/MainLoopMac.mm`: `MainLoop` on macOS, AppKit's loop driving
  (`nextEventMatchingMask:` in `run()`); the timers are `CFRunLoopTimer`s and
  the fds `CFFileDescriptor`s, both in the run loop's common modes, so they
  run while AppKit runs its own loop (a live resize, a menu). The rack
  window's timer skips its `idle()` then (`MainLoop::systemTracking()`):
  pugl's `puglUpdate` takes events off the queue, and taking the resize
  loop's drag and mouse-up events started a second resize inside the first
  and crashed AppKit (2026-11-09). `requestQuit()`
  stays async-signal-safe: it writes the self-pipe, whose `CFFileDescriptor`
  posts an application-defined event to wake `nextEventMatchingMask:`.
- `main.cpp`: the editor window by platform; `STAGEHAND_RACK_WINDOW`
  (Linux) has the rack window, and macOS's stand-in for it never opens, so
  the editors open at start as with `--no-ui` (until milestone 6, below). Window keys are named per
  platform (`Cmd`/`Ctrl`) in the console's hints and the help.
- **Checked on a Mac:** PlainOldSynth's, Polygraph MU's and StruckSynth's
  editors open at their sizes, in points (1200×750, 900×600, 1080×640 on a
  1728×1117-point screen; DPF's own log shows the same at 2× in pixels);
  Ctrl-C in the terminal quits through the self-pipe, cleanly.
- **Still to do in milestone 5:** nothing beyond what's checked by hand
  (two DPF plugins' editors at once wait for their namespaces).

**Milestone 4, second part (2026-10-05): devices coming and going, decision
12's memory, and the click on macOS.**
- `CoreAudioBackend` is built around `setUp()` (the IOProc on the main
  device, or the click's aggregate) and `tearDown()`, so it can move without
  the engine knowing: it sees no blocks for a moment. `service()`, on the
  app's 5 ms housekeeping timer, does the main-thread work and returns what
  to tell the user.
- **The main device going away** (unplugged, or asleep): silent until it's
  back, then everything resumes and the click is routed again. Not the
  laptop's speakers instead: on stage, sound from the wrong place is worse
  than none. (Linux quits when JACK's server goes; unchanged.)
- **Decision 12's memory:** `Session/AudioDeviceState` (`audio.json` in
  `appStateDir()`: the device's name and UID). `--play --device` chooses and
  remembers; without it, the remembered device (by UID), or the system's
  default while it isn't there, said plainly, and `service()` moves to it
  when it appears. Tests: `testAudioDeviceChoice` (setlist file tests).
- **The click (decision 3):** `CoreAudioBackend` is the `AuxRouting`.
  Playback devices are the system's outputs (ports "1".."n"). On the main
  device the click is mixed onto its first two channels; on another device
  a private aggregate (main device first, the clock; the other at its rate,
  drift-compensated) is made and the IOProc moves to it, the click on that
  device's first two channels. Its device leaving: back to the main device
  alone, and the controller routes it again when it's back (as on Linux).
- **The sample rate is held:** something else changing the device's rate
  mid-set would leave the plugins at the wrong pitch, so `service()` sets it
  back and says so; after three times in ten seconds it stops fighting and
  says pitch and tempo are off.
- **Outputs always as asked:** the engine always gets `--channels`
  outputs; a device with fewer drops the rest (told at open), so a device
  change never changes the engine's layout.
- **Checked on a Mac:** with a silent processor, the click moved from the
  MacBook's speakers to a display's audio (an aggregate) and back with
  blocks never stopping and no overloads; the aggregate adds 6 ms of output
  latency there (33.5 against 27.3 ms at 512 frames). `--device`
  remembered, reused on the next run, and a remembered device that isn't
  there (simulated) gave way to the default, said so.
- **Not checked yet:** a device unplugged and plugged back in while
  playing; the click chosen in the rig on macOS (no rack window yet: by
  hand, `"clickOutput"` in the rig); sleep and wake.

**Milestone 6, first part (2026-10-05): the rack window on macOS.**
- **DGL on both platforms:** CMake builds DPF's DGL and `stagehand-ui` with
  the app on Linux and macOS (pugl's Cocoa backend, `pugl.mm`; AppKit's
  pre-10.12 names in it quieted, not patched). `DGL_NAMESPACE` is
  `StageHandDGL` (a public definition on `dgl-opengl`), so pugl's
  Objective-C classes are `StageHandDGL_<build>_PuglWindow` and so on, apart
  from every DPF plugin's `DGL_<build>_...`. Linux sees only the C++
  namespace change.
- **The monitor:** `Ui/MonitorMac.cpp`, the display under the pointer
  (`CGGetDisplaysWithPoint`, `CGDisplayBounds`), in pixels as DGL sizes
  windows (points times the display mode's scale). The `DISPLAY` check is
  Linux's.
- **Cmd, not Ctrl** (decision 7): `ui::kCommand` (`kModifierSuper` on
  macOS, `kModifierControl` on Linux) for every shortcut and modified
  drag or wheel in the rack window and the sequence editors; the menus, the
  sequence editor's help and the controller's "saves it" notice name Cmd
  and Option on macOS. Text fields ignore characters typed with either
  modifier.
- `main.cpp`: the rack window on both, so the no-window stand-in and
  `STAGEHAND_RACK_WINDOW` go. DGL's `idle()` runs from `MainLoop`'s 16 ms
  timer as on Linux; AppKit's loop (`MainLoopMac.mm`) delivers the events.
- **Checked on a Mac:** the window opens at its size on the laptop's display
  (1100 points wide), draws the empty rack, and quits cleanly on Ctrl-C,
  saving; with PlainOldSynth loaded, none of the Objective-C runtime's
  duplicate-class warnings name StageHand's classes (the ones printed are
  between the installed plugins, from `--plugin`'s scan loading each).
- **Found: resizing cut off the top.** Growing the window left its drawing
  shifted up (the toolbar gone, a blank band below). pugl's OpenGL view,
  inside the window's content view, follows it only if the window was
  resizable when the view was made, and DGL's `setResizable()` comes after
  the widget (and so the view) exists; DPF's `puglSetResizable()` changed
  the window's style but not the view's mask (its own FIXME). Patched in
  StageHand's DPF (now the fork's: its README.fork.md); the sequence
  editors' windows had the same order. Checked by growing and shrinking the window.
- **File dialogs on macOS** (decision 10): `Ui/FileChooser`, DPF's file
  browser (the system's open and save panels, as a sheet on the rack window)
  with `FileDialog`'s start-then-poll shape and modes. StageHand's DPF (now
  the fork) is patched for what it lacked on macOS (README.fork.md): choosing a
  folder (a rack), an extension filter (`*.setlist`, through UTType), the
  suggested name when saving, the sheet begun at once, and closing a sheet
  still open. Built with the file browser on macOS only; Linux keeps
  `FileDialog` (zenity, kdialog) until DPF's portal path is patched the same
  way and tried there. Checked by hand: Open, Save, Save As and Open setlist.
- **Still to do in milestone 6:** the rest of the window by hand (menus,
  Cmd keys, a sequence editor, a plugin editor beside it); DPF's portal path
  on Linux, then `FileDialog` and `test-filedialog` go.

**Milestone 6, second part (2026-10-06): the app bundle.**
- **`StageHand.app`** (`MACOSX_BUNDLE`, `cmake/Info.plist.in`): bundle id
  `com.unlikelyware.stagehand`, HiDPI, macOS 15, the music category, no
  microphone description (decision 2). Exported types: `.rack` as
  `com.unlikelyware.stagehand.rack`, conforming to `com.apple.package`
  (decision 6), and `.setlist` as `com.unlikelyware.stagehand.setlist`
  (JSON); both document types StageHand's (`Owner`). The icon came later
  (below).
- **`build/stagehand`** stays the command line's: a script that `exec`s the
  bundle's binary by its absolute path. Not a symbolic link: run through
  one, the binary isn't the bundle's (no bundle id, and the menu bar says
  "stagehand").
- **AppKit's quit** (the Dock's Quit, AppleScript's `quit`, logging out)
  called `exit()` before: nothing saved, plugins not stopped. Found by
  quitting the bundle through AppleScript. `App/AppDelegateMac.mm`'s
  delegate answers `applicationShouldTerminate:` with `NSTerminateCancel`
  and quits as Cmd-Q does instead (`RackUi::quitAsInWindow()`: asking about
  an untitled rack's changes, if it has some). Not `NSTerminateLater`:
  AppKit waits for that reply in a modal loop of its own, and StageHand's
  shutdown runs after `MainLoop::run()` returns. So a logout or shutdown is
  cancelled once; StageHand has quit cleanly by then.
- **Found: a quit could be missed with the rack window open.** DGL's
  `idle()` (pugl's `puglUpdate()`, from a timer) takes every pending event,
  the wake-up event `requestQuit()` posts included, so `MainLoop::run()`
  could wait on forever (half of ten AppleScript quits; Ctrl-C the same).
  `run()` now waits at most 50 ms for an event before looking at its quit
  flag again: 10 of 10 quits through AppleScript, 5 of 5 Ctrl-Cs.
- **Choosing a rack:** once the app is registered, a `.rack` is a package (a
  file to the open panel), so the rack chooser takes the rack type or a
  plain folder (a rack from before, or from Linux).
- **Checked on a Mac:** run through the script and through `open`, the
  process is `StageHand`, `com.unlikelyware.stagehand`; with a scratch
  `HOME`, a launch through `open` uses its own Application Support folder;
  `MacTest.rack` reads as `com.unlikelyware.stagehand.rack`, a package, and
  `Test.setlist` as the setlist type.
- **Not yet:** opening a rack or setlist double-clicked in Finder
  (the app starts, but on its last setlist: the open-documents event isn't
  handled); choosing a rack by hand now that it's a package.

**Milestone 6, the icon (2026-10-19).** Three bars and a wave, in purple,
green and orange tuned to one lightness, so it stands apart from
KeyboardEDO's Ember (pink, orange, yellow) and KeyEDO's blues; the bars
read as the rack's rows. Its source is `resources/StageHand.icon`, an Icon
Composer document, compiled at build time by Xcode 26's `actool`, on macOS 26 (as
KeyboardEDO's is) into `Assets.car`, which macOS 26 draws with its glass and
its dark and tinted versions, and `StageHand.icns`
(`CFBundleIconName` and `CFBundleIconFile`). Not a pre-rendered
`.appiconset`: macOS 26 would set that on a plate, and it couldn't change
with the appearance. Without Xcode (only the Command Line Tools) the build
warns and the bundle has no icon. The bundle is now ad-hoc signed whole
after the build, since the linker signs only the executable and not the
Resources added after it. Checked: `codesign -vv` is valid, and the icon
Finder gets for `StageHand.app` is the compiled one.

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

None now.

Answered 2026-10-05: the main output device (decision 12). Answered 2026-10-03: macOS 15 or later; DPF's file dialogs on Linux too; the
click can go to another interface, through a private aggregate device on
macOS.
