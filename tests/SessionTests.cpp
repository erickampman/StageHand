// Session tests with fakes: plugin lookup (hint, then scan), state restored
// before activation, placeholders that round-trip, disabled slots, source
// resolution without fallback, and capture().

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
    c.enabled  = false;
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

    // Enabled, loaded slots run; the disabled one doesn't.
    runBlock(*engine);
    auto* fc = dynamic_cast<FakePlugin*>(slots[2].plugin.get());
    CHECK(fa != nullptr && fa->calls.size() == 1);
    CHECK(fc != nullptr && fc->calls.empty());

    // capture(): fresh state from running plugins; the placeholder unchanged.
    fa->state = bytes("edited-a");
    const Rack captured = session.capture();
    CHECK(captured.slots.size() == 4);
    if (captured.slots.size() == 4) {
        CHECK(captured.slots[0].state == bytes("edited-a"));
        CHECK(captured.slots[1].state == bytes("keep-me") && captured.slots[1].plugin.id == "not.installed");
        CHECK(!captured.slots[2].enabled);
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
    CHECK(session.slot("a")->runtime->currentGain == 0.0f);
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
    b.enabled  = false;
    b.source   = MidiSourceRef{MidiSourceRef::Kind::Device, "Keys:Keys MIDI 1"};
    rack.slots = {slot("a", "synth", "/clap/Synth.clap"), b};
    Session::LoadReport report;
    CHECK(session.load(rack, 48000.0, report));
    CHECK(midi.opened.empty());   // disabled: its device isn't opened yet
    runBlock(*engine);
    engine->collect();
    FakePlugin* fa = fakeOf(session, "a");
    FakePlugin* fb = fakeOf(session, "b");
    if (fa == nullptr || fb == nullptr) return;

    // Three edits, one commit: a split at 59, and b joins on its device.
    MidiFilter lower;
    lower.noteHigh = 59;
    CHECK(session.setFilter("a", lower));
    CHECK(session.setFirstOutput("a", 0));
    CHECK(session.setEnabled("b", true));
    CHECK(midi.opened.size() == 1);   // resolved on enabling
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
    CHECK(session.setEnabled("a", false));
    session.flush();
    runBlock(*engine);
    CHECK(fa->stops == 1);   // disabled: stopped on the audio thread, still loaded
    CHECK(fa->active);
    fb->calls.clear();
    engine->onMidi(42, noteOn(61));
    runBlock(*engine);
    CHECK(fb->calls.size() == 1 && fb->calls[0].events.empty());

    CHECK(session.capture().slots[1].source.name == "Unplugged:Port");
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

} // namespace

int main() {
    testLoad();
    testNoFallbackToAll();
    testLiveEdits();
    testStructuralEdits();
    testAddRemove();
    return test::finish("Session");
}
