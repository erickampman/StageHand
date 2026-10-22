// Session tests with fakes: plugin lookup (hint, then scan), state restored
// before activation, placeholders that round-trip, disabled slots, source
// resolution without fallback, and capture().

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <string>

#include "Check.h"
#include "FakePlugin.h"
#include "SessionFakes.h"
#include "Session/Session.h"

using namespace stagehand;
using namespace stagehand::test;

namespace {

std::vector<std::byte> bytes(const std::string& text) {
    std::vector<std::byte> out;
    for (const char c : text) out.push_back(static_cast<std::byte>(c));
    return out;
}


void testLoad() {
    FakeFactory factory;
    factory.installed = {installedPlugin("synth", "/clap/Synth.clap"), installedPlugin("pad", "/clap/Pad.clap")};
    FakeMidiInput midi;
    NullHost      host;
    auto          engine = std::make_unique<Engine>();
    Session       session(factory, host, midi, *engine);

    Rack rack;
    rack.name = "Test";
    RackSlot a = slot("a", "synth", "/clap/Synth.clap");   // right hint: no scan needed
    a.state    = bytes("saved-a");
    a.source   = MidiSourceRef{MidiSourceRef::Kind::Device, "Keys:Keys MIDI 1"};
    RackSlot b = slot("b", "not.installed");                // placeholder
    b.state    = bytes("keep-me");
    RackSlot c = slot("c", "pad", "/old/place/Pad.clap");   // wrong hint: found by scan
    c.muted    = true;
    RackSlot d = slot("d", "synth", "/clap/Synth.clap");
    d.source   = MidiSourceRef{MidiSourceRef::Kind::Device, "Unplugged:Port"};
    rack.slots = {a, b, c, d};

    Session::LoadReport report;
    CHECK(session.load(rack, 48000.0, report));
    CHECK(session.rackName() == "Test" && session.slots().size() == 4);
    CHECK(report.warnings.size() == 2);   // b missing, d's device unplugged
    CHECK(factory.scans == 1);            // one scan, shared by b and c

    const auto& slots = session.slots();
    CHECK(slots[0].plugin && !slots[1].plugin && slots[2].plugin && slots[3].plugin);
    CHECK(slots[2].saved.plugin.path == "/clap/Pad.clap");   // corrected path hint
    CHECK(midi.opened.size() == 1);                          // Keys opened on demand, once

    auto* fa = dynamic_cast<FakePlugin*>(slots[0].plugin.get());
    CHECK(fa != nullptr && fa->active && !fa->loadedWhileActive);
    CHECK(fa != nullptr && fa->state == bytes("saved-a"));
    if (fa == nullptr) return;

    // Loaded slots run, muted ones too (until they've been quiet a while).
    runBlock(*engine);
    auto* fc = dynamic_cast<FakePlugin*>(slots[2].plugin.get());
    CHECK(fa != nullptr && fa->calls.size() == 1);
    CHECK(fc != nullptr && fc->calls.size() == 1);

    // capture(): fresh state from running plugins; the placeholder unchanged.
    fa->state = bytes("edited-a");
    const Rack captured = session.capture();
    CHECK(captured.slots.size() == 4);
    if (captured.slots.size() == 4) {
        CHECK(captured.slots[0].state == bytes("edited-a"));
        CHECK(captured.slots[1].state == bytes("keep-me") && captured.slots[1].plugin.id == "not.installed");
        CHECK(captured.slots[2].muted);
    }

    session.unload();
    runBlock(*engine);
    CHECK(engine->committedGraphAdopted());
    CHECK(fa->stops == 1);
    engine->collect();
}

// The unplugged device's slot hears nothing, not everything.
void testNoFallbackToAll() {
    FakeFactory factory;
    factory.installed = {installedPlugin("synth", "/clap/Synth.clap")};
    FakeMidiInput midi;
    NullHost      host;
    auto          engine = std::make_unique<Engine>();
    Session       session(factory, host, midi, *engine);

    RackSlot s = slot("s", "synth", "/clap/Synth.clap");
    s.source   = MidiSourceRef{MidiSourceRef::Kind::Device, "Unplugged:Port"};
    Rack rack;
    rack.slots = {s};
    Session::LoadReport report;
    CHECK(session.load(rack, 48000.0, report));

    TimedMidiMessage noteOn;
    noteOn.captureTime       = 1'000'000'000 - 1'000'000;
    noteOn.message.words[0]  = 0x20903C64;
    noteOn.message.wordCount = 1;
    engine->onMidi(7, noteOn);   // from some other open source
    runBlock(*engine);
    auto* plugin = dynamic_cast<FakePlugin*>(session.slots()[0].plugin.get());
    CHECK(plugin != nullptr && plugin->calls.size() == 1 && plugin->calls[0].events.empty());
    session.unload();
    runBlock(*engine);
}

// A device a rack names that isn't there under its id, found by name
// (docs/macos.md, decision 5): by its device name first (the same port if it
// has one, else its first), then by its port name if exactly one device has
// it; never by an address, and never a guess between two. The rack keeps the
// id it was saved with; a device already open isn't opened again.
void testDevicesByName() {
    auto info = [](const char* id, bool hardware = true) { return MidiSourceInfo{id, "", false, hardware, MidiProtocol::Midi1}; };
    const std::vector<MidiSourceInfo> listed = {info("KeyLab 88:KeyLab 88 Port 1"), info("KeyLab 88:KeyLab 88 Port 2"),
                                                info("Keys:Keys MIDI 1"), info("Pad A:MIDI 1"), info("Pad B:MIDI 1")};
    CHECK(findDeviceByName("KeyLab 88:KeyLab 88 MIDI 1", listed) == "KeyLab 88:KeyLab 88 Port 1");   // its first port
    CHECK(findDeviceByName("KeyLab 88:KeyLab 88 Port 2", listed) == "KeyLab 88:KeyLab 88 Port 2");   // its own port
    CHECK(findDeviceByName("USB Keys:Keys MIDI 1", listed) == "Keys:Keys MIDI 1");   // the one port of that name
    CHECK(findDeviceByName("Pad C:MIDI 1", listed).empty());                       // two have it: no guess
    CHECK(findDeviceByName("Nothing:Here", listed).empty());
    CHECK(findDeviceByName("24:0", listed).empty() && findDeviceByName("", listed).empty());
    CHECK(findDeviceByName("KeyLab 88", listed) == "KeyLab 88:KeyLab 88 Port 1");   // a device name alone

    // Through a rack load: the slots resolve to what's there.
    FakeFactory factory;
    factory.installed = {installedPlugin("synth", "/clap/Synth.clap")};
    FakeMidiInput midi;
    midi.sources = listed;
    midi.present = {"KeyLab 88:KeyLab 88 Port 1", "KeyLab 88:KeyLab 88 Port 2", "Pad A:MIDI 1", "Pad B:MIDI 1"};
    NullHost      host;
    auto          engine = std::make_unique<Engine>();
    Session       session(factory, host, midi, *engine);
    auto withSource = [](const char* id, const char* device) {
        RackSlot s = slot(id, "synth", "/clap/Synth.clap");
        s.source   = MidiSourceRef{MidiSourceRef::Kind::Device, device};
        return s;
    };
    Rack rack;
    rack.slots = {withSource("a", "KeyLab 88:KeyLab 88 MIDI 1"), withSource("b", "USB Keys:Keys MIDI 1"),
                  withSource("c", "Pad C:MIDI 1"), withSource("d", "KeyLab 88:Another name")};
    Session::LoadReport report;
    CHECK(session.load(rack, 48000.0, report));
    CHECK(session.slot("a")->source == 100 && session.slot("a")->sourceProblem.empty());
    CHECK(session.slot("b")->source == 42);
    CHECK(session.slot("c")->source == kInvalidMidiSource && !session.slot("c")->sourceProblem.empty());
    CHECK(session.slot("d")->source == 100);   // the same device, opened once
    CHECK(std::count(midi.opened.begin(), midi.opened.end(), "KeyLab 88:KeyLab 88 Port 1") == 1);
    CHECK(session.capture().slots[0].source.name == "KeyLab 88:KeyLab 88 MIDI 1");   // the rack keeps its id
    session.unload();
    runBlock(*engine);
}

TimedMidiMessage noteOn(std::uint8_t note) {
    TimedMidiMessage m;
    m.captureTime       = 1'000'000'000 - 1'000'000;   // inside runBlock's window
    m.message.words[0]  = 0x20900064u | (std::uint32_t{note} << 8);
    m.message.wordCount = 1;
    return m;
}

FakePlugin* fakeOf(const Session& session, const std::string& id) {
    const Session::Slot* s = session.slot(id);
    return s != nullptr ? dynamic_cast<FakePlugin*>(s->plugin.get()) : nullptr;
}

// Gain and mute go straight to the slot's runtime: no graph commit.
void testLiveEdits() {
    FakeFactory factory;
    factory.installed = {installedPlugin("synth", "/clap/Synth.clap")};
    FakeMidiInput midi;
    NullHost      host;
    auto          engine = std::make_unique<Engine>();
    Session       session(factory, host, midi, *engine);

    Rack rack;
    rack.slots = {slot("a", "synth", "/clap/Synth.clap")};
    Session::LoadReport report;
    CHECK(session.load(rack, 48000.0, report));
    runBlock(*engine);
    CHECK(!session.modified());

    const SlotRuntime& runtime = *session.slot("a")->runtime;
    CHECK(session.setGainDb("a", -6.0f));
    CHECK(std::fabs(runtime.targetGain.load() - 0.5012f) < 0.001f);
    CHECK(session.modified() && !session.flushPending());
    CHECK(engine->committedGraphAdopted());   // nothing new was committed

    CHECK(session.setMuted("a", true));
    CHECK(runtime.targetGain.load() == 0.0f);
    CHECK(session.setGainDb("a", 0.0f));      // muted: the gain is kept, not heard
    CHECK(runtime.targetGain.load() == 0.0f);
    CHECK(session.setMuted("a", false));
    CHECK(runtime.targetGain.load() == 1.0f);
    CHECK(!session.setGainDb("nope", 0.0f));

    CHECK(session.setMuted("a", true));
    const Rack captured = session.capture();
    CHECK(captured.slots.size() == 1 && captured.slots[0].muted && captured.slots[0].gainDb == 0.0f);
    session.markSaved();
    CHECK(!session.modified());

    // A reloaded muted slot starts silent, without ramping down.
    CHECK(session.load(captured, 48000.0, report));
    CHECK(session.slot("a")->runtime->targetGain.load() == 0.0f);
    CHECK(session.slot("a")->runtime->gain.current == 0.0f);
    session.unload();
    runBlock(*engine);
    engine->collect();
}

// Filter, enable and source edits wait for flush(), which commits once.
void testStructuralEdits() {
    FakeFactory factory;
    factory.installed = {installedPlugin("synth", "/clap/Synth.clap")};
    FakeMidiInput midi;
    NullHost      host;
    auto          engine = std::make_unique<Engine>();
    Session       session(factory, host, midi, *engine);

    Rack rack;
    RackSlot b = slot("b", "synth", "/clap/Synth.clap");
    b.source   = MidiSourceRef{MidiSourceRef::Kind::Device, "Keys:Keys MIDI 1"};
    rack.slots = {slot("a", "synth", "/clap/Synth.clap"), b};
    Session::LoadReport report;
    CHECK(session.load(rack, 48000.0, report));
    CHECK(midi.opened.size() == 1);   // b's device, opened on load
    runBlock(*engine);
    engine->collect();
    FakePlugin* fa = fakeOf(session, "a");
    FakePlugin* fb = fakeOf(session, "b");
    if (fa == nullptr || fb == nullptr) return;

    // Two edits, one commit: a split at 59.
    MidiFilter lower;
    lower.noteHigh = 59;
    CHECK(session.setFilter("a", lower));
    CHECK(session.setFirstOutput("a", 0));
    CHECK(session.flushPending() && engine->committedGraphAdopted());
    session.flush();
    CHECK(!session.flushPending() && !engine->committedGraphAdopted());
    runBlock(*engine);
    CHECK(engine->collect() == 1);

    fa->calls.clear();
    fb->calls.clear();
    engine->onMidi(42, noteOn(60));   // from Keys: above a's split, b's source
    runBlock(*engine);
    CHECK(fa->calls.size() == 1 && fa->calls[0].events.empty());
    CHECK(fb->calls.size() == 1 && fb->calls[0].events.size() == 1);

    // An unplugged device: the problem is reported and the slot hears nothing.
    std::string problem;
    CHECK(session.setSource("b", MidiSourceRef{MidiSourceRef::Kind::Device, "Unplugged:Port"}, problem));
    CHECK(!problem.empty() && session.slot("b")->sourceProblem == problem);
    session.flush();
    fb->calls.clear();
    engine->onMidi(42, noteOn(61));
    runBlock(*engine);
    CHECK(fb->calls.size() == 1 && fb->calls[0].events.empty());

    CHECK(session.capture().slots[1].source.name == "Unplugged:Port");
    session.unload();
    runBlock(*engine);
    engine->collect();
}

void testTransport() {
    FakeFactory   factory;
    FakeMidiInput midi;
    NullHost      host;
    auto          engine = std::make_unique<Engine>();
    Session       session(factory, host, midi, *engine);

    Rack rack;
    rack.tempoBpm           = 90.0;
    rack.timeSigNumerator   = 6;
    rack.timeSigDenominator = 8;
    Session::LoadReport report;
    session.load(rack, 48000.0, report);
    CHECK(!session.modified());
    runBlock(*engine);   // the rack's tempo and signature come with its switch
    CHECK(engine->transport().tempoBpm == 90.0 && engine->transport().numerator == 6);

    session.setTempo(133.3);
    CHECK(session.modified() && engine->transport().tempoBpm == 133.3);
    CHECK(!session.setTimeSignature(3, 5));   // refused; unchanged
    CHECK(session.setTimeSignature(3, 4) && engine->transport().numerator == 3);
    session.setTempo(5.0);                    // clamped
    CHECK(session.tempoBpm() == Engine::kMinTempo);

    const Rack captured = session.capture();
    CHECK(captured.tempoBpm == Engine::kMinTempo && captured.timeSigNumerator == 3 && captured.timeSigDenominator == 4);
    session.unload();
    runBlock(*engine);
    engine->collect();
}

void testSequences() {
    FakeFactory factory;
    factory.installed = {installedPlugin("synth", "/clap/Synth.clap")};
    FakeMidiInput midi;
    NullHost      host;
    auto          engine = std::make_unique<Engine>();
    Session       session(factory, host, midi, *engine);

    Rack     rack;
    Sequence arp;
    arp.id       = "arp";
    arp.protocol = MidiProtocol::Midi1;
    arp.launch   = LaunchQuantize::Immediate;
    arp.notes    = {{0, 1, 60, 100}};
    rack.sequences = {arp};
    RackSlot a     = slot("a", "synth", "/clap/Synth.clap");
    a.source       = MidiSourceRef{MidiSourceRef::Kind::Sequence, "arp"};   // the old (format 1) way
    RackSlot b     = slot("b", "synth", "/clap/Synth.clap");   // "all inputs": doesn't hear it
    rack.slots     = {a, b};
    Session::LoadReport report;
    CHECK(session.load(rack, 48000.0, report) && report.warnings.empty());
    CHECK(session.sequences().size() == 1 && isSequenceSource(session.sequences()[0].source));
    // An old "sequence:<id>" slot becomes one of its targets, with no live input.
    CHECK(session.sequence("arp")->sequence.targets == std::vector<std::string>{"a"});
    CHECK(session.slot("a")->saved.source.kind == MidiSourceRef::Kind::None &&
          session.slot("a")->source == kInvalidMidiSource && session.slot("a")->sourceProblem.empty());

    // Started through the Session, it plays into its target only.
    CHECK(session.startSequence("arp"));
    runBlock(*engine);
    auto* fa = dynamic_cast<FakePlugin*>(session.slot("a")->plugin.get());
    auto* fb = dynamic_cast<FakePlugin*>(session.slot("b")->plugin.get());
    CHECK(fa != nullptr && fa->calls.size() == 1 && fa->calls[0].events.size() == 1);
    CHECK(fb != nullptr && fb->calls.size() == 1 && fb->calls[0].events.empty());
    CHECK(session.sequenceState("arp") == SequenceState::Playing);

    // The protocol changes only while there are no notes.
    Sequence edit = session.sequence("arp")->sequence;
    edit.protocol = MidiProtocol::Midi2;
    std::string problem;
    CHECK(!session.setSequence(edit, problem) && !problem.empty());
    edit.notes.clear();
    edit.protocol = MidiProtocol::Midi1;
    CHECK(session.setSequence(edit, problem));   // cleared first...
    edit.protocol = MidiProtocol::Midi2;
    CHECK(session.setSequence(edit, problem));   // ...then the toggle works
    CHECK(session.flushPending() && session.modified());
    session.flush();

    CHECK(session.addSequence() == "seq" && session.addSequence(MidiProtocol::Midi1) == "seq-2");
    const Rack captured = session.capture();
    CHECK(captured.sequences.size() == 3 && captured.sequences[0].protocol == MidiProtocol::Midi2);
    CHECK(captured.slots[0].source.kind == MidiSourceRef::Kind::None && captured.sequences[0].targets.size() == 1);

    // Aimed at "b" too, it plays into both; b keeps its live input ("all inputs").
    // Unknown and repeated targets are dropped.
    session.stopAllSequences();
    runBlock(*engine);
    edit         = session.sequence("arp")->sequence;
    edit.notes   = {{0, 1, 0, 30000}};   // MIDI 2.0 now: step 0 = the reference note
    edit.targets = {"a", "b", "b", "nope"};
    CHECK(session.setSequence(edit, problem));
    CHECK((session.sequence("arp")->sequence.targets == std::vector<std::string>{"a", "b"}));
    CHECK(session.slot("b")->saved.source.kind == MidiSourceRef::Kind::All);
    session.flush();
    CHECK(session.startSequence("arp"));   // launch: immediately
    fa->calls.clear();
    fb->calls.clear();
    runBlock(*engine);   // its first note-on reaches both rows
    CHECK(fa->calls.size() == 1 && fb->calls.size() == 1);
    CHECK(!fa->calls[0].events.empty() && !fb->calls[0].events.empty());

    // Removing a row takes it out of every sequence's targets.
    CHECK(session.removeSlot("b"));
    CHECK((session.sequence("arp")->sequence.targets == std::vector<std::string>{"a"}));

    // Removing a sequence: its rows simply stop hearing it (no source problem).
    CHECK(session.removeSequence("arp"));
    CHECK(session.slot("a")->sourceProblem.empty());
    session.flush();
    CHECK(session.sequences().size() == 2);

    session.unload();
    runBlock(*engine);
    engine->collect();
}

// A trigger's device is opened when the rack loads; one that isn't connected
// is kept and warned about, and hears nothing.
void testSequenceTriggers() {
    FakeFactory   factory;
    FakeMidiInput midi;
    NullHost      host;
    auto          engine = std::make_unique<Engine>();
    Session       session(factory, host, midi, *engine);

    Sequence seq;
    seq.id       = "s";
    seq.protocol = MidiProtocol::Midi1;
    seq.launch   = LaunchQuantize::Immediate;
    SequenceTrigger keys;
    keys.source  = MidiSourceRef{MidiSourceRef::Kind::Device, "Keys:Keys MIDI 1"};
    keys.number  = 36;
    SequenceTrigger gone = keys;
    gone.source.name     = "Pads:Pads MIDI 1";
    gone.number          = 37;
    seq.triggers         = {keys, gone};
    Rack rack;
    rack.sequences = {seq};
    Session::LoadReport report;
    session.load(rack, 48000.0, report);
    CHECK(midi.opened == std::vector<std::string>{"Keys:Keys MIDI 1"});
    CHECK(report.warnings.size() == 1 && report.warnings[0].find("Pads") != std::string::npos);
    CHECK((session.sequence("s")->triggerSources == std::vector<MidiSourceHandle>{42, kInvalidMidiSource}));
    runBlock(*engine);

    auto press = [&](std::uint8_t note) {
        TimedMidiMessage m;
        m.captureTime       = 1'000'000'000 - 1'000'000;
        m.message.words[0]  = 0x20900064u | (std::uint32_t{note} << 8);
        m.message.wordCount = 1;
        engine->onMidi(42, m);
        runBlock(*engine);
    };
    press(37);   // the unconnected device's trigger: nothing
    CHECK(session.sequenceState("s") == SequenceState::Stopped);
    press(36);
    CHECK(session.sequenceState("s") == SequenceState::Playing);
    CHECK(session.capture().sequences[0].triggers.size() == 2);   // both kept

    session.unload();
    runBlock(*engine);
    engine->collect();
}

void testAddRemove() {
    FakeFactory factory;
    factory.installed = {installedPlugin("synth", "/clap/Synth.clap")};
    FakeMidiInput midi;
    NullHost      host;
    auto          engine = std::make_unique<Engine>();
    Session       session(factory, host, midi, *engine);

    std::string problem;
    CHECK(session.addSlot(factory.installed[0], 0, problem).empty() && !problem.empty());   // nothing loaded yet

    Session::LoadReport report;
    CHECK(!session.load(Rack{}, 48000.0, report));   // empty: nothing runs, but editing works
    CHECK(session.addSlot(factory.installed[0], 0, problem) == "synth");
    CHECK(session.addSlot(factory.installed[0], 0, problem) == "synth-2");   // unique; inserted first
    CHECK(session.addSlot(installedPlugin("missing", "/nowhere"), 9, problem).empty());
    CHECK(session.slots().size() == 2 && session.slots()[0].saved.id == "synth-2");
    CHECK(session.modified());

    session.flush();
    runBlock(*engine);
    FakePlugin* first = fakeOf(session, "synth");
    CHECK(first != nullptr && first->active && first->calls.size() == 1);

    std::weak_ptr<HostedPlugin> removed = session.slot("synth")->plugin;
    CHECK(session.removeSlot("synth") && !session.removeSlot("synth"));
    CHECK(!removed.expired());   // the running graph still holds it
    session.flush();
    runBlock(*engine);
    CHECK(!removed.expired());   // stopped, and held by the retired graph
    engine->collect();
    CHECK(removed.expired());    // destroyed here, on the main thread

    const Rack captured = session.capture();
    CHECK(captured.slots.size() == 1 && captured.slots[0].id == "synth-2" && captured.slots[0].name == "synth");
    session.unload();
    runBlock(*engine);
    engine->collect();
}

// A row's MIDI effects (docs/midi-effects.md): loaded with their state in
// front of the row's plugin, a missing one kept and passed by, a bypassed one
// left out of the graph; added, moved, bypassed and removed live.
void testMidiEffects() {
    FakeFactory factory;
    factory.installed   = {installedPlugin("synth", "/clap/Synth.clap"), installedPlugin("arp", "/clap/Arp.clap"),
                           installedPlugin("shift", "/clap/Shift.clap")};
    factory.noteEffects = {"arp", "shift"};
    FakeMidiInput midi;
    NullHost      host;
    auto          engine = std::make_unique<Engine>();
    Session       session(factory, host, midi, *engine);

    RackSlot a = slot("a", "synth", "/clap/Synth.clap");
    RackMidiEffect arp;
    arp.id        = "arp";
    arp.plugin    = factory.installed[1];
    arp.state     = bytes("arp-state");
    RackMidiEffect gone;
    gone.id           = "gone";
    gone.plugin.id    = "not.installed";
    gone.state        = bytes("keep-me");
    RackMidiEffect off = arp;
    off.id        = "off";
    off.state     = {};
    off.bypassed  = true;
    a.midiEffects = {arp, gone, off};
    RackSlot b = slot("b", "not.installed");   // a placeholder row keeps its effects as they were
    b.midiEffects = {arp};
    Rack rack;
    rack.slots = {a, b};

    Session::LoadReport report;
    CHECK(session.load(rack, 48000.0, report));
    CHECK(report.warnings.size() == 2);   // the missing effect, the missing row plugin
    const Session::Slot* row = session.slot("a");
    CHECK(row != nullptr && row->midiEffects.size() == 3);
    if (row == nullptr || row->midiEffects.size() != 3) return;
    auto* first = dynamic_cast<FakeNoteEffect*>(row->midiEffects[0].plugin.get());
    auto* third = dynamic_cast<FakeNoteEffect*>(row->midiEffects[2].plugin.get());
    CHECK(first != nullptr && first->state == bytes("arp-state"));
    CHECK(!row->midiEffects[1].plugin && !row->midiEffects[1].loadProblem.empty());
    CHECK(third != nullptr && session.slot("b")->midiEffects.size() == 1 && !session.slot("b")->midiEffects[0].plugin);
    const auto problems = session.runningProblems();
    CHECK(std::count_if(problems.begin(), problems.end(),
                        [](const std::string& p) { return p.find("MIDI effect 'gone'") != std::string::npos; }) == 1);

    runBlock(*engine);
    CHECK(first != nullptr && first->processed == 1);   // in the graph, in front of the synth
    CHECK(third != nullptr && third->processed == 1 && third->received.empty());   // bypassed: runs, no MIDI

    if (first != nullptr) first->state = bytes("arp-edited");
    Rack captured = session.capture();
    CHECK(captured.slots.size() == 2 && captured.slots[0].midiEffects.size() == 3);
    if (captured.slots.size() == 2 && captured.slots[0].midiEffects.size() == 3) {
        const auto& e = captured.slots[0].midiEffects;
        CHECK(e[0].state == bytes("arp-edited") && e[1].state == bytes("keep-me") && e[1].plugin.id == "not.installed");
        CHECK(e[2].bypassed && captured.slots[1].midiEffects[0].state == bytes("arp-state"));
    }

    // Live edits.
    std::string problem;
    CHECK(session.addMidiEffect("b", factory.installed[2], 0, problem).empty() && !problem.empty());   // placeholder row
    problem.clear();
    CHECK(session.addMidiEffect("a", factory.installed[0], 0, problem).empty() && !problem.empty());   // not a note effect
    CHECK(session.addMidiEffect("a", factory.installed[1], 0, problem) == "arp-2");   // unique; first
    CHECK(session.slot("a")->saved.midiEffects[0].id == "arp-2");
    CHECK(session.moveMidiEffect("a", "arp-2", 9) && session.slot("a")->saved.midiEffects.back().id == "arp-2");
    CHECK(session.moveMidiEffect("a", "arp-2", -1) && session.slot("a")->saved.midiEffects[2].id == "arp-2");
    CHECK(session.setMidiEffectBypassed("a", "off", false));
    CHECK(!session.moveMidiEffect("a", "nope", 1) && !session.removeMidiEffect("x", "arp"));
    session.flush();
    runBlock(*engine);
    auto* added = factory.createdEffects["arp"];   // the last arp made: arp-2
    CHECK(added != nullptr && added->processed == 1 && third != nullptr && third->processed == 2);

    std::weak_ptr<HostedPlugin> removed = session.slot("a")->midiEffects[0].plugin;
    CHECK(session.removeMidiEffect("a", "arp") && session.slot("a")->midiEffects.size() == 3);
    session.flush();
    runBlock(*engine);
    CHECK(first != nullptr && first->stops == 1);   // stopped as it left the graph
    engine->collect();
    CHECK(removed.expired());
    CHECK(session.modified());

    session.unload();
    runBlock(*engine);
    engine->collect();
}

} // namespace

