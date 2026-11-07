#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "uw/audio/AudioBackend.h"
#include "uw/base/SpscQueue.h"
#include "uw/ump/Ump.h"
#include "Engine/EngineGraph.h"
#include "Engine/SequenceSet.h"
#include "uw/midi/MidiInput.h"
#include "Core/UwNames.h"

namespace stagehand {

// Runs the published EngineGraph once per audio block. It is both the audio
// backend's AudioProcessor and the MIDI input's MidiSink, and it owns the thread
// crossings between them (docs/domain-model.md, threading):
//
//   MIDI thread  -> onMidi()  -> MIDI queue      -> audio thread
//   main thread  -> commit()  -> pending graph   -> audio thread (adopts it)
//   audio thread -> retired graphs -> collect()  -> main thread (destroys them)
//
// Per block (audio thread):
//   1. Adopt the pending graph, if any and the retire queue has room: call
//      stopProcessing() on plugins the new graph no longer contains, and retire
//      the old graph.
//   2. Adopt a new sequence snapshot and apply Play/Stop commands.
//   3. Take the MIDI captured before this block started, one period behind:
//      offset = floor((t - (blockStart - frames/rate)) * rate), clamped to
//      [0, frames). Messages stamped at or after blockStart stay queued. A
//      message that matches a sequence trigger acts on it (at its offset) and
//      goes no further; one captured by MIDI learn goes no further either.
//   4. Generate the sequences' events, and merge them with the input in offset
//      order. Each message goes to every slot whose filter accepts it, in the message's
//      own protocol if the slot's note port takes it, otherwise translated
//      (MIDI 2.0 downconverted for MIDI 1.0 ports; MIDI 1.0 upconverted for
//      MIDI-2.0-only ports, with per-source state). A note-off goes wherever
//      its note-on went, at the note number each plugin actually got, even if
//      filters or transposes changed in between (the held-note table). The
//      queue is in capture order, so each slot's events come out sorted.
//   5. Split the block into chunks of at most kMaxPluginBlockFrames. For each
//      chunk, run every slot with its share of the events, then sum the slot
//      outputs into the backend outputs, times the slot's gain, and into each
//      effect bus times gain and send level (docs/rig.md). Then each bus runs
//      its effects in series on what it was sent and adds its return, times
//      the return level. Last, the master level scales the main outputs. Every
//      level (gain, send, return, master) ramps linearly over
//      kGainRampSeconds, per sample, so a move or mute doesn't click. Slot,
//      return and output meters are fed post-level.
//
// Plugin lifecycle across threads (CLAP's thread rules):
//   main:  create, loadState, activate(rate, kMaxPluginBlockFrames), commit
//   audio: process() (starts processing implicitly on the first call)
//   audio: stopProcessing() once an adopted graph no longer contains it
//   main:  after the graph that held it is collected, deactivate / destroy
//          (dropping the last shared_ptr does both)
//
// Shutdown: commit an empty graph, wait for committedGraphAdopted(), then stop
// the backend. That way every plugin is stopped on the audio thread before it's
// deactivated. If the backend has already died, stopping on the audio thread is
// impossible; plugins are then deactivated without it (logged).
//
// One MidiInput per Engine: the MIDI queue has a single producer thread.
// Large (the queues are inline): allocate on the heap.
class Engine final : public AudioProcessor, public MidiSink {
public:
    Engine();
    ~Engine() override;   // main thread, after the backend stops calling process()

    Engine(const Engine&)            = delete;
    Engine& operator=(const Engine&) = delete;

    // --- main thread ---

    // Publish a new graph (null = run nothing). The audio thread adopts it at the
    // start of its next block. Every plugin in it must already be activated. If
    // an earlier graph was still pending, it's destroyed here, never having run.
    void commit(std::unique_ptr<EngineGraph> graph);

    // True once the audio thread has adopted the most recent commit(), i.e. the
    // plugins it removed have been stopped.
    bool committedGraphAdopted() const noexcept;
    // How many graphs commit() has published so far (tests: a rack switch is one).
    std::uint64_t graphCommits() const noexcept;

    // Destroy graphs the audio thread has retired; returns how many. Call
    // regularly from the main loop. Dropping a graph may release the last
    // reference to a removed plugin, which deactivates and destroys it here, on
    // the main thread.
    std::size_t collect();

