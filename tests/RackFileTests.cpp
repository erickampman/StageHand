// Rack bundle tests: a save/load round trip, safe replacement of an existing
// rack, defaults, and the load failures and warnings from docs/rack-format.md.

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iterator>
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
    rack.name               = "Split";
    rack.tempoBpm           = 97.5;
    rack.timeSigNumerator   = 7;
    rack.timeSigDenominator = 8;

    Sequence arp;
    arp.id              = "arp";
    arp.name            = "Arp";
    arp.protocol        = MidiProtocol::Midi2;
    arp.edo             = 31;
    arp.referenceNote   = 62;
    arp.channel         = 2;
    arp.group           = 1;
    arp.rate            = SequenceRate{1, 2};
    arp.length          = 8;
    arp.launch          = LaunchQuantize::Beat;
    arp.repeat          = false;
    arp.noteOffOffsetMs = -5.0;
    arp.notes           = {{0, 2, -3, 40000}, {4, 4, 10, 65535}};
    SequenceTrigger anyPad;   // defaults: toggle, any input, note 0 on channel 1
    SequenceTrigger pedal;
    pedal.action  = SequenceTrigger::Action::Stop;
    pedal.source  = MidiSourceRef{MidiSourceRef::Kind::Device, "Keys:Keys MIDI 1"};
    pedal.control = SequenceTrigger::Control::Controller;
    pedal.channel = 9;
    pedal.group   = 2;
    pedal.number  = 64;
    arp.triggers  = {anyPad, pedal};
    arp.atStart   = true;
    Sequence line;
    line.id       = "bass-line";
    line.protocol = MidiProtocol::Midi1;
    line.notes    = {{0, 1, 36, 100}};
    rack.sequences = {arp, line};
    rack.metronome.everyNote         = true;
    rack.metronome.click             = true;
    rack.metronome.clickLevelDb      = -18.0f;
    rack.metronome.clickOutput       = "Built-in Audio Analog Stereo";
    rack.transport.triggers          = {pedal};
    rack.transport.startOnNote       = true;
    rack.transport.startOnNoteSource = MidiSourceRef{MidiSourceRef::Kind::Virtual, "MIDI 2.0 In"};

    RackSlot bass;
    bass.id                  = "bass";
    bass.name                = "Bass";
    bass.plugin.format       = PluginFormat::Clap;
    bass.plugin.id           = "com.example.synth";
    bass.plugin.path         = "/opt/clap/Synth.clap";
    bass.state               = bytes(std::string("\x00\x01binary\xff", 9));
    bass.gainDb              = -3.5f;
    bass.sendDb[1]           = -12.0f;   // A off, B at -12 dB
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
    CHECK(loaded.tempoBpm == 97.5 && loaded.timeSigNumerator == 7 && loaded.timeSigDenominator == 8);
    CHECK(loaded.sequences.size() == 2);
    if (loaded.sequences.size() == 2) {
        const Sequence& a = loaded.sequences[0];
        CHECK(a.id == "arp" && a.name == "Arp" && a.protocol == MidiProtocol::Midi2 && a.edo == 31);
        CHECK(a.referenceNote == 62 && a.channel == 2 && a.group == 1 && a.length == 8);
        CHECK(a.rate.numerator == 1 && a.rate.denominator == 2 && a.launch == LaunchQuantize::Beat);
        CHECK(!a.repeat && a.noteOffOffsetMs == -5.0 && a.notes.size() == 2);
        CHECK(a.triggers.size() == 2 && a.triggers[0] == SequenceTrigger{} && a.atStart);
        if (a.triggers.size() == 2)
            CHECK(a.triggers[1].action == SequenceTrigger::Action::Stop && a.triggers[1].channel == 9 &&
                  a.triggers[1].group == 2 && a.triggers[1].number == 64 &&
                  a.triggers[1].control == SequenceTrigger::Control::Controller &&
                  a.triggers[1].source == (MidiSourceRef{MidiSourceRef::Kind::Device, "Keys:Keys MIDI 1"}));
        if (a.notes.size() == 2)
            CHECK(a.notes[0].pitch == -3 && a.notes[0].velocity == 40000 && a.notes[1].start == 4 &&
                  a.notes[1].length == 4 && a.notes[1].velocity == 65535);
        const Sequence& b = loaded.sequences[1];
        CHECK(b.id == "bass-line" && b.protocol == MidiProtocol::Midi1 && b.rate.numerator == 4);
        CHECK(b.launch == LaunchQuantize::Bar && b.repeat && b.notes.size() == 1 && b.triggers.empty() && !b.atStart);
        CHECK(loaded.transport == sampleRack().transport && loaded.transport.startOnNote);
        CHECK(loaded.metronome == sampleRack().metronome);
    }
    if (loaded.slots.size() != 2) return;

    const RackSlot& b = loaded.slots[0];
    CHECK(b.id == "bass" && b.name == "Bass");
    CHECK(b.plugin.format == PluginFormat::Clap && b.plugin.id == "com.example.synth");
    CHECK(b.plugin.path == "/opt/clap/Synth.clap");
    CHECK(b.state == saved.slots[0].state);                 // opaque bytes, exactly
    CHECK(!b.muted && b.gainDb == -3.5f && b.firstOutput == 2 && b.notePort == 1);
    CHECK(!b.sendDb[0] && b.sendDb[1] && *b.sendDb[1] == -12.0f);
    CHECK(b.source.kind == MidiSourceRef::Kind::Device && b.source.name == "KeyLab 88:KeyLab 88 MIDI 1");
    CHECK(b.filter.channelMask == 0x0003 && b.filter.groupMask == 0xFFFF);
    CHECK(b.filter.noteLow == 0 && b.filter.noteHigh == 59);
    CHECK(b.filter.velocityLow == saved.slots[0].filter.velocityLow && b.filter.velocityHigh == 0xFFFF);
    CHECK(b.filter.transpose == 12);

    const RackSlot& p = loaded.slots[1];
    CHECK(p.muted && p.state.empty());
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

