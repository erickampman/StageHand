#include "Session/RigFile.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>

#include "Session/BundleFiles.h"

namespace stagehand {

namespace fs = std::filesystem;
using Json   = nlohmann::ordered_json;
using namespace bundle;

namespace {

constexpr int         kFormatVersion = 1;
constexpr const char* kFormatName    = "stagehand-rig";
constexpr const char* kBusNames[kRigBuses] = {"A", "B"};

// State files: state/A1.bin is bus A's first effect.
std::string stateFile(std::size_t bus, std::size_t slot) {
    return std::string("state/") + kBusNames[bus] + std::to_string(slot + 1) + ".bin";
}

} // namespace

RackFileResult loadRig(const std::string& bundlePath, Rig& rig) {
    RackFileResult result;
    const fs::path bundle   = bundleDirectory(bundlePath);
    const fs::path jsonPath = bundle / "rig.json";
    std::ifstream  in(jsonPath);
    if (!in) {
        result.error = "cannot read " + jsonPath.string();
        return result;
    }
    Rig loaded;
    try {
        const Json doc = Json::parse(in);
        if (!doc.is_object() || doc.value("format", std::string{}) != kFormatName)
            fail("not a StageHand rig (expected \"format\": \"stagehand-rig\")");
        const int version = doc.at("version").get<int>();
        if (version > kFormatVersion)
            fail("made by a newer StageHand (format version " + std::to_string(version) + "; this one reads up to " +
                 std::to_string(kFormatVersion) + ")");
        if (version < 1) fail("invalid format version");

        std::string stem = bundle.filename().string();
        if (stem.size() > 4 && stem.compare(stem.size() - 4, 4, ".rig") == 0) stem.resize(stem.size() - 4);
        loaded.name     = doc.value("name", stem);
        loaded.masterDb = doc.value("masterDb", 0.0f);
        if (doc.contains("buses")) {
            const Json& buses = doc.at("buses");
            if (!buses.is_array() || buses.size() > kRigBuses) fail("\"buses\" must be a list of up to 2 buses");
            for (std::size_t b = 0; b < buses.size(); ++b) {
                const Json& j   = buses[b];
                RigBus&     bus = loaded.buses[b];
                if (!j.is_object()) fail("each bus is an object");
                bus.returnDb = j.value("returnDb", 0.0f);
                bus.muted    = j.value("muted", false);
                const int output = j.value("output", 1);
                if (output < 1) fail("a bus's output must be 1 or more");
                bus.firstOutput = static_cast<std::uint32_t>(output - 1);
                if (!j.contains("effects")) continue;
                const Json& effects = j.at("effects");
                if (!effects.is_array() || effects.size() > RigBus::kEffects)
                    fail("a bus's \"effects\" must be a list of up to 2 (an effect or null)");
                for (std::size_t e = 0; e < effects.size(); ++e) {
                    const Json& fx = effects[e];
                    if (fx.is_null()) continue;
                    RigEffect effect;
                    effect.plugin.format = parseFormat(fx.at("format").get<std::string>());
                    effect.plugin.id     = fx.at("id").get<std::string>();
                    effect.plugin.path   = expandHome(fx.value("path", std::string{}));
                    effect.plugin.kind   = PluginKind::Effect;
                    if (fx.contains("state") && !readBytes(bundle / fx.at("state").get<std::string>(), effect.state)) {
                        effect.state.clear();
                        result.warnings.push_back(std::string("bus ") + kBusNames[b] + ", effect " +
                                                  std::to_string(e + 1) +
                                                  ": its state file is missing; it starts from its defaults");
                    }
                    bus.effects[e] = std::move(effect);
                }
            }
        }
    } catch (const std::exception& e) {
        result.error = jsonPath.string() + ": " + e.what();
        return result;
    }
    rig       = std::move(loaded);
    result.ok = true;
    return result;
}

RackFileResult saveRig(const std::string& bundlePath, const Rig& rig) {
    RackFileResult  result;
    const fs::path  bundle = bundleDirectory(bundlePath);
    const fs::path  temp   = bundle.string() + ".tmp";
    std::error_code ec;
    try {
        fs::remove_all(temp);
        fs::create_directories(temp / "state");
        Json doc;
        doc["format"]   = kFormatName;
        doc["version"]  = kFormatVersion;
        doc["name"]     = rig.name;
        doc["masterDb"] = rig.masterDb;
        Json buses      = Json::array();
        for (std::size_t b = 0; b < kRigBuses; ++b) {
            const RigBus& bus = rig.buses[b];
            Json          j;
            j["name"]     = kBusNames[b];
            j["returnDb"] = bus.returnDb;
            j["muted"]    = bus.muted;
            j["output"]   = bus.firstOutput + 1;
            Json effects  = Json::array();
            for (std::size_t e = 0; e < RigBus::kEffects; ++e) {
                if (!bus.effects[e]) {
                    effects.push_back(nullptr);
                    continue;
                }
                const RigEffect& effect = *bus.effects[e];
                Json             fx;
                fx["format"] = formatName(effect.plugin.format);
                fx["id"]     = effect.plugin.id;
                if (!effect.plugin.path.empty()) fx["path"] = abbreviateHome(effect.plugin.path);
                if (!effect.state.empty()) {
                    if (!writeBytes(temp / stateFile(b, e), effect.state))
                        fail(std::string("cannot write the state of bus ") + kBusNames[b] + "'s effect");
                    fx["state"] = stateFile(b, e);
                }
                effects.push_back(fx);
            }
            j["effects"] = effects;
            buses.push_back(j);
        }
        doc["buses"] = buses;
        {
            std::ofstream out(temp / "rig.json", std::ios::trunc);
            out << doc.dump(2) << '\n';
            if (!out) fail("cannot write " + (temp / "rig.json").string());
        }
        fs::create_directories(bundle.parent_path());
        swapInBundle(temp, bundle);
    } catch (const std::exception& e) {
        result.error = e.what();
        fs::remove_all(temp, ec);
        return result;
    }
    result.ok = true;
    return result;
}

std::string rigBundleOf(const std::string& chosen) {
    if (chosen.empty()) return {};
    fs::path        path = resolveRackPath(chosen);
    std::error_code ec;
    if (path.filename() == "rig.json") path = path.parent_path();
    return fs::is_regular_file(path / "rig.json", ec) ? path.string() : std::string{};
}

std::string defaultRigPath() {
    const char*    data = std::getenv("XDG_DATA_HOME");
    const fs::path base = data != nullptr && *data != '\0' ? fs::path(data) : fs::path(homeDir()) / ".local/share";
    return (base / "stagehand" / "Default.rig").string();
}

} // namespace stagehand