    struct Stats {
        std::uint64_t midiReceived       = 0;   // messages accepted into the queue
        std::uint64_t midiQueueFull      = 0;   // dropped: MIDI queue full
        std::uint64_t slotEventsFull     = 0;   // dropped: a slot's per-block buffer full
        std::uint64_t unsupportedDialect = 0;   // dropped: no form the slot takes (CLAP-only port, SysEx)
        std::uint64_t lateMidi           = 0;   // older than the window; placed at offset 0
        std::uint64_t sequenceEventsFull = 0;   // dropped: too many sequence events in one block
    };
    Stats stats() const noexcept;   // any thread

    // --- sequences (docs/sequences.md, section 3) ---
    // Publish a new snapshot of every sequence (null = none). Adopted at the next
    // block like a graph: playback continues through edits, and the old snapshot
    // is destroyed in collect(). Sequences are MIDI sources: rows hear one only
    // if they select it (never through "all inputs").
    void commitSequences(std::unique_ptr<SequenceSet> sequences);

    // --- a song switch (docs/setlists.md, sections 3 and 5) ---
    struct SwitchPlan {
        bool   atNextBar     = false;   // at the block holding the next bar line; stopped, at once
        bool   stopTransport = false;   // else it keeps running (medleys)
        double tempoBpm      = 120.0;   // the new rack's, from the switch on
        int    numerator     = 4;
        int    denominator   = 4;
    };
    // Publish the next song's graph and sequences, to be adopted together at
    // the switch with its tempo and signature (a new signature counts from the
    // switch's bar line). If the transport runs on, the new rack's At Start
    // sequences start at the next bar, as if it had just started there. Until
    // the switch, later commit()s and commitSequences() wait too (they hold the
    // new rack); a further switch replaces this one's plan.
    void commitSwitch(std::unique_ptr<EngineGraph> graph, std::unique_ptr<SequenceSet> sequences,
                      const SwitchPlan& plan);
    // A switch waiting for its bar happens at the next block instead (unload, shutdown).
    void switchNow() noexcept { switchForced.store(true, std::memory_order_release); }
    // Start or stop one sequence (kInvalidMidiSource = all of them) at the next
    // block. False if the command queue is full (try again next tick).
    bool sendSequenceCommand(MidiSourceHandle source, SequenceCommand command) noexcept;
    // A looper's command (docs/looper.md), at the next block: its footswitch's
    // press and release, Stop, Clear, Undo. Arming one while the transport is
    // stopped starts it, unless the transport has a start of its own. Main
    // thread; false if the queue is full.
    bool sendLooperCommand(MidiSourceHandle looper, LooperCommand command) noexcept;
    // The looper the looper switch (TransportControl::looperSwitch) drives;
    // invalid: none. Any thread.
    void setCurrentLooper(MidiSourceHandle looper) noexcept { currentLooper.store(looper, std::memory_order_relaxed); }

    // Audition one note while editing (docs/sequences.md, section 5): `on` now,
    // `off` after `seconds`, from `sequence` into the rows it plays into
    // (through their filters), whether or not it is playing or the transport
    // runs. Held apart from the sequence's own notes (kAuditionSourceBit). A
    // new audition of a sounding note number restarts it. Main thread; false if
    // the queue is full.
    bool auditionNote(MidiSourceHandle sequence, const MidiMessage& on, const MidiMessage& off,
                      double seconds) noexcept;
    // End every audition of `sequence` still sounding, at the next block's start
    // (in order with auditionNote(): a note moved to a new pitch ends, then the
    // new one starts). With `allNotesOff` (CC 123, in the sequence's protocol,
    // group and channel), then clear those rows on that channel for good: a
    // note-off for every key (for plugins that ignore CC 123), then the CC
    // itself. Main thread; false if the queue is full.
    bool endAuditions(MidiSourceHandle sequence, const MidiMessage* allNotesOff = nullptr) noexcept;

    // --- Panic (docs/panic.md) ---
    // At the next block, in every row of the graph: a note-off for every note
    // sounding (the held-note table, sequences', loopers' and auditions'
    // included), then the pedals up (CC 64, 66, 67) and All Notes Off (CC 123)
    // on all 16 channels of group 1; the held pedals forgotten. The transport,
    // sequences and loopers run on. A Panic within kPanicCutSeconds of the
    // last also cuts the sound: All Sound Off (CC 120) first, and every row and
    // bus effect reset (HostedPlugin::reset()). Panic's learned control
    // (TransportControl::panic) does the same at its own frame. Any thread.
    void panic() noexcept { panicRequested.store(true, std::memory_order_relaxed); }

