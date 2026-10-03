# StageHand — Count-in and the Perform view (design)

Proposed 2026-10-03; nothing built yet. A count-in before a song starts,
chosen per song, and a Perform view: the main window as it should look on
stage, with the beat large enough to read from across it. Builds on:
- sequences.md, section 7: Start and Stop, At Start, start on first note,
  the beat lights and the click (whose pulses `metronomePulse()` counts).
  Its "Later" list has the count-in bar.
- setlists.md, section 5 (switch settings: a switch with the transport kept
  running lands at the next bar) and "Later" (the Perform view).
- looper.md, decision 8: arming a looper while stopped starts the transport.
- rig.md: the rig holds the click's output, the same in every song.

## Count-in

1. **Per song: off, half a bar, 1 bar or 2 bars** (half a bar added
   2026-10-03), saved with the rack beside the click
   (`"metronome": {"countIn": 0.5 | 1 | 2}`, in bars; absent: off). Half a
   bar is half the bar's pulses, rounded up: 4/4 two, 6/8 one, 7/8 four. It's the song's, as its
   tempo is: a ballad may want two bars, a song that starts on a held chord
   none.
2. **Count-in is pre-roll.** Start (▶, Space, a learned trigger) begins the
   count-in, not the song: the count-in's bars click, and **bar 1, beat 1
   lands right after them**. Everything that starts with the transport starts
   there, exactly as it does today without a count-in: At Start sequences,
   a sequence started while stopped (on bar 1, after the count-in), a looper
   armed while stopped (recording from bar 1).
3. **Plugins see the transport stopped** during the count-in
   (`IS_PLAYING` off, the position at bar 1), and running from bar 1. No
   negative positions: some plugins mishandle them, and a synced LFO or
   delay should start with the song, not the count.
4. **It always clicks,** even in a song whose click is off: the count-in is
   for the band's ears. At the song's click level (or the default, -12 dB,
   if its click is off), on the click's output. The downbeat of each
   count-in bar takes the downbeat's pitch, as now.
5. **It counts the click's pulses** (`metronomePulse()`): 4/4 counts four a
   bar, 6/8 two, 7/8 seven, or every note if the song's lights count notes.
6. **No count-in when the music is already going:**
   - **Start on first note:** the note is bar 1. A count-in would put it
     a bar or two early.
   - **A song switch with the transport kept running** (setlists.md, section
     5): it lands at the next bar, in time.
   - **A count-in already running:** Start again does nothing.
7. **Stop during a count-in** cancels it: back to stopped at bar 1, nothing
   started. A Toggle trigger pressed during the count-in is a Stop.
8. **A switch during a count-in** (the next song chosen while counting): the
   count-in is cancelled with the old song; the new song starts stopped (or
   as its switch settings say).
9. **Seen:** the toolbar's beat lights show the count-in in amber, and the
   tempo control's place shows "Count-in 2 … 1"; the Perform view shows it
   large (below).

### In the engine

- A **count-in state** in the transport: stopped → counting (N bars) →
  running. Counting keeps its own sample clock from the start's frame; the
  beat clock (and what plugins see) stays at bar 1, stopped. At the frame
  where the count-in's last beat ends, the transport starts exactly as a
  Start does today at that frame: the same code path, so At Start and the
  rest need no change.
- The count-in's clicks are rendered by the click's renderer, placed to the
  sample, whether the song's click is on or not.
- Published for the UI with the beat: counting or not, and the beats left.

## The Perform view

10. **A mode of the main window,** not another window (on stage it's one
    screen): **Perform** in the toolbar, or **Tab** (decided 2026-10-03:
    easy to hit, and that's safe: switching views never changes the sound or
    the rack), and the same to go back. Not while a name is being typed.
    StageHand remembers which it was in (with the window's size, this
    machine's) and reopens in it.
11. **What it shows**, large, for a dark stage:
    - **The song:** its number and name, large, and "next: …" under it,
      with the setlist's ◀ ▶ as big buttons (touch-sized). Without a
      setlist: the rack's name, and no ◀ ▶.
    - **The beat:** one large box per pulse of the bar (4/4: four), the
      current one lit; the downbeat in its own colour. Bar and beat in big
      digits ("17 · 3"), the tempo and time signature beside them.
    - **The count-in:** the beats left, very large, in amber ("4 3 2 1"),
      replacing the beat boxes until bar 1.
    - **The transport:** ▶/■ large; stopped, counting and running each
      clearly different.
    - **The loopers:** the current looper's state and progress, as one wide
      bar (it's what the looper switch drives).
    - **Status:** the click on or off, and warnings (a missing device), as
      today's status bar does.
12. **What it hides:** the rows, the dock and every editing control, so a
    stray click can't change the rack. Plugin editors stay as they are.
13. **Edge flash** (an option, off by default): the window's whole border
    flashes on each pulse, brighter on the downbeat, readable from across
    the stage or in peripheral vision. Available in both views. **Kept in
    the rig** (decided 2026-10-03), with the click's output and the looper
    switch: the stage's setup. So it's the same in every song of a set;
    a setlist naming another rig can have it otherwise; it travels with the
    rig file to another machine; turning it on marks the rig modified
    (Ctrl-S saves it); and a rack opened on its own uses the default rig's.
14. **In time with the sound:** the beat display runs at the display's rate
    while the transport runs (not the 30 Hz tick), and is drawn ahead by
    the audio output's latency, so a flash lands with the click you hear,
    not 10–20 ms after it.
15. **Keys:** in the Perform view, ← → are Previous and Next song, Space is
    Start/Stop, Tab goes back. Nothing else edits.

## Milestones (proposed)

1. **Count-in, engine:** the counting state, its clicks, bar 1 after it,
   and the cases above (start on first note, a switch while running, Stop
   and a switch during it, sequences and loopers waiting for bar 1, half a
   bar's pulses). Tests
   with the fake plugin: sample positions of the clicks and of bar 1, what
   plugins see during the count-in, every case in 6 to 8.
2. **Count-in, setting:** `"countIn"` in the rack file, the ⋮ menu (Count-in:
   off, half a bar, 1 bar, 2 bars), the amber lights and the toolbar countdown. Tests:
   the rack file, the controller.
3. **The Perform view:** the mode, the layout above, the keys, the remembered
   mode, the edge flash (in the rig), drawing at the display's rate and
   ahead by the output latency.

## Open questions

None at the moment (answered 2026-10-03: half a bar as well; the edge flash
in the rig; Tab for the Perform view).