// The rig (docs/rig.md): its effects outlive rack loads (same instances, never
// stopped), an effect that won't load is kept for saving, sends come from the
// rack, and the rig's edits mark the rig modified, not the rack.
void testRig() {
    FakeFactory factory;
    factory.installed = {installedPlugin("synth", "/clap/Synth.clap"), installedPlugin("verb", "/clap/Verb.clap")};
    FakeMidiInput midi;
    NullHost      host;
    auto          engine = std::make_unique<Engine>();
    Session       session(factory, host, midi, *engine);

    Rack     rack;
    RackSlot a = slot("a", "synth", "/clap/Synth.clap");
    a.sendDb[0] = -6.0f;
    rack.slots  = {a};
    Session::LoadReport report;
    CHECK(session.load(rack, 48000.0, report));
    CHECK(!session.hasRig());

    Rig       rig;
    RigEffect verb;
    verb.plugin = installedPlugin("verb", "/clap/Verb.clap");
    verb.state  = bytes("verb-state");
    rig.buses[0].effects[0] = verb;
    RigEffect gone;
    gone.plugin             = installedPlugin("not.installed", "/clap/Gone.clap");
    gone.state              = bytes("keep-me");
    rig.buses[1].effects[1] = gone;
    rig.buses[1].returnDb   = -6.0f;
    session.loadRig(rig, 48000.0, report);
    CHECK(session.hasRig() && !session.rigModified() && report.warnings.size() == 1);
    // Held here too, so it can be checked after the rig lets go of it.
    const std::shared_ptr<HostedPlugin> held = session.rig().effects[0][0].plugin;
    auto* fx = dynamic_cast<FakePlugin*>(held.get());
    CHECK(fx != nullptr && fx->active && fx->state == bytes("verb-state"));
    CHECK(!session.rig().effects[1][1].plugin && !session.rig().effects[1][1].problem.empty());
    CHECK(std::fabs(session.rig().returns[1]->targetGain.load() - 0.501f) < 0.001f);
    if (fx == nullptr) return;
    runBlock(*engine);
    CHECK(!fx->calls.empty());   // it runs, in the graph

    // Sends from the rack.
    CHECK(std::fabs(session.slot("a")->runtime->targetSend[0].load() - 0.501f) < 0.001f);
    CHECK(session.slot("a")->runtime->targetSend[1].load() == 0.0f);
    session.markSaved();
    CHECK(session.setSendDb("a", 1, -20.0f) && session.modified() && !session.rigModified());
    CHECK(std::fabs(session.slot("a")->runtime->targetSend[1].load() - 0.1f) < 0.001f);
    CHECK(session.setSendDb("a", 1, std::nullopt) && session.slot("a")->runtime->targetSend[1].load() == 0.0f);

    // A rack load keeps the rig: the same effect, never stopped.
    Rack other;
    other.slots = {slot("b", "synth", "/clap/Synth.clap")};
    CHECK(session.load(other, 48000.0, report));
    runBlock(*engine);
    engine->collect();
    CHECK(session.rig().effects[0][0].plugin.get() == fx && fx->stops == 0);

    // Rig edits: live ones mark the rig modified; structural ones wait for flush().
    CHECK(session.setBusMuted(1, true) && session.rigModified() && session.rig().returns[1]->targetGain.load() == 0.0f);
    session.markRigSaved();
    session.setMasterDb(-3.0f);
    CHECK(session.rigModified() && session.rig().saved.masterDb == -3.0f);
    std::string problem;
    const PluginDescriptor missing = installedPlugin("nothing", "/clap/Nothing.clap");
    CHECK(!session.setBusEffect(0, 1, &missing, problem) && !problem.empty());
    const PluginDescriptor synth = installedPlugin("synth", "/clap/Synth.clap");
    CHECK(session.setBusEffect(0, 1, &synth, problem) && session.flushPending());
    session.flush();
    runBlock(*engine);
    CHECK(session.setBusEffect(0, 0, nullptr, problem));   // emptied: verb leaves the rig
    session.flush();
    runBlock(*engine);
    engine->collect();
    CHECK(fx->stops == 1);

    // capture: fresh states; the effect that didn't load kept as it was.
    const Rig captured = session.captureRig();
    CHECK(!captured.buses[0].effects[0] && captured.buses[0].effects[1] &&
          captured.buses[0].effects[1]->plugin.id == "synth");
    CHECK(captured.buses[1].effects[1] && captured.buses[1].effects[1]->state == bytes("keep-me"));
    CHECK(captured.buses[1].muted && captured.masterDb == -3.0f && captured.buses[1].returnDb == -6.0f);

    session.unload();
    runBlock(*engine);
    engine->collect();
}

