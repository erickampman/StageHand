#pragma once

#include <cstdint>

#include "Core/MidiMessage.h"

namespace stagehand {

// Identifies a parameter within a plugin. `cookie` is the plugin's opaque
// fast-lookup handle (CLAP cookie); may be null.
struct ParamRef {
    std::uint32_t id     = 0;
    void*         cookie = nullptr;
};

// The kinds of event that share the per-block, time-sorted stream. Mirrors
// CLAP's single interleaved input event list; the VST3 seam demultiplexes these
// into IEventList + IParameterChanges (docs/architecture.md 3a).
enum class EventType {
    Midi,               // a UMP MidiMessage
    ParamValue,         // set a parameter to `value`
    ParamMod,           // add modulation `value` to a parameter
    ParamGestureBegin,  // user began touching a parameter (automation recording)
    ParamGestureEnd,    // user released a parameter
};

// One event with a sample offset into the current block. A tagged record rather
// than a variant, to stay trivially copyable and RT-friendly: `midi` is valid
// when type == Midi, the parameter fields for the Param* kinds.
struct Event {
    std::uint32_t sampleOffset = 0;   // in [0, frameCount)
    EventType     type         = EventType::Midi;

    MidiMessage midi;            // valid when type == Midi

    ParamRef param;             // valid when type is a Param* kind
    double   value = 0.0;       // new value (ParamValue) / mod amount (ParamMod)
};

// A non-owning, time-sorted (ascending sampleOffset) view of the events for one
// plugin for one block. Matches CLAP's sorted input event list.
struct EventBlock {
    const Event*  events = nullptr;
    std::uint32_t count  = 0;
};

// Where a plugin pushes its OUTPUT events during process() — parameter changes
// driven by the plugin's own GUI or internal modulation, fed back to the host
// (the RT->Main path in docs/domain-model.md). Called on the audio thread;
// implementations must be non-blocking.
class EventSink {
public:
    virtual ~EventSink() = default;
    virtual void push(const Event& event) noexcept = 0;
};

} // namespace stagehand
