#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "Core/Event.h"
#include "Core/MidiFilter.h"
#include "Engine/SlotRuntime.h"
#include "Plugin/HostedPlugin.h"
#include "Plugin/PortLayout.h"

namespace stagehand {

// Largest block a plugin is ever asked to process. Plugins are activated with
// this maximum and larger backend blocks are split. It equals PipeWire's
// documented quantum-limit, so under PipeWire blocks are not split in practice
// (docs/architecture.md section 2).
inline constexpr std::uint32_t kMaxPluginBlockFrames = 8192;

// One plugin slot as the engine runs it: the plugin plus the host-owned per-slot
// settings that affect processing (docs/domain-model.md, Session model).
struct SlotConfig {
    // Shared with the main thread's own bookkeeping (UI, Rack). Must already be
    // activated with kMaxPluginBlockFrames when the graph is committed. Graphs
    // are destroyed only on the main thread, so the last reference never drops
    // on the audio thread.
    std::shared_ptr<HostedPlugin> plugin;

    // Live gain, meter and MIDI activity, shared by every graph built for this
    // slot. Null = the graph creates one at unity gain (tests, one-off graphs).
    std::shared_ptr<SlotRuntime> runtime;

    MidiFilter    midiFilter;           // which MIDI this slot hears (its live input)
    // The sequences that play into this slot, heard on top of its live input
    // (and through the same filter). Preallocated here; read-only on the audio thread.
    std::vector<MidiSourceHandle> sequenceSources;
    std::uint16_t notePort    = 0;      // plugin note-input port that receives it
    std::uint32_t firstOutput = 0;      // backend output for the plugin's first channel
    // A slot of the rack switched away from, ringing out (docs/setlists.md,
    // section 3): no new note-ons reach it, the rest of its MIDI does (a
    // note-off through the held-note table, a pedal), its gain and sends stay,
    // and once its output has been quiet for Engine::kQuietSeconds (or after
    // Engine::kMaxDrainSeconds) it's suspended and SlotRuntime::drained is set.
    bool          draining    = false;
};

// One of the rig's effect buses (docs/rig.md): what the slots send to it runs
// through up to two effects in series, and the result is mixed into the
// outputs at its return level. The same plugins and runtime go into every
// graph built while the rig is loaded, so a song switch never reloads them.
struct BusConfig {
    static constexpr std::size_t kEffects = 2;
    // Either may be null (an empty slot passes the audio on). Activated with
    // kMaxPluginBlockFrames before the graph is committed, like a slot's plugin.
    std::shared_ptr<HostedPlugin> effects[kEffects];
    // The return level (targetGain; 0 when muted) and the return's meter. Null
    // = the graph creates one at unity.
    std::shared_ptr<SlotRuntime>  runtime;
    std::uint32_t                 firstOutput = 0;   // the return's first backend output
};

// An immutable description of what the engine runs, plus the scratch memory the
// audio thread needs to run it. Built on the main thread (where all allocation
// happens), published whole with Engine::commit(), and destroyed only on the
// main thread by Engine::collect().
//
// v1 topology is MainStage-style: incoming MIDI fans out to every slot whose
// filter accepts it (layers, splits), and each slot's main audio output is
// summed into the backend outputs. A mono plugin feeds its first two outputs.
// Insert effects and general routing extend this type later; the Engine
// contract (commit / adopt / retire) does not change.
class EngineGraph {
public:
    // Queries each plugin's PortLayout and preallocates the per-slot event and
    // audio buffers. Main thread.
    // Bus `b` is what slots send to with SlotRuntime::targetSend[b]; at most
    // kBusCount, the rest ignored.
    explicit EngineGraph(std::vector<SlotConfig> slotConfigs, std::vector<BusConfig> busConfigs = {},
                         std::uint32_t maxEventsPerSlotBlock = 1024);

    const std::vector<SlotConfig>& slots() const noexcept { return configs; }
    const std::vector<BusConfig>&  buses() const noexcept { return busConfigs; }
    bool contains(const HostedPlugin* plugin) const noexcept;

    static constexpr std::uint32_t kMaxBusEffectEvents = 256;   // a bus effect's events a block

private:
    friend class Engine;

    // Audio-thread working state for one slot; sized at construction.
    struct SlotState {
        NoteDialectMask     dialects      = 0;   // of the slot's note port (0 = no MIDI in)
        std::uint32_t       audioChannels = 0;   // of the plugin's main output port
        std::vector<Event>  events;              // one block's events; fixed capacity
        std::uint32_t       eventCount    = 0;
        std::uint32_t       cursor        = 0;   // next event to hand out (block splitting)
        std::vector<float>  audio;               // audioChannels * kMaxPluginBlockFrames
        std::vector<float*> channels;            // pointers into `audio`
        bool                fresh         = false;   // new in this graph: gets the held pedals (set at adoption)
        std::int64_t        processNs     = 0;       // this block's time in process(), for its LoadMeter
    };

    // Audio-thread working state for one bus, sized at construction. The bus
    // signal is stereo. Each effect gets the previous stage's channels (bus
    // input, or the effect before) mapped onto its main input port (a mono
    // stage feeds both channels of a stereo input; a mono input takes the
    // left), and writes its main output to buffers of its own (CLAP processes
    // in place only when a plugin says it can).
    struct BusState {
        std::vector<float>         input;   // 2 * kMaxPluginBlockFrames, summed sends
        float*                     inputChannels[2] = {};
        struct Stage {
            std::vector<const float*> in;     // the effect's main input channels
            std::vector<float>         audio;  // its output, channels * kMaxPluginBlockFrames
            std::vector<float*>        out;
            std::vector<Event>         events;   // one block's (control links' parameter changes); fixed capacity
            std::uint32_t              eventCount = 0;
            std::uint32_t              cursor     = 0;
        };
        Stage                      stages[BusConfig::kEffects];
        const float*               returnChannels[2] = {};   // the last stage's output (or the input), as stereo
        bool                       silent = false;            // an effect with no output: nothing to return
        std::int64_t               processNs = 0;             // this block's time in its effects
    };

    std::vector<SlotConfig> configs;
    std::vector<SlotState>  states;
    std::vector<BusConfig>  busConfigs;
    std::vector<BusState>   busStates;
    std::vector<float>      gainRamp;            // kMaxPluginBlockFrames per-sample gains
    std::vector<float>      sendRamp;            // the same, for a send (times the slot's gain)
    std::uint64_t           sequence = 0;        // set by Engine::commit()
};

} // namespace stagehand