// load() in two steps (docs/setlists.md, section 2): prepare() creates nothing,
// each prepareStep() one plugin (restored before it's activated), nothing
// reaches the engine meanwhile, and activate() commits one graph that keeps
// the rig's effects.
void testPrepare() {
    FakeFactory factory;
    factory.installed = {installedPlugin("synth", "/clap/Synth.clap"), installedPlugin("pad", "/clap/Pad.clap"),
                         installedPlugin("verb", "/clap/Verb.clap")};
    FakeMidiInput midi;
    NullHost      host;
    auto          engine = std::make_unique<Engine>();
    Session       session(factory, host, midi, *engine);

    Rack first;
    first.slots = {slot("old", "synth", "/clap/Synth.clap")};
    Session::LoadReport report;
    CHECK(session.load(first, 48000.0, report));
    Rig       rig;
    RigEffect verb;
    verb.plugin             = installedPlugin("verb", "/clap/Verb.clap");
    rig.buses[0].effects[0] = verb;
    session.loadRig(rig, 48000.0, report);
    runBlock(*engine);
    auto* oldPlugin = dynamic_cast<FakePlugin*>(session.slot("old")->plugin.get());
    auto* fx        = dynamic_cast<FakePlugin*>(session.rig().effects[0][0].plugin.get());
    CHECK(oldPlugin != nullptr && fx != nullptr);
    if (oldPlugin == nullptr || fx == nullptr) return;
    const std::weak_ptr<HostedPlugin> oldRef = session.slot("old")->plugin;

    Rack next;
    next.name      = "Next";
    next.tempoBpm  = 90.0;
    RackSlot a     = slot("a", "pad", "/clap/Pad.clap");
    a.state        = bytes("pad-state");
    a.source       = MidiSourceRef{MidiSourceRef::Kind::Device, "Keys:Keys MIDI 1"};
    RackSlot b     = slot("b", "not.installed");
    RackSlot c     = slot("c", "synth", "/clap/Synth.clap");
    next.slots     = {a, b, c};
    Sequence seq;
    seq.id         = "seq";
    seq.targets    = {"c"};
    next.sequences = {seq};

    const std::uint64_t           commits  = engine->graphCommits();
    std::unique_ptr<PreparedRack> prepared = session.prepare(next, 48000.0);
    CHECK(prepared->pluginCount() == 3 && prepared->preparedCount() == 0 && !prepared->ready());
    CHECK(factory.created.count("pad") == 0);   // nothing created yet

    // One plugin per step; the running rack plays on, and the engine hears nothing.
    CHECK(!session.prepareStep(*prepared) && prepared->preparedCount() == 1);
    auto* pad = factory.created.count("pad") != 0 ? factory.created["pad"] : nullptr;
    CHECK(pad != nullptr && pad->active && !pad->loadedWhileActive && pad->state == bytes("pad-state"));
    CHECK(midi.opened.empty());   // sources resolve at activate()
    CHECK(!session.prepareStep(*prepared));   // the placeholder is a step too
    CHECK(session.prepareStep(*prepared) && prepared->ready());
    CHECK(session.prepareStep(*prepared));    // nothing left to do
    runBlock(*engine);
    CHECK(engine->graphCommits() == commits && session.slot("old") != nullptr && session.slot("a") == nullptr);
    CHECK(pad != nullptr && pad->calls.empty());
    CHECK(oldPlugin->calls.size() == 2);

    // The switch: one graph commit, the new slots and the same rig.
    report = {};
    CHECK(session.activate(std::move(prepared), report));
    CHECK(engine->graphCommits() == commits + 1);
    CHECK(report.warnings.size() == 1);   // b isn't installed
    CHECK(session.rackName() == "Next" && session.tempoBpm() == 90.0 && session.slots().size() == 3);
    CHECK(session.slot("a")->source == 42 && midi.opened.size() == 1);
    CHECK(session.sequences().size() == 1 && session.sequence("seq") != nullptr);
    CHECK(!session.modified() && !session.flushPending());
    runBlock(*engine);
    engine->collect();
    CHECK(pad != nullptr && pad->calls.size() == 1);
    CHECK(!oldRef.expired() && session.drainingSlots().size() == 1);   // the old rack rings out
    session.drainingSlots()[0].runtime->drained.store(true);           // until done
    session.flush();
    runBlock(*engine);
    engine->collect();
    CHECK(oldRef.expired());   // then stopped, retired, destroyed
    CHECK(session.rig().effects[0][0].plugin.get() == fx && fx->stops == 0);

    // activate() finishes a rack prepared only partly; one dropped is simply destroyed.
    std::unique_ptr<PreparedRack> partly = session.prepare(next, 48000.0);
    CHECK(!session.prepareStep(*partly));
    CHECK(session.activate(std::move(partly), report) && session.slots().size() == 3 && session.slot("c")->plugin);
    std::unique_ptr<PreparedRack> dropped = session.prepare(first, 48000.0);
    CHECK(session.prepareStep(*dropped));
    dropped.reset();   // never ran
    session.unload();
    runBlock(*engine);
    engine->collect();
}

