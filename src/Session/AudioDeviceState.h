#pragma once

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

// audio.json in appStateDir() (Session/AppPaths.h).
std::string defaultAudioDevicePath();
bool        loadAudioDeviceChoice(const std::string& path, AudioDeviceChoice& choice);   // false: none, or unreadable
bool        saveAudioDeviceChoice(const std::string& path, const AudioDeviceChoice& choice);

} // namespace stagehand