    // Set a plugin's parameter from the host's own controls (the generic
    // editor; docs/generic-editor.md): a ParamValue at the start of the next
    // block, to whichever plugin in the running graph `plugin` is (a row's, a
    // row's MIDI effect, a bus effect). Control links pick it up as they do
    // the plugin's own changes. Main thread; false if the queue is full.
    bool setParameter(const HostedPlugin* plugin, std::uint32_t param, double value) noexcept {
        return parameterRequests.push(ParameterRequest{plugin, param, value});
    }
    static constexpr double kPanicCutSeconds = 2.0;
    struct Panics {
        std::uint32_t count = 0;       // so far (the button and the control)
        bool          cut   = false;   // the last one cut the sound
    };
    Panics panics() const noexcept;   // any thread

    // --- DSP load (docs/panic.md, section 6) ---
    // The share of each period process() takes (1 = all of it: an xrun). The
    // average over about a second, and the worst block since the last take.
    // Each row's and bus's own are in their SlotRuntime's `load`.
    struct DspLoad {
        float average = 0.0f;
        float peak    = 0.0f;
    };
    DspLoad takeDspLoad() noexcept;   // main thread

    // --- MIDI learn (docs/sequences.md, section 4). Main thread.
    // Arm: the next note-on, or controller press (a value of 64 or more), from
    // any input is captured instead of reaching rows or triggers.
    // `programChanges`: capture the next Program Change instead (a setlist's
    // Song trigger, docs/setlists.md section 4).
    void armLearn(bool programChanges = false) noexcept;
    void cancelLearn() noexcept;
    struct Learned {
        MidiSourceHandle source = kInvalidMidiSource;
        MidiMessage      message;
    };
    // True once per capture, with what was captured; learning is then over.
    bool takeLearned(Learned& learned) noexcept;

    // Learn from a plugin's editor (docs/controls.md, milestone 4). Main
    // thread. While armed, each parameter a plugin reports touched (its
    // gesture begins, or it changes a value itself, from its editor or the
    // generic one) is passed on, oldest first. Disarming drops what's left.
    struct Touched {
        const HostedPlugin* plugin = nullptr;
        std::uint32_t       param  = 0;
    };
    void armTouch(bool on) noexcept;
    bool takeTouched(Touched& touched) noexcept;

    // The setlist's triggers (published with the sequences, TransportControl::
    // setlist) fired: the audio thread can't switch racks, so it passes each
    // on. Main thread; true once per request, oldest first.
    using SetlistRequest = SequenceSet::SetlistHit;
    bool takeSetlistRequest(SetlistRequest& request) noexcept;

    // Messages received from one MIDI input so far (0 if none yet): the sources
    // dock's activity lights compare it between ticks. Any thread.
    std::uint32_t sourceActivity(MidiSourceHandle source) const noexcept;

    // --- transport (docs/sequences.md, sections 1 and 7) ---
    // A beat clock that starts stopped at beat 0. Start puts beat 0 at the
    // frame it happens (a block's start from here, a MIDI trigger's own frame),
    // then it advances; Stop ends every sequence at once and returns to beat 0
    // after that block. Starting a sequence while stopped starts the transport,
    // and At Start sequences start with it. Tempo and time signature are read
    // once per block, so a change lands at the next block boundary. Main
    // thread (the values are atomics).
    enum class TransportCommand : std::uint8_t { Start, Stop, Toggle };
    // At the next block's start. False if the queue is full (try again next tick).
    bool sendTransportCommand(TransportCommand command) noexcept;
    void setTempo(double bpm) noexcept;                       // clamped to kMinTempo..kMaxTempo
    void setTimeSignature(int numerator, int denominator) noexcept;
    struct Transport {
        double       tempoBpm      = 120.0;
        int          numerator     = 4;
        int          denominator   = 4;
        double       beatPosition  = 0.0;   // as of the last block (for display)
        double       barStartBeats = 0.0;
        std::int32_t barNumber     = 0;
        bool         running       = false;
        int          countIn       = 0;     // counting in: the pulses still to count (4, 3, 2, 1); 0: not
        double       countInBeat   = 0.0;   // counting in: beats into it (for the lights)
        int          countInPulses = 0;     // counting in: its pulses in all
        // When beatPosition (and countInBeat) is the transport's position, on
        // nowNanoseconds()'s clock: the next block's start, so the display can
        // carry the beat on between ticks (docs/perform.md, decision 14).
        Nanoseconds  at            = 0;
    };
    Transport transport() const noexcept;   // any thread

