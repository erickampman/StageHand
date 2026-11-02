# Contributing to StageHand

Bug reports and pull requests are welcome. Building and testing: [BUILDING.md](BUILDING.md); how it
fits together: [docs/architecture.md](docs/architecture.md) and the other design records in
[docs/](docs). This page is the rules the code depends on.

## Before each commit

```sh
ctest --test-dir build --output-on-failure
```

The tests need no audio or MIDI hardware. For a change you can hear or see, also run StageHand with a
plugin (`stagehand --plugin <id>`, ids from `stagehand --list-plugins`) on the platform you changed;
a rebuilt plugin needs StageHand restarted, since a running host keeps the plugin binary it loaded.

## Rules

- **MIDI 1.0 and MIDI 2.0 are both first-class.** A MIDI 2.0 source reaches a MIDI 2.0 plugin with
  its full resolution and per-note expression; translation happens only where the two meet.
- **Nothing stage-facing moves by accident.** Faders move only when dragged, a song switch lets the
  last song's tail ring out, and the Perform view has nothing on it to knock. Keep it that way.
- **Real time.** No allocation, locks or file access on the audio thread.
- **Documents.** A set (`.setlist`) and a rack (`.rack`) are formats people keep: a change that
  alters what's saved also updates [docs/documents.md](docs/documents.md) or
  [docs/rack-format.md](docs/rack-format.md), and older files still open.
- **The help page.** `resources/help/help.html` is generated from `help.md` beside it, by the pandoc
  command at the help page in `CMakeLists.txt`. Its template isn't in this repository yet: edit
  `help.md`, and say so in the pull request.

## Changing uw-core or DPF

When StageHand's change needs a uw-core change (or a DPF one), commit and push uw-core (or pugl,
then DPF) first, then commit StageHand's new pin. Otherwise the pin can't be fetched elsewhere.
To have git refuse such a push:

```bash
git push --recurse-submodules=check             # this push only
git config push.recurseSubmodules check         # the default for this clone
```

The `git config` line is per clone: run it in every clone that has submodules, on each machine.

## Third-party code and licenses

StageHand is MIT licensed, and contributions are taken under the same license: a pull request is
offered as MIT.

- **What ships.** [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) lists every third-party
  component in the builds, with its version, license and full license text, and every binary
  package includes it. A change that adds a component, or moves one to a new version, updates its
  entry in the same commit.
- **Upstream code stays as it came.** Submodules aren't edited in place: a fix goes upstream, to
  Unlikelyware's DPF or pugl fork, or into a patch file that `tools/fetch_deps.sh` applies.
  Copied-in files keep their copyright and license headers.
- **A new dependency** must allow MIT-licensed source and binary distribution with nothing more
  than keeping its notice: MIT, BSD, ISC, zlib, Apache-2.0 (with its NOTICE file, if it has one) or
  public domain, and the SIL Open Font License for fonts. No GPL or LGPL code.
- **Trademarks.** VST® is a registered trademark of Steinberg: the first "VST" in any document
  carries the ®, and a document that names VST ends with Steinberg's attribution line. Showing the
  VST Compatible Logo brings in the rest of [Steinberg's VST Usage Guidelines](https://github.com/steinbergmedia/vst3sdk/blob/master/VST3_Usage_Guidelines.pdf). Name
  hosts and other products plainly, to say what works with what, never so it reads as an
  endorsement.

VST is a registered trademark of Steinberg Media Technologies GmbH.
