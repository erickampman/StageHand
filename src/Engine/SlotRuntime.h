#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace stagehand {

// Peak levels written by the audio thread and taken by the main thread (a level
// meter). Two channels: a stereo pair, or a mono signal shown on both. Channels
// past the second are folded into the second. Ballistics (decay, peak hold)
// belong to the display, not here.
class LevelMeter {
public:
    static constexpr std::size_t kChannels = 2;

    struct Reading {
        float peak[kChannels] = {};   // absolute sample peak since the last take()
        bool  clipped         = false;   // latched until clearClip()
    };

    // --- audio thread ---
    void add(std::size_t channel, float peak) noexcept {
        std::atomic<float>& p = peaks[std::min(channel, kChannels - 1)];
        if (peak > p.load(std::memory_order_relaxed)) p.store(peak, std::memory_order_relaxed);
        if (peak > 1.0f) clipped.store(true, std::memory_order_relaxed);
    }

    // --- main thread ---
    Reading take() noexcept {
        Reading r;
        for (std::size_t c = 0; c < kChannels; ++c) r.peak[c] = peaks[c].exchange(0.0f, std::memory_order_relaxed);
        r.clipped = clipped.load(std::memory_order_relaxed);
        return r;
    }
    void clearClip() noexcept { clipped.store(false, std::memory_order_relaxed); }

private:
    // add() is a load-then-store, not a compare-exchange: a take() in between
    // can lose one block's peak. Approximate by design, like any meter.
    std::atomic<float> peaks[kChannels]{};
    std::atomic<bool>  clipped{false};
};

// The part of a slot that outlives any one EngineGraph (docs/rack-view.md,
// section 2). The Session creates one per slot and puts the same one in every
// graph it builds for that slot. So live gain, the gain ramp's position, the
// meter and the MIDI activity count carry across commits, and are tied to the
// slot rather than to its index in a graph. Like the plugin, it's held by
// shared_ptr and released only on the main thread.
struct SlotRuntime {
    explicit SlotRuntime(float initialGain = 1.0f) noexcept
        : targetGain(initialGain), currentGain(initialGain), rampTarget(initialGain) {}

    // Main thread writes, audio thread reads. Linear; 0 when muted. The audio
    // thread ramps to each new value over Engine::kGainRampSeconds.
    std::atomic<float> targetGain;

    LevelMeter meter;                            // post-gain; audio writes, main takes
    std::atomic<std::uint32_t> midiEvents{0};    // events delivered; audio adds, main compares

    // --- audio thread only: the ramp ---
    float         currentGain;
    float         rampTarget;          // the target the running ramp heads for
    float         rampStep      = 0.0f;
    std::uint32_t rampRemaining = 0;   // frames left in the ramp; 0 = at rampTarget
};

} // namespace stagehand
