#pragma once

#include <string>
#include <vector>

#include "Core/Sequence.h"

namespace stagehand {

// A setlist (docs/setlists.md): the order of a set and how to move through it.
// It refers to racks; it doesn't contain them, and it says nothing about the
// sound (that's the rack's). See "Domains" there.

// One place in the set: a rack, and optionally the name it goes by here.
struct SetlistSong {
    std::string rack;   // the rack bundle, absolute (relative to the setlist in the file)
    std::string name;   // "" = the rack's own name

    bool operator==(const SetlistSong&) const = default;
};

// How a switch behaves, for the whole set (docs/setlists.md, section 5).
// Saved and loaded now; acted on from milestone 5.
struct SwitchSettings {
    enum class Transport { Keep, Stop };
    enum class When { Now, NextBar };
    enum class OldRack { RingOut, Cut };
    Transport transport = Transport::Keep;
    When      when      = When::NextBar;   // the default since 2026-10-03 (was Now): in time
    OldRack   oldRack   = OldRack::RingOut;

    bool operator==(const SwitchSettings&) const = default;
};

struct Setlist {
    std::vector<SetlistSong> songs;
    SwitchSettings           switching;
    std::string              rig;   // its rig bundle (docs/rig.md), absolute; "" = the default rig
    std::vector<SetlistTrigger> triggers;   // section 4: Next, Previous, Program Change

    bool operator==(const Setlist&) const = default;
};

// --- the set (docs/documents.md): a package holding its racks and its rig ---

// One place in a set: a rack in the set's own Racks/ folder, by name, and
// optionally the name it goes by here. Two songs naming one rack are a reprise.
struct SetSong {
    std::string rack;   // "So what" is Racks/So what.rack
    std::string name;   // "" = the rack's name

    bool operator==(const SetSong&) const = default;
};

// setlist.json in a set package. The rig and the racks are files of their own
// beside it (Session/SetFile.h).
struct Set {
    std::vector<SetSong>        songs;   // at least one, once saved
    SwitchSettings              switching;
    std::vector<SetlistTrigger> triggers;

    bool operator==(const Set&) const = default;
};

} // namespace stagehand