    // --- the count-in (docs/perform.md, decisions 1 to 8) ---
    // Bars clicked before bar 1 when the transport is started: 0 (none), 0.5
    // (half the bar's pulses, rounded up), 1 or 2. A start (Start, Toggle, a
    // sequence or a looper started while stopped) counts in first, and bar 1
    // lands right after: plugins see the transport stopped until then, and At
    // Start sequences, loopers and the sequences started meanwhile start
    // there. Start on first note never counts in. Stop or a song switch
    // during it cancels it. It clicks whether the click is on or not, at the
    // click's level. Main thread (an atomic, read when a count-in begins).
    void setCountIn(double bars) noexcept;

    // --- the click (docs/sequences.md, section 7) ---
    // A short blip on each metronome pulse (metronomePulse(): counted beats, or
    // every note), higher on the downbeat, on the aux outputs only, while the
    // transport runs. `gain` is linear. Main thread (atomics, read per block).
    void setClick(bool on, bool everyNote, float gain) noexcept;
    // One downbeat blip at the next block, whether the click is on or the
    // transport runs: to check where it's heard.
    void testClick() noexcept { clickTest.store(true, std::memory_order_relaxed); }
    static constexpr double kClickSeconds = 0.03;
    static constexpr double kClickHz[2]   = {1000.0, 1600.0};   // a beat, the downbeat
    static constexpr double kMinTempo = 20.0;
    static constexpr double kMaxTempo = 400.0;

    // The master level (docs/rig.md): scales every main output (not the
    // click's aux outputs), ramped like a slot's gain. Linear. Any thread.
    void setMasterGain(float gain) noexcept { masterTarget.store(gain, std::memory_order_relaxed); }

    // The backend outputs' level (the master meter): outputs 1 and 2, with any
    // further outputs folded into the second channel. take() from the main thread.
    LevelMeter& outputMeter() noexcept { return outputLevels; }

    // --- control links (docs/controls.md, sections 8 and 10) ---
    // Each running link target as the last block left it, for the Control
    // Link dialog's live markers: its `origin` as compiled; where the target
    // is (a parameter's plain value, or a level's dB); the control's position
    // (0 to 1, -1 before it has moved); and whether it's waiting for pick-up
    // (the control has moved but hasn't reached the target). The first
    // kShownLinkTargets only. Main thread; just after a commit, a block may
    // still show the previous set's.
    struct LinkState {
        std::uint64_t origin  = 0;
        double        value   = 0.0;
        double        travel  = -1.0;
        bool          waiting = false;
    };
    std::vector<LinkState> linkStates() const;
    static constexpr std::size_t kShownLinkTargets = 64;

    // How long a change of slot gain takes to arrive (docs/rack-view.md, section 3).
    static constexpr double kGainRampSeconds = 0.010;
    static constexpr double kLoadAverageSeconds = 1.0;   // the DSP load's average: about this long
    // A muted slot is suspended (SlotRuntime::muted) once its output stays under
    // kQuietLevel (-100 dBFS) for kQuietSeconds, or after kMaxMutedSeconds
    // whatever it does (a drone, noise).
    static constexpr float  kQuietLevel      = 1e-5f;
    static constexpr double kQuietSeconds    = 0.2;
    static constexpr double kMaxMutedSeconds = 5.0;
    // A draining slot (SlotConfig::draining) finishes once quiet as above, or
    // after this long whatever it does.
    static constexpr double kMaxDrainSeconds = 8.0;

    // --- audio thread ---
    void process(const AudioCallbackContext& ctx) noexcept override;

    // --- MIDI thread ---
    void onMidi(MidiSourceHandle source, const TimedMidiMessage& msg) noexcept override;

private:
    struct QueuedMidi {
        MidiSourceHandle source = kInvalidMidiSource;
        TimedMidiMessage message;
    };

    // Most slots one held note is tracked for (layers per key).
    static constexpr std::size_t kMaxLayers = 8;

    // A sounding note: which plugins got its note-on, and at which note number
    // (after transpose). Keyed by plugin, not slot index, so it survives graph
    // swaps. Audio thread only.
    struct HeldNote {
        bool                used    = false;
        MidiSourceHandle    source  = kInvalidMidiSource;
        std::uint8_t        group   = 0;
        std::uint8_t        channel = 0;
        std::uint8_t        note    = 0;
        std::uint8_t        count   = 0;
        const HostedPlugin* plugins[kMaxLayers]        = {};
        std::uint8_t        deliveredNotes[kMaxLayers] = {};
    };

