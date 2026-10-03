# StageHand — macOS (plan)

Proposed 2026-10-03; nothing built yet. StageHand on macOS, keeping DGL for
the UI (no native Cocoa interface). Builds on:
- architecture.md, sections 1 to 3: MIDI, audio and plugins sit behind seams
  (`MidiInput`, `AudioBackend`, `PluginFactory`) so CoreMIDI and CoreAudio
  can be added without touching routing, the engine or the session.
- command-line.md, "MIDI devices coming and going": the hotplug design
  (`MidiInput::changes()` and `sync()`) a CoreMIDI input implements too.

## Where things stand (2026-10-03)

About 10,300 lines (the engine, sequencer and looper, Session, the rack, rig
and setlist files, the controller and the DGL widgets) are plain C++20 with
no Linux dependency. About 2,200 lines are Linux-specific, most of them
behind the seams already:

| Area | Linux | macOS | Size |
|---|---|---|---|
| Audio | `JackBackend` (JACK, through PipeWire) | `CoreAudioBackend` (AUHAL, outputs only) | Large |
| MIDI | `AlsaSeqInput` (sequencer, UMP mode) | `CoreMidiInput` (UMP, `MIDIEventList`) | Medium |
| Plugin editors | `EditorWindow` (a raw X11 window) | A DGL window as the parent, on both | Medium |
| Main loop | `MainLoop` (`poll()`, `eventfd`) | Timers on a `CFRunLoopTimer`; a pipe for wake-ups | Small to medium |
| CLAP loading | `dlopen` of `.clap` files; `~/.clap`, `/usr/lib/clap` | `.clap` bundles (CFBundle); `~/Library/Audio/Plug-Ins/CLAP`, `/Library/Audio/Plug-Ins/CLAP` | Small |
| File dialogs | zenity or kdialog, as a process | DPF's file browser (the native panel) | Small |
| Monitor size | X11 / XRandR (`Ui/Monitor.cpp`) | CoreGraphics (plain C) | Tiny |
| Paths | XDG (`~/.local/share`, `~/.local/state`) | `~/Library/Application Support/StageHand` | Tiny |
| `main.cpp` | the pw-jack restart, `--quantum`, `/proc/self/exe` | left out; the buffer size set through CoreAudio | Small |

The C++ is fine for Apple's toolchain as it is: no `<format>`, ranges or
`jthread`, and `std::atomic<double>` is lock-free on arm64 and x86_64.

## Decisions (proposed)

1. **DGL stays** for the rack window and the sequence editor (pugl's Cocoa
   backend). No native menu bar or controls.
