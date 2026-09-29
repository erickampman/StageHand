#include "Session/RackFile.h"

#include <nlohmann/json.hpp>

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
#include <utility>

#include "Core/Ump.h"

namespace stagehand {

namespace fs = std::filesystem;
using Json   = nlohmann::ordered_json;   // keeps fields in the order written

namespace {

constexpr int         kFormatVersion = 1;
constexpr const char* kFormatName    = "stagehand-rack";

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

bool validSlotId(const std::string& id) {
    if (id.empty()) return false;
    for (const char c : id)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')) return false;
    return true;
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

fs::path bundleDirectory(const std::string& bundlePath) {
    fs::path bundle = fs::path(expandHome(bundlePath)).lexically_normal();
    if (!bundle.has_filename()) bundle = bundle.parent_path();   // "X.rack/" -> "X.rack"
    return bundle;
}

bool readBytes(const fs::path& path, std::vector<std::byte>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    in.seekg(0, std::ios::beg);
    out.resize(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char*>(out.data()), size);
    return static_cast<bool>(in);
}

bool writeBytes(const fs::path& path, const std::vector<std::byte>& data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(out);
}

PluginFormat parseFormat(const std::string& text) {
    if (text == "clap") return PluginFormat::Clap;
    if (text == "vst3") return PluginFormat::Vst3;
    if (text == "auv2") return PluginFormat::Auv2;
    fail("unknown plugin format '" + text + "' (expected \"clap\")");
}

const char* formatName(PluginFormat format) {
    switch (format) {
        case PluginFormat::Clap: return "clap";
        case PluginFormat::Vst3: return "vst3";
        case PluginFormat::Auv2: return "auv2";
    }
    return "clap";
}

MidiSourceRef parseSource(const std::string& text) {
    MidiSourceRef ref;
    if (text == "all") return ref;
    if (text.rfind("device:", 0) == 0 && text.size() > 7) {
        ref.kind = MidiSourceRef::Kind::Device;
        ref.name = text.substr(7);
        return ref;
    }
    if (text.rfind("virtual:", 0) == 0 && text.size() > 8) {
        ref.kind = MidiSourceRef::Kind::Virtual;
        ref.name = text.substr(8);
        return ref;
    }
    if (text.rfind("sequence:", 0) == 0) fail("sequence sources aren't supported yet");
    fail("midi.source '" + text + "' should be \"all\", \"device:<id>\" or \"virtual:<name>\"");
}

std::string sourceText(const MidiSourceRef& ref) {
    switch (ref.kind) {
        case MidiSourceRef::Kind::All:     return "all";
        case MidiSourceRef::Kind::Device:  return "device:" + ref.name;
        case MidiSourceRef::Kind::Virtual: return "virtual:" + ref.name;
    }
    return "all";
}

// "all", or a list of numbers 1-16, as a bit mask.
std::uint16_t parseMask(const Json& value, const char* field) {
    if (value.is_string() && value.get<std::string>() == "all") return 0xFFFF;
    if (!value.is_array()) fail(std::string("midi.") + field + " should be \"all\" or a list of 1-16");
    std::uint16_t mask = 0;
    for (const Json& item : value) {
        const int n = item.get<int>();
        if (n < 1 || n > 16) fail(std::string("midi.") + field + " values must be 1-16");
        mask |= static_cast<std::uint16_t>(1u << (n - 1));
    }
    return mask;
}

Json maskJson(std::uint16_t mask) {
    if (mask == 0xFFFF) return "all";
    Json list = Json::array();
    for (int bit = 0; bit < 16; ++bit)
        if (mask & (1u << bit)) list.push_back(bit + 1);
    return list;
}

std::pair<int, int> parseRange(const Json& value, const char* field) {
    if (!value.is_array() || value.size() != 2) fail(std::string("midi.") + field + " should be [low, high]");
    const int low = value[0].get<int>(), high = value[1].get<int>();
    if (low < 0 || high > 127 || low > high) fail(std::string("midi.") + field + " must be within 0-127, low <= high");
    return {low, high};
}

RackSlot parseSlot(const Json& s, const fs::path& bundle, std::vector<std::string>& warnings) {
    RackSlot slot;
    slot.id = s.at("id").get<std::string>();
    if (!validSlotId(slot.id)) fail("slot id '" + slot.id + "' may use only letters, digits, '-' and '_'");
    slot.name = s.value("name", slot.id);

    const Json& plugin    = s.at("plugin");
    slot.plugin.format    = parseFormat(plugin.at("format").get<std::string>());
    slot.plugin.id        = plugin.at("id").get<std::string>();
    slot.plugin.path      = expandHome(plugin.value("path", std::string{}));

    if (s.contains("state")) {
        const fs::path statePath = bundle / s.at("state").get<std::string>();
        if (!readBytes(statePath, slot.state)) {
            slot.state.clear();
            warnings.push_back("slot '" + slot.id + "': state file " + statePath.string() +
                               " is missing; the plugin will start from its defaults");
        }
    }
    slot.enabled = s.value("enabled", true);
    slot.muted   = s.value("muted", false);
    slot.gainDb  = s.value("gainDb", 0.0f);
    const int output = s.value("output", 1);
    if (output < 1) fail("output must be 1 or more");
    slot.firstOutput = static_cast<std::uint32_t>(output - 1);

    if (s.contains("midi")) {
        const Json& midi = s.at("midi");
        slot.source = parseSource(midi.value("source", std::string("all")));
        if (midi.contains("channels")) slot.filter.channelMask = parseMask(midi.at("channels"), "channels");
        if (midi.contains("groups")) slot.filter.groupMask = parseMask(midi.at("groups"), "groups");
        if (midi.contains("notes")) {
            const auto [low, high] = parseRange(midi.at("notes"), "notes");
            slot.filter.noteLow  = static_cast<std::uint8_t>(low);
            slot.filter.noteHigh = static_cast<std::uint8_t>(high);
        }
        if (midi.contains("velocity")) {   // 0-127 in the file, compared at 16 bits
            const auto [low, high] = parseRange(midi.at("velocity"), "velocity");
            slot.filter.velocityLow  = static_cast<std::uint16_t>(ump::scaleUp(static_cast<std::uint32_t>(low), 7, 16));
            slot.filter.velocityHigh = static_cast<std::uint16_t>(ump::scaleUp(static_cast<std::uint32_t>(high), 7, 16));
        }
        const int transpose = midi.value("transpose", 0);
        if (transpose < -127 || transpose > 127) fail("midi.transpose must be within -127..127");
        slot.filter.transpose = static_cast<std::int8_t>(transpose);
        const int notePort = midi.value("notePort", 0);
        if (notePort < 0) fail("midi.notePort must be 0 or more");
        slot.notePort = static_cast<std::uint16_t>(notePort);
    }
    return slot;
}

Json slotJson(const RackSlot& slot, bool hasState) {
    Json s;
    s["id"]   = slot.id;
    s["name"] = slot.name;
    Json plugin;
    plugin["format"] = formatName(slot.plugin.format);
    plugin["id"]     = slot.plugin.id;
    if (!slot.plugin.path.empty()) plugin["path"] = abbreviateHome(slot.plugin.path);
    s["plugin"] = plugin;
    if (hasState) s["state"] = "state/" + slot.id + ".bin";
    s["enabled"] = slot.enabled;
    s["muted"]   = slot.muted;
    s["gainDb"]  = slot.gainDb;
    s["output"]  = slot.firstOutput + 1;

    Json midi;
    midi["source"]    = sourceText(slot.source);
    midi["channels"]  = maskJson(slot.filter.channelMask);
    midi["groups"]    = maskJson(slot.filter.groupMask);
    midi["notes"]     = Json::array({slot.filter.noteLow, slot.filter.noteHigh});
    midi["velocity"]  = Json::array({ump::scaleDown(slot.filter.velocityLow, 16, 7),
                                     ump::scaleDown(slot.filter.velocityHigh, 16, 7)});
    midi["transpose"] = static_cast<int>(slot.filter.transpose);
    midi["notePort"]  = slot.notePort;
    s["midi"]         = midi;
    return s;
}

} // namespace

RackFileResult loadRack(const std::string& bundlePath, Rack& rack) {
    RackFileResult result;
    const fs::path bundle   = bundleDirectory(bundlePath);
    const fs::path jsonPath = bundle / "rack.json";

    std::ifstream in(jsonPath);
    if (!in) {
        result.error = "cannot read " + jsonPath.string();
        return result;
    }
    Rack loaded;
    try {
        const Json doc = Json::parse(in);
        if (!doc.is_object() || doc.value("format", std::string{}) != kFormatName)
            fail("not a StageHand rack (expected \"format\": \"stagehand-rack\")");
        const int version = doc.at("version").get<int>();
        if (version > kFormatVersion)
            fail("made by a newer StageHand (format version " + std::to_string(version) +
                 "; this one reads up to " + std::to_string(kFormatVersion) + ")");
        if (version < 1) fail("invalid format version");

        std::string stem = bundle.filename().string();
        if (stem.size() > 5 && stem.compare(stem.size() - 5, 5, ".rack") == 0) stem.resize(stem.size() - 5);
        loaded.name     = doc.value("name", stem);
        loaded.tempoBpm = doc.value("tempo", 120.0);
        if (loaded.tempoBpm < 20.0 || loaded.tempoBpm > 400.0) fail("tempo must be within 20-400 BPM");
        if (doc.contains("timeSignature")) {
            const Json& sig = doc.at("timeSignature");
            if (!sig.is_array() || sig.size() != 2) fail("timeSignature must be [numerator, denominator]");
            const int n = sig[0].get<int>(), d = sig[1].get<int>();
            if (n < 1 || n > 32 || d < 1 || d > 32 || (d & (d - 1)) != 0)
                fail("timeSignature must be 1-32 over 1, 2, 4, 8, 16 or 32");
            loaded.timeSigNumerator   = n;
            loaded.timeSigDenominator = d;
        }

        std::set<std::string> ids;
        std::size_t           index = 0;
        for (const Json& s : doc.at("slots")) {
            ++index;
            try {
                RackSlot slot = parseSlot(s, bundle, result.warnings);
                if (!ids.insert(slot.id).second) fail("duplicate slot id '" + slot.id + "'");
                loaded.slots.push_back(std::move(slot));
            } catch (const std::exception& e) {
                fail("slot " + std::to_string(index) + ": " + e.what());
            }
        }
    } catch (const std::exception& e) {   // nlohmann's errors are std::exceptions too
        result.error = jsonPath.string() + ": " + e.what();
        return result;
    }
    rack      = std::move(loaded);
    result.ok = true;
    return result;
}

RackFileResult saveRack(const std::string& bundlePath, const Rack& rack) {
    RackFileResult result;
    const fs::path bundle = bundleDirectory(bundlePath);
    const fs::path temp   = bundle.string() + ".tmp";
    const fs::path old    = bundle.string() + ".old";
    std::error_code ec;
    try {
        fs::remove_all(temp);
        fs::create_directories(temp / "state");

        Json doc;
        doc["format"]  = kFormatName;
        doc["version"] = kFormatVersion;
        doc["name"]          = rack.name;
        doc["tempo"]         = rack.tempoBpm;
        doc["timeSignature"] = Json::array({rack.timeSigNumerator, rack.timeSigDenominator});
        Json slots     = Json::array();
        std::set<std::string> ids;
        for (const RackSlot& slot : rack.slots) {
            if (!validSlotId(slot.id) || !ids.insert(slot.id).second)
                fail("invalid or duplicate slot id '" + slot.id + "'");
            if (!slot.state.empty() && !writeBytes(temp / "state" / (slot.id + ".bin"), slot.state))
                fail("cannot write the state of slot '" + slot.id + "'");
            slots.push_back(slotJson(slot, !slot.state.empty()));
        }
        doc["slots"] = slots;
        {
            std::ofstream out(temp / "rack.json", std::ios::trunc);
            out << doc.dump(2) << '\n';
            if (!out) fail("cannot write " + (temp / "rack.json").string());
        }

        // Swap in: until the last rename, the old rack is untouched (or, briefly,
        // renamed to X.rack.old); a crash never leaves a half-written X.rack.
        fs::remove_all(old);
        if (fs::exists(bundle)) fs::rename(bundle, old);
        fs::rename(temp, bundle);
        fs::remove_all(old);
    } catch (const std::exception& e) {
        result.error = e.what();
        fs::remove_all(temp, ec);
        return result;
    }
    result.ok = true;
    return result;
}

} // namespace stagehand
