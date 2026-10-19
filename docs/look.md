# The look

StageHand's windows in the house look: the plugins' shapes and shading
(PlainOldSynth's), as KeyboardEDO has them (its `docs/decisions.md`, "Look
and feel"), in StageHand's own colours. Plugin editors keep their own look.
Begun 2026-10-19; settled with the author.

## Brand
- **The toolbar starts with the Unlikelyware logo and a two-colour
  "StageHand" wordmark**, as KeyboardEDO's and the plugins' top strips do:
  "Stage" in the icon's purple, "Hand" in its green, in Karla (KeyboardEDO's
  wordmark font) with its spacing tightened. A click on the logo opens
  unlikelyware.com, the browser brought to the front (on GNOME/Wayland with
  an X11 startup ID as its activation token: Ui/StartupId.h; Help too). Then a faint divider and the rack's name, with its
  "unsaved" note under it as before.
- **The brand and Help are on the top row** (2026-10-19): with a setlist
  open, that's the setlist row, the brand before the song controls and Help
  after the setlist's MIDI button; the toolbar then starts with the rack's
  name and ends with File.
- **The transport moved right, to the input column's edge** (330 px, from
  280), to make room. At the default width (1100) it ends about 10 px short
  of Perform; with 9 or more beats in the bar, or a narrower window, they
  meet, as they already did before the move.
- The logo and Karla are embedded at build time (`cmake/EmbedFile.cmake`;
  `resources/README.md`). The rest of the text stays DejaVu Sans, DGL's own
  and the plugins' font.

## Colours
From the icon (`resources/StageHand.icon`), tuned to one lightness, on
neutral charcoal panels (a step more neutral than before), so StageHand
stands apart from KeyboardEDO's plum and slate blue:
- **Green** (`ui::kAccent`, 116 201 142): the accent. What's on, lit or
  chosen; the faders' light; the buses' titles.
- **Purple** (`ui::kPurple`, 180 160 234): the second colour, for the
  sources. The dock's "Sources", the sequences' and loopers' lit buttons and
  step maps, and the sequence editor.
- **Orange** (`ui::kOrange`) only in the brand for now: amber is still the
  warning colour, and the two are close.
- Red, amber and the running green keep their meanings; the rows' colours
  (each row's stripe and tint) are unchanged.

## Controls (`Ui/UiWidget`)
- **Buttons**: KeyboardEDO's, a faint slate over whatever is behind them
  with a fainter border; lit, the colour itself with dark text
  (`buttonFace()`, under `button()`, the MIDI and remove buttons, play and
  Panic).
- **Faders are the house slider** (`slider()`), KeyboardEDO's lying down: a
  recessed slot, lit from 0 dB to the value with a faint glow, 0 dB marked
  across it, and the thumb the knob's glowing sphere with a white dot (grey
  when muted). The rows', the buses' returns and the master. How they move
  is unchanged: a drag moves them as far as the pointer, a double-click is 0
  dB, Shift is finer.
- **Menus are the plugins' dropdowns** (`dropdown()`): the choice in a dark
  box, a chevron at its right. A row's input, the output pairs, the buses'
  effect slots, the time signature, the setlist's song, and the sequences'
  and loopers' Rate, launch, quantize, start and length.
- **Value boxes** (`valueBox()`): the tempo and the steppers' values.
- **Sections** (`section()`): the plugins' group boxes, a rounded box a step
  up from the panel, its title in a colour. Each bus strip, and the dock's
  header and inputs. Each rack row is one too (`tintedSection()`, added
  2026-10-19), keeping its colour: the tint across it and the stripe down
  its left edge, inside the rounding; a 4 px gap between rows instead of a
  line. The sequence and looper rows stay full width.

## Adding an instrument
- **The space below the rows has a large + in its middle** (a faint circle
  and cross, added 2026-10-19). A right-click anywhere in that space, or a
  click on the +, opens the plugin list with only the instruments ("Add an
  instrument"); the choice is added at the bottom, as + Add plugin's is.
  The toolbar's + Add plugin still lists everything. With no rows, the
  rack's "no plugins yet" note sits under the +. When the space is too short
  for the +, the right-click still works there.

## Not yet
- Knobs: StageHand has none yet; `sphere()` is the knob's body, ready for
  the generic editor (docs/generic-editor.md), which should be built in this
  look from the start.
- The Perform view, the filter and Control Links panels, and the menus have
  the new colours and buttons but not yet a pass of their own.
- Checked by eye on macOS (Retina); Linux to build and look at.