    // A pedal held down on a live input (sustain, sostenuto, soft: CC 64, 66,
    // 67), as its last message. A slot new in a graph (the next song's, at a
    // switch) is sent the ones held, so it knows the pedal is down
    // (docs/setlists.md, edge case 2). Audio thread only.
    struct HeldPedal {
        bool             used   = false;
        MidiSourceHandle source = kInvalidMidiSource;
        MidiMessage      message;
    };

    // MIDI 1.0 -> 2.0 translation state belongs to the incoming stream.
    struct SourceState {
        MidiSourceHandle    source = kInvalidMidiSource;   // invalid = free
        ump::UpconvertState upconvert;
    };

    void adoptPendingGraph() noexcept;
    // A switch due in this block: true once its plan applies (graph and
    // sequences adopted with it); `holdSwitch` set while one waits for its bar.
    bool takeSwitch(const AudioCallbackContext& ctx) noexcept;
    void applySwitch(const AudioCallbackContext& ctx) noexcept;   // after the sequences are adopted
    void adoptSequences(const AudioCallbackContext& ctx, bool switching) noexcept;
    void takeMidi(const AudioCallbackContext& ctx) noexcept;
    void generateSequences(const AudioCallbackContext& ctx) noexcept;
    void generateAuditions(const AudioCallbackContext& ctx) noexcept;
    void routeMidi(const AudioCallbackContext& ctx) noexcept;
    void applyMutes() noexcept;
    void trackPedal(MidiSourceHandle source, const MidiMessage& m) noexcept;
    void sendHeldPedals() noexcept;   // to the graph's fresh slots
    void route(const QueuedMidi& item, std::uint32_t offset) noexcept;
    void silence(const AudioCallbackContext& ctx, std::uint32_t offset) noexcept;   // a Panic, at `offset`
    // Control links (docs/controls.md): a controller a link takes, at `offset`.
    void applyLinks(MidiSourceHandle source, const MidiMessage& message, std::uint32_t offset) noexcept;
    double linkLevel(const CompiledControlLink::Target& target) noexcept;   // a level target's dB now
    void setLinkTarget(const CompiledControlLink::Target& target, double value, std::uint32_t offset) noexcept;
    SlotRuntime* findRuntime(const SlotRuntime* key) const noexcept;   // a row's or bus's, in the current graph
    // A plugin's own parameter change (its GUI, a preset): pick-up waits for the control again.
    void parameterChanged(const HostedPlugin* plugin, std::uint32_t param, double value) noexcept;
    void parameterTouched(const HostedPlugin* plugin, std::uint32_t param) noexcept;   // armTouch()
    bool deliver(std::size_t slot, const MidiMessage& original, const MidiMessage* upconverted,
                 std::size_t upconvertedCount, std::uint32_t offset, int noteOverride,
                 std::uint8_t* deliveredNote) noexcept;
    bool append(std::size_t slot, const MidiMessage& message, std::uint32_t offset,
                std::uint8_t* deliveredNote) noexcept;
    ump::UpconvertState* upconvertStateFor(MidiSourceHandle source) noexcept;
    HeldNote*            findHeld(MidiSourceHandle source, const MidiMessage& m) noexcept;
    HeldNote*            claimHeld(MidiSourceHandle source, const MidiMessage& m) noexcept;
    void                 runSlots(const AudioCallbackContext& ctx) noexcept;
    // Run a slot's MIDI effects over one chunk's events, in order; returns what
    // the last one sent, ready for the slot's plugin (`input` if it has none).
    // `input` is the slot's events for this chunk: its MIDI goes through the
    // effects, its parameter changes to the plugin, each effect getting its own.
    // A Panic's Per-Note Management resets for the keys the slot's plugin has
    // retuned, at `at` in the chunk, merged into its input; and the tracking
    // of retuned keys over what the plugin is given.
    EventBlock           addRetuneResets(std::size_t slot, EventBlock input, std::uint32_t at) noexcept;
    static void          trackRetuned(SlotRuntime& runtime, EventBlock input) noexcept;
    EventBlock           runMidiEffects(std::size_t slot, const TimeInfo& time, std::uint32_t chunkStart,
                                        std::uint32_t chunk, EventBlock input) noexcept;
    void                 advanceTransport(const AudioCallbackContext& ctx) noexcept;
    void                 makeSequenceBlock(const AudioCallbackContext& ctx) noexcept;
    void                 renderClick(const AudioCallbackContext& ctx) noexcept;
    // A start: counts in first if the count-in is set and `countIn` (not for
    // start on first note); else starts at once.
    void                 requestStart(const AudioCallbackContext& ctx, std::uint32_t frame, bool countIn) noexcept;
    void                 advanceCountIn(const AudioCallbackContext& ctx) noexcept;   // bar 1, if it's in this block
    void                 cancelCountIn(std::uint32_t frame) noexcept;
    void                 startTransport(const AudioCallbackContext& ctx, std::uint32_t frame) noexcept;
    void                 stopTransport(std::uint32_t frame) noexcept;
    void                 transportAction(const AudioCallbackContext& ctx, TransportCommand command,
                                         std::uint32_t frame) noexcept;
    // A sequence command at `frame`; one that starts a sequence starts a stopped transport first.
    void                 sequenceCommand(const AudioCallbackContext& ctx, MidiSourceHandle source,
                                         SequenceCommand command, std::uint32_t frame) noexcept;
    TimeInfo             timeAt(const AudioCallbackContext& ctx, std::uint32_t frame) const noexcept;
    // Move `ramp` toward `target` over `frames`: per-sample gains written to
    // `buffer` (returned), or null with `constantGain` set when it's steady.
    static const float*  advanceGain(GainRamp& ramp, float target, std::uint32_t frames, double sampleRate,
                                     float* buffer, float& constantGain) noexcept;
    void                 runBuses(const AudioCallbackContext& ctx, std::uint32_t chunkStart, std::uint32_t chunk) noexcept;

