#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <string>

#include "Core/SpscQueue.h"
#include "Midi/MidiInput.h"

namespace stagehand {

// M2 test fixture: a MidiSink that prints what actually arrives at input —
// source, UMP message type, raw words, a decoded summary, and capture-time
// spacing. The MIDI thread pushes into a lock-free queue (as the engine's sink
// will); the main thread drains and prints. Large: allocate on the heap.
class MidiMonitor final : public MidiSink {
public:
    void onMidi(MidiSourceHandle source, const TimedMidiMessage& msg) noexcept override;

    // Main thread: print everything queued. `names` maps handles to labels.
    void drain(const std::map<MidiSourceHandle, std::string>& names);

private:
    struct Entry {
        MidiSourceHandle source = kInvalidMidiSource;
        TimedMidiMessage message;
    };

    SpscQueue<Entry, 4096>     queue;
    std::atomic<std::uint64_t> dropped{0};
    std::uint64_t              reportedDrops = 0;
    Nanoseconds                firstTime     = 0;
    Nanoseconds                lastTime      = 0;
};

// One-line description of a UMP packet, e.g.
// "MIDI2 g0 ch1  NoteOn    note 61 vel 0x8000 pitch 61.312".
std::string describeUmp(const MidiMessage& m);

} // namespace stagehand
