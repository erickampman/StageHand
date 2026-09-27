#pragma once

#include <cstdint>

namespace stagehand {

// Musical transport state for one process block. Today it is mostly static; the
// future sequencer is "the thing that advances TimeInfo". Maps cleanly onto
// CLAP's transport event and VST3's ProcessContext (see docs/architecture.md
// section 3a — this is one of the clean matches).
struct TimeInfo {
    double       sampleRate     = 0.0;    // frames per second for this block
    std::int64_t samplePosition = 0;      // frames elapsed since transport start

    double tempoBpm            = 120.0;   // beats per minute
    int    timeSigNumerator    = 4;
    int    timeSigDenominator  = 4;
    bool   playing             = false;   // is the transport running?

    // Loop region in frames; meaningful only when `looping` is true.
    bool         looping   = false;
    std::int64_t loopStart = 0;
    std::int64_t loopEnd   = 0;
};

} // namespace stagehand