    HeldNote    heldNotes[256];     // audio thread only
    HeldPedal   heldPedals[64];     // audio thread only
    SourceState sourceStates[16];   // audio thread only

    SpscQueue<QueuedMidi, 4096>   midiQueue;                // MIDI thread -> audio
    std::atomic<EngineGraph*>     pendingGraph{nullptr};    // main -> audio
    std::atomic<std::uint64_t>    issuedSequence{0};        // written by main
    std::atomic<std::uint64_t>    adoptedSequence{0};       // written by audio
    SpscQueue<EngineGraph*, 16>   retiredGraphs;            // audio -> main
    EngineGraph*                  currentGraph = nullptr;   // audio thread only
    LevelMeter                    outputLevels;
    std::atomic<float>            masterTarget{1.0f};
    GainRamp                      masterRamp{1.0f};                     // audio thread only
    float                         masterGains[kMaxPluginBlockFrames] = {};   // its per-sample gains

    // Sequences: snapshots main -> audio -> main like graphs, commands main ->
    // audio, and this block's generated events (sorted by offset).
    struct QueuedCommand {
        MidiSourceHandle source  = kInvalidMidiSource;
        SequenceCommand  command = SequenceCommand::Stop;
    };
    static constexpr std::size_t kMaxSequenceEvents = 1024;
    std::atomic<SequenceSet*>     pendingSequences{nullptr};
    SpscQueue<SequenceSet*, 16>   retiredSequences;
    SequenceSet*                  currentSequences = nullptr;   // audio thread only
    SpscQueue<QueuedCommand, 64>  sequenceCommands;
    struct QueuedLooperCommand {
        MidiSourceHandle looper  = kInvalidMidiSource;
        LooperCommand    command = LooperCommand::Press;
    };
    SpscQueue<QueuedLooperCommand, 64> looperCommands;
    struct ParameterRequest {
        const HostedPlugin* plugin = nullptr;
        std::uint32_t       param  = 0;
        double              value  = 0.0;
    };
    SpscQueue<ParameterRequest, 256> parameterRequests;   // main -> audio (setParameter())
    // A parameter change for `plugin`, wherever it runs in the current graph,
    // queued in time order with that plugin's other events.
    void appendParameter(const HostedPlugin* plugin, const Event& event) noexcept;
    void                          recordLoops() noexcept;   // this block's staged input, into the loopers
    std::atomic<MidiSourceHandle> currentLooper{kInvalidMidiSource};
    // A looper's command at `frame`; a press arming one while stopped starts the transport.
    void                          looperAction(const AudioCallbackContext& ctx, MidiSourceHandle looper,
                                               LooperCommand command, std::uint32_t frame) noexcept;
    SpscQueue<TransportCommand, 16> transportCommands;
    SequenceEvent                 sequenceEvents[kMaxSequenceEvents];
    std::size_t                   sequenceEventCount = 0;
    std::atomic<std::uint64_t>    sequenceEventsFull{0};
    SequenceSet::Block            sequenceBlock;   // this block, for the sequences