// "enabled": false, from before mute replaced it, loads as muted.
void testEnabledBecomesMuted(const fs::path& dir) {
    writeText(dir / "Old.rack/rack.json", R"({
        "format": "stagehand-rack", "version": 1,
        "slots": [ { "id": "off", "plugin": { "format": "clap", "id": "x.y" }, "enabled": false },
                   { "id": "on", "plugin": { "format": "clap", "id": "x.y" }, "enabled": true } ] })");
    Rack rack;
    CHECK(loadRack((dir / "Old.rack").string(), rack).ok && rack.slots.size() == 2);
    if (rack.slots.size() != 2) return;
    CHECK(rack.slots[0].muted && !rack.slots[1].muted);
    CHECK(saveRack((dir / "Old.rack").string(), rack).ok);
    std::ifstream in(dir / "Old.rack/rack.json");
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(text.find("\"enabled\"") == std::string::npos);   // not written any more
}

void testDefaults(const fs::path& dir) {
    writeText(dir / "Minimal.rack/rack.json", R"({
        "format": "stagehand-rack", "version": 1,
        "slots": [ { "id": "one", "plugin": { "format": "clap", "id": "x.y" } } ] })");
    Rack rack;
    const RackFileResult result = loadRack((dir / "Minimal.rack/").string(), rack);   // trailing slash ok
    CHECK(result.ok);
    CHECK(rack.name == "Minimal");                            // from the bundle name
    CHECK(rack.tempoBpm == 120.0 && rack.timeSigNumerator == 4 && rack.timeSigDenominator == 4);
    if (rack.slots.size() != 1) {
        CHECK(false);
        return;
    }
    const RackSlot& s = rack.slots[0];
    CHECK(s.name == "one" && !s.muted && s.gainDb == 0.0f && s.firstOutput == 0 && s.notePort == 0);
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
    expectFailure(dir, "Newer.rack", R"({"format":"stagehand-rack","version":3,"slots":[]})", "newer StageHand");
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
                  "no sequence 'arp'");
    expectFailure(dir, "BadNotes.rack",
                  R"({"format":"stagehand-rack","version":1,"slots":[
                      {"id":"a","plugin":{"format":"clap","id":"x"},"midi":{"notes":[80,20]}}]})",
                  "low <= high");
    expectFailure(dir, "BadChannel.rack",
                  R"({"format":"stagehand-rack","version":1,"slots":[
                      {"id":"a","plugin":{"format":"clap","id":"x"},"midi":{"channels":[17]}}]})",
                  "1-16");
    expectFailure(dir, "BadRate.rack",
                  R"({"format":"stagehand-rack","version":1,"slots":[],"sequences":[{"id":"s","rate":"0"}]})",
                  "units per beat");
    expectFailure(dir, "LongNote.rack",
                  R"({"format":"stagehand-rack","version":1,"slots":[],
                      "sequences":[{"id":"s","length":4,"notes":[[2,3,0,100]]}]})",
                  "within the sequence");
    expectFailure(dir, "LoudNote.rack",
                  R"({"format":"stagehand-rack","version":1,"slots":[],
                      "sequences":[{"id":"s","protocol":1,"notes":[[0,1,60,200]]}]})",
                  "1-127");
    expectFailure(dir, "TwoControls.rack",
                  R"({"format":"stagehand-rack","version":1,"slots":[],
                      "sequences":[{"id":"s","triggers":[{"note":36,"controller":64}]}]})",
                  "\"note\" or a \"controller\"");
    expectFailure(dir, "SeqTrigger.rack",
                  R"({"format":"stagehand-rack","version":1,"slots":[],
                      "sequences":[{"id":"s","triggers":[{"source":"sequence:s","note":36}]}]})",
                  "an input");
    expectFailure(dir, "BadAction.rack",
                  R"({"format":"stagehand-rack","version":1,"slots":[],
                      "sequences":[{"id":"s","triggers":[{"action":"pause","note":36}]}]})",
                  "toggle");
    expectFailure(dir, "OnNote.rack",
                  R"({"format":"stagehand-rack","version":1,"slots":[],"transport":{"startOnNote":"sequence:x"}})",
                  "transport: ");
    expectFailure(dir, "Count.rack",
                  R"({"format":"stagehand-rack","version":1,"slots":[],"metronome":{"count":"halves"}})",
                  "metronome: ");
    expectFailure(dir, "Fast.rack", R"({"format":"stagehand-rack","version":1,"tempo":500,"slots":[]})",
                  "20-400 BPM");
    expectFailure(dir, "Sig.rack",
                  R"({"format":"stagehand-rack","version":1,"timeSignature":[4,3],"slots":[]})", "1-32 over");
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

