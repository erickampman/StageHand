DPF (DISTRHO Plugin Framework): DGL, pugl and NanoVG for StageHand's own UI.
ISC license (LICENSE), with bundled components under their own licenses
(LICENSING.md).

Source: copied from PolygraphModularUnified's vendored copy (dgl-ui/dpf at PMU
commit ed3761e, 2026-09-28) so both projects run the same DGL. That copy
carries PMU's local patches (search for "PMU LOCAL PATCH"):

- dgl/src/WindowPrivateData.cpp: re-read the scale factor on every configure
  event and call onScaleFactorChanged(). Applies on all platforms.
- dgl/src/pugl-upstream/src/mac.m, mac_gl.m: macOS cross-display scale fixes.

When PMU's copy changes, copy it here again (git archive of dgl-ui/dpf) and
update this note. StageHand uses only the DGL library (dpf__add_dgl_opengl);
no plugin wrappers.
