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
        Sequence,  // a rack file's old "sequence:<id>" slot source; converted on load
                   // into the sequence's targets (docs/sequences.md, "Where a sequence plays")
        None,      // no live input: a row that only plays the sequences aimed at it
    };
    Kind        kind = Kind::All;
    std::string name;   // Device: "KeyLab 88:KeyLab 88 MIDI 1"; Virtual: "MIDI 2.0 In"; Sequence: its id

    bool operator==(const MidiSourceRef&) const = default;
};

} // namespace stagehand