// Where sequences play (format 2): a sequence's "targets" and a slot's "none"
// source round-trip; a format-1 "sequence:<id>" slot converts to a target with
// no live input; a target that isn't a slot is an error.
void testSequenceTargets(const fs::path& dir) {
    writeText(dir / "Old.rack/rack.json", R"({"format":"stagehand-rack","version":1,
        "sequences":[{"id":"arp"}],
        "slots":[{"id":"a","plugin":{"format":"clap","id":"x"},"midi":{"source":"sequence:arp"}},
                 {"id":"b","plugin":{"format":"clap","id":"x"}}]})");
    Rack old;
    CHECK(loadRack((dir / "Old.rack").string(), old).ok);
    CHECK(old.sequences.size() == 1 && (old.sequences[0].targets == std::vector<std::string>{"a"}));
    CHECK(old.slots[0].source.kind == MidiSourceRef::Kind::None && old.slots[1].source.kind == MidiSourceRef::Kind::All);

    old.sequences[0].targets.push_back("b");   // b plays it and keeps its live input
    const std::string path = (dir / "Targets.rack").string();
    CHECK(saveRack(path, old).ok);
    Rack reloaded;
    CHECK(loadRack(path, reloaded).ok);
    CHECK((reloaded.sequences[0].targets == std::vector<std::string>{"a", "b"}));
    CHECK(reloaded.slots[0].source.kind == MidiSourceRef::Kind::None);
    CHECK(reloaded.slots[1].source.kind == MidiSourceRef::Kind::All);

    writeText(dir / "BadTarget.rack/rack.json", R"({"format":"stagehand-rack","version":2,
        "sequences":[{"id":"arp","targets":["ghost"]}],"slots":[]})");
    Rack bad;
    const RackFileResult result = loadRack((dir / "BadTarget.rack").string(), bad);
    CHECK(!result.ok && result.error.find("no slot 'ghost'") != std::string::npos);
}

} // namespace

int main() {
    const fs::path dir = scratch();
    testRoundTrip(dir);
    testDefaults(dir);
    testEnabledBecomesMuted(dir);
    testFailures(dir);
    testMissingStateWarns(dir);
    testSequenceTargets(dir);
    fs::remove_all(dir);
    return test::finish("RackFile");
}
