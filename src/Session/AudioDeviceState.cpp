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

namespace {

// The file as it is, so saving one part keeps the others; an object either way.
Json readDoc(const std::string& path) {
    std::ifstream in(path);
    if (!in) return Json::object();
    try {
        Json doc = Json::parse(in);
        return doc.is_object() ? doc : Json::object();
    } catch (const std::exception&) {
        return Json::object();
    }
}

bool writeDoc(const std::string& path, const Json& doc) {
    std::string error;
    return bundle::writeTextAtomically(path, doc.dump(2) + "\n", error);
}

} // namespace

bool saveAudioDeviceChoice(const std::string& path, const AudioDeviceChoice& choice) {
    Json doc      = readDoc(path);
    doc["device"] = choice.name;
    if (!choice.uid.empty()) doc["uid"] = choice.uid;
    else doc.erase("uid");
    return writeDoc(path, doc);
}

AudioSettings loadAudioSettings(const std::string& path) {
    const Json    doc = readDoc(path);
    AudioSettings settings;
    if (const auto it = doc.find("sampleRate"); it != doc.end() && it->is_number() && it->get<double>() > 0.0)
        settings.sampleRate = it->get<double>();
    if (const auto it = doc.find("bufferSize"); it != doc.end() && it->is_number_unsigned())
        settings.bufferSize = it->get<std::uint32_t>();
    return settings;
}

bool saveAudioSettings(const std::string& path, const AudioSettings& settings) {
    Json doc = readDoc(path);
    if (settings.sampleRate > 0.0) doc["sampleRate"] = settings.sampleRate;
    else doc.erase("sampleRate");
    if (settings.bufferSize != 0) doc["bufferSize"] = settings.bufferSize;
    else doc.erase("bufferSize");
    return writeDoc(path, doc);
}

} // namespace stagehand
