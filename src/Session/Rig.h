#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "Core/Sequence.h"
#include "Plugin/PluginDescriptor.h"
#include "Session/Rack.h"

namespace stagehand {

// The rig (docs/rig.md): what every song shares, loaded once and kept across
// song switches. Two effect buses, which the rows' sends feed, and the master
// level.

// An effect in one of a bus's slots, as saved: like a rack slot's plugin.
struct RigEffect {
    PluginDescriptor       plugin;   // format + id identify it; path is a hint
    std::vector<std::byte> state;    // opaque; empty = the plugin's defaults

    bool operator==(const RigEffect& o) const {
        return plugin.format == o.plugin.format && plugin.id == o.plugin.id && plugin.path == o.plugin.path &&
               state == o.state;
    }
};

struct RigBus {
    static constexpr std::size_t kEffects = 2;
    std::array<std::optional<RigEffect>, kEffects> effects;   // in series; an empty slot passes audio on
    float         returnDb    = 0.0f;
    bool          muted       = false;
    std::uint32_t firstOutput = 0;   // 0-based here, 1-based in the file

    bool operator==(const RigBus&) const = default;
};

struct Rig {
    std::string                      name;
    std::array<RigBus, kRigBuses>    buses;
    float                            masterDb = 0.0f;
    // The looper footswitch (docs/looper.md, "Controls"): the performer's, so
    // the same in every song. It drives the current looper. Its action is unused.
    std::optional<SequenceTrigger>   looperSwitch;
    // Where the click plays (docs/sequences.md, section 7): a playback device,
    // by name; "" = none chosen. The stage's, so the same in every song; each
    // rack says whether it clicks, and how loud (MetronomeSettings).
    std::string                      clickOutput;

    bool operator==(const Rig&) const = default;
};

} // namespace stagehand
