#pragma once

#include <string>
#include <vector>

#include "Core/MidiMessage.h"

namespace stagehand {

// Describes an available MIDI input port.
struct MidiSourceInfo {
    std::string  id;                            // stable identifier used to open it
    std::string  name;                          // human-readable, for UI
    bool         isVirtual = false;
    MidiProtocol protocol  = MidiProtocol::Midi1;   // the source's native protocol
};

// Implemented by the engine. Called from the MIDI thread when a message
// arrives. Implementations MUST be non-blocking (push to a lock-free queue and
// return; no locks, no allocation).
class MidiSink {
public:
    virtual ~MidiSink() = default;
    virtual void onMidi(MidiSourceHandle source,
                        const TimedMidiMessage& msg) noexcept = 0;
};

// The MIDI-input seam. The only component that knows ALSA (initial impl: the
// sequencer in UMP mode, with one MIDI 1.0 client and one MIDI 2.0 client —
// docs/architecture.md section 1), later libremidi/CoreMIDI. Owns a non-RT MIDI
// thread; timestamps each message at capture and delivers it to the sink.
//
// It WRAPS BUT NEVER TRANSLATES: MIDI 1.0 sources arrive as MT 0x2/0x1/0x3 (a
// lossless wrap), MIDI 2.0 sources as MT 0x4. Routing and protocol translation
// are the engine's job. The one exception is the global protocol preference set
// to Midi1, which deliberately receives every source as MIDI 1.0.
//
// Lifecycle: setProtocolPreference() / setSink() -> open() sources -> [events
// on the MIDI thread] -> close(). Everything except sink delivery is
// main-thread.
class MidiInput {
public:
    virtual ~MidiInput() = default;

    // App-wide (Logic-style) protocol preference. Midi2 (default): UMP-native
    // sources are received as MIDI 2.0, legacy sources as MIDI 1.0. Midi1: every
    // source is received as MIDI 1.0 (a compatibility safety valve). Applies to
    // sources opened afterwards; reopen existing sources to change them.
    virtual void setProtocolPreference(MidiProtocol preference) = 0;

    // Where captured messages are delivered. Set before opening any source.
    virtual void setSink(MidiSink* sink) = 0;

    // List currently available input sources.
    virtual std::vector<MidiSourceInfo> enumerate() = 0;

    // Open a source by its MidiSourceInfo::id. Returns kInvalidMidiSource on
    // failure. Multiple sources may be open at once.
    virtual MidiSourceHandle open(const std::string& sourceId) = 0;

    // Create a named input port that other apps can connect to (e.g. a KeyEDO
    // port or a MIDI 2.0 test tool), receiving in `protocol`. Returns
    // kInvalidMidiSource on failure.
    virtual MidiSourceHandle openVirtual(const std::string& name,
                                         MidiProtocol protocol) = 0;

    virtual void close(MidiSourceHandle source) = 0;

    // Human-readable backend name, e.g. "ALSA sequencer". For UI and logging.
    virtual const char* name() const = 0;
};

} // namespace stagehand
