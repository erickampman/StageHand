#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Core/MidiMessage.h"

namespace stagehand {

// Opaque handle to an opened MIDI source. kInvalidMidiSource (0) means none.
using MidiSourceHandle = std::uint32_t;
inline constexpr MidiSourceHandle kInvalidMidiSource = 0;

// Describes an available MIDI input port.
struct MidiSourceInfo {
    std::string id;                    // stable identifier used to open it
    std::string name;                  // human-readable, for UI
    bool        isVirtual     = false;
    bool        supportsMidi2  = false; // native UMP endpoint vs legacy-converted
};

// Implemented by the engine. Called from the MIDI thread when a message
// arrives — implementations MUST be non-blocking (push to a lock-free queue and
// return; no locks, no allocation).
class MidiSink {
public:
    virtual ~MidiSink() = default;
    virtual void onMidi(MidiSourceHandle source,
                        const TimedMidiMessage& msg) noexcept = 0;
};

// The MIDI-input seam. The only component that knows ALSA rawmidi/UMP (initial
// impl), later libremidi/CoreMIDI for cross-platform. Owns a non-RT MIDI thread;
// timestamps each message at capture and delivers it to the sink.
//
// Lifecycle: setSink() -> open() one or more sources -> [events on the MIDI
// thread] -> close(). enumerate()/setSink()/open()/close() are main-thread.
class MidiInput {
public:
    virtual ~MidiInput() = default;

    // List currently available input sources.
    virtual std::vector<MidiSourceInfo> enumerate() = 0;

    // Where captured messages are delivered. Set before opening any source.
    virtual void setSink(MidiSink* sink) = 0;

    // Open a source by its MidiSourceInfo::id. Returns kInvalidMidiSource on
    // failure. Multiple sources may be open at once.
    virtual MidiSourceHandle open(const std::string& sourceId) = 0;
    virtual void             close(MidiSourceHandle source) = 0;

    // Human-readable backend name, e.g. "ALSA-UMP". For UI and logging.
    virtual const char* name() const = 0;
};

} // namespace stagehand
