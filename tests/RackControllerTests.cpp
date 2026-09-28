// RackController tests with fakes: the view model (rows, sources, status,
// protocol warning), meter ballistics and MIDI activity, editors, adding and
// removing plugins, and saving. No DGL, X11 or JACK.

#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "App/RackController.h"
#include "Check.h"
#include "FakePlugin.h"
#include "SessionFakes.h"
#include "Session/RackFile.h"

using namespace stagehand;
using namespace stagehand::test;

namespace {

class FakeEditor final : public EditorHandle {
public:
    explicit FakeEditor(std::shared_ptr<int> openCount) : count(std::move(openCount)) { ++*count; }
    ~FakeEditor() override { --*count; }
    bool isOpen() const override { return open; }

    bool open = true;

private:
    std::shared_ptr<int> count;
};

// Everything one test needs, wired as the app wires it.
struct Rig {
    FakeFactory              factory;
    FakeMidiInput            midi;
    NullHost                 host;
    std::unique_ptr<Engine>  engine = std::make_unique<Engine>();
    std::unique_ptr<Session> session;
    std::shared_ptr<int>     editorsOpen = std::make_shared<int>(0);
    std::vector<FakeEditor*> editors;
    int                      quits = 0;

    Rig() {
        factory.installed = {installedPlugin("synth", "/clap/Synth.clap"), installedPlugin("bass", "/clap/Bass.clap"),
                             installedPlugin("old", "/clap/Old.clap")};
        factory.dialects["old"] = NoteDialect::Midi;   // MIDI 1.0 only
        midi.sources = {MidiSourceInfo{"Through:Port 0", "Midi Through", false, false, MidiProtocol::Midi1},
                        MidiSourceInfo{"Keys:Keys MIDI 1", "Keys", false, true, MidiProtocol::Midi2}};
        session = std::make_unique<Session>(factory, host, midi, *engine);
    }

    RackController::Config config(const std::string& saveTarget = {}) {
        RackController::Config c;
        c.openEditor = [this](HostedPlugin&, const std::string&) -> std::unique_ptr<EditorHandle> {
            auto editor = std::make_unique<FakeEditor>(editorsOpen);
            editors.push_back(editor.get());
            return editor;
        };
        c.audioStatus   = [] { return RackController::AudioStatus{48000.0, 256, 3}; };
        c.quit          = [this] { ++quits; };
        c.virtualInputs = {{"MIDI 2.0 In", MidiProtocol::Midi2}, {"MIDI 1.0 In", MidiProtocol::Midi1}};
        c.saveTarget    = saveTarget;
        c.noSaveReason  = "saving is off";
        return c;
    }

