#include "Session/SetlistFile.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace stagehand {

namespace fs = std::filesystem;
using Json   = nlohmann::ordered_json;

namespace {

constexpr int         kFormatVersion = 1;
constexpr const char* kFormatName    = "stagehand-setlist";

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

std::string homeDir() {
    const char* home = std::getenv("HOME");
    return home != nullptr ? home : "";
}

std::string expandHome(const std::string& path) {
    if (path == "~" || path.rfind("~/", 0) == 0) return homeDir() + path.substr(1);
    return path;
}

std::string abbreviateHome(const std::string& path) {
    const std::string home = homeDir();
    if (!home.empty() && path.rfind(home + "/", 0) == 0) return "~" + path.substr(home.size());
    return path;
}

// How a rack is written: relative to the setlist's folder if they share more
// than "/" (e.g. both under the home folder), else absolute.
std::string rackPathText(const fs::path& rack, const fs::path& folder) {
    auto r = rack.begin(), f = folder.begin();
    int  shared = 0;
    for (; r != rack.end() && f != folder.end() && *r == *f; ++r, ++f) ++shared;
    if (shared > 1) {   // more than the root
        const fs::path relative = rack.lexically_relative(folder);
        if (!relative.empty()) return relative.generic_string();
    }
    return abbreviateHome(rack.string());
}

constexpr std::pair<const char*, SwitchSettings::Transport> kTransports[] = {
    {"keep", SwitchSettings::Transport::Keep}, {"stop", SwitchSettings::Transport::Stop}};
constexpr std::pair<const char*, SwitchSettings::When> kWhens[] = {
    {"now", SwitchSettings::When::Now}, {"nextBar", SwitchSettings::When::NextBar}};
constexpr std::pair<const char*, SwitchSettings::OldRack> kOldRacks[] = {
    {"ringOut", SwitchSettings::OldRack::RingOut}, {"cut", SwitchSettings::OldRack::Cut}};

template <typename Enum, std::size_t N>
Enum choice(const Json& j, const char* field, const std::pair<const char*, Enum> (&values)[N]) {
    if (!j.contains(field)) return values[0].second;   // the first is the default
    const std::string text = j.at(field).get<std::string>();
    for (const auto& [name, value] : values)
        if (text == name) return value;
    fail(std::string("unknown \"") + field + "\": \"" + text + "\"");
}

template <typename Enum, std::size_t N>
const char* nameOf(const std::pair<const char*, Enum> (&values)[N], Enum value) {
    for (const auto& [name, v] : values)
        if (v == value) return name;
    return values[0].first;
}

bool writeAtomically(const fs::path& path, const std::string& text, std::string& error) {
    const fs::path  temp = path.string() + ".tmp";
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    {
        std::ofstream out(temp, std::ios::trunc);
        out << text;
        if (!out) {
            error = "cannot write " + temp.string();
            return false;
        }
    }
    fs::rename(temp, path, ec);
    if (ec) {
        error = "cannot replace " + path.string() + ": " + ec.message();
        fs::remove(temp, ec);
        return false;
    }
    return true;
}

} // namespace

