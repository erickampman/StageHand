#pragma once

#include <cstdint>
#include <string>

namespace stagehand {

// The output device chosen on this machine (docs/macos.md, decision 12): kept
// per machine, not in the rig, since a rig or setlist copied to another Mac (or
// to Linux) mustn't carry this machine's interface. macOS's; Linux has no
// counterpart (PipeWire routes). `uid` finds it first (CoreAudio's persistent
// id), `name` if that fails, and for messages.
struct AudioDeviceChoice {
    std::string name;
    std::string uid;

    bool empty() const noexcept { return name.empty() && uid.empty(); }
    bool operator==(const AudioDeviceChoice&) const = default;
};

// The sample rate and buffer size chosen in the status bar (R2-20), kept per
// machine for the same reason, app-wide (not per set). 0: not chosen (the
// device's own rate; the device's buffer, or --quantum's).
struct AudioSettings {
    double        sampleRate = 0.0;
    std::uint32_t bufferSize = 0;

    bool operator==(const AudioSettings&) const = default;
};

// audio.json in appStateDir() (Session/AppPaths.h), holding both. Saving one
// keeps the other.
std::string defaultAudioDevicePath();
bool        loadAudioDeviceChoice(const std::string& path, AudioDeviceChoice& choice);   // false: none, or unreadable
bool        saveAudioDeviceChoice(const std::string& path, const AudioDeviceChoice& choice);
AudioSettings loadAudioSettings(const std::string& path);   // all 0 if none, or unreadable
bool          saveAudioSettings(const std::string& path, const AudioSettings& settings);

} // namespace stagehand
