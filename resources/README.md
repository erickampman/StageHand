# resources

What StageHand is built with besides code, and where each came from.

- `StageHand.icon`: the app icon, an Icon Composer document (three bars and a
  wave in purple, green and orange). The Mac build compiles it with Xcode's
  `actool` (CMakeLists.txt; docs/macos.md, "Milestone 6, the icon"). Its
  source is the author's artwork folder (the bars apart, from 2026-10-19).
  `tools/render-mac-icon.sh` renders a `.icon` to 1024 px PNGs (full, and
  in Apple's 824 px grid) for making an icon set by hand.
- `brand/Unlikelyware-logo-144.png`: the Unlikelyware logo at the left of the
  toolbar, as KeyboardEDO has it (the same file: IFSynth's and
  PlainOldSynth's `Resources/Unlikelyware-Final-Color.png`, scaled to 144 px
  tall with ImageMagick's Lanczos filter).
- `fonts/Karla-Regular.ttf`: the wordmark's font, as KeyboardEDO's, from Dear
  ImGui's `misc/fonts`. By Jonathan Pinhorn, under the SIL Open Font License
  1.1, whose text is `fonts/OFL.txt`; it must ship with the font. "Karla" is
  a Reserved Font Name, so a modified version can't be called that.

- `help/help.md`: the help page's source, in Markdown Web Writer's dialect
  (cards, disclosures, `(#anchor)` lines), as KeyboardEDO's is. Its export,
  `help/help.html`, is made in Markdown Web Writer by the author; the page
  shows the logo beside it as `Unlikelyware.png`. The build copies both
  (the logo from `brand/Unlikelyware-logo-144.png`) to `build/help`, and on
  the Mac into the bundle's `Resources/help`, where Help (the toolbar's
  "?", the File menu) finds them (docs/rack-view.md).

The logo and font are embedded in the executable at build time
(`cmake/EmbedFile.cmake`). The rest of the window's text is DejaVu Sans,
DGL's own (the plugins' font too).
