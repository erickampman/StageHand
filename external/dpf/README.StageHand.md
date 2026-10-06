DPF (DISTRHO Plugin Framework): DGL, pugl and NanoVG for StageHand's own UI.
ISC license (LICENSE), with bundled components under their own licenses
(LICENSING.md).

Source: copied from PolygraphModularUnified's vendored copy (dgl-ui/dpf at PMU
commit ed3761e, 2026-09-28) so both projects run the same DGL. That copy
carries PMU's local patches (search for "PMU LOCAL PATCH"):

- dgl/src/WindowPrivateData.cpp: re-read the scale factor on every configure
  event and call onScaleFactorChanged(). Applies on all platforms.
- dgl/src/pugl-upstream/src/mac.m, mac_gl.m: macOS cross-display scale fixes.

StageHand's own patches (search for "STAGEHAND LOCAL PATCH"; docs/macos.md,
milestone 6):

- dgl/src/pugl.cpp (pugl.mm is a link to it), `puglSetResizable()`: on
  macOS, also sets the draw view's autoresizing mask, so a window made
  resizable after it's realized resizes its drawing with it.
- distrho/extra/FileBrowserDialogImpl.hpp, .cpp: on macOS, choosing a folder
  (`FileBrowserOptions::directories`; with an extension, a package of that
  type too, as a rack is once StageHand.app declares it), an extension filter
  (`FileBrowserOptions::extension`, through UTType), the suggested name when
  saving, the sheet begun at once rather than from `dispatch_async`, and
  `fileBrowserClose()` ending a sheet still open.

When PMU's copy changes, copy it here again (git archive of dgl-ui/dpf) and
update this note, keeping StageHand's patches. StageHand uses only the DGL
library (dpf__add_dgl_opengl), with the file browser on macOS; no plugin
wrappers.
