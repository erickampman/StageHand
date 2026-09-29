#pragma once

#include <string>

namespace stagehand {

// Where MIDI comes from, as saved in a Rack: a slot's source, or the input a
// sequence's trigger listens to. Resolved to a live MidiSourceHandle when the
// Rack loads (docs/rack-format.md, "Sources").
struct MidiSourceRef {
    enum class Kind {
        All,       // every open input
        Device,    // one device, by its MidiSourceInfo id
        Virtual,   // one of StageHand's virtual inputs, by name
        Sequence,  // one of the rack's sequences, by id (docs/sequences.md)
    };
    Kind        kind = Kind::All;
    std::string name;   // Device: "KeyLab 88:KeyLab 88 MIDI 1"; Virtual: "MIDI 2.0 In"; Sequence: its id

    bool operator==(const MidiSourceRef&) const = default;
};

} // namespace stagehand