    ~Rig() {
        session->unload();
        runBlock(*engine);
        engine->collect();
    }
};

Rack testRack() {
    Rack rack;
    rack.name  = "Test";
    RackSlot a = slot("a", "synth", "/clap/Synth.clap");
    a.source   = MidiSourceRef{MidiSourceRef::Kind::Device, "Keys:Keys MIDI 1"};
    RackSlot b = slot("b", "missing");
    RackSlot c = slot("c", "bass", "/clap/Bass.clap");
    c.enabled  = false;
    RackSlot d = slot("d", "old", "/clap/Old.clap");
    d.source   = MidiSourceRef{MidiSourceRef::Kind::Virtual, "MIDI 2.0 In"};
    RackSlot e = slot("e", "synth", "/clap/Synth.clap");
    e.source   = MidiSourceRef{MidiSourceRef::Kind::Device, "Unplugged:Port"};
    rack.slots = {a, b, c, d, e};
    return rack;
}

void testViewModel() {
    Rig                 rig;
    Session::LoadReport report;
    rig.session->load(testRack(), 48000.0, report);
    RackController controller(*rig.session, *rig.engine, rig.midi, rig.factory, rig.config());
    controller.tick(1.0);

    const RackViewModel& view = controller.view();
    CHECK(view.rackName == "Test" && !view.modified);
    CHECK(view.rows.size() == 5);
    if (view.rows.size() != 5) return;
    const RowModel &a = view.rows[0], &b = view.rows[1], &c = view.rows[2], &d = view.rows[3], &e = view.rows[4];
    CHECK(a.state == RowState::Running && a.pluginName == "synth");
    CHECK(a.sourceLabel == "Keys" && a.sourceConnected && a.sourceProtocol == SourceProtocol::Midi2);
    CHECK(!a.protocolWarning);   // the synth takes MIDI 2.0
    CHECK(b.state == RowState::Placeholder && b.pluginName == "missing" && !b.problem.empty());
    CHECK(c.state == RowState::Disabled);
    CHECK(d.sourceProtocol == SourceProtocol::Midi2 && d.protocolWarning);   // MIDI 2.0 into a MIDI 1.0 plugin
    CHECK(!e.sourceConnected && e.sourceLabel == "Unplugged:Port (not connected)");

    // The source menu: All, the virtual inputs, then hardware before software.
    CHECK(view.sources.size() == 5);
    if (view.sources.size() == 5) {
        CHECK(view.sources[0].ref.kind == MidiSourceRef::Kind::All);
        CHECK(view.sources[1].label == "MIDI 2.0 In" && view.sources[1].protocol == SourceProtocol::Midi2);
        CHECK(view.sources[3].label == "Keys" && view.sources[3].hardware);
        CHECK(view.sources[4].label == "Midi Through" && !view.sources[4].hardware);
    }
    CHECK(view.status.sampleRate == 48000.0 && view.status.bufferSize == 256 && view.status.xruns == 3);
    CHECK(view.status.saveTarget.empty() && view.status.noSaveReason == "saving is off");

    // Edits show up in the next model; structural ones are committed by tick().
    MidiFilter lower;
    lower.noteHigh = 59;
    controller.setFilter("a", lower);
    controller.setGainDb("a", -6.0f);
    controller.setMuted("c", true);
    CHECK(rig.session->flushPending());
    controller.tick(1.1);
    CHECK(!rig.session->flushPending() && !rig.engine->committedGraphAdopted());
    CHECK(view.modified && view.rows[0].filter.noteHigh == 59 && view.rows[0].gainDb == -6.0f);
    CHECK(view.rows[2].muted);
    controller.quit();
    CHECK(rig.quits == 1);
}

void testMetersAndActivity() {
    Rig  rig;
    Rack rack;
    rack.slots = {slot("a", "synth", "/clap/Synth.clap")};
    Session::LoadReport report;
    rig.session->load(rack, 48000.0, report);
    RackController controller(*rig.session, *rig.engine, rig.midi, rig.factory, rig.config());
    controller.tick(10.0);
    const RackViewModel& view = controller.view();
    CHECK(view.rows[0].meterDb[0] == -60.0f && !view.rows[0].midiActive);   // nothing has run yet

    // The fake plays a constant 1.0: 0 dBFS on both channels, not a clip (that needs > 1.0).
    rig.engine->onMidi(42, [] {
        TimedMidiMessage m;
        m.captureTime       = 1'000'000'000 - 1'000'000;
        m.message.words[0]  = 0x20903C64;
        m.message.wordCount = 1;
        return m;
    }());
    runBlock(*rig.engine);
    controller.tick(10.1);
    CHECK(view.rows[0].meterDb[0] == 0.0f && view.rows[0].meterDb[1] == 0.0f);
    CHECK(view.rows[0].peakHoldDb[0] == 0.0f && !view.rows[0].clipped);
    CHECK(view.status.outputDb[0] == 0.0f);
    CHECK(view.rows[0].midiActive);

    // Silence: the meter falls at 24 dB/s; the peak hold stays for 1.5 s.
    controller.tick(10.6);
    CHECK(std::fabs(view.rows[0].meterDb[0] - -12.0f) < 0.01f);
    CHECK(view.rows[0].peakHoldDb[0] == 0.0f);
    CHECK(!view.rows[0].midiActive);   // the light is brief
    controller.tick(11.7);             // past the hold: released, falling with the meter
    CHECK(view.rows[0].peakHoldDb[0] == view.rows[0].meterDb[0] && view.rows[0].meterDb[0] < -30.0f);
    controller.tick(20.0);
    CHECK(view.rows[0].meterDb[0] == -60.0f);   // the floor

    // A clip latches until cleared.
    controller.setGainDb("a", 6.0f);
    for (int i = 0; i < 10; ++i) runBlock(*rig.engine);   // past the 10 ms ramp
    controller.tick(20.1);
    CHECK(view.rows[0].clipped && view.status.outputClipped);
    controller.tick(20.2);
    CHECK(view.rows[0].clipped);
    controller.clearClip("a");
    controller.clearOutputClip();
    controller.tick(20.3);
    CHECK(!view.rows[0].clipped && !view.status.outputClipped);
}

void testEditorsAndRemove() {
    Rig                 rig;
    Session::LoadReport report;
    rig.session->load(testRack(), 48000.0, report);
    {
        RackController controller(*rig.session, *rig.engine, rig.midi, rig.factory, rig.config());
        controller.openAllEditors();
        CHECK(*rig.editorsOpen == 4 && controller.openEditorCount() == 4);   // not the placeholder
        controller.openEditor("a");                                          // already open: no second window
        CHECK(*rig.editorsOpen == 4);
        controller.tick(1.0);
        CHECK(controller.view().rows[0].editorOpen && !controller.view().rows[1].editorOpen);

        rig.editors[0]->open = false;   // the user closed a's window
        controller.tick(1.1);
        CHECK(!controller.view().rows[0].editorOpen && *rig.editorsOpen == 3);

        controller.removeSlot("c");     // closes its editor first
        CHECK(*rig.editorsOpen == 2);
        controller.tick(1.2);
        CHECK(controller.view().rows.size() == 4 && controller.view().rows[2].id == "d");
    }
    CHECK(*rig.editorsOpen == 0);   // the controller closes the rest
}

void testAddPlugin() {
    Rig                 rig;
    Session::LoadReport report;
    rig.session->load(Rack{}, 48000.0, report);
    RackController controller(*rig.session, *rig.engine, rig.midi, rig.factory, rig.config());

    const std::vector<PluginDescriptor>& plugins = controller.availablePlugins();
    controller.availablePlugins();
    CHECK(rig.factory.scans == 1 && plugins.size() == 3 && plugins[0].name == "bass");   // sorted, scanned once
    CHECK(controller.addPlugin(plugins[2], 0) == "synth");
    CHECK(controller.addPlugin(installedPlugin("nope", "/nowhere"), 0).empty());
    const std::vector<std::string> notices = controller.takeNotices();
    CHECK(notices.size() == 1 && notices[0].rfind("Couldn't add nope", 0) == 0);
    controller.tick(1.0);
    CHECK(controller.view().rows.size() == 1 && controller.view().rows[0].state == RowState::Running);
    CHECK(controller.view().modified);
}

void testSave() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "stagehand-controller-test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::string target = (dir / "Saved.rack").string();

    Rig                 rig;
    Session::LoadReport report;
    Rack                rack;
    rack.name  = "Saved";
    rack.slots = {slot("a", "synth", "/clap/Synth.clap")};
    rig.session->load(rack, 48000.0, report);
    {
        RackController controller(*rig.session, *rig.engine, rig.midi, rig.factory, rig.config());
        CHECK(!controller.save());   // no target
        const std::vector<std::string> notices = controller.takeNotices();
        CHECK(notices.size() == 1 && notices[0] == "Not saved: saving is off");
    }
    RackController controller(*rig.session, *rig.engine, rig.midi, rig.factory, rig.config(target));
    controller.setMuted("a", true);
    CHECK(rig.session->modified());
    CHECK(controller.save());
    CHECK(!rig.session->modified());
    CHECK(controller.takeNotices() == std::vector<std::string>{"Saved the rack to " + target});

    Rack loaded;
    CHECK(loadRack(target, loaded).ok);
    CHECK(loaded.slots.size() == 1 && loaded.slots[0].muted);
    std::filesystem::remove_all(dir);
}

} // namespace

int main() {
    testViewModel();
    testMetersAndActivity();
    testEditorsAndRemove();
    testAddPlugin();
    testSave();
    return test::finish("RackController");
}
