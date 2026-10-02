#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Core/MidiMessage.h"
#include "Core/MidiSourceRef.h"

namespace stagehand {

// A step sequence as edited and saved (docs/sequences.md, section 2). Plain
// data: the Session holds these, the rack file saves them, and the engine
// compiles each into an immutable playback snapshot (Engine/SequenceSet.h).

struct SequenceNote {
    std::uint32_t start  = 0;   // unit where it begins
    std::uint32_t length = 1;   // in units, >= 1; start + length <= the sequence's length
    // MIDI 1.0: a note number, 0-127. MIDI 2.0: an EDO step relative to the
    // sequence's reference note (negative below it).
    std::int32_t  pitch    = 0;
    // MIDI 1.0: 1-127. MIDI 2.0: 1-65535. (0 would be a note-off.)
    std::uint16_t velocity = 100;

    bool operator==(const SequenceNote&) const = default;
};

// Units per beat as a ratio: 4/1 = sixteenth notes, 1/2 = a unit every two beats.
// A beat is the tempo's beat (a quarter note).
struct SequenceRate {
    std::uint16_t numerator   = 4;
    std::uint16_t denominator = 1;
    double unitsPerBeat() const noexcept { return static_cast<double>(numerator) / denominator; }
    bool operator==(const SequenceRate&) const = default;
};

enum class LaunchQuantize { Bar, Beat, Immediate };

// A MIDI control that starts or stops a sequence (docs/sequences.md, section
// 4): a note (its note-on acts) or a controller (acts when its value rises
// to 64 or more, so a pedal or pad acts once per press). Messages that match
// are the trigger's alone: no row hears them, note-offs included.
struct SequenceTrigger {
    enum class Action : std::uint8_t { Toggle, Start, Stop };
    enum class Control : std::uint8_t { Note, Controller };

    Action        action  = Action::Toggle;
    MidiSourceRef source;                    // Device or Virtual; All = any input
    Control       control = Control::Note;
    std::uint8_t  group   = 0;               // 0-based
    std::uint8_t  channel = 0;               // 0-based
    std::uint8_t  number  = 0;               // note or controller number

    // The same control, whatever it does.
    bool sameControl(const SequenceTrigger& o) const noexcept {
        return source == o.source && control == o.control && group == o.group && channel == o.channel &&
               number == o.number;
    }
    bool operator==(const SequenceTrigger&) const = default;
};

struct Sequence {
    std::string  id;     // unique in the rack; rows play it by being in `targets`
    std::string  name;
    MidiProtocol protocol      = MidiProtocol::Midi2;   // changeable only while `notes` is empty
    int          edo           = 12;                    // MIDI 2.0: steps per octave
    int          referenceNote = 60;                    // MIDI 2.0: the pitch of step 0 (MIDI note)
    std::uint8_t channel       = 0;                     // 0-based, sent on
    std::uint8_t group         = 0;                     // 0-based
    SequenceRate rate;
    std::uint32_t   length          = 16;               // loop length in units
    LaunchQuantize  launch          = LaunchQuantize::Bar;
    bool            repeat          = true;             // off: play once, then stop
    double          noteOffOffsetMs = 0.0;              // moves every note-off (+ legato, - staccato)
    std::vector<SequenceNote> notes;
    std::vector<SequenceTrigger> triggers;              // MIDI controls that start or stop it
    bool            atStart         = false;            // starts on beat 1 whenever the transport starts
    // The rows (slot ids) it plays into. A row hears these on top of its own live
    // input, so a sequence never takes a row away from the keyboard.
    std::vector<std::string> targets;
};

// A sequence's MidiSourceHandle lives above the range MIDI inputs use, so the
// engine can tell a sequence from a device: a row listening to "all inputs"
// hears every device but no sequence (a sequence plays only into its targets).
inline constexpr MidiSourceHandle kSequenceSourceBase = 0x80000000u;
constexpr bool isSequenceSource(MidiSourceHandle handle) noexcept {
    return handle >= kSequenceSourceBase;
}
// Notes auditioned from the editor (Engine::auditionNote) come from the
// sequence's handle with this bit set: they reach the same rows, but are held
// apart from the notes the sequence is playing.
inline constexpr MidiSourceHandle kAuditionSourceBit = 0x40000000u;
constexpr MidiSourceHandle auditionSourceOf(MidiSourceHandle sequence) noexcept {
    return sequence | kAuditionSourceBit;
}
constexpr MidiSourceHandle sequenceOfSource(MidiSourceHandle source) noexcept {   // an audition's sequence
    return isSequenceSource(source) ? source & ~kAuditionSourceBit : source;
}

} // namespace stagehand
