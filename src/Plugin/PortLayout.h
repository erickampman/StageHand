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

// One note/event port and the MIDI dialects it accepts. `supportsMidi2` is the
// CLAP MIDI2 dialect; a VST3 event bus carries decoded events and honors neither
// flag as such (docs/architecture.md 3a).
struct NotePortInfo {
    std::string name;
    bool        supportsMidi1 = true;
    bool        supportsMidi2 = false;
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
