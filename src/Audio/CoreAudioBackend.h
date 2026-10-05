#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "Audio/AudioBackend.h"

typedef unsigned int AudioObjectID;   // CoreAudio's (UInt32), so callers needn't include it

namespace stagehand {

// An output device CoreAudio offers, for --list-audio and --device.
struct AudioDeviceInfo {
    AudioObjectID id = 0;
    std::string   name;            // "MacBook Pro Speakers"
    std::string   uid;             // persistent across reboots: "BuiltInSpeakerDevice"
    std::uint32_t outputChannels = 0;
    double        sampleRate     = 0.0;   // nominal, now
    bool          isDefault      = false;   // the system's default output, now
};

// AudioBackend over a CoreAudio HAL IOProc on one output device
// (docs/macos.md, "CoreAudio backend"). Outputs only (decision 2).
//
// The device is chosen at construction (decision 12): a name, a UID, or a
// unique part of a name; "" for the system's default output as it is now,
// not followed afterwards. The device keeps its own sample rate unless one is
// requested; a requested buffer size is set as this process's IO buffer size
// (kAudioDevicePropertyBufferFrameSize: other apps keep theirs).
//
// A block's time is the IOProc's "now" (the period's start), not the output's
// time, which is later by the buffer and the device's latency (decision 4).
//
// The engine renders into planar buffers of ours; the IOProc interleaves them
// into the device's streams (Float32, the HAL's virtual format). The main
// outputs are the device's first channels, across its streams in order. Aux
// outputs (the click) are rendered and not yet sent anywhere: their routing
// (a pair of this device, or another device through an aggregate) is a later
// step of milestone 4.
class CoreAudioBackend final : public AudioBackend {
public:
    explicit CoreAudioBackend(std::string deviceChoice = {});
    ~CoreAudioBackend() override;

    CoreAudioBackend(const CoreAudioBackend&)            = delete;
    CoreAudioBackend& operator=(const CoreAudioBackend&) = delete;

    // Every device with outputs, in CoreAudio's order.
    static std::vector<AudioDeviceInfo> outputDevices();
    // The device `choice` names (see the class comment), or false with
    // `problem` saying why (none, or several match).
    static bool findDevice(const std::string& choice, AudioDeviceInfo& found, std::string& problem);

    bool        open(const AudioConfig& requested, AudioConfig& negotiated) override;
    void        setProcessor(AudioProcessor* audioProcessor) override;
    bool        start() override;
    void        stop() override;
    void        close() override;
    AudioConfig config() const override;
    double      outputLatencySeconds() const override;
    const char* name() const override { return "CoreAudio"; }

    // The device opened (valid after open()).
    const AudioDeviceInfo& device() const noexcept { return opened; }

    // --- diagnostics (any thread) ---
    std::uint64_t xrunCount() const noexcept { return overloads.load(std::memory_order_relaxed); }
    // The device went away (unplugged) or stopped being usable.
    bool          deviceLost() const noexcept { return lost.load(std::memory_order_relaxed); }
    // Its nominal sample rate changed while open (another app, Audio MIDI
    // Setup): config() reports the new rate. Handling it is milestone 4's.
    bool          sampleRateChanged() const noexcept { return rateChanged.load(std::memory_order_relaxed); }
    std::int64_t  framesProcessed() const noexcept { return frameCounter.load(std::memory_order_relaxed); }

    // Called from CoreAudio's threads (the IOProc, the property listener), by
    // functions in the .cpp. Not for other callers.
    void render(const void* now, void* outputBufferList) noexcept;   // AudioTimeStamp, AudioBufferList
    void onPropertyChanged(std::uint32_t selector) noexcept;

private:
    void addListeners();
    void removeListeners();

    std::string         choice;
    AudioDeviceInfo     opened;
    void*               procId    = nullptr;   // AudioDeviceIOProcID
    AudioProcessor*     processor = nullptr;
    AudioConfig         negotiatedConfig;
    std::uint32_t       restoreBufferSize = 0;   // this process's buffer before open() set one
    bool                listening = false;
    bool                running   = false;

    // Planar buffers the engine renders into, sized at open() for the largest
    // buffer the device allows, so the IOProc never allocates.
    std::uint32_t       capacityFrames = 0;
    std::vector<float>  planar;           // (outputs + aux) * capacityFrames
    std::vector<float*> outputPointers;
    std::vector<float*> auxPointers;

    std::atomic<std::uint32_t> currentBufferSize{0};
    std::atomic<double>        currentSampleRate{0.0};
    std::atomic<std::int64_t>  frameCounter{0};   // written only by the IOProc
    std::atomic<std::uint64_t> overloads{0};
    std::atomic<bool>          lost{false};
    std::atomic<bool>          rateChanged{false};
};

} // namespace stagehand