// The switch's old rack (docs/setlists.md, section 3): its slots stay in the
// graph, draining, until done; a further switch mutes the rack still draining;
// Cut mutes at once. Each drained slot leaves at the next flush().
void testDraining() {
    FakeFactory factory;
    factory.installed = {installedPlugin("synth", "/clap/Synth.clap")};
    FakeMidiInput midi;
    NullHost      host;
    auto          engine = std::make_unique<Engine>();
    Session       session(factory, host, midi, *engine);
    auto rackOf = [](const std::string& id) {
        Rack rack;
        rack.slots = {slot(id, "synth", "/clap/Synth.clap"), slot(id + "-gone", "not.installed")};
        return rack;
    };
    Session::LoadReport report;
    CHECK(session.load(rackOf("a"), 48000.0, report) && session.drainingSlots().empty());
    const std::shared_ptr<SlotRuntime> aRuntime = session.slot("a")->runtime;
    const std::weak_ptr<HostedPlugin>  aPlugin  = session.slot("a")->plugin;

    const std::uint64_t commits = engine->graphCommits();
    CHECK(session.activate(session.prepare(rackOf("b"), 48000.0), report));
    CHECK(engine->graphCommits() == commits + 1);
    CHECK(session.drainingSlots().size() == 1);   // a's plugin; not the placeholder
    if (session.drainingSlots().size() != 1) return;
    const SlotConfig& drainingA = session.drainingSlots()[0];
    CHECK(drainingA.draining && drainingA.runtime == aRuntime && !aRuntime->muted.load());
    runBlock(*engine);
    CHECK(session.slot("a") == nullptr && session.slot("b") != nullptr && !aPlugin.expired());

    // Another switch while a drains: a fades out now (muted), b drains.
    const std::shared_ptr<SlotRuntime> bRuntime = session.slot("b")->runtime;
    CHECK(session.activate(session.prepare(rackOf("c"), 48000.0), report));
    CHECK(session.drainingSlots().size() == 2 && aRuntime->muted.load() && aRuntime->targetGain.load() == 0.0f);
    CHECK(!bRuntime->muted.load());

    // Done (as the audio thread says): left out at the next flush, then freed.
    aRuntime->drained.store(true);
    session.flush();
    CHECK(session.drainingSlots().size() == 1 && engine->graphCommits() == commits + 3);
    session.flush();
    CHECK(engine->graphCommits() == commits + 3);   // nothing more to do
    runBlock(*engine);
    engine->collect();
    CHECK(aPlugin.expired());

    // Cut: the old rack is muted at the switch (its notes released, faded).
    const std::shared_ptr<SlotRuntime> cRuntime = session.slot("c")->runtime;
    CHECK(session.activate(session.prepare(rackOf("d"), 48000.0), report, {Session::OldRack::Cut}));
    CHECK(cRuntime->muted.load() && cRuntime->targetGain.load() == 0.0f && session.drainingSlots().size() == 2);

    session.unload();
    CHECK(session.drainingSlots().empty());
    runBlock(*engine);
    engine->collect();
}

