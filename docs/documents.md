# StageHand — Documents: the set as the only document (design)

Proposed 2026-10-28. Replaces the document model of setlists.md (section 1,
"On disk", and section 6), rig.md ("The rig on disk"), and rack-view.md
(section 7, "Files"). Those documents stay as the record of how StageHand
got here; where they disagree with this one, this one wins once it's built.

## Why

Today a rack can be in five states with respect to a setlist: on its own and
untitled, on its own and saved, the current song of a set, playing in a set
without being a song in it, and in a set with no songs. On top of those, a
song's rack can go missing, because a setlist only points at racks elsewhere
on disk. Saving follows three rules: a setlist is written at once, a rack is
saved on Cmd-S, at quit and when switching away from it outside a live
switch, and racks left during a live switch are kept unsaved in memory. The
rig is a third file, which a set may or may not name.

Each of those states needs its own UI ("Not a song in this set", "No songs
yet", "Find its rack...", "Add this rack to the set", "New setlist with this
rack"), and each was found the hard way.

## Decisions (2026-10-28)

1. **The set is the only document.** A set is a package holding its songs'
   racks, its rig and its order. A rack is no longer a document on its own:
   what used to be "a rack on its own" is a set with one song.
2. **A set never refers to anything outside itself.** No paths to racks
   elsewhere, no shared racks between sets, no missing songs.
3. **Racks move between sets by copy and paste**, as songs in the song list.
   A paste copies the rack into the receiving set.
4. **Several sets can be open at once**, one window each. The frontmost
   window's set is the active one: its current song plays and MIDI drives it.
5. **Bringing a window to the front switches the sound only while the
   transport is stopped.** With the transport running, the set playing keeps
   playing, so a stray Cmd-Tab or click on stage can't change the sound.
6. **Plugins are loaded only for the active set**: its current song, plus
   the songs prepared ahead (setlists.md, section 2). A set in the background
   loads none.
7. **Switching sets works like switching songs**: the new set's current song
   is prepared, then switched in under the same rules (setlists.md, sections
   3 and 5).
8. **Each set has its own rig**, inside its package. There's no app-wide
   default rig and no rig shared between sets. A new set starts with a copy
   of the active set's rig.
9. **A set always has at least one song.** A new set starts with one empty
   song; the last song can't be removed (Close is the way out).
10. **Existing files:** not a concern. If it's easy, opening an old
    `.setlist` converts it (copying its racks in, the old file left alone),
    and opening a `.rack` bundle makes an untitled one-song set from it.
11. **Linux:** a set is a folder, as a rack is today, and the Open dialog
    chooses a folder. Nothing new is needed there; file managers simply show
    it as a folder.

## The set on disk

```
Friday gig.setlist/          a package on macOS: one file in the Finder
  setlist.json               order, names, switch settings, MIDI triggers
  rig/                       the set's rig: rig.json, state/*.bin (rig.md's bundle)
  Racks/
    Autumn leaves.rack/      rack.json, state/*.bin (rack-format.md, unchanged)
    So what.rack/
```

```json
{
  "format": "stagehand-set", "version": 1,
  "songs": [
    { "rack": "Autumn leaves" },
    { "rack": "So what", "name": "So what (fast)" },
    { "rack": "Autumn leaves", "name": "Autumn leaves (reprise)" }
  ],
  "switching": { ... },
  "triggers": [ ... ]
}
```

- **`rack` is a name in `Racks/`**, not a path: always that set's own.
- **A reprise** is a second song naming the same rack. Editing it changes
  both appearances, as today (setlists.md, section 6); the prepared copy of
  the other appearance is prepared again from the edit.
- **A rack no song names any more** is deleted when the set is saved. It was
  only ever reachable through the set.
- **The rack and rig formats don't change.** Only where they live does.
- **Extension:** `.setlist`, declared as a package (conforming to
  `com.apple.package`, as `.rack` is now, with `LSTypeIsPackage`). `.rack`
  stops being a document type the app owns; it may come back as an export
  format (Later).

## Windows and the active set

- **One window per open set**, titled with the set's name ("Untitled" until
  saved). The song row at the top is as now.
- **The active set** is the one playing. It's the frontmost window's set,
  except while the transport runs (decision 5).
- **Bringing a window forward with the transport stopped** makes its set
  active: its current song is prepared and switched in, as a song switch
  would be. The old set's plugins are released once it has rung out.
- **Bringing a window forward with the transport running** changes nothing
  that sounds. The window shows that it isn't playing ("Not playing: stop
  the transport to switch to this set"). Stopping the transport doesn't
  switch either: bringing the window forward again, or a "Play this set"
  button in that notice, does.
- **Perform view** belongs to the active set's window.
- **A window in the background** shows its current song's rack as saved
  (or as edited, if it has unsaved edits in memory), without plugins: no
  plugin editors and no meters. Its song list works (select, copy, paste,
  move, rename, remove) since those need no plugins. Editing its rack needs
  it to be active.
