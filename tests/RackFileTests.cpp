// Rack bundle tests: a save/load round trip, safe replacement of an existing
// rack, defaults, and the load failures and warnings from docs/rack-format.md.

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "Check.h"
#include "Core/Ump.h"
#include "Session/RackFile.h"

using namespace stagehand;
namespace fs = std::filesystem;

namespace {

fs::path scratch() {
    const fs::path dir = fs::temp_directory_path() / ("stagehand-rackfile-" + std::to_string(getpid()));
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

Rack sampleRack() {
    Rack rack;
    rack.name = "Split";

    RackSlot bass;
    bass.id                  = "bass";
    bass.name                = "Bass";
    bass.plugin.format       = PluginFormat::Clap;
    bass.plugin.id           = "com.example.synth";
    bass.plugin.path         = "/opt/clap/Synth.clap";
    bass.state               = bytes(std::string("\x00\x01binary\xff", 9));
    bass.gainDb              = -3.5f;
    bass.firstOutput         = 2;
    bass.notePort            = 1;
    bass.source.kind         = MidiSourceRef::Kind::Device;
    bass.source.name         = "KeyLab 88:KeyLab 88 MIDI 1";
    bass.filter.channelMask  = 0x0003;   // channels 1-2
    bass.filter.noteLow      = 0;
    bass.filter.noteHigh     = 59;
    bass.filter.velocityLow  = static_cast<std::uint16_t>(ump::scaleUp(10, 7, 16));
    bass.filter.velocityHigh = 0xFFFF;
    bass.filter.transpose    = 12;

    RackSlot pad;
    pad.id            = "pad";
    pad.name          = "Pad";
    pad.plugin.format = PluginFormat::Clap;
    pad.plugin.id     = "com.example.pad";
    pad.enabled       = false;
    pad.muted         = true;
    pad.source.kind   = MidiSourceRef::Kind::Virtual;
    pad.source.name   = "MIDI 2.0 In";
    pad.filter.noteLow = 60;

    rack.slots = {bass, pad};
    return rack;
}

void testRoundTrip(const fs::path& dir) {
    const std::string path = (dir / "Split.rack").string();
    const Rack        saved = sampleRack();
    RackFileResult    result = saveRack(path, saved);
    CHECK(result.ok);
    CHECK(fs::exists(dir / "Split.rack/rack.json"));
    CHECK(fs::exists(dir / "Split.rack/state/bass.bin"));
    CHECK(!fs::exists(dir / "Split.rack/state/pad.bin"));   // no state, no file

    Rack loaded;
    result = loadRack(path, loaded);
    CHECK(result.ok && result.warnings.empty());
    CHECK(loaded.name == "Split" && loaded.slots.size() == 2);
    if (loaded.slots.size() != 2) return;

    const RackSlot& b = loaded.slots[0];
    CHECK(b.id == "bass" && b.name == "Bass");
    CHECK(b.plugin.format == PluginFormat::Clap && b.plugin.id == "com.example.synth");
    CHECK(b.plugin.path == "/opt/clap/Synth.clap");
    CHECK(b.state == saved.slots[0].state);                 // opaque bytes, exactly
    CHECK(b.enabled && !b.muted && b.gainDb == -3.5f && b.firstOutput == 2 && b.notePort == 1);
    CHECK(b.source.kind == MidiSourceRef::Kind::Device && b.source.name == "KeyLab 88:KeyLab 88 MIDI 1");
    CHECK(b.filter.channelMask == 0x0003 && b.filter.groupMask == 0xFFFF);
    CHECK(b.filter.noteLow == 0 && b.filter.noteHigh == 59);
    CHECK(b.filter.velocityLow == saved.slots[0].filter.velocityLow && b.filter.velocityHigh == 0xFFFF);
    CHECK(b.filter.transpose == 12);

    const RackSlot& p = loaded.slots[1];
    CHECK(!p.enabled && p.muted && p.state.empty());
    CHECK(p.source.kind == MidiSourceRef::Kind::Virtual && p.source.name == "MIDI 2.0 In");
    CHECK(p.filter.noteLow == 60 && p.filter.noteHigh == 127);

    // Saving over an existing rack replaces it and leaves nothing behind.
    Rack changed = saved;
    changed.name = "Split v2";
    changed.slots.pop_back();
    CHECK(saveRack(path, changed).ok);
    CHECK(!fs::exists(dir / "Split.rack.tmp") && !fs::exists(dir / "Split.rack.old"));
    Rack reloaded;
    CHECK(loadRack(path, reloaded).ok && reloaded.name == "Split v2" && reloaded.slots.size() == 1);
}

void testDefaults(const fs::path& dir) {
    writeText(dir / "Minimal.rack/rack.json", R"({
        "format": "stagehand-rack", "version": 1,
        "slots": [ { "id": "one", "plugin": { "format": "clap", "id": "x.y" } } ] })");
    Rack rack;
    const RackFileResult result = loadRack((dir / "Minimal.rack/").string(), rack);   // trailing slash ok
    CHECK(result.ok);
    CHECK(rack.name == "Minimal");                            // from the bundle name
    if (rack.slots.size() != 1) {
        CHECK(false);
        return;
    }
    const RackSlot& s = rack.slots[0];
    CHECK(s.name == "one" && s.enabled && !s.muted && s.gainDb == 0.0f && s.firstOutput == 0 && s.notePort == 0);
    CHECK(s.source.kind == MidiSourceRef::Kind::All);
    CHECK(s.filter.channelMask == 0xFFFF && s.filter.noteLow == 0 && s.filter.noteHigh == 127);
    CHECK(s.filter.velocityLow == 0 && s.filter.velocityHigh == 0xFFFF && s.filter.transpose == 0);
}

void expectFailure(const fs::path& dir, const std::string& name, const std::string& json,
                   const std::string& expectedFragment) {
    writeText(dir / name / "rack.json", json);
    Rack                 rack;
    const RackFileResult result = loadRack((dir / name).string(), rack);
    CHECK(!result.ok);
    CHECK(result.error.find(expectedFragment) != std::string::npos);
    if (result.error.find(expectedFragment) == std::string::npos)
        std::fprintf(stderr, "  (error was: %s)\n", result.error.c_str());
}

void testFailures(const fs::path& dir) {
    expectFailure(dir, "Newer.rack", R"({"format":"stagehand-rack","version":2,"slots":[]})", "newer StageHand");
    expectFailure(dir, "NotARack.rack", R"({"version":1,"slots":[]})", "not a StageHand rack");
    expectFailure(dir, "BadJson.rack", R"({"format": "stagehand-rack", )", "parse");
    expectFailure(dir, "Dup.rack",
                  R"({"format":"stagehand-rack","version":1,"slots":[
                      {"id":"a","plugin":{"format":"clap","id":"x"}},
                      {"id":"a","plugin":{"format":"clap","id":"y"}}]})",
                  "duplicate slot id");
    expectFailure(dir, "BadId.rack",
                  R"({"format":"stagehand-rack","version":1,"slots":[
                      {"id":"has space","plugin":{"format":"clap","id":"x"}}]})",
                  "letters, digits");
    expectFailure(dir, "Sequence.rack",
                  R"({"format":"stagehand-rack","version":1,"slots":[
                      {"id":"a","plugin":{"format":"clap","id":"x"},"midi":{"source":"sequence:arp"}}]})",
                  "sequence sources");
    expectFailure(dir, "BadNotes.rack",
                  R"({"format":"stagehand-rack","version":1,"slots":[
                      {"id":"a","plugin":{"format":"clap","id":"x"},"midi":{"notes":[80,20]}}]})",
                  "low <= high");
    expectFailure(dir, "BadChannel.rack",
                  R"({"format":"stagehand-rack","version":1,"slots":[
                      {"id":"a","plugin":{"format":"clap","id":"x"},"midi":{"channels":[17]}}]})",
                  "1-16");
    Rack none;
    CHECK(!loadRack((dir / "Missing.rack").string(), none).ok);
}

void testMissingStateWarns(const fs::path& dir) {
    writeText(dir / "NoState.rack/rack.json", R"({"format":"stagehand-rack","version":1,"slots":[
        {"id":"a","plugin":{"format":"clap","id":"x"},"state":"state/a.bin"}]})");
    Rack                 rack;
    const RackFileResult result = loadRack((dir / "NoState.rack").string(), rack);
    CHECK(result.ok && result.warnings.size() == 1);
    CHECK(rack.slots.size() == 1 && rack.slots[0].state.empty());
}

} // namespace

int main() {
    const fs::path dir = scratch();
    testRoundTrip(dir);
    testDefaults(dir);
    testFailures(dir);
    testMissingStateWarns(dir);
    fs::remove_all(dir);
    return test::finish("RackFile");
}
