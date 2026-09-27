#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "Core/AudioBuffer.h"
#include "Core/Event.h"
#include "Core/TimeInfo.h"
#include "Plugin/Parameter.h"
#include "Plugin/PluginDescriptor.h"
#include "Plugin/PortLayout.h"

namespace stagehand {

// Everything a plugin needs to process one block. By the time this is built the
// host has already mapped MIDI to sample offsets and merged param events into
// `eventsIn` (time-sorted). `eventsOut` may be null if the host is not
// collecting feedback this block.
struct ProcessContext {
    std::uint32_t    frameCount = 0;
    TimeInfo         time;
    ConstAudioBuffer audioIn;              // may be empty (channelCount == 0)
    AudioBuffer      audioOut;
    EventBlock       eventsIn;             // time-sorted MIDI + param events
    EventSink*       eventsOut = nullptr;  // plugin pushes param feedback here
};

// The plugin seam. The only place that knows CLAP (initial) or VST3 (later);
// each format's quirks live in its impl (docs/architecture.md 3a). One
// HostedPlugin wraps one plugin instance, shared by every output format the host
// exposes.
//
// Lifecycle: create (via PluginFactory) -> loadState (optional, BEFORE activate)
// -> activate -> [process on the audio thread] -> deactivate -> destroy.
class HostedPlugin {
public:
    virtual ~HostedPlugin() = default;

    // --- identity / introspection (main thread) ---
    virtual const PluginDescriptor&    descriptor() const = 0;
    virtual PortLayout                 portLayout() const = 0;
    virtual std::vector<ParameterInfo> parameters() const = 0;

    // --- lifecycle (main thread) ---
    // Prepare to process at `sampleRate` with at most `maxBlockFrames` per block.
    // Returns false on failure.
    virtual bool activate(double sampleRate, std::uint32_t maxBlockFrames) = 0;
    virtual void deactivate() = 0;

    // --- real-time processing (audio thread) ---
    // Consume ctx.eventsIn, render ctx.audioOut, emit feedback via ctx.eventsOut.
    // No locks, allocation, syscalls, or blocking.
    virtual void process(const ProcessContext& ctx) noexcept = 0;

    // --- parameters ---
    // Current plain value of a parameter (main thread, for UI). NOTE: to change a
    // parameter audibly, the engine sends a ParamValue Event into process();
    // there is deliberately no direct RT setter here.
    virtual double paramValue(std::uint32_t id) const = 0;

    // Apply parameter changes / collect feedback while NOT processing (main
    // thread) — the stopped-transport path (CLAP params.flush).
    virtual void flushParams(const EventBlock& in, EventSink* out) = 0;

    // --- state (main thread) ---
    // Serialize opaque plugin state into `out` (appends). For VST3 this wraps
    // both component and controller state. Returns false on failure.
    virtual bool saveState(std::vector<std::byte>& out) const = 0;
    // Restore from a blob previously produced by saveState(). Call BEFORE the
    // plugin joins the live graph (docs/domain-model.md, save/restore).
    virtual bool loadState(const std::byte* data, std::size_t size) = 0;
};

} // namespace stagehand
