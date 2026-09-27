// Session tests with fakes: plugin lookup (hint, then scan), state restored
// before activation, placeholders that round-trip, disabled slots, source
// resolution without fallback, and capture().

#include <map>
#include <memory>
#include <string>

#include "Check.h"
#include "FakePlugin.h"
#include "Session/Session.h"

using namespace stagehand;
using stagehand::test::FakePlugin;

namespace {

class FakeFactory final : public PluginFactory {
public:
    PluginFormat                  format() const override { return PluginFormat::Clap; }
    std::vector<PluginDescriptor> scan() override {
        ++scans;
        return installed;
    }
    std::unique_ptr<HostedPlugin> create(const PluginDescriptor& desc, HostServices&) override {
        for (const PluginDescriptor& d : installed)
            if (d.id == desc.id && d.path == desc.path) {
                auto plugin  = std::make_unique<FakePlugin>(2, NoteDialect::Midi | NoteDialect::Midi2, 1.0f, d.id);
                plugin->desc = d;
                created[d.id] = plugin.get();
                return plugin;
            }
        return nullptr;
    }

    std::vector<PluginDescriptor>       installed;
    std::map<std::string, FakePlugin*>  created;
    int                                 scans = 0;
};

class FakeMidiInput final : public MidiInput {
public:
    void                        setProtocolPreference(MidiProtocol) override {}
    void                        setSink(MidiSink*) override {}
    std::vector<MidiSourceInfo> enumerate() override { return {}; }
    MidiSourceHandle            open(const std::string& id) override {
        if (id != "Keys:Keys MIDI 1") return kInvalidMidiSource;
        opened.push_back(id);
        return 42;
    }
    MidiSourceHandle openVirtual(const std::string&, MidiProtocol) override { return kInvalidMidiSource; }
    void             close(MidiSourceHandle) override {}
    const char*      name() const override { return "fake"; }

    std::vector<std::string> opened;
};

class NullHost final : public HostServices {
public:
    void requestRestart() override {}
    void requestProcess() override {}
    void requestParamFlush() override {}
    void log(LogLevel, const char*) override {}
};

PluginDescriptor installedPlugin(const std::string& id, const std::string& path) {
    PluginDescriptor d;
    d.format = PluginFormat::Clap;
    d.id     = id;
    d.name   = id;
    d.path   = path;
    return d;
}

RackSlot slot(const std::string& id, const std::string& pluginId, const std::string& pathHint = "") {
    RackSlot s;
    s.id            = id;
    s.name          = id;
    s.plugin.format = PluginFormat::Clap;
    s.plugin.id     = pluginId;
    s.plugin.path   = pathHint;
    return s;
}

std::vector<std::byte> bytes(const std::string& text) {
    std::vector<std::byte> out;
    for (const char c : text) out.push_back(static_cast<std::byte>(c));
    return out;
}

// One audio block, so the engine adopts what was committed and runs it.
void runBlock(Engine& engine) {
    std::vector<float>   left(64), right(64);
    float*               channels[2] = {left.data(), right.data()};
    AudioCallbackContext ctx;
    ctx.frameCount      = 64;
    ctx.blockStartTime  = 1'000'000'000;
    ctx.time.sampleRate = 48000.0;
    ctx.output          = AudioBuffer{channels, 2, 64};
    engine.process(ctx);
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

} // namespace

int main() {
    testLoad();
    testNoFallbackToAll();
    return test::finish("Session");
}
