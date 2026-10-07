#pragma once

#include "uw/Types.h"
#include "Core/UwNames.h"
#include "Engine/Engine.h"

namespace stagehand {

// The beat as it's heard at a moment (docs/perform.md, decision 14): for the
// Perform view's beat boxes and the edge flash, drawn at the display's rate.
// The engine's position is for the block it processes next; the sound of it
// reaches the outputs `latencySeconds` later, so the beat heard at `now` is
// that position carried on by the time since, less the latency.
struct BeatNow {
    enum class State { Stopped, CountingIn, Running };
    State  state       = State::Stopped;
    int    pulse       = 0;      // the pulse sounding, 0-based in its bar (the box lit)
    int    pulsesInBar = 4;      // metronomePulse(): 4/4 four, 6/8 two
    double sincePulse  = -1.0;   // seconds since that pulse began (< 0: not yet heard)
    bool   downbeat    = false;  // that pulse is a bar's first
    int    bar         = 1;      // running: the bar and beat, from 1 ("17 · 3")
    int    beat        = 1;
    int    countIn     = 0;      // counting in: the pulse sounding, counted down (4, 3, 2, 1)
};

BeatNow beatNow(const Engine::Transport& transport, Nanoseconds now, double latencySeconds, bool everyNote);

} // namespace stagehand
