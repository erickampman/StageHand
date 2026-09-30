#pragma once

#include <string>
#include <vector>

#include "Session/Rack.h"

namespace stagehand {

// Outcome of reading or writing a rack bundle.
struct RackFileResult {
    bool                     ok = false;
    std::string              error;      // why it failed (when !ok)
    std::vector<std::string> warnings;   // succeeded, with problems worth showing
};

// Read a rack bundle (`<name>.rack/`: rack.json plus state blobs; see
// docs/rack-format.md). Structural problems (bad JSON, a newer format version,
// duplicate slot ids) fail; recoverable ones (a missing state file) warn and
// load. Main thread.
RackFileResult loadRack(const std::string& bundlePath, Rack& rack);

// Write a rack bundle safely: a complete new bundle is written beside the old
// one and swapped in, so a crash leaves either the old rack or the new one
// intact. Main thread.
RackFileResult saveRack(const std::string& bundlePath, const Rack& rack);

// --- paths (the command line and the rack window's Open and Save As) ---

// "~/x" expanded, then made absolute and normalized (it needn't exist). A
// trailing '/' is dropped: "X.rack/" is "X.rack".
std::string resolveRackPath(const std::string& text);

// The bundle a chosen path means: the bundle itself, or the bundle holding a
// chosen rack.json. Empty if it's neither (no rack.json there).
std::string rackBundleOf(const std::string& chosen);

// A Save As path: resolved, with ".rack" added if it has another extension or
// none ("Gig" -> "Gig.rack").
std::string saveAsPath(const std::string& chosen);

// What saving to `target` would replace. `current` is the rack being played
// (empty if none); saving back to it is always fine.
enum class SaveTarget {
    New,         // nothing there yet
    Current,     // the rack being played
    OtherRack,   // an existing rack: replace it only if the user says so
    NotARack,    // something else: never replaced
};
SaveTarget classifySaveTarget(const std::string& target, const std::string& current);

// A rack's display name from its bundle path: "~/Racks/Gig.rack" -> "Gig".
std::string rackNameOf(const std::string& bundlePath);

} // namespace stagehand
