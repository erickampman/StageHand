#pragma once

#include <map>
#include <set>
#include <string>

#include "Session/RackFile.h"
#include "Session/Rig.h"
#include "Session/Setlist.h"

namespace stagehand {

// A set package (docs/documents.md, "The set on disk"): `<name>.setlist/`
// holding setlist.json (the Set), rig/ (a rig bundle, docs/rig.md) and
// Racks/<rack>.rack/ for its songs (docs/rack-format.md). A set refers to
// nothing outside itself. Main thread.

// Where a set's parts are.
std::string setRackPath(const std::string& packagePath, const std::string& rack);
std::string setRigPath(const std::string& packagePath);

// Is there a set package at `path` (a folder holding setlist.json)?
bool isSetPackage(const std::string& path);
// A path chosen to save a set to: resolved, with ".setlist" added if it's
// missing ("Friday" -> "Friday.setlist").
std::string setPathOf(const std::string& chosen);
// A set's display name: "~/Sets/Friday gig.setlist" -> "Friday gig".
std::string setNameOf(const std::string& packagePath);

// Can `name` be a rack's name in a set (a folder name in Racks/)? Not empty,
// no '/', not starting with '.'.
bool validRackName(const std::string& name);
// `base`, or "base 2", "base 3"... : the first that names none of `taken`.
std::string freeRackName(const std::string& base, const std::set<std::string>& taken);

// Read setlist.json. A song naming a rack that isn't in Racks/ is left out,
// with a warning. Bad JSON or a newer format version fails.
RackFileResult loadSet(const std::string& packagePath, Set& set);

// Write the set to `packagePath`. Racks in `racks` (by name) are written;
// every other rack a song names is kept as it is in `from` (the package the
// set was read from; "" = `packagePath` itself). `rig` null keeps `from`'s rig
// (none there: an empty one). Racks no song names are left out, so a removed
// song's rack is gone once the set is saved.
// - Saving in place (`from` is `packagePath`, or ""): each edited rack and the
//   rig are swapped in whole, then setlist.json, then unused racks go.
// - Saving to a new package: built beside it and swapped in whole. Something
//   at `packagePath` that isn't a set is never replaced.
// Fails if the set has no songs, a song's rack name isn't valid, or a rack is
// neither in `racks` nor in `from`.
RackFileResult saveSet(const std::string& packagePath, const Set& set, const Rig* rig,
                       const std::map<std::string, Rack>& racks, const std::string& from = {});

// A set read whole into memory, not saved anywhere yet: what opening an older
// file makes (docs/documents.md, decision 10).
struct LoadedSet {
    Set                         set;
    Rig                         rig;
    std::map<std::string, Rack> racks;
};
// An older `.setlist` file, which refers to racks elsewhere: each rack copied
// in (named for its bundle, numbered if two differ in folder only; a reprise
// stays one rack), with its rig (or the default rig). A missing or unreadable
// rack is left out, with a warning; fails if none is left.
RackFileResult convertSetlist(const std::string& setlistFile, LoadedSet& out);
// A rack bundle on its own, as a one-song set with the default rig.
RackFileResult setFromRack(const std::string& rackBundle, LoadedSet& out);

} // namespace stagehand
