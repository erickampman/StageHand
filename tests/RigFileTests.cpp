// Rig file tests (docs/rig.md, "The rig on disk"): the round trip with
// effects and their state, an empty slot, a missing state file, failures, the
// bundle a path means, and the default rig's path. Files go in a scratch
// folder under /tmp.

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "Check.h"
#include "Session/AppPaths.h"
#include "Session/RigFile.h"

using namespace stagehand;
namespace fs = std::filesystem;

namespace {

fs::path scratch() {
    const fs::path dir = fs::temp_directory_path() / ("stagehand-rigfile-" + std::to_string(getpid()));
    fs::create_directories(dir);
    return dir;
}

void writeText(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path) << text;
}

std::vector<std::byte> bytes(const std::string& text) {
    std::vector<std::byte> out;
    for (const char c : text) out.push_back(static_cast<std::byte>(c));
    return out;
}

RigEffect effect(const std::string& id, const std::string& state = {}) {
    RigEffect e;
    e.plugin.format = PluginFormat::Clap;
    e.plugin.id     = id;
    e.plugin.path   = "/usr/lib/clap/" + id + ".clap";
    e.plugin.kind   = PluginKind::Effect;
    e.state         = bytes(state);
    return e;
}

void testRoundTrip(const fs::path& dir) {
    Rig rig;
    rig.name                  = "Stage";
    rig.masterDb              = -3.0f;
    rig.buses[0].effects[0]   = effect("hall", "hall-state");
    rig.buses[0].effects[1]   = effect("eq");   // no state: its defaults
    rig.buses[0].returnDb     = -6.0f;
    rig.buses[1].effects[1]   = effect("delay", "delay-state");   // the first slot empty
    rig.buses[1].muted        = true;
    rig.buses[1].firstOutput  = 2;
    SequenceTrigger looper;   // the looper switch (docs/looper.md)
    looper.source  = MidiSourceRef{MidiSourceRef::Kind::Device, "FCB1010:FCB1010 MIDI 1"};
    looper.control = SequenceTrigger::Control::Controller;
    looper.number  = 82;
    looper.channel = 1;
    rig.looperSwitch          = looper;
    rig.clickOutput           = "Scarlett 2i2 USB Analog Stereo";   // where the click plays, in every song
    const std::string path    = (dir / "Stage.rig").string();
    CHECK(saveRig(path, rig).ok);
    CHECK(fs::exists(dir / "Stage.rig/state/A1.bin") && !fs::exists(dir / "Stage.rig/state/A2.bin"));
    CHECK(fs::exists(dir / "Stage.rig/state/B2.bin") && !fs::exists(dir / "Stage.rig.tmp"));

    Rig back;
    const RackFileResult result = loadRig(path, back);
    CHECK(result.ok && result.warnings.empty());
    CHECK(back == rig);

    // Saving again replaces it whole: a state that's gone leaves no file.
    rig.buses[0].effects[0]->state.clear();
    CHECK(saveRig(path, rig).ok && !fs::exists(dir / "Stage.rig/state/A1.bin"));
}

void testMissingStateAndDefaults(const fs::path& dir) {
    writeText(dir / "Old.rig/rig.json", R"({ "format": "stagehand-rig", "version": 1,
        "buses": [ { "effects": [ { "format": "clap", "id": "hall", "state": "state/A1.bin" } ] } ] })");
    Rig rig;
    const RackFileResult result = loadRig((dir / "Old.rig").string(), rig);
    CHECK(result.ok && result.warnings.size() == 1);
    CHECK(rig.name == "Old" && rig.masterDb == 0.0f);   // the name from the bundle
    CHECK(rig.buses[0].effects[0] && rig.buses[0].effects[0]->plugin.id == "hall" &&
          rig.buses[0].effects[0]->state.empty());
    CHECK(!rig.buses[0].effects[1] && !rig.buses[1].effects[0] && rig.buses[1].returnDb == 0.0f);
}

void testFailures(const fs::path& dir) {
    Rig rig;
    CHECK(!loadRig((dir / "Nothing.rig").string(), rig).ok);
    writeText(dir / "Rack.rig/rig.json", R"({ "format": "stagehand-rack", "version": 1 })");
    CHECK(!loadRig((dir / "Rack.rig").string(), rig).ok);
    writeText(dir / "New.rig/rig.json", R"({ "format": "stagehand-rig", "version": 7 })");
    const RackFileResult newer = loadRig((dir / "New.rig").string(), rig);
    CHECK(!newer.ok && newer.error.find("newer") != std::string::npos);
    writeText(dir / "Three.rig/rig.json", R"({ "format": "stagehand-rig", "version": 1, "buses": [ {}, {}, {} ] })");
    CHECK(!loadRig((dir / "Three.rig").string(), rig).ok);
}

void testPaths(const fs::path& dir) {
    CHECK(rigBundleOf((dir / "Stage.rig").string()) == fs::canonical(dir / "Stage.rig").string());
    CHECK(rigBundleOf((dir / "Stage.rig/rig.json").string()) == fs::canonical(dir / "Stage.rig").string());
    CHECK(rigBundleOf((dir / "Nothing.rig").string()).empty());
#ifdef __APPLE__
    CHECK(defaultRigPath() == appDataDir() + "/Default.rig" &&
          appDataDir().find("/Library/Application Support/StageHand") != std::string::npos);
#else
    setenv("XDG_DATA_HOME", "/x/data", 1);
    CHECK(defaultRigPath() == "/x/data/stagehand/Default.rig");
    unsetenv("XDG_DATA_HOME");
    CHECK(defaultRigPath().find("/.local/share/stagehand/Default.rig") != std::string::npos);
#endif
}

} // namespace

int main() {
    const fs::path dir = scratch();
    testRoundTrip(dir);
    testMissingStateAndDefaults(dir);
    testFailures(dir);
    testPaths(dir);
    fs::remove_all(dir);
    return test::finish("RigFile");
}
