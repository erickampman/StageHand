# CLAUDE.md

## Workflow: fixes on a branch the author switches to

The author tests every change on their own machines (macOS and Linux) before
it goes into `main`, from a local clone at `~/Dev-MIDI/StageHand`.

- Put each fix on its own branch, based on the latest `origin/main`, commit
  it there, and push it to GitHub. Never commit to `main`.
- Then tell the author the branch's name and how to get it:
  ```bash
  cd ~/Dev-MIDI/StageHand && git fetch origin
  git switch <branch-name>
  ```
  (`git switch` makes a local branch tracking the remote one; `git switch
  main` goes back.)
- No patch files: the author prefers a branch to switch to.
- Say what was and wasn't checked. A cloud session runs on Linux and can't
  build the macOS code (`.mm` files, AppKit), so name what needs trying on a
  Mac.
- No pull request unless asked. The author merges, or deletes the branch,
  once they've tried it.
