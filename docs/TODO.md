# StageHand — TODO

- **Rename a sequence by clicking its name** (or double-clicking it). Today a
  click on the name opens the editor, and renaming is in the row's "•••" menu
  (Rename...). With the Edit button there, the name can be for renaming
  instead. The text field already exists (UiWidget::editText; RackWidget::
  startRename).
- **Replace the row's "•••" control with a red X** that brings up the same
  Remove / Cancel confirmation. It's then only for deleting the sequence:
  editing is the Edit button, and renaming moves to the name (above). The
  "•••" menu is RackWidget::openSequenceMenu.
- **Ctrl-drag (velocity) has no visual feedback.** In the sequence editor,
  Ctrl-dragging a note vertically changes its velocity (docs/sequences.md,
  section 5), and it works, but nothing shows it happening: e.g. show the
  velocity value while dragging, and make the note's brightness change
  visibly.
