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

// The looper (docs/looper.md): what's played into it is recorded, in time,
// and played back into its target rows like a sequence, with overdubs. A
// CompiledLooper is its settings in a SequenceSet snapshot; its loop lives in
// a LooperRuntime that outlives snapshots. Notes only, so far.

enum class LooperState : int {
    Empty,
    Armed,         // recording starts at the next beat or bar
    Recording,
    Playing,
    Overdubbing,
    Stopped,       // a loop, not playing
};

// One footswitch drives a looper (docs/looper.md, "Controls"); the rest are
// buttons. Press and Release are the switch: a tap while playing stops, a
// hold overdubs.
enum class LooperCommand : std::uint8_t {
    Press,
    Release,
    Stop,    // at the next beat or bar; recording ends there, then stops
    Clear,   // empty, now
    Undo,    // the last overdub pass (the one in progress, if overdubbing)
    Snapshot,   // copy the loop for the main thread (LooperRuntime::takeSnapshot()), to keep it as a sequence
};

struct LooperRuntime {
    static constexpr std::size_t kMaxNotes  = 4096;
    static constexpr std::size_t kMaxPasses = 256;    // more overdubs merge into the last
    static constexpr std::size_t kMaxOpen   = 64;     // keys held while recording
    static constexpr std::size_t kMaxSounding = 64;
    static constexpr double      kEarlyBeats  = 0.25;   // a sixteenth: early notes count (docs/looper.md, 7a)
    static constexpr double      kHoldSeconds = 0.4;    // longer: a hold (overdub), not a tap

    // Main thread: everything preallocated here, so the audio thread never allocates.
    explicit LooperRuntime(std::size_t capacity = kMaxNotes);

    // --- published (audio writes after each block, main reads)
    std::atomic<LooperState>   state{LooperState::Empty};
    std::atomic<double>        length{0.0};     // in beats; 0 while unknown
    std::atomic<double>        position{0.0};   // beats into the loop, while it plays
    std::atomic<std::uint32_t> noteCount{0};
    std::atomic<std::uint32_t> dropped{0};      // notes past the cap, since it was last cleared
    std::atomic<std::uint32_t> passes{0};       // the recording, then each overdub kept
    std::atomic<bool>          waiting{false};  // a start, play or stop waits for its beat or bar

    // --- audio thread only
    struct Note {
        double        on  = 0.0;    // beats into the loop
        double        off = -1.0;   // beats into the loop; < 0 while the key is held
        std::int64_t  firstCycle = 0;   // plays from this cycle on (not in the pass that recorded it)
        MidiMessage   onMessage, offMessage;
    };
    struct Open {   // a key held while recording
        bool          used = false;
        std::uint8_t  group = 0, channel = 0, note = 0;
        std::uint32_t index   = 0;     // into notes
        double        onBeat  = 0.0;   // as played (unquantized)
        std::int64_t  cycle   = 0;     // overdub: the cycle it started in
    };
    struct Sounding {
        bool          used    = false;
        std::uint32_t index   = 0;     // into notes
        double        offBeat = 0.0;
        MidiMessage   off;
    };
    std::size_t                capacity;
    LooperState                current     = LooperState::Empty;
    double                     startBeat   = 0.0;   // recording's start (loop beat 0)
    bool                       startKnown  = false; // armed while stopped: when the transport starts
    double                     recordEnd   = std::numeric_limits<double>::infinity();
    double                     anchorBeat  = 0.0;   // loop beat 0 of cycle 0
    double                     loopLength  = 0.0;
    double                     stopBeat    = std::numeric_limits<double>::infinity();
    bool                       stopAfterRecording = false;
    bool                       provisional = false; // an overdub begun by a press, kept only if held
    double                     pressBeat   = 0.0;
    std::uint32_t              overdubStart = 0;    // where the overdub in progress begins in notes
    std::vector<Note>          notes;               // reserved to capacity
    std::vector<std::uint32_t> passStarts;          // where each pass begins in notes
    Open                       open[kMaxOpen];
    Sounding                   sounding[kMaxSounding];
    std::uint32_t              droppedCount = 0;

    // --- a copy of the loop for the main thread (LooperCommand::Snapshot):
    // the audio thread fills it (preallocated) and sets snapshotReady.
    struct Snapshot {
        double            length = 0.0;   // in beats; 0: nothing recorded yet (or still recording)
        std::vector<Note> notes;          // every key released (none still held)
    };
    // Main thread: the copy, once the audio thread has made it; false until then.
    bool takeSnapshot(Snapshot& out);
    Snapshot          snapshot;           // reserved to capacity
    std::atomic<bool> snapshotReady{false};
};

// A loop as a sequence (docs/looper.md, "Convert to sequence"): on the grid of
// its quantize (64ths when off), in the protocol, channel and group of its
// first note, MIDI 2.0 as 12-EDO from middle C. It repeats, launching at the
// bar or beat as the looper starts; plays into nothing yet (the caller sets
// the targets, id and name).
Sequence loopToSequence(const LooperRuntime::Snapshot& loop, double quantize, bool atBar);

struct CompiledLooper {
    MidiSourceHandle               source = kInvalidMidiSource;   // its own, in the sequences' range
    std::shared_ptr<LooperRuntime> runtime;
    MidiSourceHandle               input       = kInvalidMidiSource;   // what it records; invalid: any input
    std::uint16_t                  channelMask = 0xFFFF;
    double                         quantize    = 0.25;    // in beats; 0: off (the played timing)
    bool                           atBar       = true;    // start, end and stop at a bar (else a beat)
    std::uint32_t                  fixedLength = 0;       // 0: ended by a press
    bool                           fixedInBars = true;
};

} // namespace stagehand
