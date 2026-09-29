#pragma once

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "Core/MidiFilter.h"
#include "Plugin/PluginDescriptor.h"

namespace stagehand {

// Where a slot's MIDI comes from, as saved in a Rack. Resolved to a live
// MidiSourceHandle when the Rack loads (docs/rack-format.md, "Sources").
struct MidiSourceRef {
    enum class Kind {
        All,       // every open input
        Device,    // one device, by its MidiSourceInfo id
        Virtual,   // one of StageHand's virtual inputs, by name
        // Reserved: Sequence (a MIDI source the rack owns; not designed yet)
    };
    Kind        kind = Kind::All;
    std::string name;   // Device: "KeyLab 88:KeyLab 88 MIDI 1"; Virtual: "MIDI 2.0 In"
};

// One plugin slot as saved: the plugin's identity, its opaque state, and the
// host-owned settings (docs/domain-model.md, Save / restore). Enabled or not,
// found or not, it round-trips unchanged through load and save.
struct RackSlot {
    std::string            id;                 // unique in the rack; names its state file
    std::string            name;               // display name
    PluginDescriptor       plugin;             // format + id identify it; path is a hint
    std::vector<std::byte> state;              // opaque plugin state; empty = plugin defaults
    bool                   enabled     = true; // disabled: loaded, not run
    bool                   muted       = false;// runs, but silent (gain ramps to 0)
    float                  gainDb      = 0.0f;
    std::uint32_t          firstOutput = 0;    // 0-based here, 1-based in the file
    std::uint16_t          notePort    = 0;
    MidiSourceRef          source;
    // Groups, channels, note and velocity ranges, transpose. Its source fields
    // (allSources, source) are ignored here; Session sets them from `source`.
    MidiFilter             filter;
};

// A complete setup (docs/rack-format.md). Reserved for later: the rack's own
// sequences, kept separate from `slots` because a sequence is a MIDI source,
// not something that makes sound.
struct Rack {
    std::string           name;
    double                tempoBpm           = 120.0;   // the host tempo (docs/sequences.md, section 1)
    int                   timeSigNumerator   = 4;
    int                   timeSigDenominator = 4;
    std::vector<RackSlot> slots;
};

// A slot id from a display name: lowercase letters and digits, other runs of
// characters as one '-' (the file format's id rule). "slot" if nothing is left.
inline std::string slotIdFrom(std::string_view name) {
    std::string id;
    for (const char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c)))
            id += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        else if (!id.empty() && id.back() != '-')
            id += '-';
    }
    while (!id.empty() && id.back() == '-') id.pop_back();
    return id.empty() ? "slot" : id;
}

} // namespace stagehand
