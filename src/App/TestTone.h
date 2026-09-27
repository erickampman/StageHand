#pragma once

#include <cmath>
#include <cstdint>
#include <numbers>

#include "Audio/AudioBackend.h"

namespace stagehand {

// M1 test fixture: the same sine on every output channel, to prove the audio
// path end to end before any plugin is involved. Replaced by the engine in M3.
class TestTone final : public AudioProcessor {
public:
    TestTone(double frequencyHz, float linearGain)
        : frequency(frequencyHz), gain(linearGain) {}

    void process(const AudioCallbackContext& ctx) noexcept override {
        if (ctx.time.sampleRate <= 0.0) return;
        const double increment = frequency / ctx.time.sampleRate;
        for (std::uint32_t f = 0; f < ctx.frameCount; ++f) {
            const float sample =
                gain * static_cast<float>(std::sin(2.0 * std::numbers::pi * phase));
            for (std::uint32_t c = 0; c < ctx.output.channelCount; ++c)
                ctx.output.channels[c][f] = sample;
            phase += increment;
            if (phase >= 1.0) phase -= 1.0;
        }
    }

private:
    double frequency;
    float  gain;
    double phase = 0.0;
};

} // namespace stagehand
