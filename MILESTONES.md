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
- [x] Count-in per song; Perform view with large beat display
- [x] Panic: all notes off and pedals up, button and learned trigger; CPU and xrun display
- [ ] Panic clears retuned keys: track keys sent Pitch 7.25, send Per-Note Management Reset to those only
- [x] Take the uw-core pin with Pitch 7.25 transpose (value moves with the key)
- [x] MIDI controllers to plugin parameters (knobs, faders, expression pedal), learned per rack or global (Control Links, learned from a hardware control)
- [x] Control Links made and edited graphically, not only learned from a hardware control: pick a controller and a parameter on screen, edit or delete an existing link, per rack or global. Before the 0.9.0 preview
- [ ] Velocity curves per row, or fixed velocity
- [x] MIDI effects per row: plugin note/MIDI output (uw-core), the chain in the engine, Retune → StruckSynth test (docs/midi-effects.md)
- [x] MIDI effects in the rack file and the row's input side: MIDI FX menu, editors, bypass (docs/midi-effects.md, section 3)
- [ ] MIDI out: program changes at song switch, MIDI clock, click as MIDI notes
- [ ] Looper: controllers (sustain pedal) and pitch bend in loops

## M4: Editor and later
- [x] House look and feel in the rack view: brand, house slider, dropdowns, sections (docs/look.md)
- [ ] House look in the Perform view and panels; knobs in the generic editor
- [x] Generic editor, first version: sliders, switches and menus for plugins with no editor of their own, opened only when chosen (docs/generic-editor.md)
- [ ] Generic editor: groups, search, typed text the plugin parses, rescan
- [ ] Reorganize the MIDI plugins UI: how a row shows and edits its MIDI effects (the MIDI FX menu, E, the generic editor)
- [ ] Backing tracks: an audio file per song, in time with the transport
- [ ] Song notes in the Perform view
- [ ] Undo for rack edits
- [x] Sequence editor: horizontal scrolling by a trackpad's sideways swipe too, not only Shift+wheel, as Umpire's grid