    // Auditioned notes: requests main -> audio, and the ones sounding (audio
    // thread only), each ending after its own count of frames.
    struct AuditionRequest {
        MidiSourceHandle source = kInvalidMidiSource;   // the sequence's, with kAuditionSourceBit
        MidiMessage      on, off;
        double           seconds = 0.0;
        bool             end     = false;   // endAuditions(): `on` is its allNotesOff (if any words)
    };
    struct AuditionVoice {
        bool             used   = false;
        MidiSourceHandle source = kInvalidMidiSource;
        MidiMessage      off;
        std::uint64_t    framesLeft = 0;
    };
    SpscQueue<AuditionRequest, 64> auditionRequests;
    AuditionVoice                  auditionVoices[32];

    // This block's input MIDI, taken from the queue before the sequences run
    // (a trigger must act before they generate). As large as the queue.
    struct StagedMidi {
        QueuedMidi    item;
        std::uint32_t offset   = 0;
        bool          linked   = false;   // a control link's controller (applied in routeMidi())
        bool          consumed = false;   // and it goes nowhere else
    };
    static constexpr std::size_t kMaxStagedMidi = 4096;
    StagedMidi                    staged[kMaxStagedMidi];
    std::size_t                   stagedCount = 0;

    // MIDI learn: main arms (Idle/Captured -> Armed); audio fills `learned`,
    // then Armed -> Captured; main reads it and goes back to Idle.
    enum : int { kLearnIdle, kLearnArmed, kLearnCaptured };
    std::atomic<int>              learnState{kLearnIdle};
    std::atomic<bool>             learnPrograms{false};

    // Panic: main asks (panicRequested); the audio thread places it in the
    // block (panicFrame: UINT32_MAX none), and publishes the count.
    std::atomic<bool>             panicRequested{false};
    std::uint32_t                 panicFrame    = UINT32_MAX;   // audio thread only
    bool                          panickedOnce  = false;
    Nanoseconds                   lastPanicTime = 0;
    std::atomic<std::uint32_t>    panicCount{0};
    std::atomic<bool>             panicCut{false};

    // Plugins' parameter changes out of process(), for control links' pick-up.
    struct ParamFeedback final : EventSink {
        explicit ParamFeedback(Engine* owner) noexcept : engine(owner) {}
        Engine*             engine;
        const HostedPlugin* plugin = nullptr;   // the one processing
        void push(const Event& event) noexcept override {
            if (event.type == EventType::ParamValue) {
                engine->parameterChanged(plugin, event.param.id, event.value);
                engine->parameterTouched(plugin, event.param.id);
            } else if (event.type == EventType::ParamGestureBegin) {
                engine->parameterTouched(plugin, event.param.id);
            }
        }
    };
    ParamFeedback feedback{this};
    std::atomic<bool>             touchArmed{false};
    SpscQueue<Touched, 32>        touches;   // audio -> main (armTouch())
    // A MIDI effect's output (runMidiEffects()): its notes and MIDI, translated
    // for the next stage's note port as deliver() does, into that stage's
    // buffer at their offsets; its parameter changes, as ParamFeedback's.
    struct ChainSink final : EventSink {
        explicit ChainSink(Engine* owner) noexcept : engine(owner) {}
        Engine*              engine;
        const HostedPlugin*  plugin    = nullptr;   // the effect processing
        Event*               events    = nullptr;   // the next stage's input
        std::uint32_t        capacity  = 0;
        std::uint32_t        count     = 0;
        std::uint32_t        frames    = 0;         // the chunk's length: offsets past it are clamped
        NoteDialectMask      dialects  = 0;         // of the next stage's note port
        std::uint16_t        port      = 0;         // and its index
        ump::UpconvertState* upconvert = nullptr;   // the effect's output stream's
        void push(const Event& event) noexcept override;
        void add(const MidiMessage& message, std::uint32_t offset) noexcept;
    };
    ChainSink chainSink{this};
    double        linkMasterDb = 0.0;   // the master's control link level (audio thread)
    // linkStates(): written at the end of each block.
    struct ShownLinkTarget {
        std::atomic<std::uint64_t> origin{0};
        std::atomic<double>        value{0.0};
        std::atomic<double>        travel{-1.0};
        std::atomic<bool>          waiting{false};
    };
    ShownLinkTarget            shownLinks[kShownLinkTargets];
    std::atomic<std::uint32_t> shownLinkCount{0};
    void                       publishLinks() noexcept;

    // DSP load: the running average (audio thread only) and what's published.
    float                         loadAverage = 0.0f;
    std::atomic<float>            publishedLoadAverage{0.0f};
    LoadMeter                     loadPeak;
    SpscQueue<SequenceSet::SetlistHit, 16> setlistRequests;   // audio -> main

