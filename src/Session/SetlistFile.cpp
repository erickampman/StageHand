#include "Session/SetlistFile.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include "Session/AppPaths.h"
#include "Session/BundleFiles.h"

namespace stagehand {

namespace fs = std::filesystem;
using Json   = nlohmann::ordered_json;
using namespace bundle;

namespace {

constexpr int         kFormatVersion = 1;
constexpr const char* kFormatName    = "stagehand-setlist";

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

constexpr std::pair<const char*, SetlistTrigger::Action> kTriggerActions[] = {
    {"next", SetlistTrigger::Action::Next}, {"previous", SetlistTrigger::Action::Previous},
    {"song", SetlistTrigger::Action::Song}};

int numberIn(const Json& j, const char* field, int fallback, int low, int high) {
    const int value = j.value(field, fallback);
    if (value < low || value > high)
        fail(std::string(field) + " must be within " + std::to_string(low) + "-" + std::to_string(high));
    return value;
}

template <typename Enum, std::size_t N>
const char* nameOf(const std::pair<const char*, Enum> (&values)[N], Enum value) {
    for (const auto& [name, v] : values)
        if (v == value) return name;
    return values[0].first;
}

// {"action": "next", "source": "device:<id>", "controller": 80, "channel": 1,
// "group": 1}, or "note" instead of "controller"; "song" (a Program Change)
// has neither.
SetlistTrigger parseTrigger(const Json& j) {
    if (!j.is_object()) fail("each trigger is an object");
    SetlistTrigger t;
    t.action = choice(j, "action", kTriggerActions);
    t.source = parseSource(j.value("source", std::string("all")));
    if (t.source.kind == MidiSourceRef::Kind::Sequence || t.source.kind == MidiSourceRef::Kind::None)
        fail("a trigger's source must be an input");
    if (t.action != SetlistTrigger::Action::Song) {
        if (j.contains("note") == j.contains("controller")) fail("a Next or Previous trigger has a \"note\" or a \"controller\"");
        t.control = j.contains("note") ? SequenceTrigger::Control::Note : SequenceTrigger::Control::Controller;
        t.number  = static_cast<std::uint8_t>(numberIn(j, j.contains("note") ? "note" : "controller", 0, 0, 127));
    }
    t.channel = static_cast<std::uint8_t>(numberIn(j, "channel", 1, 1, 16) - 1);
    t.group   = static_cast<std::uint8_t>(numberIn(j, "group", 1, 1, 16) - 1);
    return t;
}

Json triggerJson(const SetlistTrigger& t) {
    Json j;
    j["action"] = nameOf(kTriggerActions, t.action);
    j["source"] = sourceText(t.source);
    if (t.action != SetlistTrigger::Action::Song)
        j[t.control == SequenceTrigger::Control::Note ? "note" : "controller"] = t.number;
    j["channel"] = t.channel + 1;
    j["group"]   = t.group + 1;
    return j;
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
        if (doc.contains("rig")) {   // relative to the setlist, like its racks
            const fs::path rig = expandHome(doc.at("rig").get<std::string>());
            loaded.rig         = (rig.is_absolute() ? rig : folder / rig).lexically_normal().string();
            if (!loaded.rig.empty() && loaded.rig.back() == '/') loaded.rig.pop_back();
        }
        if (doc.contains("switching")) {
            const Json& sw = doc.at("switching");
            if (!sw.is_object()) fail("\"switching\" must be an object");
            loaded.switching.transport = choice(sw, "transport", kTransports);
            loaded.switching.when      = choice(sw, "at", kWhens);
            loaded.switching.oldRack   = choice(sw, "oldRack", kOldRacks);
        }
        if (doc.contains("triggers")) {
            if (!doc.at("triggers").is_array()) fail("\"triggers\" must be a list");
            for (const Json& t : doc.at("triggers")) loaded.triggers.push_back(parseTrigger(t));
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
    if (!setlist.rig.empty()) doc["rig"] = rackPathText(fs::path(setlist.rig).lexically_normal(), folder);
    if (setlist.switching != SwitchSettings{}) {
        Json sw;
        sw["transport"] = nameOf(kTransports, setlist.switching.transport);
        sw["at"]        = nameOf(kWhens, setlist.switching.when);
        sw["oldRack"]   = nameOf(kOldRacks, setlist.switching.oldRack);
        doc["switching"] = sw;
    }
    if (!setlist.triggers.empty()) {
        Json triggers = Json::array();
        for (const SetlistTrigger& t : setlist.triggers) triggers.push_back(triggerJson(t));
        doc["triggers"] = triggers;
    }
    if (!writeTextAtomically(file, doc.dump(2) + "\n", result.error)) return result;
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
    return (fs::path(appStateDir()) / "last.json").string();
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
    return writeTextAtomically(path, doc.dump(2) + "\n", error);
}

} // namespace stagehand
