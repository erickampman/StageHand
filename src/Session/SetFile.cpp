#include "Session/SetFile.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>

#include "Session/BundleFiles.h"
#include "Session/RigFile.h"
#include "Session/SetlistFile.h"
#include "Session/SetlistJson.h"

namespace stagehand {

namespace fs = std::filesystem;
using namespace bundle;

namespace {

constexpr int         kFormatVersion = 1;
constexpr const char* kFormatName    = "stagehand-set";
constexpr const char* kSetFile       = "setlist.json";

bool hasRack(const fs::path& package, const std::string& rack) {
    std::error_code ec;
    return fs::is_regular_file(fs::path(setRackPath(package.string(), rack)) / "rack.json", ec);
}

Json setJson(const Set& set) {
    Json doc;
    doc["format"]  = kFormatName;
    doc["version"] = kFormatVersion;
    Json songs     = Json::array();
    for (const SetSong& song : set.songs) {
        Json s;
        s["rack"] = song.rack;
        if (!song.name.empty()) s["name"] = song.name;
        songs.push_back(s);
    }
    doc["songs"] = songs;
    if (set.switching != SwitchSettings{}) doc["switching"] = switchingJson(set.switching);
    if (!set.triggers.empty()) {
        Json triggers = Json::array();
        for (const SetlistTrigger& t : set.triggers) triggers.push_back(triggerJson(t));
        doc["triggers"] = triggers;
    }
    return doc;
}

void throwIfFailed(const RackFileResult& result) {
    if (!result.ok) fail(result.error);
}

// The default rig as a converted set's own: a copy of what's there, or empty.
Rig rigFrom(const std::string& bundlePath, std::vector<std::string>& warnings) {
    Rig rig;
    std::error_code ec;
    if (bundlePath.empty() || !fs::exists(bundleDirectory(bundlePath), ec)) return rig;
    const RackFileResult result = loadRig(bundlePath, rig);
    if (!result.ok) {
        warnings.push_back("the rig " + bundlePath + " wasn't copied: " + result.error);
        return Rig{};
    }
    warnings.insert(warnings.end(), result.warnings.begin(), result.warnings.end());
    return rig;
}

} // namespace

std::string setRackPath(const std::string& packagePath, const std::string& rack) {
    return (bundleDirectory(packagePath) / "Racks" / (rack + ".rack")).string();
}

std::string setRigPath(const std::string& packagePath) {
    return (bundleDirectory(packagePath) / "rig").string();
}

bool isSetPackage(const std::string& path) {
    std::error_code ec;
    return fs::is_regular_file(fs::path(resolveRackPath(path)) / kSetFile, ec);
}

std::string setPathOf(const std::string& chosen) {
    return setlistPathOf(chosen);   // the same extension, now a package
}

std::string setNameOf(const std::string& packagePath) {
    return setlistNameOf(packagePath);
}

bool validRackName(const std::string& name) {
    return !name.empty() && name.front() != '.' && name.find('/') == std::string::npos;
}

std::string freeRackName(const std::string& base, const std::set<std::string>& taken) {
    if (taken.count(base) == 0) return base;
    for (int n = 2;; ++n) {
        std::string name = base + " " + std::to_string(n);
        if (taken.count(name) == 0) return name;
    }
}

RackFileResult loadSet(const std::string& packagePath, Set& set) {
    RackFileResult result;
    const fs::path package = resolveRackPath(packagePath);
    const fs::path file    = package / kSetFile;
    std::ifstream  in(file);
    if (!in) {
        result.error = "cannot read " + file.string();
        return result;
    }
    Set loaded;
    try {
        const Json doc = Json::parse(in);
        if (!doc.is_object() || doc.value("format", std::string{}) != kFormatName)
            fail("not a StageHand set (expected \"format\": \"stagehand-set\")");
        const int version = doc.at("version").get<int>();
        if (version > kFormatVersion)
            fail("made by a newer StageHand (format version " + std::to_string(version) +
                 "; this one reads up to " + std::to_string(kFormatVersion) + ")");
        if (version < 1) fail("invalid format version");

        if (doc.contains("songs")) {
            if (!doc.at("songs").is_array()) fail("\"songs\" must be a list");
            std::size_t number = 0;
            for (const Json& s : doc.at("songs")) {
                ++number;
                if (!s.is_object() || !s.contains("rack")) fail("each song needs a \"rack\"");
                SetSong song;
                song.rack = s.at("rack").get<std::string>();
                song.name = s.value("name", std::string{});
                if (!validRackName(song.rack)) fail("song " + std::to_string(number) + ": \"" + song.rack + "\" isn't a rack's name");
                if (!hasRack(package, song.rack)) {
                    result.warnings.push_back("song " + std::to_string(number) + ": no rack \"" + song.rack +
                                              "\" in the set; it's left out");
                    continue;
                }
                loaded.songs.push_back(std::move(song));
            }
        }
        if (doc.contains("switching")) loaded.switching = parseSwitching(doc.at("switching"));
        if (doc.contains("triggers")) {
            if (!doc.at("triggers").is_array()) fail("\"triggers\" must be a list");
            for (const Json& t : doc.at("triggers")) loaded.triggers.push_back(parseTrigger(t));
        }
    } catch (const std::exception& e) {
        result.error = file.string() + ": " + e.what();
        return result;
    }
    set       = std::move(loaded);
    result.ok = true;
    return result;
}

RackFileResult saveSet(const std::string& packagePath, const Set& set, const Rig* rig,
                       const std::map<std::string, Rack>& racks, const std::string& from) {
    RackFileResult result;
    const fs::path target = resolveRackPath(packagePath);
    const fs::path source = from.empty() ? target : fs::path(resolveRackPath(from));
    const bool     inPlace = source == target && isSetPackage(target.string());

    if (set.songs.empty()) {
        result.error = "a set needs at least one song";
        return result;
    }
    std::set<std::string> used;
    for (const SetSong& song : set.songs) {
        if (!validRackName(song.rack)) {
            result.error = "\"" + song.rack + "\" isn't a rack's name";
            return result;
        }
        used.insert(song.rack);
    }
    for (const std::string& rack : used)
        if (racks.count(rack) == 0 && !hasRack(source, rack)) {
            result.error = "no rack \"" + rack + "\" to save in " + target.string();
            return result;
        }

    const fs::path  temp = target.string() + ".tmp";
    std::error_code ec;
    try {
        if (inPlace) {
            for (const std::string& rack : used)
                if (const auto it = racks.find(rack); it != racks.end())
                    throwIfFailed(saveRack(setRackPath(target.string(), rack), it->second));
            if (rig != nullptr || !fs::exists(setRigPath(target.string()), ec))
                throwIfFailed(saveRig(setRigPath(target.string()), rig != nullptr ? *rig : Rig{}));
            if (!writeTextAtomically(target / kSetFile, setJson(set).dump(2) + "\n", result.error)) return result;
            for (const fs::directory_entry& entry : fs::directory_iterator(target / "Racks"))   // racks no song names
                if (entry.path().extension() == ".rack" && used.count(entry.path().stem().string()) == 0)
                    fs::remove_all(entry.path());
        } else {
            if (fs::exists(target) && !isSetPackage(target.string()))
                fail(target.string() + " exists and isn't a set; it's never replaced");
            fs::remove_all(temp);
            fs::create_directories(temp / "Racks");
            for (const std::string& rack : used) {
                if (const auto it = racks.find(rack); it != racks.end())
                    throwIfFailed(saveRack(setRackPath(temp.string(), rack), it->second));
                else
                    fs::copy(setRackPath(source.string(), rack), setRackPath(temp.string(), rack),
                             fs::copy_options::recursive);
            }
            if (rig != nullptr)
                throwIfFailed(saveRig(setRigPath(temp.string()), *rig));
            else if (fs::exists(setRigPath(source.string()), ec))
                fs::copy(setRigPath(source.string()), setRigPath(temp.string()), fs::copy_options::recursive);
            else
                throwIfFailed(saveRig(setRigPath(temp.string()), Rig{}));
            if (!writeTextAtomically(temp / kSetFile, setJson(set).dump(2) + "\n", result.error)) {
                fs::remove_all(temp, ec);
                return result;
            }
            swapInBundle(temp, target);
        }
    } catch (const std::exception& e) {
        if (!inPlace) fs::remove_all(temp, ec);
        result.error = "couldn't save " + target.string() + ": " + e.what();
        return result;
    }
    result.ok = true;
    return result;
}

RackFileResult convertSetlist(const std::string& setlistFile, LoadedSet& out) {
    Setlist        old;
    RackFileResult result = loadSetlist(setlistFile, old);
    if (!result.ok) return result;
    result.ok = false;
    result.warnings.clear();   // missing racks are reported below, as left out

    LoadedSet                          loaded;
    std::map<std::string, std::string> nameOf;   // bundle -> its rack's name here (a reprise: one rack)
    std::set<std::string>              taken;
    for (std::size_t i = 0; i < old.songs.size(); ++i) {
        const SetlistSong& song = old.songs[i];
        auto               it   = nameOf.find(song.rack);
        if (it == nameOf.end()) {
            Rack                 rack;
            const RackFileResult read = loadRack(song.rack, rack);
            if (!read.ok) {
                result.warnings.push_back("song " + std::to_string(i + 1) + " is left out: " + read.error);
                continue;
            }
            result.warnings.insert(result.warnings.end(), read.warnings.begin(), read.warnings.end());
            const std::string own  = rackNameOf(song.rack);
            const std::string name = freeRackName(validRackName(own) ? own : "Song", taken);
            rack.name              = name;
            taken.insert(name);
            loaded.racks.emplace(name, std::move(rack));
            it = nameOf.emplace(song.rack, name).first;
        }
        SetSong added{it->second, song.name};
        if (added.name.empty() && added.rack != rackNameOf(song.rack)) added.name = rackNameOf(song.rack);   // numbered: shown as before
        loaded.set.songs.push_back(std::move(added));
    }
    if (loaded.set.songs.empty()) {
        result.error = "none of the racks in " + setlistFile + " could be read";
        return result;
    }
    loaded.set.switching = old.switching;
    loaded.set.triggers  = old.triggers;
    loaded.rig           = rigFrom(old.rig.empty() ? defaultRigPath() : old.rig, result.warnings);
    out                  = std::move(loaded);
    result.ok            = true;
    return result;
}

RackFileResult setFromRack(const std::string& rackBundle, LoadedSet& out) {
    LoadedSet      loaded;
    Rack           rack;
    RackFileResult result = loadRack(rackBundle, rack);
    if (!result.ok) return result;
    const std::string own  = rackNameOf(rackBundle);
    const std::string name = validRackName(own) ? own : "Song";
    rack.name              = name;
    loaded.racks.emplace(name, std::move(rack));
    loaded.set.songs.push_back(SetSong{name, ""});
    loaded.rig = rigFrom(defaultRigPath(), result.warnings);
    out        = std::move(loaded);
    return result;
}

} // namespace stagehand
