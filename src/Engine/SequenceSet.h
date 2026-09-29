#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

#include "Core/MidiMessage.h"
#include "Core/Sequence.h"

namespace stagehand {

// Sequence playback on the audio thread (docs/sequences.md, section 3). A
// SequenceSet is an immutable snapshot of every sequence's notes and settings,
// built on the main thread and published with Engine::commitSequences(), like
// an EngineGraph. Each sequence's playback state lives in a SequenceRuntime
// that outlives snapshots, so an edit doesn't interrupt playback.

enum class SequenceState : int {
    Stopped,
    Starting,   // launched; waiting for its quantized start
    Playing,
    Stopping,   // stopped; waiting for its quantized stop
};

enum class SequenceCommand : std::uint8_t {
    Start,     // at the next launch boundary (bar, beat, or now)
    Stop,      // at the next launch boundary
    StopNow,   // this block, with note-offs at its start
};

// A generated message, positioned in the block.
struct SequenceEvent {
    std::uint32_t    offset = 0;
    std::uint8_t     order  = 0;   // at one offset: note-offs (0) before note-ons (1)
    MidiSourceHandle source = kInvalidMidiSource;
    MidiMessage      message;
};

// One sequence's state across snapshots. The Session creates it, the audio
// thread runs it, the main thread reads the published part.
struct SequenceRuntime {
    // --- published (audio writes after each block, main reads)
    std::atomic<SequenceState> state{SequenceState::Stopped};
    std::atomic<double>        position{0.0};   // units into the loop

    // --- audio thread only
    static constexpr std::size_t kMaxSounding = 64;
    struct Sounding {
        bool          used    = false;
        std::uint32_t start   = 0;     // the note's start unit, for matching after an edit
        std::int32_t  pitch   = 0;
        std::uint8_t  note    = 0;     // the note number it was sent on
        double        onBeat  = 0.0;
        double        offBeat = 0.0;
    };
    SequenceState current     = SequenceState::Stopped;
    double        anchorBeat  = 0.0;   // the beat of unit 0 of the first loop
    double        stopBeat    = std::numeric_limits<double>::infinity();
    double        unitsPerBeat = 0.0;  // of the snapshot last played, to keep phase through edits
    std::uint32_t length       = 0;
    Sounding      sounding[kMaxSounding];
};

// A Sequence compiled for playback: pitches resolved to what's sent.
struct CompiledSequence {
    MidiSourceHandle                 source = kInvalidMidiSource;
    std::shared_ptr<SequenceRuntime> runtime;
    MidiProtocol                     protocol     = MidiProtocol::Midi2;
    std::uint8_t                     channel      = 0;
    std::uint8_t                     group        = 0;
    double                           unitsPerBeat = 4.0;
    std::uint32_t                    length       = 16;
    LaunchQuantize                   launch       = LaunchQuantize::Bar;
    bool                             repeat       = true;
    double                           noteOffOffsetMs = 0.0;

    struct Note {
        std::uint32_t start = 0, length = 1;
        std::int32_t  pitch = 0;          // as edited (note number or EDO step)
        std::uint16_t velocity = 0;       // in the protocol's resolution
        double        semitones = 0.0;    // the pitch it sounds, as a MIDI note number
        std::uint16_t pitch79   = 0;      // MIDI 2.0: the pitch 7.9 attribute
    };
    std::vector<Note> notes;              // sorted by start, then pitch

    static CompiledSequence compile(const Sequence& sequence, MidiSourceHandle source,
                                    std::shared_ptr<SequenceRuntime> runtime);
};

class SequenceSet {
public:
    explicit SequenceSet(std::vector<CompiledSequence> sequences) : all(std::move(sequences)) {}

    const std::vector<CompiledSequence>& sequences() const noexcept { return all; }

    // --- audio thread
    struct Block {
        double        beat          = 0.0;   // at the block's first frame
        double        beatsPerFrame = 0.0;
        double        tempoBpm      = 120.0;
        std::uint32_t frames        = 0;
        double        barStartBeats = 0.0;   // of the bar containing `beat`
        double        barBeats      = 4.0;
    };

    // Replace `previous` (null for the first set). Sounding notes whose note was
    // deleted or shortened past now end at once; a rate or length change keeps
    // the loop's phase. Sequences that disappeared release their notes: those
    // note-offs are written to `out` (offset 0).
    std::size_t adoptFrom(const SequenceSet* previous, const Block& block, SequenceEvent* out,
                          std::size_t capacity) noexcept;

    // Apply a command (source kInvalidMidiSource = every sequence).
    void command(MidiSourceHandle source, SequenceCommand command, const Block& block) noexcept;

    // This block's events for every sequence, appended to `out`; returns how
    // many were written. The caller sorts the whole block's events by
    // (offset, order).
    std::size_t generate(const Block& block, SequenceEvent* out, std::size_t capacity) noexcept;

private:
    std::vector<CompiledSequence> all;
};

} // namespace stagehand
