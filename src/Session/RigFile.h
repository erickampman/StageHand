#pragma once

#include <string>

#include "Session/RackFile.h"
#include "Session/Rig.h"

namespace stagehand {

// A rig bundle (docs/rig.md, "The rig on disk"): `<name>.rig/` with rig.json
// and state/ for its effects' saved state, read and written like a rack
// bundle (a newer format fails; a missing state file warns; written whole and
// swapped in). Main thread.
RackFileResult loadRig(const std::string& bundlePath, Rig& rig);
RackFileResult saveRig(const std::string& bundlePath, const Rig& rig);

// The bundle a chosen path means (the bundle, or its rig.json), or "" if it's
// neither.
std::string rigBundleOf(const std::string& chosen);

// The default rig, used by a rack on its own and a setlist that names none:
// $XDG_DATA_HOME/stagehand/Default.rig (default ~/.local/share/...).
std::string defaultRigPath();

} // namespace stagehand
