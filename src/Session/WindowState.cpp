#include "Session/WindowState.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "Session/BundleFiles.h"

namespace stagehand {

namespace fs = std::filesystem;
using Json   = nlohmann::ordered_json;
using namespace bundle;

namespace {

constexpr double kMonitorShare = 0.92;   // room for the desktop's panels and the title bar

} // namespace

std::string defaultWindowStatePath() {
    const char*    state = std::getenv("XDG_STATE_HOME");
    const fs::path base  = state != nullptr && *state != '\0' ? fs::path(state) : fs::path(homeDir()) / ".local/state";
    return (base / "stagehand" / "window.json").string();
}

bool loadWindowSize(const std::string& path, WindowSize& size) {
    std::ifstream in(path);
    if (!in) return false;
    try {
        const Json doc = Json::parse(in);
        WindowSize s;
        s.width  = doc.at("width").get<double>();
        s.height = doc.at("height").get<double>();
        if (!(s.width > 0.0) || !(s.height > 0.0)) return false;
        size = s;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool saveWindowSize(const std::string& path, const WindowSize& size) {
    Json doc;
    doc["width"]  = std::lround(size.width);
    doc["height"] = std::lround(size.height);
    std::string error;
    return writeTextAtomically(path, doc.dump(2) + "\n", error);
}

WindowSize fitWindowSize(WindowSize wanted, WindowSize minimum, WindowSize monitor) {
    if (monitor.width > 0.0) wanted.width = std::min(wanted.width, monitor.width * kMonitorShare);
    if (monitor.height > 0.0) wanted.height = std::min(wanted.height, monitor.height * kMonitorShare);
    return WindowSize{std::max(wanted.width, minimum.width), std::max(wanted.height, minimum.height)};
}

} // namespace stagehand
