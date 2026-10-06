#pragma once

#include <functional>
#include <string>

namespace stagehand {

// AppKit's own quit (the Dock's Quit, "quit" from AppleScript, logging out or
// shutting down) would call exit() and skip StageHand's shutdown: nothing
// saved, plugins not stopped. With this installed, AppKit's quit is cancelled
// and `quit` runs instead, which quits the way Cmd-Q does (asking first if
// there's something to ask). A logout or shutdown is cancelled too: StageHand
// quits cleanly and the user logs out again. Main thread; replaces any
// delegate NSApp has.
//
// And opening documents: a rack or setlist double-clicked in Finder, dropped
// on the Dock icon, or `open`ed, arrives as `open` with its path (one call per
// document, in order), where AppKit would otherwise say it can't open them.
void installAppHandlers(std::function<void()> quit, std::function<void(const std::string& path)> open);

} // namespace stagehand