// Control links (docs/controls.md, milestone 2): compiled from both scopes
// with their targets found in what's running: a row's parameter; all songs'
// by plugin in every row that has it, unless the song has its own link on
// the control; a bus effect's; a link whose input isn't there, or whose
// parameter the plugin doesn't have, leaves it out. A row removed takes its
// targets with it; capture() keeps the song's links; editing the rig's marks it.
void testControlLinks() {
    using Kind = ControlLinkTarget::Kind;
    FakeFactory factory;
    factory.installed = {installedPlugin("synth", "/clap/Synth.clap"), installedPlugin("verb", "/clap/Verb.clap")};
    FakeMidiInput midi;
    NullHost      host;
    auto          engine = std::make_unique<Engine>();
    Session       session(factory, host, midi, *engine);
    Rack rack;
    rack.slots = {slot("a", "synth", "/clap/Synth.clap"), slot("b", "synth", "/clap/Synth.clap")};
    Session::LoadReport report;
    CHECK(session.load(rack, 48000.0, report));
    Rig       rig;
    RigEffect verb;
    verb.plugin             = installedPlugin("verb", "/clap/Verb.clap");
    rig.buses[0].effects[0] = verb;
    session.loadRig(rig, 48000.0, report);

    auto fake = [&](const std::string& row) { return dynamic_cast<FakePlugin*>(session.slot(row)->plugin.get()); };
    FakePlugin* a = fake("a");
    FakePlugin* b = fake("b");
    auto* fx      = dynamic_cast<FakePlugin*>(session.rig().effects[0][0].plugin.get());
    for (FakePlugin* p : {a, b}) p->params = {ParameterInfo{7, nullptr, "Cutoff", 0.0, 1.0, 0.5}};
    fx->params = {ParameterInfo{3, nullptr, "Size", 0.0, 10.0, 0.0, true, true}};

    auto link = [](std::uint8_t cc, std::vector<ControlLinkTarget> targets) {
        ControlLinkSetup l;
        l.number  = cc;
        l.pickUp  = false;   // jump: every move acts
        l.targets = std::move(targets);
        return l;
    };
    auto param = [](std::string row, std::string plugin, std::uint32_t id, double top) {
        ControlLinkTarget t;
        t.row    = std::move(row);
        t.plugin = std::move(plugin);
        t.param  = id;
        t.top    = top;
        return t;
    };
    ControlLinkTarget size = param("", "", 3, 10.0);
    size.bus               = 0;
    size.effect            = 0;
    ControlLinkSetup elsewhere = link(77, {param("", "", 0, 0.0)});
    elsewhere.targets[0].kind  = Kind::Master;
    elsewhere.source           = MidiSourceRef{MidiSourceRef::Kind::Device, "Gone:Gone 1"};
    session.setControlLinks(ControlScope::Song, {link(74, {param("a", "", 7, 1.0)}), link(78, {param("a", "", 99, 1.0)})});
    session.setControlLinks(ControlScope::AllSongs, {link(74, {param("", "synth", 7, 0.5)}),
                                                     link(75, {param("", "synth", 7, 1.0)}), link(76, {size}), elsewhere});
    CHECK(session.modified() && session.rigModified());
    CHECK(session.controlLinkProblem(ControlScope::AllSongs, 3) != "" &&
          session.controlLinkProblem(ControlScope::AllSongs, 0).empty());
    session.flush();
    runBlock(*engine);

    // The parameter values each plugin was sent since the last call.
    auto sent = [](FakePlugin* p) {
        std::vector<std::pair<std::uint32_t, double>> values;
        for (const FakePlugin::Call& call : p->calls)
            for (const Event& e : call.events)
                if (e.type == EventType::ParamValue) values.emplace_back(e.param.id, e.value);
        p->calls.clear();
        return values;
    };
    auto cc = [&](std::uint8_t number, std::uint8_t value) {
        TimedMidiMessage m;
        m.captureTime       = 1'000'000'000 - 1'000'000;
        m.message.words[0]  = 0x20B00000u | (std::uint32_t{number} << 8) | value;
        m.message.wordCount = 1;
        engine->onMidi(42, m);
        runBlock(*engine);
    };
    using Sent = std::vector<std::pair<std::uint32_t, double>>;
    sent(a), sent(b), sent(fx);
    cc(74, 127);   // the song's own: row a, to 1 (the rig's, to 0.5, is overridden)
    CHECK((sent(a) == Sent{{7, 1.0}}) && sent(b).empty());
    cc(75, 127);   // all songs', by plugin: both rows
    CHECK((sent(a) == Sent{{7, 1.0}}) && (sent(b) == Sent{{7, 1.0}}));
    cc(76, 64);    // the bus effect's stepped parameter
    CHECK((sent(fx) == Sent{{3, 5.0}}));
    cc(78, 127);   // a parameter the plugin doesn't have: left out
    CHECK(sent(a).empty());

    // Row a removed: its targets go, and with them the links that had only it.
    CHECK(session.removeSlot("a"));
    CHECK(session.controlLinks(ControlScope::Song).empty() && session.capture().controls.empty());
    session.flush();
    runBlock(*engine);
    sent(b);
    cc(74, 127);   // now all songs' 74 acts: row b, to 0.5
    CHECK((sent(b) == Sent{{7, 0.5}}));

    session.setControlLinks(ControlScope::Song, {link(74, {param("b", "", 7, 1.0)})});
    CHECK(session.capture().controls.size() == 1);
    session.unload();
}

