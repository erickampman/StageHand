# StageHand — TODO

- **A looper that adds a sequence.** Recording what's played into a new
  sequence (docs/sequences.md), with a MIDI signal to start it and to
  stop / repeat it (MIDI learn, like the sequences' and the transport's
  triggers). The recorded sequence is not saved into the rack automatically.

- **Rack rows come first when they don't all fit.** Today the two bus strips
  take fixed space between the rows and the Sources dock, so with many rows the
  rows area shrinks and scrolls while the buses stay. The rack's rows should
  have priority over the bus rows (e.g. the bus strips scroll with the rows,
  after the last one, or collapse).
