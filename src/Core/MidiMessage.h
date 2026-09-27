#pragma once

#include <cstdint>

#include "Core/Types.h"

namespace stagehand {

// A single Universal MIDI Packet (UMP) — the MIDI 2.0 wire format, which also
// represents MIDI 1.0. One to four 32-bit words; `wordCount` says how many are
// used. This is StageHand's canonical MIDI unit: coding to UMP handles both
// MIDI 1.0 (which ALSA presents as UMP too) and native MIDI 2.0 from one path,
// and maps ~1:1 onto CLAP's clap_event_midi2. For VST3 it must be decoded (see
// docs/architecture.md 3a).
struct MidiMessage {
    std::uint32_t words[4]  = {0, 0, 0, 0};
    std::uint8_t  wordCount = 0;   // 1..4
};

// A MidiMessage tagged with the monotonic time it was captured, as delivered
// from the MIDI thread to the engine. The engine maps `captureTime` to a sample
// offset within the current block (docs/domain-model.md, sample-accurate
// alignment).
struct TimedMidiMessage {
    Nanoseconds captureTime = 0;
    MidiMessage message;
};

} // namespace stagehand