// Replace a row's plugin: the row stays (id, settings, MIDI effects, its
// links but the old plugin's parameters), the name follows the plugin unless
// it was renamed, the new one starts from its defaults, and the old one fades
// out and goes once quiet. A plugin that won't load changes nothing; a
// placeholder replaced gets its MIDI effects loaded.
void testReplacePlugin() {
    using Kind = ControlLinkTarget::Kind;
    FakeFactory factory;
    factory.installed   = {installedPlugin("synth", "/clap/Synth.clap"), installedPlugin("organ", "/clap/Organ.clap"),
                           installedPlugin("arp", "/clap/Arp.clap")};
    factory.noteEffects = {"arp"};
    FakeMidiInput midi;
    NullHost      host;
    auto          engine = std::make_unique<Engine>();
    Session       session(factory, host, midi, *engine);

    RackMidiEffect arp;
    arp.id     = "arp";
    arp.plugin = factory.installed[2];
    RackSlot a = slot("a", "synth", "/clap/Synth.clap");
    a.name               = "synth";   // still its plugin's name
    a.state              = bytes("synth-state");
    a.gainDb             = -6.0f;
    a.filter.transpose   = 5;
    a.midiEffects        = {arp};
    RackSlot b           = slot("b", "not.installed");   // named "b": kept
    b.midiEffects        = {arp};
    Rack rack;
    rack.slots = {a, b};
    Session::LoadReport report;
    CHECK(session.load(rack, 48000.0, report));

    auto target = [](Kind kind, std::uint32_t param) {
        ControlLinkTarget t;
        t.kind  = kind;
        t.row   = "a";
        t.param = param;
        return t;
    };
    auto link = [](std::uint8_t cc, std::vector<ControlLinkTarget> targets) {
        ControlLinkSetup l;
        l.number  = cc;
        l.targets = std::move(targets);
        return l;
    };
    session.setControlLinks(ControlScope::Song, {link(74, {target(Kind::Parameter, 7)}),
                                                 link(75, {target(Kind::Parameter, 7), target(Kind::Gain, 0)})});
    session.flush();
    runBlock(*engine);

    const std::shared_ptr<SlotRuntime> oldRuntime = session.slot("a")->runtime;
    const std::weak_ptr<HostedPlugin>  oldPlugin  = session.slot("a")->plugin;
    const HostedPlugin* const          effect     = session.slot("a")->midiEffects[0].plugin.get();

    std::string problem;
    CHECK(!session.replacePlugin("a", installedPlugin("missing", "/nowhere"), problem) && !problem.empty());
    CHECK(session.slot("a")->plugin == oldPlugin.lock() && session.drainingSlots().empty());
    CHECK(!session.replacePlugin("zz", factory.installed[1], problem));

    CHECK(session.replacePlugin("a", factory.installed[1], problem));
    const Session::Slot* row = session.slot("a");
    CHECK(row != nullptr && session.slots()[0].saved.id == "a");
    if (row == nullptr) return;
    CHECK(row->saved.name == "organ" && row->saved.plugin.id == "organ" && row->saved.state.empty());
    CHECK(row->saved.gainDb == -6.0f && row->saved.filter.transpose == 5);
    CHECK(row->midiEffects.size() == 1 && row->midiEffects[0].plugin.get() == effect);
    auto* organ = dynamic_cast<FakePlugin*>(row->plugin.get());
    CHECK(organ != nullptr && organ->active && organ->state.empty());
    CHECK(row->runtime != oldRuntime);
    // The old one: muted (its notes end, it fades), draining, without the row's effects.
    CHECK(session.drainingSlots().size() == 1);
    if (session.drainingSlots().size() != 1) return;
    CHECK(session.drainingSlots()[0].runtime == oldRuntime && session.drainingSlots()[0].midiEffects.empty());
    CHECK(oldRuntime->muted.load() && oldRuntime->targetGain.load() == 0.0f);
    // Only the links to the old plugin's parameters went.
    const std::vector<ControlLinkSetup>& links = session.controlLinks(ControlScope::Song);
    CHECK(links.size() == 1 && links[0].number == 75 && links[0].targets.size() == 1 &&
          links[0].targets[0].kind == Kind::Gain);
    CHECK(session.modified());

    session.flush();
    runBlock(*engine);
    CHECK(!oldPlugin.expired());   // ringing off
    oldRuntime->drained.store(true);
    session.flush();
    runBlock(*engine);
    engine->collect();
    CHECK(oldPlugin.expired() && session.drainingSlots().empty());

    // A placeholder replaced: it runs, with its MIDI effects now loaded.
    CHECK(session.replacePlugin("b", factory.installed[0], problem));
    row = session.slot("b");
    CHECK(row->plugin && row->loadProblem.empty() && row->saved.name == "b");
    CHECK(row->midiEffects.size() == 1 && row->midiEffects[0].plugin);
    CHECK(session.drainingSlots().empty());   // nothing was running there

    const Rack captured = session.capture();
    CHECK(captured.slots.size() == 2 && captured.slots[0].plugin.id == "organ" && captured.slots[1].plugin.id == "synth");
    session.unload();
    runBlock(*engine);
    engine->collect();
}

int main() {
    testMidiEffects();
    testLoad();
    testNoFallbackToAll();
    testDevicesByName();
    testLiveEdits();
    testStructuralEdits();
    testAddRemove();
    testTransport();
    testSequences();
    testSequenceTriggers();
    testRig();
    testPrepare();
    testDraining();
    testControlLinks();
    testReplacePlugin();
    return test::finish("Session");
}
