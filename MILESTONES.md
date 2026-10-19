# StageHand milestones

## Tests
- all: ninja -C build && ctest --test-dir build --output-on-failure

## M1: Linux platform
- [x] Engine: JACK audio (PipeWire), ALSA sequencer MIDI input (UMP mode, MIDI 1.0 and 2.0)
- [x] CLAP plugin hosting: load, activate, RT process, editor windows (X11)
- [x] Rack, setlist and rig files; sequences and looper
- [x] DGL rack view, sequence editor, file dialogs (zenity/kdialog)
- [x] Test suites: engine, session, rack/rig/setlist files, sequences, looper, beat-now

## M2: macOS port
- [x] Libraries and test suites build on macOS (no Linux deps, CI on both platforms)
- [x] Clock (`nowNanoseconds`) and app paths on both platforms
- [x] CoreMIDI input: two ports, hotplug, ids in ALSA shape, devices by name
- [x] CoreAudio backend: HAL IOProc, `--quantum`, click on aggregate device, device memory
- [x] CLAP bundle loader, plugin scan, editor windows (NSView in plain NSWindow)
- [x] Rack window on macOS: DGL on both platforms, Cmd shortcuts, DPF file dialogs
- [x] App bundle: `StageHand.app`, `Info.plist`, `.rack` as a package type
- [ ] Signing, notarization, Developer ID entitlement for plugin loading
- [x] Icon (`resources/StageHand.icon`, compiled by actool)
- [ ] Open-documents event (rack/setlist double-clicked in Finder)
- [ ] DPF portal file dialog on Linux (replaces zenity/kdialog)

## M3: Performance features
- [ ] Count-in per song; Perform view with large beat display
- [ ] Panic: all notes off and pedals up, button and learned trigger; CPU and xrun display
- [ ] MIDI controllers to plugin parameters (knobs, faders, expression pedal), learned per rack or global
- [ ] Velocity curves per row, or fixed velocity
- [ ] MIDI out: program changes at song switch, MIDI clock, click as MIDI notes
- [ ] Looper: controllers (sustain pedal) and pitch bend in loops

## M4: Editor and later
- [ ] House look and feel (plugins' knobs, group boxes, slider, DejaVu Sans) in the rack view and generic editor
- [ ] Generic editor: sliders and menus for plugins with no editor of their own
- [ ] Backing tracks: an audio file per song, in time with the transport
- [ ] Song notes in the Perform view
- [ ] Undo for rack edits
