#pragma once

#include <string>
#include <vector>

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
    When      when      = When::Now;
    OldRack   oldRack   = OldRack::RingOut;

    bool operator==(const SwitchSettings&) const = default;
};

struct Setlist {
    std::vector<SetlistSong> songs;
    SwitchSettings           switching;
    std::string              rig;   // its rig bundle (docs/rig.md), absolute; "" = the default rig

    bool operator==(const Setlist&) const = default;
};

} // namespace stagehand
