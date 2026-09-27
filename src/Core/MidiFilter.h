#pragma once

#include <cstdint>

#include "Core/MidiMessage.h"

namespace stagehand {

// Which incoming MIDI a plugin slot receives, and how it's transformed on the
// way in. One per PluginSlot: host-owned per-slot state, persisted in the Rack
// (by source *id*, resolved to a live handle at load time).
//
// Overlapping filters on several slots = layers; disjoint note ranges = splits
// (MainStage-style keyboard zones). Note and velocity ranges apply only to note
// messages; other channel messages pass whenever source, group, and channel
// match. Note-offs are NOT filtered here: the router sends each note-off to
// wherever its note-on went (docs/domain-model.md, "Routing").
struct MidiFilter {
    bool             allSources = true;                // accept every open source...
    MidiSourceHandle source     = kInvalidMidiSource;  // ...or only this one

    std::uint16_t groupMask   = 0xFFFF;   // bit g set = accept UMP group g
    std::uint16_t channelMask = 0xFFFF;   // bit c set = accept channel c

    std::uint8_t noteLow  = 0;            // inclusive note range (splits)
    std::uint8_t noteHigh = 127;

    std::uint16_t velocityLow  = 0;       // inclusive, 16-bit (MIDI 1.0 upscaled)
    std::uint16_t velocityHigh = 0xFFFF;

    std::int8_t transpose = 0;            // semitones; pitch-attribute-aware
};

} // namespace stagehand
