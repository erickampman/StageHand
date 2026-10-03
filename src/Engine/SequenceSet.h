#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

#include "Core/MidiMessage.h"
#include "Core/Sequence.h"
#include "Engine/ControlLink.h"
#include "Engine/Looper.h"

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
    Toggle,    // Start if stopped or stopping, else Stop
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
    bool                             atStart      = false;

    struct Note {
        std::uint32_t start = 0, length = 1;
        std::int32_t  pitch = 0;          // as edited (note number or EDO step)
        std::uint16_t velocity = 0;       // in the protocol's resolution
        double        semitones = 0.0;    // the pitch it sounds, as a MIDI note number
        std::uint16_t pitch79   = 0;      // MIDI 2.0: the pitch 7.9 attribute
    };
    std::vector<Note> notes;              // sorted by start, then pitch

    // The sequence's MIDI triggers with their inputs resolved. compile() leaves
    // this empty: the Session fills it, leaving out triggers whose input isn't
    // open (they hear nothing, like an unresolved slot).
    struct Trigger {
        MidiSourceHandle         source  = kInvalidMidiSource;   // invalid: any input
        SequenceTrigger::Action  action  = SequenceTrigger::Action::Toggle;
        SequenceTrigger::Control control = SequenceTrigger::Control::Note;
        std::uint8_t             group = 0, channel = 0, number = 0;
        bool                     high  = false;   // audio thread: a controller's last value was >= 64
    };
    std::vector<Trigger> triggers;

    static CompiledSequence compile(const Sequence& sequence, MidiSourceHandle source,
                                    std::shared_ptr<SequenceRuntime> runtime);
};

// One note of `sequence` as playback would send it (MIDI 2.0: on the note
// number nearest its pitch), for auditioning it while editing.
struct AuditionMessages {
    MidiMessage on, off;
};
AuditionMessages auditionMessages(const Sequence& sequence, const SequenceNote& note) noexcept;

// A setlist trigger with its input resolved, as for a sequence's triggers.
struct CompiledSetlistTrigger {
    MidiSourceHandle         source  = kInvalidMidiSource;   // invalid: any input
    SetlistTrigger::Action   action  = SetlistTrigger::Action::Next;
    SequenceTrigger::Control control = SequenceTrigger::Control::Note;
    std::uint8_t             group = 0, channel = 0, number = 0;
    bool                     high  = false;   // audio thread: a controller's last value was >= 64
};

// The transport's own MIDI control (docs/sequences.md, section 7), published
// with the sequences. Its triggers' actions start and stop the transport.
struct TransportControl {
    std::vector<CompiledSequence::Trigger> triggers;
    bool             startOnNote       = false;
    MidiSourceHandle startOnNoteSource = kInvalidMidiSource;   // invalid: any input

    // The setlist's triggers (docs/setlists.md, section 4), published with
    // every rack's sequences and matched before all of them.
    std::vector<CompiledSetlistTrigger> setlist;

    // The looper switch (docs/looper.md, "Controls"): its press and release go
    // to the current looper. Matched after the setlist's triggers, before the
    // rack's. `action` unused.
    bool                      hasLooperSwitch = false;
    CompiledSequence::Trigger looperSwitch;

    // Panic's control (docs/panic.md): a press is a Panic. Matched with the
    // looper switch. `action` unused.
    bool                      hasPanic = false;
    CompiledSequence::Trigger panic;

    // Control links (docs/controls.md): matched after every trigger.
    std::vector<CompiledControlLink> links;
};

class SequenceSet {
public:
    explicit SequenceSet(std::vector<CompiledSequence> sequences, TransportControl transportControl = {},
                         std::vector<CompiledLooper> loopersIn = {})
        : all(std::move(sequences)), transport(std::move(transportControl)), loopers(std::move(loopersIn)) {
        for (CompiledControlLink& link : transport.links)
            for (CompiledControlLink::Target& t : link.targets) t.current = t.initial;
    }


    const std::vector<CompiledSequence>& sequences() const noexcept { return all; }

    // --- audio thread
    struct Block {
        double        beat          = 0.0;   // at the block's first frame
        double        beatsPerFrame = 0.0;
        double        tempoBpm      = 120.0;
        std::uint32_t frames        = 0;
        double        barStartBeats = 0.0;   // of the bar containing `beat`
        double        barBeats      = 4.0;

        // The rest of the block from `frame` on: where something that happens
        // at that frame (a trigger) takes effect.
        Block from(std::uint32_t frame) const noexcept;
    };