RackFileResult loadSetlist(const std::string& path, Setlist& setlist) {
    RackFileResult  result;
    const fs::path  file   = resolveRackPath(path);
    const fs::path  folder = file.parent_path();
    std::ifstream   in(file);
    if (!in) {
        result.error = "cannot read " + file.string();
        return result;
    }
    Setlist loaded;
    try {
        const Json doc = Json::parse(in);
        if (!doc.is_object() || doc.value("format", std::string{}) != kFormatName)
            fail("not a StageHand setlist (expected \"format\": \"stagehand-setlist\")");
        const int version = doc.at("version").get<int>();
        if (version > kFormatVersion)
            fail("made by a newer StageHand (format version " + std::to_string(version) +
                 "; this one reads up to " + std::to_string(kFormatVersion) + ")");
        if (version < 1) fail("invalid format version");

        if (doc.contains("songs")) {
            if (!doc.at("songs").is_array()) fail("\"songs\" must be a list");
            for (const Json& s : doc.at("songs")) {
                if (!s.is_object() || !s.contains("rack")) fail("each song needs a \"rack\"");
                SetlistSong song;
                const fs::path rack = expandHome(s.at("rack").get<std::string>());
                song.rack           = (rack.is_absolute() ? rack : folder / rack).lexically_normal().string();
                if (!song.rack.empty() && song.rack.back() == '/') song.rack.pop_back();
                song.name = s.value("name", std::string{});
                if (rackBundleOf(song.rack).empty())
                    result.warnings.push_back("song " + std::to_string(loaded.songs.size() + 1) + ": no rack at " +
                                              song.rack + "; it's kept, and skipped until it's back");
                loaded.songs.push_back(std::move(song));
            }
        }
        if (doc.contains("switching")) {
            const Json& sw = doc.at("switching");
            if (!sw.is_object()) fail("\"switching\" must be an object");
            loaded.switching.transport = choice(sw, "transport", kTransports);
            loaded.switching.when      = choice(sw, "at", kWhens);
            loaded.switching.oldRack   = choice(sw, "oldRack", kOldRacks);
        }
    } catch (const std::exception& e) {
        result.error = file.string() + ": " + e.what();
        return result;
    }
    setlist   = std::move(loaded);
    result.ok = true;
    return result;
}

RackFileResult saveSetlist(const std::string& path, const Setlist& setlist) {
    RackFileResult result;
    const fs::path file   = resolveRackPath(path);
    const fs::path folder = file.parent_path();

    Json doc;
    doc["format"]  = kFormatName;
    doc["version"] = kFormatVersion;
    Json songs     = Json::array();
    for (const SetlistSong& song : setlist.songs) {
        Json s;
        s["rack"] = rackPathText(fs::path(song.rack).lexically_normal(), folder);
        if (!song.name.empty()) s["name"] = song.name;
        songs.push_back(s);
    }
    doc["songs"] = songs;
    if (setlist.switching != SwitchSettings{}) {
        Json sw;
        sw["transport"] = nameOf(kTransports, setlist.switching.transport);
        sw["at"]        = nameOf(kWhens, setlist.switching.when);
        sw["oldRack"]   = nameOf(kOldRacks, setlist.switching.oldRack);
        doc["switching"] = sw;
    }
    if (!writeAtomically(file, doc.dump(2) + "\n", result.error)) return result;
    result.ok = true;
    return result;
}

std::string setlistPathOf(const std::string& chosen) {
    if (chosen.empty()) return {};
    fs::path path = resolveRackPath(chosen);
    if (path.extension() != ".setlist") path += ".setlist";
    return path.string();
}

std::string setlistNameOf(const std::string& path) {
    return fs::path(resolveRackPath(path)).stem().string();
}

bool isSetlistFile(const std::string& path) {
    std::error_code ec;
    const fs::path  file = resolveRackPath(path);
    return file.extension() == ".setlist" && fs::is_regular_file(file, ec);
}

// --- last position -------------------------------------------------------------------

std::string defaultLastPositionPath() {
    const char* state = std::getenv("XDG_STATE_HOME");
    const fs::path base = state != nullptr && *state != '\0' ? fs::path(state) : fs::path(homeDir()) / ".local/state";
    return (base / "stagehand" / "last.json").string();
}

bool loadLastPosition(const std::string& path, LastPosition& position) {
    std::ifstream in(path);
    if (!in) return false;
    try {
        const Json doc = Json::parse(in);
        LastPosition p;
        p.setlist = doc.value("setlist", std::string{});
        p.song    = doc.value("song", std::size_t{0});
        position  = p;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool saveLastPosition(const std::string& path, const LastPosition& position) {
    Json doc;
    doc["setlist"] = position.setlist;
    doc["song"]    = position.song;
    std::string error;
    return writeAtomically(path, doc.dump(2) + "\n", error);
}

} // namespace stagehand
