# StageHand — Working with cloud sessions

How to hand a piece of StageHand work to a Claude Code cloud session (run
from claude.ai/code on a virtual machine) and bring the result back. Written
2026-10-02.

## What a cloud session can and can't do

A cloud session works on a copy of the repository from GitHub, not on this
machine, and delivers its work as commits on a branch with a pull request.

It **can**:
- Build StageHand, once it has installed the build dependencies (below).
- Run the test suites. They use fakes (`tests/FakePlugin.h`,
  `tests/SessionFakes.h`), so they need no audio, MIDI or plugins.
- Read the design docs in `docs/` and follow them.

It **can't**:
- Run StageHand or hear anything: there's no PipeWire, JACK, MIDI keyboard
  or speakers.
- See or click the window: there's no display, so UI work can't be checked.
- Use the CLAP plugins installed here (`~/.clap`, `/usr/lib/clap`).
- See work that isn't pushed, or the notes a local session keeps in
  `~/.claude` on this machine. The prompt has to carry what it needs.

**Good fits:** engine, Session and controller work with tests (for example
setlist milestone 2, prepared racks). **Poor fits:** UI work (rig milestone
3), and anything that has to be heard.

Run one cloud task at a time, and don't work locally on the same files
meanwhile: their branches would conflict.

## The workflow

1. **Commit and push** everything first. The cloud session starts from what's
   on GitHub.
2. **Start the session** at claude.ai/code on `erickampman/StageHand`, branch
   **`rack-view`** (the setlist and rig work is there, not on `main`).
3. **Paste a self-contained prompt** (the template below).
4. **When it finishes,** it pushes a branch and opens a pull request into
   `rack-view`.
5. **Try it before approving.** Fetch its branch, build, run the tests, and
   try it in StageHand:

   ```bash
   git fetch
   git switch <its-branch>
   cmake --build build
   (cd build && ctest)
   stagehand
   ```

6. **If it's good,** merge the pull request on GitHub, then bring it into
   your local branch:

   ```bash
   git switch rack-view
   git pull
   ```

7. **If it isn't,** comment on the pull request or tell the cloud session
   what's wrong; it keeps working on the same branch.
8. **Back in a local session,** say what was merged, so its notes catch up.

## Prompt template

Fill in the task; the rest stays the same.

```
Repo: StageHand (C++20, CMake + Ninja). Work on branch rack-view; open a PR into
rack-view when done.

Setup: git submodule update --init --recursive (external/uw-core, a private repo, and the CLAP SDK inside it). Build deps: cmake, ninja-build,
pkg-config, libjack-jackd2-dev, libasound2-dev (alsa >= 1.2.10), libx11-dev, libgl-dev
(libxrandr-dev optional: a new window fits its monitor).
Build: cmake -B build -G Ninja && cmake --build build && (cd build && ctest).
There is no audio, MIDI or display here: rely on the test suites (they use fakes in
tests/).

Task: <the task, naming the doc and milestone>

Conventions: read the design doc for the task, plus docs/rack-view.md, first. Match the
code's existing style and comment density. Add tests next to the existing ones. When
done, add a short "implemented" note under the milestone in its doc, like the ones
already there. All suites must pass.
```

### Example: setlist milestone 2

```
Task: implement milestone 2 of docs/setlists.md ("Prepared racks", section 2): split
Session::load() into prepare and activate steps, prepare the next and previous songs'
racks incrementally (one plugin per controller tick), and switch with a single graph
commit. Respect the edge cases the doc assigns to milestone 2 (one in-memory copy per
rack path; windows closed at the switch) and docs/rig.md: the rig's effects must stay
loaded across switches. Add tests in tests/SessionTests.cpp and
tests/RackControllerTests.cpp.
```
