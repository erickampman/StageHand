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

    MidiFilter    midiFilter;           // which MIDI this slot hears
    std::uint16_t notePort    = 0;      // plugin note-input port that receives it
    std::uint32_t firstOutput = 0;      // backend output for the plugin's first channel
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
    explicit EngineGraph(std::vector<SlotConfig> slotConfigs,
                         std::uint32_t maxEventsPerSlotBlock = 1024);

    const std::vector<SlotConfig>& slots() const noexcept { return configs; }
    bool contains(const HostedPlugin* plugin) const noexcept;

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
    };

    std::vector<SlotConfig> configs;
    std::vector<SlotState>  states;
    std::vector<float>      gainRamp;            // kMaxPluginBlockFrames per-sample gains
    std::uint64_t           sequence = 0;        // set by Engine::commit()
};

} // namespace stagehand