    // --- the loopers (docs/looper.md; Looper.cpp). Audio thread.
    const std::vector<CompiledLooper>& looperList() const noexcept { return loopers; }
    // A command to one looper (kInvalidMidiSource: all). True if a press armed
    // a looper while the transport is stopped (`running` false): the caller
    // starts the transport, unless it has a start of its own
    // (hasTransportStart()), and the recording begins on its first beat.
    bool looperCommand(MidiSourceHandle looper, LooperCommand command, const Block& block, bool running) noexcept;
    bool hasTransportStart() const noexcept;   // transport triggers, or start on first note
    // A live input's message at `at` (the block from its frame): recorded by
    // the loopers taking that input. Before generate().
    void recordLoops(MidiSourceHandle source, const MidiMessage& message, const Block& at) noexcept;
    void stopLoopers(const Block& block) noexcept;   // the transport stopped: at once, back to the start
    // The looper switch: true if `message` is its (it goes nowhere else);
    // `pressed` / `released` as it went down or up (a note, or a controller
    // crossing 64).
    bool matchLooperSwitch(MidiSourceHandle source, const MidiMessage& message, bool& pressed, bool& released) noexcept;
    // Panic's control: true if `message` is its (it goes nowhere else);
    // `pressed` on a note-on or a controller's rise to 64.
    bool matchPanic(MidiSourceHandle source, const MidiMessage& message, bool& pressed) noexcept;
    // Panic: the sequences and loopers forget the notes they're sounding (the
    // engine has ended them), so their note-offs later don't end a note played
    // since. They play on.
    void forgetSounding() noexcept;

    // The control links (docs/controls.md), with their pick-up state. Audio thread.
    std::vector<CompiledControlLink>& controlLinks() noexcept { return transport.links; }
    // An edit (not a song switch): each link target that was in `previous`
    // keeps where it was and whether it had picked up.
    void keepControlLinks(const SequenceSet& previous) noexcept;

    // Replace `previous` (null for the first set). Sounding notes whose note was
    // deleted or shortened past now end at once; a rate or length change keeps
    // the loop's phase. Sequences that disappeared release their notes: those
    // note-offs are written to `out` (offset 0).
    std::size_t adoptFrom(const SequenceSet* previous, const Block& block, SequenceEvent* out,
                          std::size_t capacity) noexcept;

    // Apply a command (source kInvalidMidiSource = every sequence).
    void command(MidiSourceHandle source, SequenceCommand command, const Block& block) noexcept;

    // A MIDI message from an input, against every trigger (the sequences' and
    // the transport's). Each trigger it fires (a note-on, or a controller's
    // rise to 64) is written to `hits`; the caller acts on them. `matched`:
    // the message belongs to a trigger and goes nowhere else (note-offs and
    // releases included). Call before generate().
    struct TriggerHit {
        MidiSourceHandle        sequence = kInvalidMidiSource;   // invalid: the transport
        SequenceTrigger::Action action   = SequenceTrigger::Action::Toggle;
    };
    std::size_t matchTriggers(MidiSourceHandle source, const MidiMessage& message, TriggerHit* hits,
                              std::size_t capacity, bool& matched) noexcept;
    // The same for the setlist's triggers, which come first: true if `message`
    // is theirs (it goes nowhere else). `fired`: a Next or Previous press, or
    // a Program Change (`program` set), to pass to the main thread.
    struct SetlistHit {
        SetlistTrigger::Action action  = SetlistTrigger::Action::Next;
        std::uint8_t           program = 0;
    };
    bool matchSetlist(MidiSourceHandle source, const MidiMessage& message, SetlistHit& hit, bool& fired) noexcept;
    // Would `command` start this sequence (Start, or Toggle while it's stopped)?
    bool wouldStart(MidiSourceHandle sequence, SequenceCommand command) const noexcept;
    // A note-on that start-on-note accepts (from its input, or any).
    bool startsOnNote(MidiSourceHandle source, const MidiMessage& message) const noexcept;
    // The transport started at `block.beat`: At Start sequences start there, exactly.
    void transportStarted(const Block& block) noexcept;

    // This block's events for every sequence, appended to `out`; returns how
    // many were written. The caller sorts the whole block's events by
    // (offset, order).
    std::size_t generate(const Block& block, SequenceEvent* out, std::size_t capacity) noexcept;

private:
    void apply(CompiledSequence& s, SequenceCommand command, const Block& block) noexcept;
    std::size_t generateLoops(const Block& block, SequenceEvent* out, std::size_t capacity) noexcept;
    void        loopersTransportStarted(const Block& block) noexcept;
    // A looper gone from the snapshot (a song switch): stopped, its notes ended now.
    static std::size_t releaseLooper(const CompiledLooper& looper, const Block& block, SequenceEvent* out,
                                     std::size_t capacity) noexcept;

    std::vector<CompiledSequence> all;
    TransportControl              transport;
    std::vector<CompiledLooper>   loopers;
};

} // namespace stagehand