- **MIDI triggers** (Next, Previous, Program Change, the transport's) are the
  active set's. A background set's triggers do nothing.

## Copy and paste

- **Copy** in the song list (Cmd-C on the selected song, or its menu) puts
  the song on StageHand's clipboard: its rack as it is now, unsaved edits
  included, and its display name.
- **Paste** (Cmd-V, or the song list's menu) adds it after the current song.
  - Into another set: the rack is copied into that set's `Racks/`. A name
    already there gets a number ("So what 2").
  - Into the same set: a new, independent copy ("So what 2"), not a reprise.
    "Add this song again" stays for a reprise.
- **Within one process only.** The clipboard holds a captured rack in
  memory; pasting between two StageHand processes isn't supported.
- **New song** (Cmd-N in a set) is as today's "New rack in this set": a copy
  of the current song, named by the user, added after it and played. It no
  longer asks where to save: it's saved in the set.

## Saving

- **Cmd-S saves the whole set**: `setlist.json`, the rig, and every rack
  with edits (the current song's and those kept in memory). One rule
  replaces three.
- **Edits to the song list are unsaved changes too**, no longer written at
  once. The window shows the set as edited until it's saved.
- **Switching songs never saves**: an edited rack is kept in memory, as a
  live switch keeps it now (setlists.md, section 6). The set shows its count
  of unsaved songs, as now.
- **Switching sets never saves either**: the set left keeps its edits in
  memory, and its window shows them.
- **Save As** saves the set as a new package, which becomes its file.
- **Close and Quit** ask about each set with unsaved changes: Save, Don't
  save, Cancel.
- **`--no-save-rack`** becomes `--no-save`: nothing is ever written.

## Reopening

`last.json` (in the app's state folder) records every open set: its path,
its current song, its window's position and size, and which one was active.
A start with no arguments reopens them all, with the same set active. An
untitled set isn't reopened (Later: a working copy, as KeyboardEDO keeps).

## The command line

| Option | Becomes |
|---|---|
| `--setlist PATH` | `--set PATH`: open that set. Several may be given; the last is active |
| `--rack PATH` | Opens the rack as an untitled one-song set (decision 10) |
| `--save-rack PATH` | Goes away. With `--plugin`, `--save PATH` names the new set |
| `--plugin PATH` | An untitled one-song set playing that plugin |
| no arguments | The sets open last (above), else one untitled set |

## What goes away

- The untitled rack on its own, and its "Keep the changes to Untitled?"
  (now the set's Close question).
- "Not a song in this set", "No songs yet", "Add this rack to the set"
  (added 2026-10-28), "New setlist with this rack", "New rack outside the
  set", "Close setlist" as a step back to a lone rack.
- Missing songs: "Find its rack...", relinking, `relinkSong()`.
- The default rig (`Default.rig` in the app's data folder) and a setlist's
  `"rig"` path.
- Saving a rack when Open or New replaces it (`replaceRack()`'s save first).
- `last.json`'s lone `rack` (added 2026-10-28).
- Shared racks between sets, and setlists.md's "A shared rack changes
  everywhere".

## Proposed milestones

1. **The set package.** `Session/SetFile`: load and save a set package
   (setlist.json, rig/, `Racks/`), with tests. Conversion from an old
   `.setlist` and from a `.rack`, if it's easy (decision 10).
   **Built 2026-10-28:** `loadSet()`; `saveSet()` in place (edited racks and
   the rig swapped in whole, then setlist.json, then unused racks removed) or
   to a new package (built beside it and swapped in, copying unedited racks
   and the rig from the set it came from); `convertSetlist()` and
   `setFromRack()` make an unsaved set in memory (`LoadedSet`), the old files
   left alone. The setlist's switch settings and triggers are read and
   written by the same code as before (`Session/SetlistJson.h`). Not used by
   the app yet (milestone 2). Tests: `tests/SetFileTests.cpp`.
2. **One set, always.** The app always has exactly one set open: a plain
   start gives an untitled one-song set; New and Open replace it; Cmd-S saves
   it whole. The states, commands and files listed under "What goes away"
   go. Song list edits mark the set edited.
   **Built 2026-10-28:** `RackController` always holds one set (`Set`,
   `setPath()`, "" = untitled), its racks by their bundle in the package
   (for an untitled set, never on disk: every rack but the running one is
   held in memory). `newSet()`, `openSet()` (a package; an older setlist or a
   lone rack made into an untitled set), `save()`, `saveAs()`,
   `unsavedChanges()` and `discardChanges()` replace the rack and setlist
   file commands; `newSong()` (a copy, "So what 2") and `importRack()` add
   songs; `removeSong()` keeps at least one and plays the next first. Song
   switches and Open never save. The rig is the set's (`rig/`), an untitled
   set's empty, a new set's a copy. The File menu: New set, Open..., Save,
   Save As..., New song, Import a rack..., Check set, Set settings. Close and
   Quit ask about unsaved changes (Save, or Save As; Discard; Cancel).
   `--set` (and `--setlist`), `--rack` and `--plugin` start a set;
   `--no-save` replaces `--no-save-rack`; `--save-rack` and `--overwrite` are
   gone. `last.json` keeps the set and song only. `.setlist` is declared a
   package in `Info.plist` now (from milestone 6), since the open panel can't
   choose a set otherwise. Docs still to update (milestone 6):
   setlists.md, rig.md, rack-view.md, rack-format.md, architecture.md and
   the help page.
3. **Copy and paste within a set**, and New song saved into the set.
4. **Several windows.** One window per open set; the active set and the
   front-window rule (decisions 4 and 5); background windows without
   plugins; switching sets as a song switch; the rig switched with the set.
   `last.json` records every open set.
5. **Paste between sets.**
6. **The bundle and the command line:** `Info.plist` (`.setlist` a package,
   `.rack` no longer a document type), Finder opens, `--set`, `--no-save`.
   Docs: command-line.md, setlists.md, rig.md, rack-view.md updated.

## Open questions

None at the moment. Decided 2026-10-28: stopping the transport never
switches sets on its own (bringing the window forward again, or "Play this
set", does); a new set's rig is a copy of the active set's; a paste goes
after the current song; racks no song names are deleted when the set is
saved.

## Later

- **Export a song as a `.rack`**, and import one, for sharing a single
  piece outside a set.
- **A working copy of an untitled set**, kept at quit and reopened.
- **Drag and drop** of songs between windows, as well as copy and paste.
