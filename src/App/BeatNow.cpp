#include "App/BeatNow.h"

#include <algorithm>
#include <cmath>

#include "Core/TimeInfo.h"

namespace stagehand {

BeatNow beatNow(const Engine::Transport& t, Nanoseconds now, double latencySeconds, bool everyNote) {
    BeatNow b;
    const MetronomePulse pulse = metronomePulse(t.numerator, t.denominator, everyNote);
    b.pulsesInBar              = std::max(pulse.perBar, 1);
    const bool counting        = t.countIn > 0 && !t.running;
    if ((!t.running && !counting) || t.tempoBpm <= 0.0 || pulse.beats <= 0.0) return b;

    // Beats since the published position, as heard: negative while the
    // published block hasn't reached the outputs yet.
    const double seconds = static_cast<double>(now - t.at) / 1e9 - latencySeconds;
    const double moved   = seconds * t.tempoBpm / 60.0;
    auto secondsOf       = [&t](double beats) { return beats * 60.0 / t.tempoBpm; };

    if (counting) {
        b.state            = BeatNow::State::CountingIn;
        const double where = t.countInBeat + moved;
        const int    k     = std::max(0, static_cast<int>(std::floor(where / pulse.beats + 1e-9)));
        b.countIn          = std::max(1, t.countInPulses - k);
        b.pulse            = (b.pulsesInBar - b.countIn % b.pulsesInBar) % b.pulsesInBar;
        b.downbeat         = b.pulse == 0;
        b.sincePulse       = secondsOf(where - k * pulse.beats);
        return b;
    }

    b.state                = BeatNow::State::Running;
    const double where     = t.beatPosition + moved;
    const double barBeats  = t.numerator * 4.0 / t.denominator;
    const double bars      = std::floor((where - t.barStartBeats) / barBeats + 1e-9);
    const double barStart  = t.barStartBeats + bars * barBeats;
    const int    k         = static_cast<int>(std::floor((where - barStart) / pulse.beats + 1e-9));
    b.pulse                = std::clamp(k, 0, b.pulsesInBar - 1);
    b.downbeat             = b.pulse == 0;
    b.sincePulse           = secondsOf(where - barStart - b.pulse * pulse.beats);
    b.bar                  = t.barNumber + static_cast<int>(bars) + 1;
    b.beat                 = b.pulse + 1;
    if (where < 0.0) {   // started, not heard yet: bar 1 to come
        b.pulse = 0, b.bar = 1, b.beat = 1, b.sincePulse = -1.0, b.downbeat = true;
    }
    return b;
}

} // namespace stagehand