    // Song switches: plans main -> audio (pushed before their graph is
    // committed), and the one waiting (audio thread only).
    struct QueuedSwitch {
        SwitchPlan    plan;
        std::uint64_t graph = 0;   // the commit() sequence carrying the new rack
    };
    SpscQueue<QueuedSwitch, 8>    switchPlans;
    std::atomic<bool>             switchForced{false};
    QueuedSwitch                  armedSwitch;             // audio thread only
    bool                          switchArmed = false;
    bool                          holdSwitch  = false;    // this block: leave the pending graph and sequences
    double                        switchBar   = -1.0;     // at-next-bar: the bar line's beat (old signature)
    Learned                       learned;

    // Per-input message counts (audio writes, main reads). A slot is claimed by
    // the audio thread the first time a source is heard; 32 is plenty of inputs.
    struct SourceCount {
        std::atomic<MidiSourceHandle> source{kInvalidMidiSource};
        std::atomic<std::uint32_t>    count{0};
    };
    SourceCount sourceCounts[32];
    void        countSource(MidiSourceHandle source) noexcept;

    // Transport. Tempo and signature: main writes, audio reads per block. The
    // beat clock itself is audio-thread state, published after each block.
    std::atomic<double>        tempo{120.0};
    std::atomic<std::uint32_t> signature{(4u << 16) | 4u};   // numerator << 16 | denominator
    // The rack's own, which a pending switch applies (setTempo()).
    std::atomic<double>        nextTempo{120.0};
    std::atomic<std::uint32_t> nextSignature{(4u << 16) | 4u};
    std::atomic<std::uint32_t> switchesPending{0};   // committed, not yet applied
    std::uint32_t              switchesTaken = 0;    // audio thread: popped, not yet applied
    struct BeatClock {
        double       beat          = 0.0;   // at the current block's first frame
        double       tempoBpm      = 120.0; // this block's tempo
        double       beatsPerFrame = 0.0;
        int          numerator     = 4;
        int          denominator   = 4;
        double       barAnchorBeat = 0.0;   // bars count from here (moves when the signature changes)
        std::int32_t barAnchor     = 0;     // the bar number at barAnchorBeat
        bool         running       = false;
        bool         stopPending   = false; // stopped in this block: back to beat 0 after it
        std::uint32_t stopFrame    = UINT32_MAX;   // where, in this block
    } clock;                                // audio thread only

    // The count-in (audio thread only, but `countInBars`).
    struct CountIn {
        bool          active     = false;   // counting
        bool          inBlock    = false;   // it counted during this block (its clicks to render)
        double        position   = 0.0;     // beats counted at this block's first frame (< 0: it begins in this block)
        double        pulseBeats = 1.0;
        int           perBar     = 4;
        int           pulses     = 0;       // in all
        std::uint32_t endFrame   = UINT32_MAX;   // where it ended in this block (bar 1, or cancelled)
        struct Deferred {
            MidiSourceHandle source;
            SequenceCommand  command;
        };
        Deferred      deferred[16];         // sequences started meanwhile: they start on bar 1
        std::size_t   deferredCount = 0;
    } countIn;
    std::atomic<double> countInBars{0.0};
    std::atomic<int>    publishedCountIn{0};
    std::atomic<double> publishedCountInBeat{0.0};
    std::atomic<int>    publishedCountInPulses{0};
    std::atomic<Nanoseconds> publishedAt{0};

    std::atomic<bool>  clickOn{false};
    std::atomic<bool>  clickEveryNote{false};
    std::atomic<float> clickGain{0.25f};
    std::atomic<bool>  clickTest{false};
    struct ClickVoice {                     // the blip sounding now (audio thread only)
        std::uint32_t remaining = 0;
        double        phase = 0.0, step = 0.0;
        float         level = 0.0f, decay = 1.0f;
    } clickVoice;
    std::atomic<bool>         publishedRunning{false};
    std::atomic<double>       publishedBeat{0.0};
    std::atomic<double>       publishedBarStart{0.0};
    std::atomic<std::int32_t> publishedBar{0};

    std::atomic<std::uint64_t> midiReceived{0};
    std::atomic<std::uint64_t> midiQueueFull{0};
    std::atomic<std::uint64_t> slotEventsFull{0};
    std::atomic<std::uint64_t> unsupportedDialect{0};
    std::atomic<std::uint64_t> lateMidi{0};
};

} // namespace stagehand
