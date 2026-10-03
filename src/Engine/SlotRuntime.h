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

// A share of the audio period that something took to compute (docs/panic.md,
// DSP load): 1 = all of it. The worst block since the main thread last took
// it; approximate in the same way as LevelMeter.
class LoadMeter {
public:
    void  add(float share) noexcept {   // audio thread
        if (share > peak.load(std::memory_order_relaxed)) peak.store(share, std::memory_order_relaxed);
    }
    float take() noexcept { return peak.exchange(0.0f, std::memory_order_relaxed); }   // main thread

private:
    std::atomic<float> peak{0.0f};
};

// A level the audio thread ramps to whenever its target changes, over
// Engine::kGainRampSeconds, so a change never clicks. Audio thread only.
struct GainRamp {
    explicit GainRamp(float initial = 1.0f) noexcept : current(initial), target(initial) {}
    float         current;
    float         target;             // the target the running ramp heads for
    float         step      = 0.0f;
    std::uint32_t remaining = 0;      // frames left in the ramp; 0 = at target
    bool          settledAt(float level) const noexcept { return remaining == 0 && current == level; }
};

// The rig's effect buses (docs/rig.md): every row can send to each.
inline constexpr std::size_t kBusCount = 2;

// The part of a slot that outlives any one EngineGraph (docs/rack-view.md,
// section 2). The Session creates one per slot and puts the same one in every
// graph it builds for that slot. So live gain, the gain ramp's position, the
// meter and the MIDI activity count carry across commits, and are tied to the
// slot rather than to its index in a graph. Like the plugin, it's held by
// shared_ptr and released only on the main thread.
struct SlotRuntime {
    explicit SlotRuntime(float initialGain = 1.0f, bool initiallyMuted = false) noexcept
        : targetGain(initialGain), muted(initiallyMuted), gain(initialGain) {}

    // Main thread writes, audio thread reads. Linear; 0 when muted. The audio
    // thread ramps to each new value over Engine::kGainRampSeconds.
    std::atomic<float> targetGain;
    // Main thread writes, audio thread reads (docs/rack-view.md, "Mute"). Muted:
    // the plugin's held notes are released and no new note-ons reach it; once
    // its output has been quiet for Engine::kQuietSeconds (or it has been
    // muted for Engine::kMaxMutedSeconds) it is suspended: not processed, but
    // for blocks that bring it events (controllers, note-offs). Unmuting
    // resumes it at the next block. Set targetGain to 0 with it.
    std::atomic<bool>  muted;
    // Main thread writes, audio thread reads: how much of the slot's output
    // (after its gain and mute) goes to each effect bus. Linear; 0 = off.
    std::atomic<float> targetSend[kBusCount] = {};

    LevelMeter meter;                            // post-gain; audio writes, main takes
    std::atomic<std::uint32_t> midiEvents{0};    // events delivered; audio adds, main compares
    LoadMeter  load;                             // its plugin's (a bus's: its effects') share of each period

    // --- audio thread only: the ramps ---
    GainRamp gain;
    GainRamp send[kBusCount] = {GainRamp(0.0f), GainRamp(0.0f)};
    // Control links' levels (docs/controls.md), in dB, after the gain (a bus's:
    // its return) and after each send level. 0 dB until a link moves them.
    double linkGainDb              = 0.0;
    double linkSendDb[kBusCount]   = {};

    // Audio thread writes, main thread reads: a draining slot (SlotConfig::draining)
    // has finished ringing out; the next graph can leave it out.
    std::atomic<bool> drained{false};

    // --- audio thread only: muting and draining ---
    bool          muteApplied = false;   // its notes released for this mute
    bool          suspended   = false;
    std::uint64_t quietFrames = 0;       // muted at gain 0, or draining: frames of quiet output in a row
    std::uint64_t mutedFrames = 0;       // muted and at gain 0, in all
    std::uint64_t drainFrames = 0;       // draining, in all
};

} // namespace stagehand
