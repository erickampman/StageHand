# Handoff: sessions, documents and packages (from KeyboardEDO, for StageHand)

KeyboardEDO got a set of changes on 2026-10-28 so it behaves like a normal
Mac document app. StageHand needs many of the same. This note explains what
was done, why, and the traps found along the way, so the work can be repeated
there without rediscovering them.

The KeyboardEDO commits (in `~/Dev-MIDI/KeyboardEDO`, branch `main`):

- `b4c0c4f`: sessions and held notes are remembered; New document; arpeggiator Clear
- `b27cae5`: on the Mac, a `.kedo` document is one file (declared a package)

`git show b4c0c4f` and `git show b27cae5` give the full diffs. The main files
are `src/app/KeyboardApp.cpp`, `src/app/main.cpp`, `src/kedo/Preferences.*`,
`src/kedo/Document.*` and `cmake/Info.plist.in`.

StageHand is laid out differently (racks, rigs, setlists, `src/Session/`,
`AppDelegateMac.mm`, its own `FileDialog`). Treat what follows as patterns to
apply, and check what StageHand already does before changing anything. See
"StageHand: what to check first" at the end.

---

## 1. Reopen where the app left off

**Problem:** every launch started from defaults, so all settings were lost.

**What KeyboardEDO does now:**

- **On quit:** the app saves the current document, saved or not, as a working
  copy in its per-user folder (`SDL_GetPrefPath` → `session.kedo`). It also
  records in `preferences.json` which real document that copy came from
  (`documentPath`, empty if untitled).
- **On a plain launch** (no document and no settings on the command line), it
  loads the working copy. If `documentPath` still exists, that document is
  loaded too, but only to compare against. That's how the title knows whether
  to say "edited". If the original is gone, the session opens as Untitled.
- **The user's own document is never written behind their back.** Unsaved
  edits stay unsaved and show as edited.
- **Test runs** (`--snapshot`) neither read nor write the session or the
  preferences.

**Traps:**

- **Save before releasing anything on quit.** The destructor used to release
  every note first, which emptied the very state that needed saving.
  `saveSession()` must run before `releaseAll()`.
- **Every running instance shares one session file**, and whichever quits last
  overwrites it. When testing, back the file up first and put it back
  afterwards; the user may have their own copy running.
- **Decide what counts as "no arguments".** KeyboardEDO restores only when
  there's no document argument and no option other than `--publish`.
  Options like `--edo` apply to a fresh document instead.

## 2. A "New document" command

Once launch reopens the last session, there has to be a way to start fresh.
KeyboardEDO has New as a toolbar button, Cmd/Ctrl-N, and a button in Settings'
File section. It releases what's held, resets to a default document with an
empty "saved" baseline and no path, and rebuilds, mirroring `open()`.

The user considered having every launch show a "Continue / New / Open"
chooser instead, and turned it down: launching keeps reopening where you left
off, and New is always available.

Neither New nor Open asks to save unsaved changes. That's a known gap, worth
deciding on for StageHand.

## 3. Live state saved in the document, in order

