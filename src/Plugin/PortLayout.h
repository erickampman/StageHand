#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace stagehand {

// One audio bus/port and its channel count. (A CLAP "audio port" ~ a VST3
// "bus".) N-channel by construction — see docs/architecture.md 3b.
struct AudioPortInfo {
    std::string   name;
    std::uint32_t channelCount = 0;
    bool          isMain       = false;
};

// Note dialects a note port can accept, as bit flags. Values match CLAP's
// CLAP_NOTE_DIALECT_* so the CLAP seam can copy them straight through.
using NoteDialectMask = std::uint32_t;
namespace NoteDialect {
inline constexpr NoteDialectMask Clap    = 1u << 0;   // native CLAP note events
inline constexpr NoteDialectMask Midi    = 1u << 1;   // MIDI 1.0
inline constexpr NoteDialectMask MidiMpe = 1u << 2;   // MIDI 1.0 with MPE semantics
inline constexpr NoteDialectMask Midi2   = 1u << 3;   // MIDI 2.0 (UMP)
} // namespace NoteDialect

// One note/event port, what it accepts, and what it prefers. The engine
// delivers each message in its own protocol when the port accepts it and
// translates only when it doesn't, using `preferredDialect` to pick the target
// (docs/domain-model.md, "Delivery policy"). A VST3 event bus carries decoded
// events; which dialect its impl advertises is its choice (docs/architecture.md
// 3a).
struct NotePortInfo {
    std::string     name;
    NoteDialectMask supportedDialects = NoteDialect::Midi;
    NoteDialectMask preferredDialect  = NoteDialect::Midi;   // exactly one bit
};

// A plugin's full I/O shape, reported after instantiation. Drives how the engine
// wires buffers and which MIDI dialect it sends.
struct PortLayout {
    std::vector<AudioPortInfo> audioInputs;
    std::vector<AudioPortInfo> audioOutputs;
    std::vector<NotePortInfo>  noteInputs;
    std::vector<NotePortInfo>  noteOutputs;
};

} // namespace stagehand
