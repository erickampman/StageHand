#pragma once

#include <string>

namespace stagehand {

// Where StageHand keeps its own files (docs/macos.md, "Paths"); racks,
// setlists and rigs elsewhere are wherever the user puts them.
//   data (the default rig):
//     Linux: $XDG_DATA_HOME/stagehand, default ~/.local/share/stagehand
//     macOS: ~/Library/Application Support/StageHand
//   state (where the app was, the window's size: this machine's):
//     Linux: $XDG_STATE_HOME/stagehand, default ~/.local/state/stagehand
//     macOS: ~/Library/Application Support/StageHand (not Preferences:
//     these aren't preferences)
// Neither is created here; files are written into them as needed.
std::string appDataDir();
std::string appStateDir();

} // namespace stagehand
