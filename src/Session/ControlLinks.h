#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "Core/MidiSourceRef.h"
#include "Engine/ControlLink.h"

namespace stagehand {

// Control links as saved (docs/controls.md): a controller and what it drives.
// A song's are in its rack ("controls" in rack.json), the ones for all songs
// in the rig ("controls" in rig.json).

enum class ControlScope : std::uint8_t {
    Song,       // this rack's
    AllSongs,   // the rig's
};

struct ControlLinkTarget {
    using Kind = CompiledControlLink::Target::Kind;
    Kind kind = Kind::Parameter;
    // Parameter: a row's plugin (`row`, a slot id: a song's link), every row
    // with plugin `plugin` (by id: a link for all songs), or a bus effect
    // (`bus` and `effect`, either scope). Gain: the row. Send: the row and
    // `bus`. Return: `bus`. Master: nothing.
    std::string   row;
    std::string   plugin;
    int           bus    = -1;   // 0 = A, 1 = B
    int           effect = -1;   // 0 or 1
    std::uint32_t param  = 0;    // Parameter: its id
    std::string   paramName;     // as last seen: shown while its plugin isn't there
    // At the control's bottom and top: the parameter's plain value, or dB.
    double        bottom = 0.0;
    double        top    = 1.0;
    ControlCurve  curve  = ControlCurve::Linear;

    // The same thing driven (whatever the range)?
    bool sameTarget(const ControlLinkTarget& o) const noexcept {
        return kind == o.kind && row == o.row && plugin == o.plugin && bus == o.bus && effect == o.effect &&
               param == o.param;
    }
    bool operator==(const ControlLinkTarget&) const = default;
};

struct ControlLinkSetup {
    MidiSourceRef source;   // an input; All = any
    std::uint8_t  group = 0, channel = 0, number = 0;   // the controller, 0-based
    bool          pickUp = true;    // else jump
    bool          passOn = false;   // else the controller goes nowhere else
    std::vector<ControlLinkTarget> targets;

    bool sameControl(const ControlLinkSetup& o) const noexcept {
        return source == o.source && group == o.group && channel == o.channel && number == o.number;
    }
    bool operator==(const ControlLinkSetup&) const = default;
};

// Which saved link and target a compiled one came from
// (CompiledControlLink::Target::origin), to show the engine's state for it.
inline std::uint64_t linkOrigin(ControlScope scope, std::size_t link, std::size_t target) noexcept {
    return (std::uint64_t{scope == ControlScope::AllSongs} << 48) | (std::uint64_t{link & 0xFFFFFF} << 24) |
           (target & 0xFFFFFF);
}

} // namespace stagehand
