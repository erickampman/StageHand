# Contributing to StageHand

Building and testing: [BUILDING.md](BUILDING.md).

## Changing uw-core or DPF

When StageHand's change needs a uw-core change (or a DPF one), commit and push uw-core (or pugl,
then DPF) first, then commit StageHand's new pin. Otherwise the other machine can't fetch the pin.
To have git refuse such a push:

```bash
git push --recurse-submodules=check             # this push only
git config push.recurseSubmodules check         # the default for this clone
```

The `git config` line is per clone: run it in every clone that has submodules (StageHand, Umpire,
each plugin), on each machine.
