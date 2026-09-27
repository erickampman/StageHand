#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "Audio/AudioBackend.h"
#include "Core/SpscQueue.h"
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
//      message goes to every slot whose filter accepts it. The queue is in
//      capture order, so each slot's events come out already sorted.
//   3. Split the block into chunks of at most kMaxPluginBlockFrames. For each
//      chunk, run every slot with its share of the events, then sum the slot
//      outputs (times gain) into the backend outputs.
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
        std::uint64_t unsupportedDialect = 0;   // dropped: slot can't take the protocol (M3; M5 translates)
        std::uint64_t lateMidi           = 0;   // older than the window; placed at offset 0
    };
    Stats stats() const noexcept;   // any thread

    // Peak absolute sample value across all outputs since the last call, then
    // reset. A level meter for the main thread; approximate by design.
    float takeOutputPeak() noexcept;

    // --- audio thread ---
    void process(const AudioCallbackContext& ctx) noexcept override;

    // --- MIDI thread ---
    void onMidi(MidiSourceHandle source, const TimedMidiMessage& msg) noexcept override;

private:
    struct QueuedMidi {
        MidiSourceHandle source = kInvalidMidiSource;
        TimedMidiMessage message;
    };

    void adoptPendingGraph() noexcept;
    void gatherMidi(const AudioCallbackContext& ctx) noexcept;
    void route(const QueuedMidi& item, std::uint32_t offset) noexcept;
    void runSlots(const AudioCallbackContext& ctx) noexcept;

    SpscQueue<QueuedMidi, 4096>   midiQueue;                // MIDI thread -> audio
    std::atomic<EngineGraph*>     pendingGraph{nullptr};    // main -> audio
    std::atomic<std::uint64_t>    issuedSequence{0};        // written by main
    std::atomic<std::uint64_t>    adoptedSequence{0};       // written by audio
    SpscQueue<EngineGraph*, 16>   retiredGraphs;            // audio -> main
    EngineGraph*                  currentGraph = nullptr;   // audio thread only
    std::atomic<float>            outputPeak{0.0f};

    std::atomic<std::uint64_t> midiReceived{0};
    std::atomic<std::uint64_t> midiQueueFull{0};
    std::atomic<std::uint64_t> slotEventsFull{0};
    std::atomic<std::uint64_t> unsupportedDialect{0};
    std::atomic<std::uint64_t> lateMidi{0};
};

} // namespace stagehand
