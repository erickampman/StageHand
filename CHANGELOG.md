# Changelog

All notable changes to StageHand. Versions follow the git tags; the format is
[Keep a Changelog](https://keepachangelog.com/).

## [Unreleased]

### Changed

- Help opens the help page in a window of the app's own, not in the web browser. Nothing is
  written to the temp folder, and on Linux no web server runs in the host any more.

### Changed

- All inputs hears software and app MIDI sources too (another app's port, a
  virtual keyboard, the IAC bus), not only hardware: StageHand now opens every
  source at start and on hotplug, except loop-backs like Linux's Midi Through
  (`--midi all` still takes those). Before, an app's source was heard only
  after a row picked it by name
  ([#1](https://github.com/erickampman/unlikelyware/issues/1)).

### Fixed

- macOS: closing the rack window with unsaved changes asks first, as Cmd-Q does, and closing it
  quits StageHand even with a sequence or plugin editor window still open.

## [0.9.0] - 2026-11-14

First public preview.

### Added

- Live-performance host for CLAP plugins, with MIDI 1.0 and MIDI 2.0 both first-class: a MIDI 2.0
  keyboard reaches a MIDI 2.0 plugin at full resolution.
- Racks of plugins with their own inputs, filters and MIDI effects, in one window built for the
  stage.
- Sets: every song's rack and every plugin's state in one `.setlist` file, moving through the songs
  without gaps.
- A click, sequences and loopers to keep the band in time.
- App for Linux (x86-64) and macOS 26 or later (Apple Silicon).

### Known gaps

- See the README's Known gaps section.
