# resources

What StageHand is built with besides code, and where each came from.

- `StageHand.icon`: the app icon, an Icon Composer document (three bars and a
  wave in purple, green and orange). The Mac build compiles it with Xcode's
  `actool` (CMakeLists.txt; docs/macos.md, "Milestone 6, the icon"). Its
  source is the author's artwork folder.
- `brand/Unlikelyware-logo-144.png`: the Unlikelyware logo at the left of the
  toolbar, as KeyboardEDO has it (the same file: IFSynth's and
  PlainOldSynth's `Resources/Unlikelyware-Final-Color.png`, scaled to 144 px
  tall with ImageMagick's Lanczos filter).
- `fonts/Karla-Regular.ttf`: the wordmark's font, as KeyboardEDO's, from Dear
  ImGui's `misc/fonts`. By Jonathan Pinhorn, under the SIL Open Font License
  1.1, whose text is `fonts/OFL.txt`; it must ship with the font. "Karla" is
  a Reserved Font Name, so a modified version can't be called that.

The logo and font are embedded in the executable at build time
(`cmake/EmbedFile.cmake`). The rest of the window's text is DejaVu Sans,
DGL's own (the plugins' font too).
