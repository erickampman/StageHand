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

} // namespace stagehand
