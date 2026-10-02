#pragma once

#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Core/MidiFilter.h"
#include "Core/MidiSourceRef.h"
#include "Core/Sequence.h"
#include "Plugin/PluginDescriptor.h"

namespace stagehand {

// The rig's effect buses (docs/rig.md); equal to the engine's kBusCount.
inline constexpr std::size_t kRigBuses = 2;

// One plugin slot as saved: the plugin's identity, its opaque state, and the
// host-owned settings (docs/domain-model.md, Save / restore). Found or not, it
// round-trips unchanged through load and save.
struct RackSlot {
    std::string            id;                 // unique in the rack; names its state file
    std::string            name;               // display name
    PluginDescriptor       plugin;             // format + id identify it; path is a hint
    std::vector<std::byte> state;              // opaque plugin state; empty = plugin defaults
    // Silent (gain ramps to 0), its notes released, and once quiet not run at
    // all (SlotRuntime::muted). A file's "enabled": false loads as muted.
    bool                   muted       = false;
    float                  gainDb      = 0.0f;
    // How much goes to each of the rig's effect buses, after the fader: a level
    // in dB, or none (off). "sends": [dbA, dbB] in the file, null for off.
    std::array<std::optional<float>, kRigBuses> sendDb;
    std::uint32_t          firstOutput = 0;    // 0-based here, 1-based in the file
    std::uint16_t          notePort    = 0;
    MidiSourceRef          source;
    // Groups, channels, note and velocity ranges, transpose. Its source fields
    // (allSources, source) are ignored here; Session sets them from `source`.
    MidiFilter             filter;
};

// The transport's MIDI control (docs/sequences.md, section 7).
struct TransportSettings {
    std::vector<SequenceTrigger> triggers;            // their actions start and stop the transport
    bool                         startOnNote = false; // while stopped, a note-on starts it
    MidiSourceRef                startOnNoteSource;   // from this input; All = any input

    bool operator==(const TransportSettings&) const = default;
};

// The beat lights and the click (docs/sequences.md, section 7).
struct MetronomeSettings {
    bool        everyNote    = false;    // count every note (6/8: six), not the counted beats (6/8: two)
    bool        click        = false;
    float       clickLevelDb = -12.0f;
    std::string clickOutput;             // a playback device, by name; "" = none chosen

    bool operator==(const MetronomeSettings&) const = default;
};

// A looper (docs/looper.md), saved as a container: its settings. What it
// records never is.
struct LooperSetup {
    std::string              id;            // unique among the rack's loopers
    std::string              name;
    MidiSourceRef            source;        // the input it records; All = any input
    std::uint16_t            channelMask = 0xFFFF;
    std::vector<std::string> targets;       // the rows it plays into
    double                   quantize    = 0.25;   // in beats; 0 = off (the played timing)
    bool                     atBar       = true;   // start, end and stop at a bar, else a beat
    std::uint32_t            fixedLength = 0;      // 0 = ended by a press
    bool                     fixedInBars = true;

    bool operator==(const LooperSetup&) const = default;
};

// A complete setup (docs/rack-format.md). Its sequences are kept separate from
// `slots` because a sequence is a MIDI source, not something that makes sound.
struct Rack {
    std::string           name;
    double                tempoBpm           = 120.0;   // the host tempo (docs/sequences.md, section 1)
    int                   timeSigNumerator   = 4;
    int                   timeSigDenominator = 4;
    std::vector<RackSlot> slots;
    std::vector<Sequence> sequences;
    std::vector<LooperSetup> loopers;   // docs/looper.md
    TransportSettings     transport;
    MetronomeSettings     metronome;
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