2. **Outputs only.** StageHand has no audio inputs (`AudioConfig::
   inputChannels` is 0 and the engine never reads input; the rig's "audio
   into plugins" is rows feeding bus effects, inside StageHand). So no
   microphone permission, and the backend opens an output device only.
3. **One output device.** The main outputs, the click's pair and the bus
   output pairs are channels of one device (an audio interface's 3-4, say).
   A second device needs an aggregate device or drift correction: later, if
   ever. `AuxRouting` (connect the click to a playback device, as JACK does)
   becomes a choice of output channels on that device; on Linux the same
   choice maps to ports.
4. **One clock.** MIDI timestamps and audio block times must share a clock:
   `CLOCK_MONOTONIC` on Linux, host time (`mach_absolute_time`, in
   nanoseconds through `mach_timebase_info`) on macOS, where CoreMIDI and
   CoreAudio both report it. A `nowNanoseconds()` in Core, used by every
   backend.
5. **Devices by name across platforms.** A rack names MIDI devices by ALSA
   id (`"KeyLab 88:KeyLab 88 MIDI 1"`); CoreMIDI's names differ, so a rack
   made on Linux would find nothing on a Mac. A device that isn't found by
   id is looked for by name (the device's, then the port's), on both
   platforms; a saved rack keeps its id until the device is chosen again.
6. **The rack bundle is a package.** `.rack` declared as a package type in
   the app's `Info.plist`, so Finder (and the file dialogs) treat a rack as
   one file. The folder format doesn't change.
7. **Cmd, not Ctrl.** Ctrl-S, Ctrl-N, Ctrl-O, Ctrl-Q become Cmd (DGL's
   `kModifierSuper`) on macOS, and the menus say so. Cmd-Q is handled in the
   window (a pugl app has no menu bar).
8. **OpenGL** (DPF's NanoVG backend) is deprecated on macOS but works,
   Apple Silicon included. Kept until it doesn't.

## Engine and backends

### CoreAudio backend
- An AUHAL output unit on the chosen device; its render callback calls the
  `AudioProcessor` with the block's host time (decision 4).
- The buffer size through `kAudioDevicePropertyBufferFrameSize` (what
  `--quantum` does on Linux, without touching other apps), the sample rate
  through the device's nominal rate.
- Property listeners: the sample rate changing (prepared racks dropped and
  remade, as now), the device list (hotplug), the device going away.
- Output channels: the main pair, the click's pair and the buses' pairs as
  channel numbers on the device (decision 3).
- Real-time: CoreAudio's I/O thread. Plugins that start threads should join
  its workgroup (`os_workgroup`, macOS 11+); CLAP's thread pool extension
  runs on the host's threads, so this is the host's job when it's added.

### CoreMIDI input
- A client, an input port, and a connection per opened source, with
  `kMIDIProtocol_2_0` (or 1.0, with the protocol preference): CoreMIDI
  converts per connection, so one port does what ALSA's two clients do. UMP
  arrives through `MIDIEventList`, wrapped as now (MIDI 1.0 as MT 0x2, MIDI
  2.0 as MT 0x4).
- "From other apps": `MIDIDestinationCreateWithProtocol`, one per protocol,
  named as now.
- Timestamps: the packet's host time (decision 4), or now if it's 0.
- Hotplug: `MIDINotifyProc` (objects added and removed) bumps `changes()`;
  `sync()` reconnects a source that came back (by its unique id, then its
  name).
- CoreMIDI calls back on its own high-priority thread: no thread of ours,
  no SCHED_FIFO.

### Plugins
- `.clap` on macOS is a bundle: loaded through CFBundle, the binary inside
  it found by the bundle, `clap_entry->init()` given the bundle's path.
- The scan looks in `~/Library/Audio/Plug-Ins/CLAP` and
  `/Library/Audio/Plug-Ins/CLAP`, and `CLAP_PATH`, for bundles (folders),
  not files.
- Editors: `CLAP_WINDOW_API_COCOA`, parented to an `NSView`. `EditorWindow`
  becomes a DGL (pugl) window whose native handle is handed to the plugin:
  an `NSView*` on macOS, an X11 window on Linux, so one class serves both
  and the raw X11 code goes. Plugins on macOS use the host's timer support;
  `posix-fd-support` stays Linux's.

## The app

- **Main loop:** Cocoa delivers events only while its run loop runs, and
  runs its own loop while a window is resized or a menu tracked, so a
  `poll()` loop stalls then (audio plays on, but ticks and plugins' main
  thread work wait). On macOS, `MainLoop`'s timers run from a
  `CFRunLoopTimer` in the common modes, and DGL's `idle()` from the same;
  its wake-up `eventfd` becomes a pipe. Watched file descriptors (Linux
  plugins' `posix-fd-support`) aren't needed on macOS.
- **File dialogs:** DPF's file browser, which uses the native open and save
  panels on macOS (and could replace zenity and kdialog on Linux too: one
  path). Racks as packages (decision 6) make choosing one a file choice.
- **The monitor** a window opens on: `CGGetDisplaysWithPoint()` at the
  pointer, `CGDisplayBounds()`, through CoreGraphics' C API.
- **Paths:** the default rig, the last position and the window size under
  `~/Library/Application Support/StageHand` (and `~/Library/Preferences` is
  left alone: these aren't preferences).
- **`main.cpp`:** the pw-jack restart and the PipeWire quantum are Linux
  only; `--quantum` sets the CoreAudio buffer size instead; `--list-midi`
  and `--monitor` work as they do.

## Building and shipping

- **CMake:** the Linux dependencies (JACK, ALSA, X11, XRandR) and sources
  only on Linux; CoreAudio, AudioToolbox, CoreMIDI, CoreFoundation and
  CoreGraphics on macOS; DPF's Cocoa build. A universal binary (arm64 and
  x86_64), or arm64 only.
- **CI:** the test suites run on a macOS GitHub Actions runner as well as
  Linux. They use fakes (no audio, MIDI or display), so they run anywhere.
- **The app bundle:** `StageHand.app`, generated by CMake (`MACOSX_BUNDLE`),
  with an `Info.plist`: HiDPI (`NSHighResolutionCapable`), the `.rack`
  package type, an icon. No microphone usage description (decision 2).
- **Signing:** a Developer ID and notarization, for running it on other
  Macs without a Gatekeeper override. With the hardened runtime, loading
  other developers' plugins needs the
  `com.apple.security.cs.disable-library-validation` entitlement, or every
  CLAP plugin signed by someone else fails to load.

## Milestones (proposed)

1. **Builds and tests on macOS:** CMake made conditional, the Linux sources
   left out, a stub audio backend and MIDI input so it links, and the suites
   running on a macOS CI runner. Shows any toolchain problems early, without
   a Mac.
2. **Clock and paths** in Core and Session (decisions 4, and the paths), on
   both platforms. Tests: the paths, as the XDG ones are tested.
3. **CoreMIDI input,** with hotplug and devices by name (decision 5, both
   platforms). Tests: the name fallback in the Session tests; the backend on
   a Mac.
4. **CoreAudio backend,** main outputs first, then the click's and the
   buses' channel pairs (decision 3), the buffer size and sample-rate
   changes.
5. **Plugins and editors:** the CLAP bundle loader and scan paths, and the
   DGL editor window (replacing the X11 one on Linux too), with the run
   loop timers.
6. **The app:** file dialogs, the monitor, Cmd shortcuts, the app bundle.
7. **Shipping:** signing, notarization, the entitlement, a release build.

Milestones 3 to 5 are the real work, each about the size of a setlist
milestone. Everything past milestone 1 needs a Mac to try: CI can build and
run the tests, but not play audio or MIDI.

## Open questions

- **Minimum macOS version.** 11 (Big Sur) is the floor for CoreMIDI's UMP
  API and `os_workgroup`; 12 or 13 would trim testing.
- **Linux file dialogs:** switch them to DPF's file browser too (one path),
  or keep zenity and kdialog there?
- **A second output device** (the click on headphones from another
  interface): wanted, or one device enough?
