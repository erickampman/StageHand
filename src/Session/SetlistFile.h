#pragma once

#include <cstddef>
#include <string>

#include "Session/RackFile.h"
#include "Session/Setlist.h"

namespace stagehand {

// Read a `.setlist` file (docs/setlists.md, section 1). Song racks come back
// absolute: relative paths are resolved against the setlist's folder, "~"
// expanded. A song whose rack is missing is kept (it's marked missing where
// it's shown, never dropped). Bad JSON or a newer format version fails.
// Main thread.
RackFileResult loadSetlist(const std::string& path, Setlist& setlist);

// Write it atomically (a temporary file renamed over the old one). A rack is
// written relative to the setlist's folder when they share more than the
// filesystem root (so copying a folder holding both keeps it working),
// otherwise absolute with "~" abbreviated.
RackFileResult saveSetlist(const std::string& path, const Setlist& setlist);

// A path chosen to save a setlist to: resolved, with ".setlist" added if it's
// missing ("Friday" -> "Friday.setlist").
std::string setlistPathOf(const std::string& chosen);
// A setlist's display name: "~/Sets/Friday gig.setlist" -> "Friday gig".
std::string setlistNameOf(const std::string& path);
// Is there a setlist file at `path` (resolved)?
bool isSetlistFile(const std::string& path);

// --- where the app was (docs/documents.md, "Reopening") ---
// The last set and the song it was on, so a start with no arguments reopens
// there. `setlist` (the set's package) empty: it was untitled (start as usual).
struct LastPosition {
    std::string setlist;
    std::size_t song = 0;
};
// last.json in appStateDir() (Session/AppPaths.h).
std::string defaultLastPositionPath();
bool        loadLastPosition(const std::string& path, LastPosition& position);   // false: none, or unreadable
bool        saveLastPosition(const std::string& path, const LastPosition& position);

} // namespace stagehand