The user wanted what was being *played* (notes latched with Shift or the Latch
toggle, and notes the arpeggiator's Hold keeps) to survive both a relaunch and
Save/Open. The pattern applies to any live state StageHand might want to keep,
such as looper or sequence state:

- **The document has a field for it** (`Document::held`, a list of
  `{id, velocity, latched}`), written to `document.json` only when it isn't
  empty.
- **While the document is showing, the live objects own the state**, not the
  document struct. `documentNow()` collects it into the document whenever the
  document is saved, compared, or written as the session; `doc_` itself keeps
  none.
- **When opening:** move the stored list out of the document, rebuild the keys
  first, then replay it (`restoreHeldNotes()`), in the constructor and in
  `open()`.
- **Changing that state marks the document edited.** That follows from it
  being saved content. Merely playing notes doesn't.

**Traps (each one came up as a user report):**

1. **One piece of state lived in two places.** A Shift-latched note is held
   both by the keyboard's latch and in the arpeggiator's pool. Saving only
   one of them lost the other.
2. **Order matters.** The arpeggiator's As Played pattern follows the order
   notes were played, but `std::map` sorts by id. The fix was to keep the
   order explicitly (`Performer::latchOrder_`), take the save order from the
   arpeggiator's pool, and replay in that order.
3. **Velocity wasn't kept** for latched notes, so it had to be tracked.

## 4. Arpeggiator behaviour (KeyboardEDO only)

This is probably not needed in StageHand, but listed so the commit reads
clearly:

- Turning the arpeggiator off keeps the notes Hold holds, silently, and plays
  them again when it's turned back on.
- A Clear button lets go of those notes and of every latch.
- Clicking a held note lets go of it. A latched note struck again lets go
  even past Hold, through a new `NoteSink::letGo()` (by default it's
  `noteOff`; the arpeggiator overrides it).

## 5. A document folder that looks like one file on the Mac

**Problem:** a `.kedo` document is a folder (`document.json`, `layouts/`).
The user wanted it to look like one file, as other apps' documents do.

**What KeyboardEDO does now:**

- **`Info.plist`** has `UTExportedTypeDeclarations` for
  `com.unlikelyware.keyboardedo.document`, conforming to `com.apple.package`
  and `public.composite-content`, with filename extension `kedo`. It also has
  a `CFBundleDocumentTypes` entry with role Editor, `LSHandlerRank` Owner,
  `LSTypeIsPackage` true, and that type in `LSItemContentTypes`. The Finder
  then shows the folder as one "KeyboardEDO Document", and double-clicking
  it opens the app.
- **The Open dialog on the Mac** uses `SDL_ShowOpenFileDialog` with **no
  filter**. Other platforms keep `SDL_ShowOpenFolderDialog`.
  - **The trap:** SDL turns a filter like `"kedo"` into a type with
    `[UTType typeWithFilenameExtension:]`, which assumes an ordinary file
    (`public.data`). It returns a made-up `dyn.…` type that a package never
    matches, so the document showed **greyed out**. With no filter, a package
    can be picked like a file and an ordinary folder still opens for browsing.
    You can confirm it with
    `swift -e 'import UniformTypeIdentifiers; print(UTType(filenameExtension: "kedo")!.identifier)'`.
- **Opening from the Finder, or dropping onto the window:** SDL delivers both
  as `SDL_EVENT_DROP_FILE`, which KeyboardEDO handles by calling `open()`.
  StageHand has its own `AppDelegateMac` `openURLs:`, so this part may already
  be covered.
- **Registering the build:** for a development build, run
  `lsregister -f build/KeyboardEDO.app`. `lsregister` lives at
  `/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister`.
  Check the result with `URL.resourceValues(forKeys: [.isPackageKey, .contentTypeKey])`.
  `mdls` may still say "Folder" for files in folders Spotlight doesn't index;
  that's stale and can be ignored.

## Testing notes

- **KeyboardEDO's `--snapshot` steps** (`--tap X,Y`, `--key`) are good for
  drawing and plain clicks. Its `shift+` taps were unreliable, so the
  Shift-latch logic was checked with a unit test instead
  (`testLatchThroughHold` in `tests/PerformerTests.cpp`).
- **Simulated keypresses don't press a sheet's Open button.** An AppleScript
  Return keystroke left the dialog open, and plain UI scripting couldn't see
  the sheet. Pressing Open through the accessibility API did work
  (computer-use `app_click` on the AXButton "Open"). This only works on an app
  launched as a bundle (`open -a build/X.app`), not through the
  `build/keyboardedo` symlink, which doesn't register under the bundle id.
- **To check a session round trip:** edit the session's `document.json`
  (for example its `held` list), launch, quit, and confirm it comes back
  unchanged. A key that no longer exists, or a restore that failed, shows up
  as a missing entry.

---

## StageHand: what to check first

From a quick look at `~/Dev-MIDI/StageHand`:

- **`cmake/Info.plist.in` already exports packages for `.rack` and `.setlist`,
  with `LSTypeIsPackage`.** Section 5's plist work may be done. Check that
  both show as one file in the Finder and that double-clicking opens them.
- **`src/App/FileDialog.h` opens a `.rack` with `OpenFolder`.** On the Mac,
  check whether a declared package can be chosen in that dialog, and whether
  an `OpenFile` with a `rack` filter shows it greyed out (section 5's trap).
- **`src/Session/AppPaths.h` already has a state folder** ("where the app was,
  the window's size"). That's where a session or last-document record belongs
  (section 1), rather than a new preferences file.
- **`src/App/AppDelegateMac.mm` handles `openURLs:`,** so opening from the
  Finder probably exists already.
- **To find out:** does StageHand already reopen the last rack, setlist or rig
  on launch? Does it have New? What live state (looper, sequences, held
  notes) should survive a save and a relaunch, and in what order?
