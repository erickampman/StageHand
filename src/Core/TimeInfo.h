#pragma once

#include <cstdint>

namespace stagehand {

// Musical transport state for one process block (or chunk of one). The backend
// fills the sample fields; the engine fills the musical ones from its beat clock
// (docs/sequences.md, section 1). Maps cleanly onto CLAP's transport event and
// VST3's ProcessContext (see docs/architecture.md section 3a — this is one of the
// clean matches). Beats are quarter notes: BPM counts them.
struct TimeInfo {
    double       sampleRate     = 0.0;    // frames per second for this block
    std::int64_t samplePosition = 0;      // frames elapsed since transport start

    double tempoBpm            = 120.0;   // beats per minute
    int    timeSigNumerator    = 4;
    int    timeSigDenominator  = 4;
    bool   playing             = false;   // is the transport running?

    double       beatPosition  = 0.0;     // beats since the transport started, at the block's first frame
    double       barStartBeats = 0.0;     // where the bar containing beatPosition began
    std::int32_t barNumber     = 0;       // that bar, counting from 0

    // Loop region in frames; meaningful only when `looping` is true.
    bool         looping   = false;
    std::int64_t loopStart = 0;
    std::int64_t loopEnd   = 0;
};

} // namespace stagehand
