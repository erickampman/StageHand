#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "Audio/AudioBackend.h"
#include "Core/SpscQueue.h"
#include "Core/Ump.h"
#include "Engine/EngineGraph.h"
#include "Midi/MidiInput.h"

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
//   2. Drain the MIDI captured before this block started, one period behind:
//      offset = floor((t - (blockStart - frames/rate)) * rate), clamped to
//      [0, frames). Messages stamped at or after blockStart stay queued. Each
//      message goes to every slot whose filter accepts it, in the message's
//      own protocol if the slot's note port takes it, otherwise translated
//      (MIDI 2.0 downconverted for MIDI 1.0 ports; MIDI 1.0 upconverted for
//      MIDI-2.0-only ports, with per-source state). A note-off goes wherever
//      its note-on went, at the note number each plugin actually got, even if
//      filters or transposes changed in between (the held-note table). The
//      queue is in capture order, so each slot's events come out sorted.
//   3. Split the block into chunks of at most kMaxPluginBlockFrames. For each
//      chunk, run every slot with its share of the events, then sum the slot
//      outputs into the backend outputs, times the slot's gain. A change of
//      gain (SlotRuntime::targetGain) ramps linearly over kGainRampSeconds, per
//      sample, so a fader move or mute doesn't click. Slot and output meters
//      are fed post-gain.
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
    };
    Stats stats() const noexcept;   // any thread

    // The backend outputs' level (the master meter): outputs 1 and 2, with any
    // further outputs folded into the second channel. take() from the main thread.
    LevelMeter& outputMeter() noexcept { return outputLevels; }

    // How long a change of slot gain takes to arrive (docs/rack-view.md, section 3).
    static constexpr double kGainRampSeconds = 0.010;

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

    // MIDI 1.0 -> 2.0 translation state belongs to the incoming stream.
    struct SourceState {
        MidiSourceHandle    source = kInvalidMidiSource;   // invalid = free
        ump::UpconvertState upconvert;
    };

    void adoptPendingGraph() noexcept;
    void gatherMidi(const AudioCallbackContext& ctx) noexcept;
    void route(const QueuedMidi& item, std::uint32_t offset) noexcept;
    bool deliver(std::size_t slot, const MidiMessage& original, const MidiMessage* upconverted,
                 std::size_t upconvertedCount, std::uint32_t offset, int noteOverride,
                 std::uint8_t* deliveredNote) noexcept;
    bool append(std::size_t slot, const MidiMessage& message, std::uint32_t offset,
                std::uint8_t* deliveredNote) noexcept;
    ump::UpconvertState* upconvertStateFor(MidiSourceHandle source) noexcept;
    HeldNote*            findHeld(MidiSourceHandle source, const MidiMessage& m) noexcept;
    HeldNote*            claimHeld(MidiSourceHandle source, const MidiMessage& m) noexcept;
    void                 runSlots(const AudioCallbackContext& ctx) noexcept;
    const float*         advanceGain(SlotRuntime& runtime, std::uint32_t frames, double sampleRate,
                                     float& constantGain) noexcept;

    HeldNote    heldNotes[256];     // audio thread only
    SourceState sourceStates[16];   // audio thread only

    SpscQueue<QueuedMidi, 4096>   midiQueue;                // MIDI thread -> audio
    std::atomic<EngineGraph*>     pendingGraph{nullptr};    // main -> audio
    std::atomic<std::uint64_t>    issuedSequence{0};        // written by main
    std::atomic<std::uint64_t>    adoptedSequence{0};       // written by audio
    SpscQueue<EngineGraph*, 16>   retiredGraphs;            // audio -> main
    EngineGraph*                  currentGraph = nullptr;   // audio thread only
    LevelMeter                    outputLevels;

    std::atomic<std::uint64_t> midiReceived{0};
    std::atomic<std::uint64_t> midiQueueFull{0};
    std::atomic<std::uint64_t> slotEventsFull{0};
    std::atomic<std::uint64_t> unsupportedDialect{0};
    std::atomic<std::uint64_t> lateMidi{0};
};

} // namespace stagehand
