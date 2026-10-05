#include "Session/AudioDeviceState.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>

#include "Session/AppPaths.h"
#include "Session/BundleFiles.h"

namespace stagehand {

namespace fs = std::filesystem;
using Json   = nlohmann::ordered_json;

std::string defaultAudioDevicePath() {
    return (fs::path(appStateDir()) / "audio.json").string();
}

bool loadAudioDeviceChoice(const std::string& path, AudioDeviceChoice& choice) {
    std::ifstream in(path);
    if (!in) return false;
    try {
        const Json        doc = Json::parse(in);
        AudioDeviceChoice c;
        c.name = doc.value("device", std::string{});
        c.uid  = doc.value("uid", std::string{});
        if (c.empty()) return false;
        choice = c;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool saveAudioDeviceChoice(const std::string& path, const AudioDeviceChoice& choice) {
    Json doc;
    doc["device"] = choice.name;
    if (!choice.uid.empty()) doc["uid"] = choice.uid;
    std::string error;
    return bundle::writeTextAtomically(path, doc.dump(2) + "\n", error);
}

} // namespace stagehand
