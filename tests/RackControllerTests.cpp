// RackController tests with fakes: the view model (rows, sources, status,
// protocol warning), meter ballistics and MIDI activity, editors, adding and
// removing plugins, and saving. No DGL, X11 or JACK.

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "App/ParameterEditor.h"
#include "App/RackController.h"
#include "Check.h"
#include "FakePlugin.h"
#include "SessionFakes.h"
#include "Session/RackFile.h"
#include "Session/RigFile.h"
#include "Session/SetFile.h"
#include "Session/SetlistFile.h"

using namespace stagehand;
using namespace stagehand::test;

namespace {

class FakeEditor final : public EditorHandle {
public:
    explicit FakeEditor(std::shared_ptr<int> openCount) : count(std::move(openCount)) { ++*count; }
    ~FakeEditor() override { --*count; }
    bool isOpen() const override { return open; }
    void setTitle(const std::string& t) override { retitled.push_back(t); }

    bool                     open = true;
    std::vector<std::string> retitled;   // setTitle() calls, after opening

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
    std::vector<HostedPlugin*> genericFor;   // what the generic editor was opened for
    int                      quits = 0;

    Rig() {
        factory.installed = {installedPlugin("synth", "/clap/Synth.clap"), installedPlugin("bass", "/clap/Bass.clap"),
                             installedPlugin("old", "/clap/Old.clap")};
        factory.dialects["old"] = NoteDialect::Midi;   // MIDI 1.0 only
        factory.withEditor      = {"synth", "bass", "old"};   // their own editors
        midi.sources = {MidiSourceInfo{"Through:Port 0", "Midi Through", false, false, MidiProtocol::Midi1},
                        MidiSourceInfo{"Keys:Keys MIDI 1", "Keys", false, true, MidiProtocol::Midi2}};
        session = std::make_unique<Session>(factory, host, midi, *engine);
    }

    RackController::Config config() {
        RackController::Config c;
        c.openGenericEditor = [this](HostedPlugin& plugin, const std::string&) -> std::unique_ptr<EditorHandle> {
            genericFor.push_back(&plugin);
            return std::make_unique<FakeEditor>(editorsOpen);
        };
        c.openEditor = [this](HostedPlugin&, const std::string&) -> std::unique_ptr<EditorHandle> {
            auto editor = std::make_unique<FakeEditor>(editorsOpen);
            editors.push_back(editor.get());
            return editor;
        };
        c.audioStatus   = [] { return RackController::AudioStatus{48000.0, 256, 3, 4}; };
        c.quit          = [this] { ++quits; };
        c.virtualInputs = {{"MIDI 2.0 In", MidiProtocol::Midi2}, {"MIDI 1.0 In", MidiProtocol::Midi1}};
        return c;
    }

    ~Rig() {
        session->unload();
        runBlock(*engine);
        engine->collect();
    }
};

// A scratch folder under the system's temp folder, as its real path: on macOS
// the temp folder is under /var, a symlink to /private/var, and racks and
// setlists are kept by their real paths.
std::filesystem::path scratchDir(const char* name) {
    return std::filesystem::canonical(std::filesystem::temp_directory_path()) / name;
}

Rack testRack() {
    Rack rack;
    rack.name  = "Test";
    RackSlot a = slot("a", "synth", "/clap/Synth.clap");
    a.source   = MidiSourceRef{MidiSourceRef::Kind::Device, "Keys:Keys MIDI 1"};
    RackSlot b = slot("b", "missing");
    RackSlot c = slot("c", "bass", "/clap/Bass.clap");
    c.muted    = true;
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
    CHECK(c.state == RowState::Running && c.muted);   // muted rows run (until quiet)
    CHECK(d.sourceProtocol == SourceProtocol::Midi2 && d.protocolWarning);   // MIDI 2.0 into a MIDI 1.0 plugin
    CHECK(!e.sourceConnected && e.sourceLabel == "Unplugged:Port (not connected)");

    // The source menu: All, No live input, hardware before software, then the
    // virtual inputs (labelled as what they are: where other apps send). No
    // sequences: a sequence chooses its rows.
    CHECK(view.sources.size() == 6);
    if (view.sources.size() == 6) {
        CHECK(view.sources[0].ref.kind == MidiSourceRef::Kind::All);
        CHECK(view.sources[1].ref.kind == MidiSourceRef::Kind::None && view.sources[1].label == "No live input");
        CHECK(view.sources[2].label == "Keys" && view.sources[2].hardware);
        CHECK(view.sources[3].label == "Midi Through" && !view.sources[3].hardware);
        CHECK(view.sources[4].label == "From other apps (MIDI 2.0)" && view.sources[4].protocol == SourceProtocol::Midi2);
        CHECK(view.sources[5].label == "From other apps (MIDI 1.0)" && view.sources[5].ref.name == "MIDI 1.0 In");
    }
    CHECK(view.status.sampleRate == 48000.0 && view.status.bufferSize == 256 && view.status.xruns == 3);
    CHECK(view.status.outputs == 4);
    CHECK(view.status.saveTarget.empty() && view.status.noSaveReason == "Untitled set: File > Save As keeps it");

    // Edits show up in the next model; structural ones are committed by tick().
    MidiFilter lower;
    lower.noteHigh = 59;
    controller.setFilter("a", lower);
    controller.setGainDb("a", -6.0f);
    controller.setMuted("c", true);
    controller.setFirstOutput("a", 2);
    // The model shows edits at once, before the next tick (the UI builds on them).
    CHECK(view.rows[0].filter.noteHigh == 59 && view.rows[0].gainDb == -6.0f && view.rows[2].muted);
    CHECK(view.rows[0].firstOutput == 2);
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

// A row's MIDI effects (docs/midi-effects.md): in the view model, added (or
// refused, with a notice), bypassed, moved, their editors (closed with the
// effect or its row), and idled with the rest.
void testMidiEffects() {
    Rig rig;
    rig.factory.installed.push_back(installedPlugin("retune", "/clap/Retune.clap"));
    rig.factory.noteEffects = {"retune"};
    rig.factory.withEditor  = {"retune"};
    Session::LoadReport report;
    rig.session->load(testRack(), 48000.0, report);
    RackController controller(*rig.session, *rig.engine, rig.midi, rig.factory, rig.config());

    const PluginDescriptor retune = rig.factory.installed.back();
    controller.addMidiEffect("a", retune);
    controller.addMidiEffect("a", retune);
    controller.addMidiEffect("b", retune);            // a placeholder row: refused
    controller.addMidiEffect("a", rig.factory.installed[0]);   // an instrument: refused
    CHECK(controller.takeNotices().size() == 2);
    controller.tick(1.0);
    const RowModel& a = controller.view().rows[0];
    CHECK(a.midiEffects.size() == 2);
    if (a.midiEffects.size() != 2) return;
    CHECK(a.midiEffects[0].id == "retune" && a.midiEffects[1].id == "retune-2" && a.midiEffects[0].name == "retune");
    CHECK(a.midiEffects[0].hasEditor && !a.midiEffects[0].editorOpen && !a.midiEffects[0].bypassed);
    CHECK(controller.view().modified);

    controller.setMidiEffectBypassed("a", "retune", true);
    controller.moveMidiEffect("a", "retune-2", -1);
    controller.openMidiEffectEditor("a", "retune");
    controller.openMidiEffectEditor("a", "retune");   // already open: no second window
    CHECK(*rig.editorsOpen == 1);
    controller.tick(1.1);
    const RowModel& moved = controller.view().rows[0];
    CHECK(moved.midiEffects.size() == 2 && moved.midiEffects[0].id == "retune-2" && moved.midiEffects[1].bypassed &&
          moved.midiEffects[1].editorOpen);

    // The editor's title says it's bypassed, since the plugin can't tell, and follows the bypass.
    FakeEditor* window = rig.editors.empty() ? nullptr : rig.editors.back();
    CHECK(window != nullptr && window->title.find(" (bypassed) (StageHand)") != std::string::npos &&
          window->retitled.empty());
    controller.setMidiEffectBypassed("a", "retune", false);
    controller.tick(1.15);
    controller.tick(1.2);   // unchanged: not retitled again
    CHECK(window != nullptr && window->retitled.size() == 1 &&
          window->retitled[0].find("(bypassed)") == std::string::npos &&
          window->retitled[0].find(" (StageHand)") != std::string::npos);
    controller.setMidiEffectBypassed("a", "retune", true);
    controller.tick(1.25);
    CHECK(window != nullptr && window->retitled.size() == 2 && window->retitled[1] == window->title &&
          window->title.find(" (bypassed) (StageHand)") != std::string::npos);

    FakeNoteEffect* effect = rig.factory.createdEffects["retune"];   // the last made: retune-2
    const int idles = effect != nullptr ? effect->idles : 0;
    controller.service();
    CHECK(effect != nullptr && effect->idles == idles + 1);

    controller.removeMidiEffect("a", "retune");   // closes its editor first
    CHECK(*rig.editorsOpen == 0);
    controller.openMidiEffectEditor("a", "retune-2");
    CHECK(*rig.editorsOpen == 1);
    controller.removeSlot("a");                   // its effects' editors close with it
    CHECK(*rig.editorsOpen == 0);
}

ParameterInfo param(std::uint32_t id, const char* name, double low, double high, double def, bool stepped = false) {
    ParameterInfo p;
    p.id           = id;
    p.name         = name;
    p.minValue     = low;
    p.maxValue     = high;
    p.defaultValue = def;
    p.isStepped    = stepped;
    return p;
}

// The generic editor (docs/generic-editor.md): offered (and opened) only for a
// plugin with no editor of its own and parameters to show, never by
// openAllEditors(); its model's rows, kinds, snapping and read-back; its
// changes reach the running plugin and modify the rack.
void testGenericEditor() {
    Rig rig;
    rig.factory.withEditor = {"bass"};   // synth and old have none
    ParameterInfo hidden = param(9, "Internal", 0, 1, 0);
    hidden.isHidden = true;
    ParameterInfo meter = param(8, "Level", 0, 1, 0);
    meter.isReadOnly = true;
    rig.factory.params["synth"] = {param(0, "Cutoff", 20, 20000, 1000), param(1, "Mono", 0, 1, 0, true),
                                   param(2, "Wave", 0, 3, 0, true), param(3, "Divisions", 1, 96, 12, true), hidden, meter};
    rig.factory.params["old"] = {hidden};   // nothing to show
    Session::LoadReport report;
    rig.session->load(testRack(), 48000.0, report);
    RackController controller(*rig.session, *rig.engine, rig.midi, rig.factory, rig.config());
    controller.tick(1.0);
    const auto& rows = controller.view().rows;   // a synth, b missing, c bass, d old, e synth
    CHECK(rows[0].hasEditor && rows[2].hasEditor && !rows[3].hasEditor);

    controller.openAllEditors();   // the plugins' own only
    CHECK(*rig.editorsOpen == 1 && rig.genericFor.empty());
    controller.openEditor("d");    // nothing to show: nothing opens
    CHECK(*rig.editorsOpen == 1 && rig.genericFor.empty());
    controller.openEditor("a");
    CHECK(*rig.editorsOpen == 2 && rig.genericFor.size() == 1 && rig.genericFor[0] == rig.session->slot("a")->plugin.get());
    controller.tick(1.1);
    CHECK(controller.view().rows[0].editorOpen);

    // The model.
    auto* synth = dynamic_cast<FakePlugin*>(rig.session->slot("a")->plugin.get());
    CHECK(synth != nullptr);
    if (synth == nullptr) return;
    synth->values[0] = 440.0;
    ParameterEditor editor(controller, *synth);
    const auto& list = editor.rows();
    CHECK(list.size() == 5);   // not the hidden one
    if (list.size() != 5) return;
    CHECK(list[0].kind == ParameterEditor::Kind::Slider && list[0].value == 440.0 && list[0].text == "440");
    CHECK(list[1].kind == ParameterEditor::Kind::Switch && list[2].kind == ParameterEditor::Kind::Menu);
    CHECK(list[3].kind == ParameterEditor::Kind::Slider && list[4].info.isReadOnly);
    CHECK(editor.choices(2).size() == 4 && editor.choices(2)[3].second == "3");

    rig.session->markSaved();
    editor.set(3, 31.4);       // snapped
    editor.set(0, 99999.0);    // clamped
    editor.set(4, 0.5);        // read-only: not set
    CHECK(list[3].value == 31.0 && list[3].text == "31" && list[0].value == 20000.0 && list[4].value == 0.0);
    CHECK(rig.session->modified());
    runBlock(*rig.engine);
    const auto sent = [&](std::uint32_t id) {
        for (const auto& call : synth->calls)
            for (const Event& e : call.events)
                if (e.type == EventType::ParamValue && e.param.id == id) return e.value;
        return -1.0;
    };
    CHECK(sent(3) == 31.0 && sent(0) == 20000.0 && sent(8) == -1.0);

    // Read back from the plugin once the change has had time to land.
    synth->values[3] = 19.0;
    editor.sync();
    CHECK(list[3].value == 31.0);   // just set: not yet
    for (int i = 0; i < 4; ++i) editor.sync();
    CHECK(list[3].value == 19.0 && list[3].text == "19");
    editor.reset(3);
    CHECK(list[3].value == 12.0);

    double v = 0;
    CHECK(ParameterEditor::parseValue("0.25", v) && v == 0.25 && !ParameterEditor::parseValue("abc", v));
    CHECK(ParameterEditor::editable(*synth) && !ParameterEditor::editable(*rig.session->slot("d")->plugin));
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

void testTransport() {
    Rig                 rig;
    Session::LoadReport report;
    rig.session->load(Rack{}, 48000.0, report);
    RackController controller(*rig.session, *rig.engine, rig.midi, rig.factory, rig.config());
    const RackViewModel& view = controller.view();
    CHECK(view.transport.tempoBpm == 120.0 && view.transport.beatsInBar == 4);

    // Taps half a second apart: 120 BPM. A 2 s gap starts a new count.
    for (double t : {10.0, 10.5, 11.0, 11.5}) controller.tapTempo(t);
    CHECK(view.transport.tempoBpm == 120.0);
    controller.tapTempo(20.0);                        // new count: one tap, no change
    CHECK(view.transport.tempoBpm == 120.0);
    controller.tapTempo(20.4);                        // 0.4 s: 150 BPM
    CHECK(view.transport.tempoBpm == 150.0);
    for (double t : {20.8, 21.2, 21.6, 22.0, 22.6}) controller.tapTempo(t);   // the last four intervals only
    CHECK(std::fabs(view.transport.tempoBpm - 60.0 / 0.45) < 0.051);          // (0.4+0.4+0.4+0.6)/4

    controller.setTempo(97.26);                       // rounded to 0.1
    CHECK(std::fabs(view.transport.tempoBpm - 97.3) < 1e-9);
    controller.setTimeSignature(6, 8);
    controller.setTimeSignature(6, 7);                // refused
    controller.tick(1.0);
    CHECK(view.transport.numerator == 6 && view.transport.denominator == 8 && view.transport.beatsInBar == 2);
    // Compound meters count dotted beats; others count their notes.
    for (const auto& [n, d, lights] : {std::tuple{7, 8, 7}, {9, 8, 3}, {12, 8, 4}, {3, 8, 3}, {5, 4, 5}, {2, 4, 2}}) {
        controller.setTimeSignature(n, d);
        controller.tick(1.1);
        CHECK(view.transport.beatsInBar == lights);
    }
    controller.setTimeSignature(6, 8);

    // The beat lights follow the engine's clock: 640 frames at 97.3 BPM is 0.02
    // beats, still the lit start of the first beat.
    rig.engine->sendTransportCommand(Engine::TransportCommand::Start);
    for (int i = 0; i < 10; ++i) runBlock(*rig.engine);
    controller.tick(1.2);
    CHECK(std::fabs(view.transport.beatPosition - 640.0 * 97.3 / 60.0 / 48000.0) < 1e-9);
    CHECK(view.transport.beatInBar == 0 && view.transport.onBeat);
    CHECK(view.modified);

    // The count-in (docs/perform.md), saved with the rack: half a bar of 4/4
    // at 120 BPM counts "3, 4" (a beat: 375 of runBlock's 64 frames), the
    // countdown and the lights showing it, then bar 1.
    rig.engine->sendTransportCommand(Engine::TransportCommand::Stop);
    runBlock(*rig.engine);
    controller.setTempo(120.0);
    controller.setTimeSignature(4, 4);
    controller.setCountIn(0.7);   // rounded to the nearest choice below a bar: half
    controller.tick(1.3);
    CHECK(view.transport.countInBars == 0.5 && rig.session->capture().metronome.countInBars == 0.5);
    rig.engine->sendTransportCommand(Engine::TransportCommand::Start);
    for (int i = 0; i < 10; ++i) runBlock(*rig.engine);
    controller.tick(1.4);
    CHECK(!view.transport.running && view.transport.countIn == 2);
    CHECK(view.transport.beatInBar == 2 && view.transport.onBeat);   // the third light: "3"
    for (int i = 0; i < 375; ++i) runBlock(*rig.engine);
    controller.tick(1.5);
    CHECK(view.transport.countIn == 1 && view.transport.beatInBar == 3 && view.transport.onBeat);   // "4"
    for (int i = 0; i < 375; ++i) runBlock(*rig.engine);
    controller.tick(1.6);
    CHECK(view.transport.running && view.transport.countIn == 0 && view.transport.beatInBar == 0);   // bar 1
    controller.setCountIn(0.0);
    CHECK(rig.session->capture().metronome.countInBars == 0.0);

    // The edge flash (docs/perform.md, decision 13): the rack's.
    controller.setFlash(true);
    controller.tick(1.7);
    CHECK(view.transport.flash && rig.session->capture().metronome.flash);
    // The beat heard now, from the engine: running, past bar 1's downbeat.
    const BeatNow heard = controller.heardBeat(rig.engine->transport().at);
    CHECK(heard.state == BeatNow::State::Running && heard.bar == 1);
}

void testDock() {
    Rig rig;
    rig.session->addOpenSource(MidiSourceRef{MidiSourceRef::Kind::Virtual, "MIDI 2.0 In"}, 7);
    Rack     rack;
    Sequence arp;
    arp.id       = "arp";
    arp.name     = "Arp";
    arp.protocol = MidiProtocol::Midi1;
    arp.launch   = LaunchQuantize::Immediate;
    arp.length   = 8;
    arp.notes    = {{0, 1, 60, 100}, {4, 2, 64, 100}, {4, 1, 67, 100}};
    rack.sequences = {arp};
    arp.targets    = {"lead"};
    rack.sequences = {arp};
    RackSlot lead  = slot("lead", "synth", "/clap/Synth.clap");
    lead.name      = "Lead";
    lead.source    = MidiSourceRef{MidiSourceRef::Kind::None, {}};   // plays only the sequence
    rack.slots     = {lead};
    Session::LoadReport report;
    rig.session->load(rack, 48000.0, report);
    RackController controller(*rig.session, *rig.engine, rig.midi, rig.factory, rig.config());
    const RackViewModel& view = controller.view();

    // The sequence row: settings, the mini-map's starts, who plays it.
    CHECK(view.sequences.size() == 1);
    if (view.sequences.empty()) return;
    const SequenceRowModel& row = view.sequences[0];
    CHECK(row.name == "Arp" && row.protocol == MidiProtocol::Midi1 && row.length == 8 && row.noteCount == 3);
    CHECK((row.noteStarts == std::vector<std::uint32_t>{0, 4}) && row.lastNoteEnd == 6);
    CHECK(row.usedBy == std::vector<std::string>{"Lead"} && row.state == SequenceState::Stopped);
    CHECK(row.targetIds == std::vector<std::string>{"lead"});
    CHECK(view.rows[0].sourceLabel == "No live input" && view.rows[0].sourceConnected);

    // Not in the rows' source menu: the sequence chooses its rows.
    for (const SourceChoice& choice : view.sources) CHECK(choice.ref.kind != MidiSourceRef::Kind::Sequence);

    // Play/Stop toggles; the state follows the engine.
    controller.togglePlay("arp");
    runBlock(*rig.engine);
    controller.tick(1.0);
    CHECK(view.sequences[0].state == SequenceState::Playing && view.sequences[0].position > 0.0);
    controller.togglePlay("arp");   // Immediate: stops at once
    runBlock(*rig.engine);
    controller.tick(1.1);
    CHECK(view.sequences[0].state == SequenceState::Stopped);

    // Settings; the length can't cut notes off.
    controller.setSequenceRate("arp", SequenceRate{1, 2});
    controller.setSequenceLaunch("arp", LaunchQuantize::Beat);
    controller.setSequenceRepeat("arp", false);
    controller.setSequenceLength("arp", 5);   // notes reach unit 6: refused
    const std::vector<std::string> notices = controller.takeNotices();
    CHECK(notices.size() == 1 && notices[0].find("unit 6") != std::string::npos);
    controller.setSequenceLength("arp", 6);
    controller.tick(1.2);
    CHECK(view.sequences[0].rate.denominator == 2 && view.sequences[0].launch == LaunchQuantize::Beat);
    CHECK(!view.sequences[0].repeat && view.sequences[0].length == 6);
    controller.setSequenceSwing("arp", 0.9);   // held to 0.5-0.75 and 0-50 ms
    controller.setSequenceSlop("arp", -3.0);
    CHECK(controller.sequence("arp")->swing == 0.75 && controller.sequence("arp")->slopMs == 0.0);
    controller.setSequenceSwing("arp", 0.62);
    controller.setSequenceSlop("arp", 12.0);
    CHECK(controller.sequence("arp")->swing == 0.62 && controller.sequence("arp")->slopMs == 12.0);

    // Adding (into a row, or nowhere yet), aiming, a new row for one, and removing.
    const std::string id = controller.addSequence(MidiProtocol::Midi2, "lead");
    controller.tick(1.3);
    CHECK(id == "seq" && view.sequences.size() == 2 && view.sequences[1].protocol == MidiProtocol::Midi2);
    CHECK(view.sequences[1].targetIds == std::vector<std::string>{"lead"});
    const std::string loose = controller.addSequence(MidiProtocol::Midi1);
    controller.tick(1.31);
    CHECK(view.sequences[2].usedBy.empty());
    controller.setSequenceTarget(loose, "lead", true);
    controller.setSequenceTarget(loose, "lead", true);   // once only
    controller.tick(1.32);
    CHECK(view.sequences[2].targetIds == std::vector<std::string>{"lead"});
    controller.setSequenceTarget(loose, "lead", false);
    controller.tick(1.33);
    CHECK(view.sequences[2].usedBy.empty());
    const std::string newRow = controller.addRowForSequence(loose, PluginDescriptor{PluginFormat::Clap, "synth", "Synth", "", "", "/clap/Synth.clap"});
    controller.tick(1.34);
    CHECK(!newRow.empty() && view.rows.size() == 2 && view.sequences[2].targetIds == std::vector<std::string>{newRow});
    controller.removeSequence("arp");
    controller.tick(1.4);
    CHECK(view.sequences.size() == 2 && view.rows[0].sourceConnected);   // its row just stops hearing it

    // The inputs row: one chip per open input, lit briefly by its messages.
    CHECK(view.inputs.size() == 1 && view.inputs[0].label == "From other apps (MIDI 2.0)" && view.inputs[0].isVirtual);
    CHECK(!view.inputs[0].active);
    TimedMidiMessage m;
    m.captureTime       = 1'000'000'000 - 1'000'000;
    m.message.words[0]  = 0x20903C64;
    m.message.wordCount = 1;
    rig.engine->onMidi(7, m);
    runBlock(*rig.engine);
    controller.tick(1.5);
    CHECK(view.inputs[0].active);
    controller.tick(1.8);
    CHECK(!view.inputs[0].active);
}

// MIDI learn: the next press becomes the sequence's trigger for the action.
void testLearn() {
    Rig rig;
    rig.session->addOpenSource(MidiSourceRef{MidiSourceRef::Kind::Virtual, "MIDI 2.0 In"}, 7);
    Rack     rack;
    Sequence arp;
    arp.id       = "arp";
    arp.name     = "Arp";
    arp.protocol = MidiProtocol::Midi1;
    arp.launch   = LaunchQuantize::Immediate;
    rack.sequences = {arp};
    Session::LoadReport report;
    rig.session->load(rack, 48000.0, report);
    RackController controller(*rig.session, *rig.engine, rig.midi, rig.factory, rig.config());
    const RackViewModel& view = controller.view();

    auto send = [&](std::uint32_t word) {
        TimedMidiMessage m;
        m.captureTime       = 1'000'000'000 - 1'000'000;
        m.message.words[0]  = word;
        m.message.wordCount = 1;
        rig.engine->onMidi(7, m);
        runBlock(*rig.engine);
    };

    controller.learnTrigger("arp", SequenceTrigger::Action::Toggle);
    controller.tick(1.0);
    CHECK(view.sequences[0].learning && view.sequences[0].triggers.empty());
    send(0x20993064);   // note 48 (C3) on channel 10
    controller.tick(1.1);
    CHECK(!view.sequences[0].learning);
    CHECK(view.sequences[0].triggers == std::vector<std::string>{"Play/Stop: From other apps (MIDI 2.0), note C3, ch 10"});
    const std::vector<SequenceTrigger>& triggers = controller.sequence("arp")->triggers;
    CHECK(triggers.size() == 1 && triggers[0].source == (MidiSourceRef{MidiSourceRef::Kind::Virtual, "MIDI 2.0 In"}));
    CHECK(controller.takeNotices().size() == 1 && rig.session->modified());

    // The new trigger works at once.
    runBlock(*rig.engine);
    send(0x20993064);
    CHECK(rig.session->sequenceState("arp") == SequenceState::Playing);

    // A Stop learned on the same pad replaces its Play/Stop; a controller press
    // (value 127) is learned as a controller.
    controller.learnTrigger("arp", SequenceTrigger::Action::Stop);
    send(0x20993064);
    controller.tick(1.2);
    controller.learnTrigger("arp", SequenceTrigger::Action::Start);
    send(0x20B0407F);
    controller.tick(1.3);
    CHECK((view.sequences[0].triggers ==
           std::vector<std::string>{"Stop: From other apps (MIDI 2.0), note C3, ch 10", "Start: From other apps (MIDI 2.0), CC 64, ch 1"}));

    // Removing one, and cancelling a learn.
    controller.removeTrigger("arp", 0);
    controller.learnTrigger("arp", SequenceTrigger::Action::Toggle);
    controller.cancelLearn();
    send(0x20903C64);
    controller.tick(1.4);
    CHECK(!view.sequences[0].learning &&
          view.sequences[0].triggers == std::vector<std::string>{"Start: From other apps (MIDI 2.0), CC 64, ch 1"});
}

// The transport's controls: Start/Stop, At Start, its MIDI learn, start on
// first note, and a rack switch stopping it.
void testTransportControls() {
    Rig rig;
    rig.session->addOpenSource(MidiSourceRef{MidiSourceRef::Kind::Virtual, "MIDI 2.0 In"}, 7);
    Rack     rack;
    Sequence arp;
    arp.id       = "arp";
    arp.name     = "Arp";
    arp.protocol = MidiProtocol::Midi1;
    rack.sequences = {arp};
    Session::LoadReport report;
    rig.session->load(rack, 48000.0, report);
    RackController       controller(*rig.session, *rig.engine, rig.midi, rig.factory, rig.config());
    const RackViewModel& view = controller.view();
    CHECK(!view.transport.running && !view.transport.onBeat);   // StageHand starts stopped

    controller.setSequenceAtStart("arp", true);
    controller.tick(1.0);
    CHECK(view.sequences[0].atStart);
    controller.toggleTransport();
    runBlock(*rig.engine);
    controller.tick(1.1);
    CHECK(view.transport.running && view.sequences[0].state == SequenceState::Playing);   // At Start
    controller.toggleTransport();
    runBlock(*rig.engine);
    controller.tick(1.2);
    CHECK(!view.transport.running && view.sequences[0].state == SequenceState::Stopped);

    // Learn a Start/Stop pad for the transport; it then works.
    auto send = [&](std::uint32_t word) {
        TimedMidiMessage m;
        m.captureTime       = 1'000'000'000 - 1'000'000;
        m.message.words[0]  = word;
        m.message.wordCount = 1;
        rig.engine->onMidi(7, m);
        runBlock(*rig.engine);
    };
    controller.learnTransportTrigger(SequenceTrigger::Action::Toggle);
    controller.tick(1.3);
    CHECK(view.transport.learning);
    send(0x20993064);
    controller.tick(1.4);
    CHECK(!view.transport.learning &&
          view.transport.triggers == std::vector<std::string>{"Play/Stop: From other apps (MIDI 2.0), note C3, ch 10"});
    runBlock(*rig.engine);
    send(0x20993064);
    controller.tick(1.5);
    CHECK(view.transport.running);
    controller.removeTransportTrigger(0);
    controller.tick(1.6);
    CHECK(view.transport.triggers.empty());

    // Start on first note, from one input; a rack switch stops the transport.
    controller.setStartOnNote(true, MidiSourceRef{MidiSourceRef::Kind::Virtual, "MIDI 2.0 In"});
    controller.tick(1.7);
    CHECK(view.transport.startOnNote == "From other apps (MIDI 2.0)" && view.transport.startOnNoteConnected);
    CHECK(rig.session->capture().transport.startOnNote);
    controller.newSet();
    runBlock(*rig.engine);
    controller.tick(1.8);
    CHECK(!view.transport.running && view.transport.startOnNote.empty());
}

// A stand-in for the backend's click routing: devices come and go.
struct FakeRouting final : AuxRouting {
    std::vector<PlaybackDevice> devices;
    std::string                 connected;   // "" = nowhere
    bool                        changed = false;
    std::vector<PlaybackDevice> playbackDevices() override { return devices; }
    bool connectAux(const PlaybackDevice& d) override {
        connected = d.name;
        return true;
    }
    void disconnectAux() override { connected.clear(); }
    bool takeDevicesChanged() override { return std::exchange(changed, false); }
    std::string                 main;   // where the mains go
    std::string mainDevice() override { return main; }
};

void testFindPlaybackDevice() {
    auto dev = [](std::initializer_list<const char*> names) {
        std::vector<PlaybackDevice> list;
        for (const char* n : names) list.push_back(PlaybackDevice{n, {std::string(n) + ":playback_FL"}});
        return list;
    };
    const auto both = dev({"Built-in Audio Analog Stereo", "Scarlett 2i2 USB Analog Stereo"});
    CHECK(findPlaybackDevice("Scarlett 2i2 USB Analog Stereo", both) == 1);                 // by name
    CHECK(findPlaybackDevice("Scarlett 2i2 USB Analog Stereo 2", both) == 1);               // numbered twin
    CHECK(findPlaybackDevice("Scarlett 2i2 USB Pro", both) == 1);                           // another profile
    CHECK(findPlaybackDevice("Built-in Audio Analog Surround 5.1", both) == 0);
    CHECK(findPlaybackDevice("MOTU M4 Analog Stereo", both) == -1);                         // not there
    CHECK(findPlaybackDevice("", both) == -1);
    // Never analog -> HDMI (it could be the PA), and no guessing between two.
    CHECK(findPlaybackDevice("Built-in Audio Analog Stereo", dev({"Built-in Audio Digital Stereo (HDMI)"})) == -1);
    CHECK(findPlaybackDevice("Scarlett 2i2 USB Pro", dev({"Scarlett 2i2 USB Analog Stereo", "Scarlett 2i2 USB Analog Surround 4.0"})) == -1);
}

// The click goes only to its saved output; a missing one is reported once,
// never replaced by the mains, and connected when it's plugged in.
void testClickRouting() {
    FakeRouting routing;
    routing.devices = {PlaybackDevice{"Built-in Audio Analog Stereo", {"a:FL", "a:FR"}}};
    Rig rig;
    Rack rack;
    rack.metronome.click       = true;
    rack.metronome.clickOutput = "Scarlett 2i2 USB Analog Stereo";
    Session::LoadReport report;
    rig.session->load(rack, 48000.0, report);
    RackController::Config config = rig.config();
    config.clickRouting           = &routing;
    RackController       controller(*rig.session, *rig.engine, rig.midi, rig.factory, config);
    const RackViewModel& view = controller.view();
    // An older rack's own click output goes to the rig (every song's), and the rack drops it.
    CHECK(rig.session->clickOutput() == "Scarlett 2i2 USB Analog Stereo" && rig.session->rigModified() &&
          rig.session->capture().metronome.clickOutput.empty() && rig.session->capture().metronome.click);
    CHECK(routing.connected.empty() && !view.transport.clickConnected);
    std::vector<std::string> notices = controller.takeNotices();
    CHECK(notices.size() == 1 && notices[0].find("isn't connected") != std::string::npos);
    controller.tick(1.0);
    CHECK(controller.takeNotices().empty());   // said once

    routing.devices.push_back(PlaybackDevice{"Scarlett 2i2 USB Analog Stereo", {"s:FL", "s:FR"}});
    routing.changed = true;   // plugged in
    controller.tick(1.1);
    CHECK(routing.connected == "Scarlett 2i2 USB Analog Stereo" && view.transport.clickConnected);
    CHECK(controller.takeNotices().size() == 1);

    // Off: disconnected. A chosen output connects.
    controller.setClick(false);
    CHECK(routing.connected.empty());
    controller.setClickOutput("Built-in Audio Analog Stereo");   // choosing where it plays turns it on
    CHECK(rig.session->metronome().click);
    controller.setClick(false);
    controller.setClickLevel(-18.0f);   // so does choosing a level
    CHECK(rig.session->metronome().click && rig.session->metronome().clickLevelDb == -18.0f &&
          routing.connected == "Built-in Audio Analog Stereo");
    CHECK(routing.connected == "Built-in Audio Analog Stereo");
    controller.setMetronomeCount(true);
    controller.tick(1.2);
    CHECK(view.transport.everyNote && rig.session->capture().metronome.everyNote);
    controller.refreshPlaybackDevices();
    CHECK(view.transport.playbackDevices.size() == 2);

    // Turned on with no output chosen: a device other than the mains', never
    // the mains' own.
    routing.main = "Scarlett 2i2 USB Analog Stereo";
    controller.setClick(false);
    controller.setClickOutput("");
    controller.takeNotices();
    controller.setClick(true);
    CHECK(routing.connected == "Built-in Audio Analog Stereo" &&
          rig.session->clickOutput() == "Built-in Audio Analog Stereo");
    routing.devices.erase(routing.devices.begin());   // only the mains' device left
    controller.setClick(false);
    controller.setClickOutput("");
    controller.setClick(true);
    CHECK(routing.connected.empty() && rig.session->clickOutput().empty());
}

// Renaming a sequence: trimmed, never empty, at most 64 bytes (cut on a
// character), the id kept, and the source menus follow.
void testRenameSequence() {
    Rig      rig;
    Rack     rack;
    Sequence arp;
    arp.id         = "arp";
    arp.name       = "Arp";
    rack.sequences = {arp};
    rack.sequences[0].targets = {"lead"};
    RackSlot lead  = slot("lead", "synth", "/clap/Synth.clap");
    rack.slots     = {lead};
    Session::LoadReport report;
    rig.session->load(rack, 48000.0, report);
    RackController       controller(*rig.session, *rig.engine, rig.midi, rig.factory, rig.config());
    const RackViewModel& view = controller.view();

    controller.renameSequence("arp", "  Verse arp ");
    controller.tick(1.0);
    CHECK(view.sequences[0].name == "Verse arp" && view.sequences[0].id == "arp");
    CHECK(view.sequences[0].usedBy == std::vector<std::string>{"lead"});   // still plays into its row

    controller.takeNotices();
    controller.renameSequence("arp", "   ");
    CHECK(controller.sequence("arp")->name == "Verse arp" && controller.takeNotices().size() == 1);

    std::string longName(63, 'x');
    longName += "\xC3\xA9\xC3\xA9";   // "éé": the first é straddles byte 64
    controller.renameSequence("arp", longName);
    CHECK(controller.sequence("arp")->name == std::string(63, 'x'));
    CHECK(rig.session->modified());
}

// A row renamed in place: the id stays; an empty name is its plugin's again,
// and a name still its plugin's follows the plugin when it's replaced.
void testRenameRow() {
    Rig  rig;
    Rack rack;
    rack.slots = {slot("lead", "synth", "/clap/Synth.clap")};   // named "lead": renamed
    Session::LoadReport report;
    rig.session->load(rack, 48000.0, report);
    RackController       controller(*rig.session, *rig.engine, rig.midi, rig.factory, rig.config());
    const RackViewModel& view = controller.view();

    controller.renameSlot("lead", "  Pad ");
    CHECK(view.rows[0].name == "Pad" && view.rows[0].id == "lead");   // at once, before a tick
    controller.tick(1.0);
    CHECK(view.rows[0].name == "Pad" && rig.session->slot("lead")->saved.name == "Pad" && rig.session->modified());

    controller.replacePlugin("lead", installedPlugin("bass", "/clap/Bass.clap"));   // renamed: kept
    controller.tick(1.1);
    CHECK(view.rows[0].name == "Pad" && view.rows[0].pluginName == "bass");

    controller.renameSlot("lead", "   ");   // empty: its plugin's name
    CHECK(view.rows[0].name == "bass");
    controller.replacePlugin("lead", installedPlugin("synth", "/clap/Synth.clap"));   // not renamed: follows
    controller.tick(1.2);
    CHECK(view.rows[0].name == "synth" && rig.session->capture().slots[0].name == "synth");
}

// A set package at `path` holding `racks` (by their names), its songs naming
// them in order, with `setRig` as its rig.
std::string makeSet(const std::filesystem::path& path, const std::vector<Rack>& racks,
                    const std::vector<std::string>& songs, const stagehand::Rig& setRig = {}) {
    Set set;
    for (const std::string& name : songs) set.songs.push_back(SetSong{name, ""});
    std::map<std::string, Rack> byName;
    for (const Rack& rack : racks) byName.emplace(rack.name, rack);
    CHECK(saveSet(path.string(), set, &setRig, byName).ok);
    return path.string();
}

// An older setlist file as a set package beside it ("Gig.setlist" -> "Gig
// set.setlist"), its racks and rig copied in (none named: an empty rig).
std::string packageOf(const std::string& file) {
    Setlist   old;
    LoadedSet made;
    CHECK(loadSetlist(file, old).ok && convertSetlist(file, made).ok);
    if (old.rig.empty()) made.rig = stagehand::Rig{};
    const std::string package = file.substr(0, file.size() - 8) + " set.setlist";
    CHECK(saveSet(package, made.set, &made.rig, made.racks).ok);
    return package;
}

Rack keysRack(const std::string& name) {
    Rack rack;
    rack.name  = name;
    rack.slots = {slot("keys", "synth", "/clap/Synth.clap")};
    return rack;
}

void testSave() {
    const std::filesystem::path dir = scratchDir("stagehand-controller-test");
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    Rack rack;
    rack.name  = "Saved";
    rack.slots = {slot("a", "synth", "/clap/Synth.clap")};
    const std::string set = makeSet(dir / "Saved.setlist", {rack}, {"Saved"});

    Rig                 rig;
    Session::LoadReport report;
    rig.session->load(rack, 48000.0, report);
    RackController controller(*rig.session, *rig.engine, rig.midi, rig.factory, rig.config());
    CHECK(!controller.save());   // untitled
    std::vector<std::string> notices = controller.takeNotices();
    CHECK(notices.size() == 1 && notices[0] == "Not saved: this set is untitled; use Save As");

    CHECK(controller.openSet(set));
    controller.takeNotices();
    controller.setMuted("a", true);
    CHECK(rig.session->modified() && controller.unsavedChanges());
    CHECK(controller.save());
    CHECK(!rig.session->modified() && !controller.unsavedChanges());
    CHECK(controller.takeNotices() == std::vector<std::string>{"Saved the set to " + set});

    Rack loaded;
    CHECK(loadRack(setRackPath(set, "Saved"), loaded).ok);
    CHECK(loaded.slots.size() == 1 && loaded.slots[0].muted);
    std::filesystem::remove_all(dir);
}

// New, Open and Save As (docs/documents.md): the set replaced while running,
// nothing saved by it, an older setlist or a lone rack opened as an untitled
// set, and what Save As will and won't replace.
void testFiles() {
    namespace fs = std::filesystem;
    const fs::path dir = scratchDir("stagehand-files-test");
    fs::remove_all(dir);
    fs::create_directories(dir / "notes.setlist");   // exists, but isn't a set

    Rack gig  = keysRack("Gig");
    Sequence arp;
    arp.id        = "arp";
    gig.sequences = {arp};
    const std::string gigSet = makeSet(dir / "Gig.setlist", {gig}, {"Gig"});
    CHECK(saveRack((dir / "Lone.rack").string(), keysRack("Lone")).ok);

    Rig                 rig;
    Session::LoadReport report;
    Rack                untitled;
    untitled.name = "Song";
    rig.session->load(untitled, 48000.0, report);
    RackController       controller(*rig.session, *rig.engine, rig.midi, rig.factory, rig.config());
    const RackViewModel& view = controller.view();
    CHECK(controller.setPath().empty() && controller.songRack() == "Song" && !controller.unsavedChanges());

    // Changes to the untitled set are what the UI asks about.
    controller.addPlugin(installedPlugin("bass", "/clap/Bass.clap"), 0);
    CHECK(controller.unsavedChanges());

    // Open a set: editors close; the generation moves.
    controller.tick(0.5);
    controller.openEditor(view.rows[0].id);
    CHECK(*rig.editorsOpen == 1);
    const std::uint64_t before = controller.rackGeneration();
    CHECK(controller.openSet(gigSet));
    controller.tick(1.0);
    CHECK(*rig.editorsOpen == 0 && controller.rackGeneration() == before + 1);
    CHECK(controller.setPath() == gigSet && view.rackName == "Gig" && !view.modified && !controller.unsavedChanges());
    CHECK(view.rows.size() == 1 && view.rows[0].id == "keys" && view.sequences.size() == 1);
    CHECK(view.setlist.name == "Gig" && view.setlist.songs.size() == 1 && view.setlist.current == 0);
    runBlock(*rig.engine);   // the old plugins go once the engine lets go of them
    rig.engine->collect();

    // Not a set: nothing changes.
    controller.takeNotices();
    CHECK(!controller.openSet((dir / "notes.setlist").string()));
    CHECK(controller.takeNotices().size() == 1 && controller.setPath() == gigSet);

    // New doesn't save the set left (the UI asks first): an untitled set, one empty song.
    controller.setMuted("keys", true);
    controller.newSet();
    controller.tick(1.1);
    CHECK(controller.setPath().empty() && view.rows.empty() && view.rackName == "Song" && !controller.unsavedChanges());
    Rack reread;
    CHECK(loadRack(setRackPath(gigSet, "Gig"), reread).ok && reread.slots.size() == 1 && !reread.slots[0].muted);

    // A lone rack opens as an untitled set of one song; so does an older setlist.
    CHECK(controller.openSet((dir / "Lone.rack").string()));
    controller.tick(1.2);
    CHECK(controller.setPath().empty() && controller.songRack() == "Lone" && view.rows.size() == 1);
    CHECK(controller.unsavedChanges());   // a set made from it, not saved yet
    Setlist old;
    old.songs = {SetlistSong{(dir / "Lone.rack").string(), ""}, SetlistSong{(dir / "Lone.rack").string(), "Again"}};
    CHECK(saveSetlist((dir / "Old.setlist").string(), old).ok);
    CHECK(controller.openSet((dir / "Old.setlist").string()));
    controller.tick(1.3);
    CHECK(controller.setPath().empty() && view.setlist.songs.size() == 2 && view.setlist.songs[1].name == "Again");

    // Save As: ".setlist" added. An existing set needs a yes; something that
    // isn't a set (the older setlist file too) is never replaced.
    CHECK(controller.saveAs((dir / "notes").string()) == RackController::SaveAsResult::Failed);
    CHECK(controller.saveAs((dir / "Old").string()) == RackController::SaveAsResult::Failed);
    CHECK(controller.saveAs(gigSet) == RackController::SaveAsResult::NeedsReplace);
    CHECK(controller.saveAs((dir / "Live").string()) == RackController::SaveAsResult::Saved);
    controller.tick(1.4);
    const std::string live = (dir / "Live.setlist").string();
    CHECK(controller.setPath() == live && !view.modified && !controller.unsavedChanges() && view.setlist.name == "Live");
    Set saved;
    CHECK(loadSet(live, saved).ok && saved.songs.size() == 2 && saved.songs[0].rack == "Lone" &&
          saved.songs[1].rack == "Lone" && saved.songs[1].name == "Again");
    CHECK(saveSetlist((dir / "Old.setlist").string(), old).ok);   // untouched: still readable as a setlist
    CHECK(controller.saveAs(gigSet, true) == RackController::SaveAsResult::Saved);
    CHECK(loadSet(gigSet, saved).ok && saved.songs.size() == 2 && !fs::exists(setRackPath(gigSet, "Gig")));

    // Saving off (--no-save): Save As refuses, and nothing counts as unsaved.
    RackController::Config off = rig.config();
    off.savingOff              = true;
    RackController readOnly(*rig.session, *rig.engine, rig.midi, rig.factory, off);
    CHECK(readOnly.saveAs((dir / "Other").string()) == RackController::SaveAsResult::Failed);
    readOnly.setMuted("keys", true);
    CHECK(!readOnly.unsavedChanges() && !fs::exists(dir / "Other.setlist"));

    rig.session->unload();
    runBlock(*rig.engine);
    rig.engine->collect();
    fs::remove_all(dir);
}

// Songs in a set (docs/setlists.md, docs/documents.md): going through it,
// editing it (unsaved until Save), a rack imported, a new song as a copy,
// removing songs (never the last), a missing rack skipped, the last position,
// and opening at a song.
void testSetlist() {
    namespace fs = std::filesystem;
    const fs::path dir = scratchDir("stagehand-setlist-test");
    fs::remove_all(dir);
    const std::string gig = makeSet(dir / "Gig.setlist", {keysRack("A")}, {"A"});
    CHECK(saveRack((dir / "Loose/B.rack").string(), keysRack("B")).ok);
    const std::string lastPath = (dir / "state/last.json").string();

    Rig                 rig;
    Session::LoadReport report;
    Rack                untitled;
    untitled.name = "Song";
    rig.session->load(untitled, 48000.0, report);
    RackController::Config config = rig.config();
    config.lastPositionPath       = lastPath;
    RackController       controller(*rig.session, *rig.engine, rig.midi, rig.factory, config);
    const RackViewModel& view = controller.view();
    double               now  = 0.0;
    auto tick = [&] { controller.tick(now += 2.0); };   // past the once-a-second disk check

    // The untitled set at start: one song, the rack running.
    tick();
    CHECK(view.setlist.open && view.setlist.name == "Untitled" && view.setlist.songs.size() == 1);
    CHECK(view.setlist.current == 0 && !view.setlist.edited);

    CHECK(controller.openSet(gig) && controller.songRack() == "A");
    tick();
    CHECK(view.setlist.name == "Gig" && view.setlist.songs.size() == 1 && view.setlist.current == 0);
    CHECK(view.setlist.next == -1 && !view.setlist.edited);

    // Import: a copy of the rack, after the current song, without switching.
    CHECK(controller.importRack((dir / "Loose/B.rack").string()));
    CHECK(!controller.importRack((dir / "Nothing").string()));
    tick();
    CHECK(view.setlist.songs.size() == 2 && view.setlist.songs[1].name == "B" && controller.songRack() == "A");
    CHECK(view.setlist.edited && controller.unsavedChanges());
    Set saved;
    CHECK(loadSet(gig, saved).ok && saved.songs.size() == 1);   // not saved yet

    // Next and Previous (B from memory), and the remembered position.
    CHECK(controller.nextSong() && controller.songRack() == "B");
    CHECK(!controller.nextSong());   // the end
    LastPosition last;
    CHECK(loadLastPosition(lastPath, last) && last.setlist == gig && last.song == 1);
    CHECK(controller.previousSong() && controller.songRack() == "A" && !controller.previousSong());

    // Again, move, rename: [A, A, B] -> [A, B, A]; B renamed, then back.
    CHECK(controller.addThisRackAgain());
    CHECK(controller.moveSong(2, -1));
    CHECK(!controller.moveSong(2, 1));
    CHECK(controller.renameSong(1, "  B (fast) "));
    tick();
    CHECK(view.setlist.songs.size() == 3 && view.setlist.songs[1].name == "B (fast)" &&
          view.setlist.songs[1].rackName == "B" && view.setlist.songs[2].name == "A");
    CHECK(view.setlist.current == 0 && view.setlist.next == 1);
    CHECK(controller.moveSong(0, 1));   // the playing song moves with it
    tick();
    CHECK(view.setlist.current == 1 && view.setlist.songs[0].name == "B (fast)");
    CHECK(controller.renameSong(0, "B"));   // the rack's own name: no override

    // Save: the song list and the imported rack.
    CHECK(controller.save());
    tick();
    CHECK(!view.setlist.edited && !controller.unsavedChanges());
    CHECK(loadSet(gig, saved).ok && saved.songs.size() == 3 && saved.songs[0] == (SetSong{"B", ""}) &&
          saved.songs[1].rack == "A" && saved.songs[2].rack == "A");
    CHECK(!rackBundleOf(setRackPath(gig, "B")).empty());

    // Remove the playing song: the next plays first (A again: the same rack,
    // so only the position moves).
    const std::uint64_t generation = controller.rackGeneration();
    CHECK(controller.removeSong(1));   // [B, A]
    tick();
    CHECK(view.setlist.current == 1 && controller.songRack() == "A" && controller.rackGeneration() == generation);
    // B removed and saved: its rack goes. The last song can't be removed.
    CHECK(controller.removeSong(0) && !controller.removeSong(0));   // [A]
    CHECK(controller.save() && !fs::exists(setRackPath(gig, "B")));

    // A new song: a copy of the one playing (with an edit not yet saved),
    // named for it, after it, and played.
    controller.setTempo(97.0);
    CHECK(controller.newSong());
    for (int i = 0; i < 4 && controller.songRack() != "A 2"; ++i) tick();   // its plugin prepared
    CHECK(controller.songRack() == "A 2" && rig.session->capture().tempoBpm == 97.0);
    tick();
    CHECK(view.setlist.songs.size() == 2 && view.setlist.songs[1].name == "A 2" && view.setlist.current == 1);
    CHECK(controller.save());
    Rack copied;
    CHECK(loadRack(setRackPath(gig, "A 2"), copied).ok && copied.tempoBpm == 97.0 && copied.slots.size() == 1);

    // A missing rack is shown and skipped.
    CHECK(controller.goToSong(0));
    fs::rename(setRackPath(gig, "A 2"), dir / "away.rack");
    tick();
    CHECK(view.setlist.songs[1].missing && view.setlist.next == -1 && !controller.goToSong(1));
    fs::rename(dir / "away.rack", setRackPath(gig, "A 2"));

    // Reopening at a song, as a start with no arguments does.
    CHECK(controller.openSet(gig, 1) && controller.songRack() == "A 2");
    CHECK(controller.openSet(gig, 9) && controller.songRack() == "A 2");   // clamped to the last
    CHECK(controller.openSet(gig, 0) && controller.songRack() == "A");
    CHECK(loadLastPosition(lastPath, last) && last.setlist == gig && last.song == 0);

    // A new set: untitled, so nothing reopens.
    controller.newSet();
    CHECK(controller.songRack() == "Song" && loadLastPosition(lastPath, last) && last.setlist.empty());

    runBlock(*rig.engine);
    rig.engine->collect();
    fs::remove_all(dir);
}

// Rigs (docs/rig.md, docs/documents.md): the set's own, in its package; an
// untitled set's empty; Ctrl-S saving it with the set; a new set starting
// with a copy of the rig in use; edits not saved by leaving the set.
// (`Rig` here is the test harness; the rig model is stagehand::Rig.)
void testRigs() {
    namespace fs = std::filesystem;
    const fs::path dir = scratchDir("stagehand-rigs-test");
    fs::remove_all(dir);
    stagehand::Rig stage;
    stage.buses[0].returnDb = -3.0f;
    const std::string gig   = makeSet(dir / "Gig.setlist", {keysRack("Song")}, {"Song"}, stage);

    Rig                 rig;
    Session::LoadReport report;
    Rack                untitled;
    untitled.name = "Song";
    rig.session->load(untitled, 48000.0, report);
    RackController::Config config = rig.config();
    config.rig                    = true;
    RackController controller(*rig.session, *rig.engine, rig.midi, rig.factory, config);

    // Untitled: an empty rig.
    CHECK(rig.session->hasRig() && rig.session->rig().saved.buses[0].returnDb == 0.0f);

    // The set's own rig while it's open.
    CHECK(controller.openSet(gig));
    CHECK(rig.session->rig().saved.buses[0].returnDb == -3.0f && !rig.session->rigModified());

    // Ctrl-S saves the rig with the set.
    CHECK(rig.session->setReturnDb(0, -10.0f) && rig.session->rigModified() && controller.unsavedChanges());
    CHECK(controller.save() && !rig.session->rigModified());
    stagehand::Rig onDisk;
    CHECK(loadRig(setRigPath(gig), onDisk).ok && onDisk.buses[0].returnDb == -10.0f);

    // A new set: a copy of the rig in use. Leaving didn't save the edit.
    CHECK(rig.session->setReturnDb(0, -12.0f));
    controller.newSet();
    CHECK(rig.session->rig().saved.buses[0].returnDb == -12.0f && !controller.unsavedChanges());
    CHECK(loadRig(setRigPath(gig), onDisk).ok && onDisk.buses[0].returnDb == -10.0f);
    CHECK(controller.openSet(gig) && rig.session->rig().saved.buses[0].returnDb == -10.0f);

    runBlock(*rig.engine);
    rig.engine->collect();
    fs::remove_all(dir);
}

// Prepared racks (docs/setlists.md, section 2): the next and previous songs'
// racks prepared one plugin per tick, Next switching at once with one graph
// commit (editors closed, the rig's effects kept), the rack left prepared again
// from memory, a reprise sharing one copy, and a jump to a rack not prepared
// waiting for it while the current one plays (only the last request counts).
void testPreparedRacks() {
    namespace fs = std::filesystem;
    const fs::path dir = scratchDir("stagehand-prepared-test");
    fs::remove_all(dir);
    auto makeRack = [&](const std::string& name, std::size_t plugins) {
        Rack rack;
        rack.name = name;
        for (std::size_t i = 0; i < plugins; ++i)
            rack.slots.push_back(slot("row" + std::to_string(i + 1), i % 2 == 0 ? "synth" : "bass",
                                      i % 2 == 0 ? "/clap/Synth.clap" : "/clap/Bass.clap"));
        const std::string path = (dir / "Racks" / (name + ".rack")).string();
        CHECK(saveRack(path, rack).ok);
        return path;
    };
    const std::string a = makeRack("A", 1), b = makeRack("B", 2), c = makeRack("C", 1), d = makeRack("D", 2);
    stagehand::Rig stage;
    RigEffect      effect;
    effect.plugin             = installedPlugin("old", "/clap/Old.clap");
    stage.buses[0].effects[0] = effect;
    const std::string stagePath = (dir / "Stage.rig").string();
    CHECK(saveRig(stagePath, stage).ok);
    Setlist set;
    set.songs = {SetlistSong{a, ""}, SetlistSong{b, ""}, SetlistSong{c, ""}, SetlistSong{d, ""}};
    set.rig   = stagePath;
    const std::string file = (dir / "Gig.setlist").string();
    CHECK(saveSetlist(file, set).ok);

    Rig                 rig;
    Session::LoadReport report;
    Rack                untitled;
    untitled.name = "Untitled";
    rig.session->load(untitled, 48000.0, report);
    RackController::Config config = rig.config();
    config.savingOff              = true;   // edits then live only in memory
    RackController       controller(*rig.session, *rig.engine, rig.midi, rig.factory, config);
    const RackViewModel& view = controller.view();
    double               now  = 0.0;
    auto tick = [&] { controller.tick(now += 2.0); };

    // Opening the set isn't a live switch: song 1 plays at once.
    CHECK(controller.openSet(packageOf(file), 0) && controller.songRack() == "A" && !controller.switchPending());
    const HostedPlugin* fx = rig.session->rig().effects[0][0].plugin.get();
    CHECK(fx != nullptr);

    // B (two plugins) is prepared over two ticks (the first came with the
    // switch's own refresh), with nothing committed.
    const std::uint64_t commits = rig.engine->graphCommits();
    CHECK(!controller.songPrepared(1) && view.status.preparing == "Preparing song 2 (B): 1/2 plugins");
    CHECK(!view.setlist.songs[1].prepared && view.setlist.songs[0].prepared);   // the playing one is
    tick();
    CHECK(controller.songPrepared(1) && view.setlist.songs[1].prepared && !controller.songPrepared(2));
    tick();
    CHECK(view.status.preparing.empty() && rig.engine->graphCommits() == commits);

    // Next: at once, one commit; editors close, the old plugins leave, the rig stays.
    CHECK(rig.session->setGainDb("row1", -6.0f));
    controller.openEditor("row1");
    CHECK(*rig.editorsOpen == 1);
    const std::weak_ptr<HostedPlugin> oldRow   = rig.session->slot("row1")->plugin;
    const std::uint64_t               switched = controller.rackGeneration();
    CHECK(controller.nextSong() && controller.songRack() == "B" && !controller.switchPending());
    CHECK(rig.engine->graphCommits() == commits + 1);
    CHECK(*rig.editorsOpen == 0 && controller.rackGeneration() == switched + 1);
    CHECK(rig.session->slots().size() == 2 && view.setlist.current == 1);
    runBlock(*rig.engine);
    rig.engine->collect();
    CHECK(!oldRow.expired() && rig.session->drainingSlots().size() == 1);   // ringing out
    CHECK(rig.session->rig().effects[0][0].plugin.get() == fx);

    // Then C (next) and A again (previous), one plugin per tick; A from memory,
    // with its edit, though nothing was saved.
    CHECK(controller.songPrepared(2) && !controller.songPrepared(0));
    tick();
    CHECK(controller.songPrepared(0) && rig.engine->graphCommits() == commits + 1);
    CHECK(controller.previousSong() && controller.songRack() == "A");
    CHECK(rig.session->slot("row1") != nullptr && rig.session->slot("row1")->saved.gainDb == -6.0f);

    // A jump to a rack not prepared waits for it while the current one plays;
    // a second jump replaces the first.
    CHECK(controller.goToSong(3) && controller.switchPending() && controller.songRack() == "A");
    tick();
    CHECK(controller.songRack() == "A" && view.setlist.waiting == 3);
    CHECK(view.status.preparing == "Preparing song 4 (D): 1/2 plugins");
    CHECK(controller.goToSong(2) && controller.switchPending());
    tick();   // C: one plugin, then the switch
    CHECK(controller.songRack() == "C" && !controller.switchPending() && view.setlist.current == 2);
    CHECK(view.setlist.waiting == -1);

    // A reprise: B before and after C is one prepared copy, ready for both at once.
    CHECK(controller.goToSong(1));
    for (int i = 0; i < 4 && controller.switchPending(); ++i) tick();
    CHECK(controller.songRack() == "B" && controller.addThisRackAgain() && controller.moveSong(2, 1));   // [A, B, C, B, D]
    CHECK(controller.goToSong(2));
    for (int i = 0; i < 4 && controller.switchPending(); ++i) tick();
    CHECK(controller.songRack() == "C");
    for (int i = 0; i < 6 && !controller.songPrepared(1); ++i) tick();
    CHECK(controller.songPrepared(1) && controller.songPrepared(3));
    CHECK(controller.goToSong(3) && controller.songRack() == "B" && view.setlist.current == 3);
    // Going to its other appearance moves the position only: it's the same rack.
    const std::uint64_t reprise = controller.rackGeneration();
    CHECK(controller.goToSong(1) && view.setlist.current == 1 && controller.rackGeneration() == reprise);

    runBlock(*rig.engine);
    rig.engine->collect();
    fs::remove_all(dir);
}

// The setlist's MIDI triggers (docs/setlists.md, section 4): learned and saved
// with the setlist, Next / Previous from a pedal, Program Change to a song,
// presses in one tick taken together, and a rack's own trigger on the same
// pedal reported when it's switched to.
void testSetlistTriggers() {
    namespace fs = std::filesystem;
    const fs::path dir = scratchDir("stagehand-setlist-triggers-test");
    fs::remove_all(dir);
    std::vector<std::string> racks;
    for (const char* name : {"A", "B", "C", "D"}) {
        Rack rack;
        rack.name = name;
        if (std::string(name) == "C") {   // a pedal the setlist will take
            SequenceTrigger start;
            start.action  = SequenceTrigger::Action::Start;
            start.source  = MidiSourceRef{MidiSourceRef::Kind::Virtual, "MIDI 2.0 In"};
            start.control = SequenceTrigger::Control::Controller;
            start.number  = 80;
            rack.transport.triggers = {start};
        }
        racks.push_back((dir / "Racks" / (std::string(name) + ".rack")).string());
        CHECK(saveRack(racks.back(), rack).ok);
    }
    Setlist set;
    for (const std::string& r : racks) set.songs.push_back(SetlistSong{r, ""});
    const std::string file = (dir / "Gig.setlist").string();
    CHECK(saveSetlist(file, set).ok);

    Rig rig;
    rig.session->addOpenSource(MidiSourceRef{MidiSourceRef::Kind::Virtual, "MIDI 2.0 In"}, 7);
    Session::LoadReport report;
    Rack                untitled;
    rig.session->load(untitled, 48000.0, report);
    RackController       controller(*rig.session, *rig.engine, rig.midi, rig.factory, rig.config());
    const RackViewModel& view = controller.view();
    double               now  = 0.0;
    auto tick = [&] { controller.tick(now += 2.0); };
    auto send = [&](std::uint32_t word) {
        TimedMidiMessage m;
        m.captureTime       = 1'000'000'000 - 1'000'000;
        m.message.words[0]  = word;
        m.message.wordCount = 1;
        rig.engine->onMidi(7, m);
        runBlock(*rig.engine);
    };
    CHECK(controller.openSet(packageOf(file), 0) && controller.songRack() == rackNameOf(racks[0]));

    // Learn Next on CC 80, and Program Change on channel 1; saved with the set.
    controller.learnSetlistTrigger(SetlistTrigger::Action::Next);
    tick();
    CHECK(view.setlist.learning && view.setlist.learningAction == SetlistTrigger::Action::Next);
    send(0x20B0507Fu);
    tick();
    send(0x20B05000u);
    controller.learnSetlistTrigger(SetlistTrigger::Action::Song);
    send(0x20B0517Fu);   // a press (CC 81) isn't a Program Change: still learning
    tick();
    CHECK(view.setlist.learning);
    send(0x20B05100u);
    send(0x20C00000u);
    tick();
    CHECK(!view.setlist.learning);
    CHECK((view.setlist.triggers == std::vector<std::string>{"Next song: From other apps (MIDI 2.0), CC 80, ch 1",
                                                             "Program Change: From other apps (MIDI 2.0), ch 1 (program n: song n)"}));
    CHECK(view.setlist.edited && controller.save());
    Set saved;
    CHECK(loadSet(controller.setPath(), saved).ok && saved.triggers.size() == 2 &&
          saved.triggers[0].action == SetlistTrigger::Action::Next && saved.triggers[0].number == 80);
    controller.takeNotices();

    // The pedal goes to the next song (B is prepared by now).
    send(0x20B0507Fu);
    send(0x20B05000u);
    tick();
    CHECK(controller.songRack() == rackNameOf(racks[1]) && view.setlist.current == 1);

    // Two presses in one tick: two songs on, in one switch. C's own Start on
    // the same pedal is reported, and the setlist's wins.
    controller.takeNotices();
    const std::uint64_t generation = controller.rackGeneration();
    send(0x20B0507Fu);
    send(0x20B05000u);
    send(0x20B0507Fu);
    send(0x20B05000u);
    for (int i = 0; i < 4 && (controller.switchPending() || controller.songRack() == rackNameOf(racks[1])); ++i) tick();
    CHECK(controller.songRack() == rackNameOf(racks[3]) && controller.rackGeneration() == generation + 1);
    send(0x20C00200u);   // Program Change 2: the third song, C
    for (int i = 0; i < 4 && controller.songRack() != rackNameOf(racks[2]); ++i) tick();
    CHECK(controller.songRack() == rackNameOf(racks[2]) && !rig.engine->transport().running);
    bool reported = false;
    for (const std::string& text : controller.takeNotices())
        reported = reported || text.find("is also the setlist's Next song; the setlist's wins") != std::string::npos;
    CHECK(reported);
    send(0x20C00900u);   // past the end: a notice, no switch
    tick();
    CHECK(controller.songRack() == rackNameOf(racks[2]) && !controller.takeNotices().empty());

    // Removed: the pedal no longer acts. A new set has none.
    controller.removeSetlistTrigger(0);
    tick();
    CHECK(view.setlist.triggers.size() == 1 && rig.session->setlistTriggers().size() == 1);
    controller.newSet();
    CHECK(rig.session->setlistTriggers().empty());

    runBlock(*rig.engine);
    rig.engine->collect();
    fs::remove_all(dir);
}

// The rig's controls (docs/rig.md, milestone 3): the view model's buses and
// sends, edits patched into it at once and marking the rig (or, for a send,
// the rack) modified, choosing an effect, the return meter, and a bus
// effect's editor staying open when the rack changes.
void testRigControls() {
    namespace fs = std::filesystem;
    const fs::path dir = scratchDir("stagehand-rig-controls-test");
    fs::remove_all(dir);
    stagehand::Rig stage;
    stage.name = "Stage";
    RigEffect verb;
    verb.plugin             = installedPlugin("old", "/clap/Old.clap");
    stage.buses[0].effects[0] = verb;
    RigEffect gone;
    gone.plugin             = installedPlugin("gone", "/clap/Gone.clap");
    stage.buses[1].effects[1] = gone;
    const std::string rigPath = (dir / "Stage.rig").string();
    CHECK(saveRig(rigPath, stage).ok);

    Rig rig;
    rig.factory.withEditor = {"old"};
    Rack rack;
    RackSlot keys = slot("keys", "synth", "/clap/Synth.clap");
    keys.sendDb[0] = -6.0f;
    rack.slots     = {keys};
    Session::LoadReport report;
    rig.session->load(rack, 48000.0, report);
    rack.name = "Song";
    const std::string gig = makeSet(dir / "Gig.setlist", {rack}, {"Song"}, stage);
    RackController       controller(*rig.session, *rig.engine, rig.midi, rig.factory, rig.config());
    const RackViewModel& view = controller.view();
    CHECK(controller.openSet(gig));
    controller.tick(0.01);

    CHECK(view.rig.loaded && view.rig.name == "Stage" && !view.rig.modified);
    CHECK(view.rig.buses[0].name == "A" && view.rig.buses[0].effects[0].name == "old" &&
          view.rig.buses[0].effects[0].hasEditor && view.rig.buses[0].effects[1].name.empty());
    CHECK(view.rig.buses[1].effects[1].name == "gone" && !view.rig.buses[1].effects[1].problem.empty());
    CHECK(view.rows.size() == 1 && view.rows[0].sendDb[0] == -6.0f && !view.rows[0].sendDb[1]);

    // Edits show at once (before the next tick), as a fader's do.
    rig.session->markSaved();
    controller.setSendDb("keys", 1, -12.0f);
    CHECK(view.rows[0].sendDb[1] == -12.0f && rig.session->modified() && !rig.session->rigModified());
    controller.setSendDb("keys", 0, std::nullopt);
    CHECK(!view.rows[0].sendDb[0]);
    controller.setReturnDb(0, -3.0f);
    controller.setBusMuted(1, true);
    controller.setBusOutput(1, 2);
    controller.setMasterDb(-1.5f);
    CHECK(view.rig.buses[0].returnDb == -3.0f && view.rig.buses[1].muted && view.rig.buses[1].firstOutput == 2 &&
          view.rig.masterDb == -1.5f);
    controller.tick(1.0);
    CHECK(view.rig.modified && view.rig.buses[0].returnDb == -3.0f && view.rig.masterDb == -1.5f);

    // Choosing effects: one that loads, one that doesn't (a notice; unchanged).
    const PluginDescriptor bass = installedPlugin("bass", "/clap/Bass.clap");
    controller.setBusEffect(1, 0, &bass);
    const PluginDescriptor nothing = installedPlugin("nothing", "/clap/Nothing.clap");
    controller.takeNotices();
    controller.setBusEffect(1, 1, &nothing);
    CHECK(controller.takeNotices().size() == 1);
    controller.tick(1.1);
    CHECK(view.rig.buses[1].effects[0].name == "bass" && view.rig.buses[1].effects[1].name == "gone");

    // The return meter: bus A's effect outputs a constant.
    runBlock(*rig.engine);
    controller.tick(1.2);
    CHECK(view.rig.buses[0].meterDb[0] > -60.0f);

    // A bus effect's editor stays open when the song changes; emptying its slot closes it.
    controller.openBusEditor(0, 0);
    controller.tick(1.3);
    CHECK(*rig.editorsOpen == 1 && view.rig.buses[0].effects[0].editorOpen);
    CHECK(controller.newSong());
    for (int i = 0; i < 4 && controller.songRack() != "Song 2"; ++i) controller.tick(1.31 + 0.01 * i);
    CHECK(controller.songRack() == "Song 2");
    CHECK(*rig.editorsOpen == 1);
    controller.setBusEffect(0, 0, nullptr);
    controller.tick(1.4);
    CHECK(*rig.editorsOpen == 0 && view.rig.buses[0].effects[0].name.empty());

    runBlock(*rig.engine);
    rig.engine->collect();
    fs::remove_all(dir);
}

// The setlist's switch settings (docs/setlists.md, section 5): saved at once;
// by default the transport runs on through a switch with the new song's tempo
// and its At Start sequences; Stop stops it; Cut mutes the old song.
void testSwitchSettings() {
    namespace fs = std::filesystem;
    const fs::path dir = scratchDir("stagehand-switch-settings-test");
    fs::remove_all(dir);
    std::vector<std::string> racks;
    for (const auto& [name, bpm] : {std::pair{"A", 100.0}, {"B", 140.0}, {"C", 80.0}}) {
        Rack rack;
        rack.name     = name;
        rack.tempoBpm = bpm;
        rack.slots    = {slot("keys", "synth", "/clap/Synth.clap")};
        if (std::string(name) == "B") {
            Sequence intro;
            intro.id      = "intro";
            intro.atStart = true;
            intro.targets = {"keys"};
            rack.sequences = {intro};
        }
        racks.push_back((dir / "Racks" / (std::string(name) + ".rack")).string());
        CHECK(saveRack(racks.back(), rack).ok);
    }
    Setlist set;
    for (const std::string& r : racks) set.songs.push_back(SetlistSong{r, ""});
    const std::string file = (dir / "Gig.setlist").string();
    CHECK(saveSetlist(file, set).ok);

    Rig                 rig;
    Session::LoadReport report;
    rig.session->load(Rack{}, 48000.0, report);
    RackController       controller(*rig.session, *rig.engine, rig.midi, rig.factory, rig.config());
    const RackViewModel& view = controller.view();
    double               now  = 0.0;
    auto tick = [&] { controller.tick(now += 2.0); };
    CHECK(controller.openSet(packageOf(file), 0));
    tick();
    tick();   // B prepared
    CHECK(view.setlist.switching == SwitchSettings{});

    // The defaults: keep running, at the next bar. A's tempo plays on until
    // the bar line (a bar of 4/4 at 100 BPM: 115200 frames, 1800 of
    // runBlock's 64), then B's tempo, with its At Start from that bar.
    controller.startTransport();
    for (int i = 0; i < 4; ++i) runBlock(*rig.engine);
    CHECK(controller.nextSong() && controller.songRack() == rackNameOf(racks[1]));
    runBlock(*rig.engine);
    CHECK(rig.engine->transport().running && rig.engine->transport().tempoBpm == 100.0);   // waiting for the bar
    int blocks = 0;
    for (; blocks < 2000 && rig.engine->transport().tempoBpm != 140.0; ++blocks) runBlock(*rig.engine);
    CHECK(blocks > 1700 && blocks < 1800 && rig.engine->transport().running);   // at the line, 5 blocks in already
    CHECK(rig.session->sequenceState("intro") == SequenceState::Starting ||
          rig.session->sequenceState("intro") == SequenceState::Playing);

    // Stop, and Cut: saved with the set; the next switch stops the transport and mutes the old song.
    SwitchSettings settings;
    settings.transport = SwitchSettings::Transport::Stop;
    settings.when      = SwitchSettings::When::Now;   // as soon as it's ready
    settings.oldRack   = SwitchSettings::OldRack::Cut;
    controller.setSwitchSettings(settings);
    CHECK(controller.save());
    Set saved;
    CHECK(loadSet(controller.setPath(), saved).ok && saved.switching == settings);
    tick();
    CHECK(view.setlist.switching == settings);
    tick();   // C prepared
    const std::shared_ptr<SlotRuntime> old = rig.session->slot("keys")->runtime;
    CHECK(controller.nextSong() && controller.songRack() == rackNameOf(racks[2]));
    runBlock(*rig.engine);
    runBlock(*rig.engine);
    CHECK(!rig.engine->transport().running && rig.engine->transport().tempoBpm == 80.0);
    CHECK(old->muted.load() && rig.session->drainingSlots().size() >= 1);

    runBlock(*rig.engine);
    rig.engine->collect();
    fs::remove_all(dir);
}

// Saving in a set (docs/setlists.md, section 6): a switch never saves, so a
// write can't hold it up; the rack left keeps its edits in memory,
// shown as unsaved, and a reprise plays them; one Save writes every unsaved
// rack.
void testSavingInASet() {
    namespace fs = std::filesystem;
    const fs::path dir = scratchDir("stagehand-saving-test");
    fs::remove_all(dir);
    std::vector<std::string> racks;
    for (const char* name : {"A", "B", "C"}) {
        Rack rack;
        rack.name  = name;
        rack.slots = {slot("keys", "synth", "/clap/Synth.clap")};
        racks.push_back((dir / "Racks" / (std::string(name) + ".rack")).string());
        CHECK(saveRack(racks.back(), rack).ok);
    }
    const std::string a = racks[0], b = racks[1], c = racks[2];
    Setlist set;
    set.songs = {SetlistSong{a, ""}, SetlistSong{b, ""}, SetlistSong{a, ""}, SetlistSong{c, ""}};   // A again: a reprise
    const std::string file = (dir / "Gig.setlist").string();
    CHECK(saveSetlist(file, set).ok);
    const std::string gig = packageOf(file);
    const std::string pa = setRackPath(gig, "A"), pb = setRackPath(gig, "B");
    auto gainOnDisk = [](const std::string& path) {
        Rack rack;
        return loadRack(path, rack).ok && !rack.slots.empty() ? rack.slots[0].gainDb : 99.0f;
    };

    Rig                 rig;
    Session::LoadReport report;
    rig.session->load(Rack{}, 48000.0, report);
    RackController       controller(*rig.session, *rig.engine, rig.midi, rig.factory, rig.config());
    const RackViewModel& view = controller.view();
    double               now  = 0.0;
    auto tick = [&] { controller.tick(now += 2.0); };
    CHECK(controller.openSet(gig, 0));
    tick();

    // Edit A, switch: nothing is written; A's edit is kept, and shown.
    controller.setGainDb("keys", -6.0f);
    CHECK(controller.nextSong() && controller.songRack() == "B");
    tick();
    CHECK(gainOnDisk(pa) == 0.0f && controller.unsavedRackCount() == 1 && view.otherUnsaved == 1 && !view.modified);
    CHECK(view.setlist.songs[0].unsaved && view.setlist.songs[2].unsaved && !view.setlist.songs[1].unsaved);

    // The reprise plays the edit (its prepared copy came from memory), and it's unsaved there.
    for (int i = 0; i < 3 && !controller.songPrepared(2); ++i) tick();
    CHECK(controller.nextSong() && controller.songRack() == "A");
    CHECK(rig.session->slot("keys")->saved.gainDb == -6.0f && rig.session->modified() &&
          controller.unsavedRackCount() == 0);

    // Edit A again and B, switching between: then one Save writes both.
    controller.setGainDb("keys", -9.0f);
    for (int i = 0; i < 4 && !controller.songPrepared(1); ++i) tick();
    CHECK(controller.previousSong() && controller.songRack() == "B");
    controller.setGainDb("keys", -3.0f);
    tick();
    CHECK(view.otherUnsaved == 1 && view.modified);   // "2 songs unsaved"
    CHECK(controller.save());
    CHECK(gainOnDisk(pa) == -9.0f && gainOnDisk(pb) == -3.0f && controller.unsavedRackCount() == 0 &&
          !rig.session->modified());

    // Edited again and left: the next Save writes what was kept.
    controller.setGainDb("keys", -1.0f);
    for (int i = 0; i < 4 && !controller.songPrepared(2); ++i) tick();
    CHECK(controller.nextSong() && controller.songRack() == "A" && controller.unsavedRackCount() == 1);
    CHECK(gainOnDisk(pb) == -3.0f);   // not written by the switch
    CHECK(controller.unsavedChanges() && controller.save());
    CHECK(controller.unsavedRackCount() == 0 && gainOnDisk(pb) == -1.0f);

    runBlock(*rig.engine);
    rig.engine->collect();
    fs::remove_all(dir);
}

// Loopers (docs/looper.md, milestone 2): made with an input and targets (the
// current looper), their settings edited and saved with the rack while what
// they record never is, the row's tap, the looper switch learned into the rig
// (press and release reaching the current looper), and a rack switch leaving
// the new rack's loopers empty.
void testLoopers() {
    namespace fs = std::filesystem;
    const fs::path dir = scratchDir("stagehand-loopers-test");
    fs::remove_all(dir);
    Rig rig;
    rig.session->addOpenSource(MidiSourceRef{MidiSourceRef::Kind::Virtual, "MIDI 2.0 In"}, 7);
    Rack rack;
    rack.name  = "Song";
    rack.slots = {slot("keys", "synth", "/clap/Synth.clap")};
    Session::LoadReport report;
    rig.session->load(rack, 48000.0, report);
    RackController::Config config = rig.config();
    config.rig                    = true;
    RackController       controller(*rig.session, *rig.engine, rig.midi, rig.factory, config);
    const std::string    gig = (dir / "Gig.setlist").string();
    CHECK(controller.saveAs(gig) == RackController::SaveAsResult::Saved);   // a set to save into
    controller.takeNotices();
    const std::string rackPath = setRackPath(gig, controller.songRack()), rigPath = setRigPath(gig);
    const RackViewModel& view = controller.view();
    double               now  = 0.0;
    auto tick = [&] { controller.tick(now += 0.05); };
    auto send = [&](std::uint32_t word) {
        TimedMidiMessage m;
        m.captureTime       = 1'000'000'000 - 1'000'000;
        m.message.words[0]  = word;
        m.message.wordCount = 1;
        rig.engine->onMidi(7, m);
        runBlock(*rig.engine);
    };
    auto run = [&](int blocks) {
        for (int i = 0; i < blocks; ++i) runBlock(*rig.engine);
        tick();
    };

    // Made, and current; then set to record one bar, from a beat, unquantized.
    const MidiSourceRef input{MidiSourceRef::Kind::Virtual, "MIDI 2.0 In"};
    const std::string   id = controller.addLooper(input, {"keys"});
    controller.setLooperStart(id, false);
    controller.setLooperQuantize(id, 0.0);
    controller.setLooperLength(id, 1, true);
    tick();
    CHECK(view.loopers.size() == 1 && view.loopers[0].current && view.loopers[0].name == "Loop 1");
    CHECK(view.loopers[0].inputLabel == "From other apps (MIDI 2.0)" &&
          (view.loopers[0].targetNames == std::vector<std::string>{"keys"}));
    CHECK(view.loopers[0].state == LooperState::Empty && view.loopers[0].capacity == LooperRuntime::kMaxNotes);
    CHECK(view.loopers[0].cleanup);   // fat fingers cleaned up, by default
    controller.setLooperCleanup(id, false);
    tick();
    CHECK(!view.loopers[0].cleanup);

    // Named: trimmed; an empty name refused (a notice), the old one kept.
    controller.renameLooper(id, "  Bass vamp ");
    controller.takeNotices();
    controller.renameLooper(id, "   ");
    tick();
    CHECK(view.loopers[0].name == "Bass vamp" && controller.looper(id) != nullptr &&
          controller.looper(id)->name == "Bass vamp" && controller.takeNotices().size() == 1);

    // The row's tap with the transport stopped: it starts, and records a bar (2 s at 120 BPM).
    controller.tapLooper(id);
    runBlock(*rig.engine);
    send(0x20903C64u);   // a note in the take
    send(0x20803C00u);
    run(10);
    CHECK(rig.engine->transport().running && view.loopers[0].state == LooperState::Recording);
    run(1600);   // past the bar: 1600 blocks of 64 frames
    CHECK(view.loopers[0].state == LooperState::Playing && view.loopers[0].lengthBeats == 4.0 &&
          view.loopers[0].notes == 1);

    // The looper switch: learned into the rig; a quick press and release (a
    // tap) stops the current looper at the next beat.
    controller.learnLooperSwitch();
    tick();
    CHECK(view.looperSwitchLearning);
    send(0x20B0527Fu);   // CC 82 pressed
    tick();
    send(0x20B05200u);
    CHECK(!view.looperSwitchLearning && view.looperSwitch == "From other apps (MIDI 2.0), CC 82, ch 1");
    CHECK(rig.session->rigModified());
    send(0x20B0527Fu);
    send(0x20B05200u);
    run(400);   // past the next beat (375 blocks)
    CHECK(view.loopers[0].state == LooperState::Stopped);
    controller.undoLooper(id);    // nothing to undo: just the first recording
    controller.clearLooper(id);
    run(2);
    CHECK(view.loopers[0].state == LooperState::Empty && view.loopers[0].notes == 0);

    // Saved: the looper with its settings (never notes), the switch in the rig.
    controller.tapLooper(id);   // something recorded again (from the next beat), which isn't saved
    run(400);
    send(0x20903E64u);
    send(0x20803E00u);
    run(1700);
    CHECK(view.loopers[0].notes == 1);
    CHECK(controller.save());
    Rack saved;
    CHECK(loadRack(rackPath, saved).ok && saved.loopers.size() == 1 && saved.loopers[0].fixedLength == 1 &&
          saved.loopers[0].name == "Bass vamp" &&
          !saved.loopers[0].atBar && saved.loopers[0].quantize == 0.0 && !saved.loopers[0].cleanup);
    stagehand::Rig savedRig;
    CHECK(loadRig(rigPath, savedRig).ok && savedRig.looperSwitch &&
          savedRig.looperSwitch->number == 82);

    // Another set, then this one again: its looper is back, empty.
    controller.newSet();
    tick();
    CHECK(view.loopers.empty());
    CHECK(controller.openSet(gig));
    tick();
    CHECK(view.loopers.size() == 1 && view.loopers[0].current && view.loopers[0].state == LooperState::Empty &&
          view.loopers[0].notes == 0 && view.loopers[0].name == "Bass vamp");

    // A row removed leaves its targets; a looper removed is gone.
    controller.removeSlot("keys");
    tick();
    CHECK(view.loopers[0].targetNames.empty());
    controller.removeLooper(view.loopers[0].id);
    tick();
    CHECK(view.loopers.empty());

    runBlock(*rig.engine);
    rig.engine->collect();
    fs::remove_all(dir);
}

// Keeping a loop as a sequence (docs/looper.md, milestone 3): a sequence in
// the rack, named and aimed as the looper, unsaved; the looper plays on. With
// nothing recorded, a notice and no sequence.
void testKeepLoop() {
    Rig rig;
    Rack rack;
    rack.slots = {slot("keys", "synth", "/clap/Synth.clap")};
    Session::LoadReport report;
    rig.session->load(rack, 48000.0, report);
    RackController       controller(*rig.session, *rig.engine, rig.midi, rig.factory, rig.config());
    const RackViewModel& view = controller.view();
    double               now  = 0.0;
    auto run = [&](int blocks) {
        for (int i = 0; i < blocks; ++i) runBlock(*rig.engine);
        controller.tick(now += 0.05);
    };
    const std::string id = controller.addLooper(MidiSourceRef{}, {"keys"});
    controller.setLooperLength(id, 1, true);
    run(1);
    controller.takeNotices();
    controller.keepLooperAsSequence(id);   // nothing yet
    run(2);
    CHECK(rig.session->sequences().empty() && controller.takeNotices().size() == 1);

    controller.tapLooper(id);   // the transport starts; a bar recorded
    runBlock(*rig.engine);
    TimedMidiMessage m;
    m.captureTime       = 1'000'000'000 - 1'000'000;
    m.message.words[0]  = 0x20903C64u;
    m.message.wordCount = 1;
    rig.engine->onMidi(5, m);
    runBlock(*rig.engine);
    m.message.words[0] = 0x20803C00u;
    rig.engine->onMidi(5, m);
    run(1700);
    CHECK(view.loopers[0].state == LooperState::Playing);
    rig.session->markSaved();
    controller.keepLooperAsSequence(id);
    run(2);
    CHECK(rig.session->sequences().size() == 1 && rig.session->modified());
    if (rig.session->sequences().size() == 1) {
        const Sequence& seq = rig.session->sequences()[0].sequence;
        CHECK(seq.name == "Loop 1" && (seq.targets == std::vector<std::string>{"keys"}) && seq.notes.size() == 1 &&
              seq.length == 16 && seq.notes[0].pitch == 60);
    }
    CHECK(view.loopers[0].state == LooperState::Playing);   // it plays on
    runBlock(*rig.engine);
    rig.engine->collect();
}

// Default looper names keep a running tab: Loop 1, Loop 2, ...; a deleted
// looper's number isn't used again, one named "Loop N" by hand counts, and a
// loaded rack counts from its loopers' names.
void testLoopNumbers() {
    Rig rig;
    Session::LoadReport report;
    rig.session->load(Rack{}, 48000.0, report);
    RackController controller(*rig.session, *rig.engine, rig.midi, rig.factory, rig.config());
    auto nameOf = [&](const std::string& id) { return controller.looper(id) ? controller.looper(id)->name : std::string{}; };

    const std::string one = controller.addLooper(MidiSourceRef{}, {});
    const std::string two = controller.addLooper(MidiSourceRef{}, {});
    CHECK(nameOf(one) == "Loop 1" && nameOf(two) == "Loop 2");
    controller.removeLooper(two);
    const std::string three = controller.addLooper(MidiSourceRef{}, {});
    CHECK(nameOf(three) == "Loop 3");   // not 2 again
    controller.renameLooper(one, "Loop 7");
    CHECK(nameOf(controller.addLooper(MidiSourceRef{}, {})) == "Loop 8");

    Rack saved = rig.session->capture();   // Loop 7, Loop 3, Loop 8
    rig.session->load(saved, 48000.0, report);
    CHECK(nameOf(controller.addLooper(MidiSourceRef{}, {})) == "Loop 9");
    runBlock(*rig.engine);
    rig.engine->collect();
}

// Check setlist (docs/setlists.md, milestone 7): each rack prepared once in
// turn and dropped; a missing plugin, an unplugged device and a rack that
// can't be read are kept per song (a reprise shares its rack's), with a
// notice; switching to a song replaces its problems with what the switch says.
void testCheckSetlist() {
    namespace fs = std::filesystem;
    const fs::path dir = scratchDir("stagehand-check-test");
    fs::remove_all(dir);
    auto make = [&](const char* name, std::vector<RackSlot> slots) {
        Rack rack;
        rack.name  = name;
        rack.slots = std::move(slots);
        const std::string path = (dir / "Racks" / (std::string(name) + ".rack")).string();
        CHECK(saveRack(path, rack).ok);
        return path;
    };
    RackSlot unplugged = slot("pad", "synth", "/clap/Synth.clap");
    unplugged.source   = MidiSourceRef{MidiSourceRef::Kind::Device, "Unplugged:Port"};
    const std::string a = make("A", {slot("keys", "synth", "/clap/Synth.clap")});
    const std::string b = make("B", {slot("gone", "missing"), unplugged});
    const std::string c = make("C", {slot("keys", "synth", "/clap/Synth.clap")});
    const std::string d = make("D", {});
    Setlist set;
    set.songs = {SetlistSong{a, ""}, SetlistSong{b, ""}, SetlistSong{a, ""}, SetlistSong{c, ""}, SetlistSong{d, ""}};
    const std::string file = (dir / "Gig.setlist").string();
    CHECK(saveSetlist(file, set).ok);
    const std::string gig = packageOf(file);
    std::ofstream(setRackPath(gig, "D") + "/rack.json") << "{ not json";   // in the set, unreadable

    Rig                 rig;
    Session::LoadReport report;
    rig.session->load(Rack{}, 48000.0, report);
    RackController       controller(*rig.session, *rig.engine, rig.midi, rig.factory, rig.config());
    const RackViewModel& view = controller.view();
    double               now  = 0.0;
    auto tick = [&] { controller.tick(now += 2.0); };
    CHECK(controller.openSet(gig, 0) && controller.songRack() == "A");
    for (int i = 0; i < 4; ++i) tick();   // B prepared ahead
    controller.takeNotices();

    CHECK(controller.checkSetlist() && controller.checkingSetlist());
    for (int i = 0; i < 40 && controller.checkingSetlist(); ++i) tick();
    tick();
    CHECK(!controller.checkingSetlist() && !view.setlist.checking && view.setlist.songs.size() == 5);
    auto has = [](const std::vector<std::string>& problems, const char* text) {
        for (const std::string& p : problems)
            if (p.find(text) != std::string::npos) return true;
        return false;
    };
    if (view.setlist.songs.size() == 5) {
        CHECK(view.setlist.songs[0].problems.empty() && view.setlist.songs[2].problems.empty() &&
              view.setlist.songs[3].problems.empty());
        CHECK(view.setlist.songs[1].problems.size() == 2 && has(view.setlist.songs[1].problems, "isn't installed") &&
              has(view.setlist.songs[1].problems, "'Unplugged:Port' isn't connected"));
        CHECK(view.setlist.songs[4].problems.size() == 1 && has(view.setlist.songs[4].problems, "can't be read"));
    }
    bool summed = false;
    for (const std::string& text : controller.takeNotices())
        summed = summed || text == "Check setlist: 2 songs have problems (in amber in the song list)";
    CHECK(summed);
    CHECK(controller.songRack() == "A" && controller.songPrepared(1));   // nothing else changed

    // Switching to B: what the switch reports replaces what the check found.
    CHECK(controller.nextSong() && controller.songRack() == "B");
    tick();
    CHECK(view.setlist.songs[1].problems.size() == 2 && has(view.setlist.songs[1].problems, "hear nothing"));
    // A new check starts afresh; another set forgets it.
    CHECK(controller.checkSetlist());
    controller.newSet();
    CHECK(!controller.checkingSetlist());
    fs::remove_all(dir);
}

// MIDI devices coming and going (hotplug): a device a row names appears and
// is opened and heard; one that leaves shows as disconnected, and comes back
// as the same input; a new software source isn't opened (hardware only).
void testHotplug() {
    Rig                 rig;
    Session::LoadReport report;
    rig.session->load(testRack(), 48000.0, report);   // a: Keys; e: Unplugged:Port, not there
    RackController       controller(*rig.session, *rig.engine, rig.midi, rig.factory, rig.config());
    const RackViewModel& view = controller.view();
    double               now  = 1.0;
    auto tick = [&] { controller.tick(now += 0.1); };
    auto row  = [&](const char* id) -> const RowModel* {
        for (const RowModel& r : view.rows)
            if (r.id == id) return &r;
        return nullptr;
    };
    auto noticed = [&](const std::string& text) {
        bool found = false;
        for (const std::string& n : controller.takeNotices()) found = found || n == text;
        return found;
    };
    tick();
    controller.takeNotices();
    CHECK(row("e") && !row("e")->sourceConnected && row("a") && row("a")->sourceConnected);

    // Plugged in: opened (hardware), and the row naming it hears it.
    rig.midi.present.insert("Unplugged:Port");
    rig.midi.sources.push_back(MidiSourceInfo{"Unplugged:Port", "Port", false, true, MidiProtocol::Midi1});
    rig.midi.sources.push_back(MidiSourceInfo{"VMPK:Out", "Out", true, false, MidiProtocol::Midi1});
    rig.midi.present.insert("VMPK:Out");
    ++rig.midi.changeCount;
    tick();
    CHECK(noticed("MIDI device 'Unplugged:Port' connected"));
    CHECK(row("e") && row("e")->sourceConnected && rig.session->slot("e")->source == 100);
    CHECK(!rig.session->sourceOpen(MidiSourceRef{MidiSourceRef::Kind::Device, "VMPK:Out"}));   // software: not opened
    bool chip = false;
    for (const InputChipModel& c : view.inputs) chip = chip || (c.label.find("Port") != std::string::npos && c.connected);
    CHECK(chip);

    // Unplugged: the row says so (it hears nothing), and so does its chip.
    rig.midi.unplugged.insert(42);
    ++rig.midi.changeCount;
    tick();
    CHECK(noticed("MIDI device 'Keys:Keys MIDI 1' disconnected"));
    CHECK(row("a") && !row("a")->sourceConnected && rig.session->slot("a")->source == kInvalidMidiSource);
    bool lost = false;
    for (const InputChipModel& c : view.inputs) lost = lost || !c.connected;
    CHECK(lost);
    CHECK(!rig.session->flushPending());   // committed at the tick

    // Back: the same input again, heard by the row.
    rig.midi.unplugged.erase(42);
    ++rig.midi.changeCount;
    tick();
    CHECK(noticed("MIDI device 'Keys:Keys MIDI 1' is back"));
    CHECK(row("a") && row("a")->sourceConnected && rig.session->slot("a")->source == 42);
    CHECK(std::count(rig.midi.opened.begin(), rig.midi.opened.end(), "Keys:Keys MIDI 1") == 1);   // never reopened

    // Nothing announced: nothing done.
    tick();
    CHECK(controller.takeNotices().empty());
}

// Panic (docs/panic.md): the button, lit for a moment, with a notice; its
// control learned into the rig (a press is a Panic, the second within 2 s
// cuts the sound), saved, and removed.
void testPanic() {
    namespace fs = std::filesystem;
    const fs::path dir = scratchDir("stagehand-panic-test");
    fs::remove_all(dir);
    Rig rig;
    rig.session->addOpenSource(MidiSourceRef{MidiSourceRef::Kind::Virtual, "MIDI 2.0 In"}, 7);
    Rack rack;
    rack.name  = "Song";
    rack.slots = {slot("keys", "synth", "/clap/Synth.clap")};
    Session::LoadReport report;
    rig.session->load(rack, 48000.0, report);
    RackController::Config config = rig.config();
    config.rig                    = true;
    RackController       controller(*rig.session, *rig.engine, rig.midi, rig.factory, config);
    const std::string    gig = (dir / "Gig.setlist").string();
    CHECK(controller.saveAs(gig) == RackController::SaveAsResult::Saved);   // a set to save into
    controller.takeNotices();
    const std::string rackPath = setRackPath(gig, controller.songRack()), rigPath = setRigPath(gig);
    const RackViewModel& view = controller.view();
    double               now  = 0.0;
    auto tick = [&] { controller.tick(now += 0.05); };
    auto send = [&](std::uint32_t word) {
        TimedMidiMessage m;
        m.captureTime       = 1'000'000'000 - 1'000'000;
        m.message.words[0]  = word;
        m.message.wordCount = 1;
        rig.engine->onMidi(7, m);
        runBlock(*rig.engine);
    };
    tick();
    controller.takeNotices();

    controller.panic();
    runBlock(*rig.engine);
    tick();
    CHECK(view.transport.panicLit && controller.takeNotices() == std::vector<std::string>{"Panic: all notes off"});
    now += 1.0;
    tick();
    CHECK(!view.transport.panicLit && controller.takeNotices().empty());

    // Learned: a controller press, kept in the rig.
    CHECK(view.transport.panicControl.empty());
    controller.learnPanic();
    tick();
    CHECK(view.transport.panicLearning);
    send(0x20B0537Fu);   // CC 83 pressed
    tick();
    send(0x20B05300u);
    CHECK(!view.transport.panicLearning && view.transport.panicControl == "From other apps (MIDI 2.0), CC 83, ch 1");
    CHECK(view.transport.panicConnected && rig.session->rigModified());
    controller.takeNotices();
    send(0x20B0537Fu);   // a press: the second Panic, at once (the blocks' clock stands still), cuts the sound
    send(0x20B05300u);
    tick();
    CHECK(view.transport.panicLit && controller.takeNotices() == std::vector<std::string>{"Panic: sound cut"});
    CHECK(rig.engine->panics().count == 2);

    CHECK(controller.save());
    stagehand::Rig savedRig;
    CHECK(loadRig(rigPath, savedRig).ok && savedRig.panic && savedRig.panic->number == 83 &&
          savedRig.panic->control == SequenceTrigger::Control::Controller);
    controller.removePanic();
    tick();
    CHECK(view.transport.panicControl.empty() && rig.session->rigModified());
    send(0x20B0537Fu);   // no longer Panic's
    tick();
    CHECK(rig.engine->panics().count == 2);

    runBlock(*rig.engine);
    rig.engine->collect();
    fs::remove_all(dir);
}

// The DSP load and xruns (docs/panic.md, sections 6 to 9): the average and
// the worst block of the last second, each row's own share; the xrun count,
// how long ago the last was, red for a few seconds, and reset on request.
void testDspAndXruns() {
    Rig rig;
    std::uint64_t xruns = 0;
    Rack rack;
    rack.slots = {slot("keys", "synth", "/clap/Synth.clap")};
    Session::LoadReport report;
    rig.session->load(rack, 48000.0, report);
    RackController::Config config = rig.config();
    config.audioStatus = [&xruns] { return RackController::AudioStatus{48000.0, 64, xruns, 2}; };
    RackController       controller(*rig.session, *rig.engine, rig.midi, rig.factory, config);
    const RackViewModel& view = controller.view();
    double               now  = 0.0;
    auto tick = [&](double seconds) { controller.tick(now += seconds); };
    tick(0.05);
    CHECK(view.status.xruns == 0 && view.status.xrunSecondsAgo == -1 && !view.status.xrunRecent);

    // A plugin taking 1 ms of a 64-frame period (1.33 ms): about 75%, shown after a second.
    rig.factory.created.at("synth")->busy = std::chrono::milliseconds(1);
    for (int i = 0; i < 4; ++i) runBlock(*rig.engine);
    tick(0.05);
    CHECK(view.rows[0].loadPercent == 0 && view.status.dspPeakPercent == 0);
    for (int i = 0; i < 100; ++i) runBlock(*rig.engine);   // 0.13 s: the second's average climbs to about 10%
    tick(1.0);
    CHECK(view.rows[0].loadPercent >= 70 && view.status.dspPeakPercent >= view.rows[0].loadPercent);
    CHECK(view.status.dspPercent > 0);
    rig.factory.created.at("synth")->busy = std::chrono::nanoseconds(0);

    // Two xruns: counted, red for 5 s, then how long ago.
    xruns = 2;
    tick(0.05);
    CHECK(view.status.xruns == 2 && view.status.xrunSecondsAgo == 0 && view.status.xrunRecent);
    tick(6.0);
    CHECK(view.status.xruns == 2 && view.status.xrunSecondsAgo == 6 && !view.status.xrunRecent);
    controller.resetXruns();
    tick(0.05);
    CHECK(view.status.xruns == 0 && view.status.xrunSecondsAgo == -1);
    xruns = 3;
    tick(0.05);
    CHECK(view.status.xruns == 1 && view.status.xrunRecent);

    runBlock(*rig.engine);
    rig.engine->collect();
}

// The status bar's rate and buffer menus (R2-20): what they offer comes from
// the app, and a choice goes to it, with its answer said in the status bar.
void testAudioMenus() {
    Rig                    rig;
    RackController::Config config = rig.config();
    {
        RackController none(*rig.session, *rig.engine, rig.midi, rig.factory, config);
        CHECK(none.audioChoices().sampleRates.empty() && none.audioChoices().bufferSizes.empty());
        none.chooseSampleRate(96000.0);   // no app behind it: nothing happens
        none.chooseBufferSize(128);
        CHECK(none.takeNotices().empty());
    }
    double        rate   = -1.0;
    std::uint32_t buffer = 0;
    config.audioChoices  = [] {
        RackController::AudioChoices c;
        c.sampleRates = {44100.0, 48000.0};
        c.bufferSizes = {64, 128, 256};
        c.chosenRate  = 48000.0;
        c.bufferLive  = true;
        return c;
    };
    config.chooseSampleRate = [&rate](double hz) { rate = hz; return std::string("rate said"); };
    config.chooseBufferSize = [&buffer](std::uint32_t frames) { buffer = frames; return std::string{}; };
    RackController controller(*rig.session, *rig.engine, rig.midi, rig.factory, config);
    const RackController::AudioChoices choices = controller.audioChoices();
    CHECK(choices.sampleRates.size() == 2 && choices.bufferSizes.size() == 3 && choices.chosenRate == 48000.0 &&
          choices.bufferLive);
    controller.chooseSampleRate(44100.0);
    CHECK(rate == 44100.0);
    std::vector<std::string> said = controller.takeNotices();
    CHECK(said.size() == 1 && said[0] == "rate said");
    controller.chooseSampleRate(0.0);   // the device's own
    CHECK(rate == 0.0);
    controller.takeNotices();
    controller.chooseBufferSize(128);   // an empty answer: nothing to say
    CHECK(buffer == 128 && controller.takeNotices().empty());
}

// A rate chosen for the next launch stays in the status bar until then.
void testNextRateShown() {
    Rig                    rig;
    RackController::Config config = rig.config();
    config.audioStatus  = [] { return RackController::AudioStatus{48000.0, 256, 0, 2}; };
    config.audioChoices = [] {
        RackController::AudioChoices c;
        c.sampleRates = {48000.0, 96000.0};
        c.chosenRate  = 96000.0;   // saved by an earlier run, not yet in effect
        return c;
    };
    config.chooseSampleRate = [](double) { return std::string("said"); };
    RackController       controller(*rig.session, *rig.engine, rig.midi, rig.factory, config);
    const RackViewModel& view = controller.view();
    controller.tick(0.05);
    CHECK(view.status.nextRate == 96000.0);
    controller.chooseSampleRate(48000.0);   // the rate it runs at: nothing pending
    controller.tick(0.1);
    CHECK(view.status.nextRate == 0.0);
    controller.chooseSampleRate(0.0);   // the device's own: nothing pending
    controller.tick(0.15);
    CHECK(view.status.nextRate == 0.0);
    controller.chooseSampleRate(96000.0);
    controller.tick(0.2);
    CHECK(view.status.nextRate == 96000.0);
}

// Control links (docs/controls.md, milestone 2): learned for a row's
// parameter, then its gain on the same control (a macro); a key, a control a
// trigger uses, and a row's gain for all songs refused; moved to all songs
// (a row's parameter becomes its plugin's) once nothing holds it to the song;
// saved in the rack and the rig; removed.
void testControlLinks() {
    namespace fs = std::filesystem;
    using Kind = ControlLinkTarget::Kind;
    const fs::path dir = scratchDir("stagehand-links-test");
    fs::remove_all(dir);
    Rig rig;
    rig.session->addOpenSource(MidiSourceRef{MidiSourceRef::Kind::Virtual, "MIDI 2.0 In"}, 7);
    Rack rack;
    rack.name  = "Song";
    rack.slots = {slot("keys", "synth", "/clap/Synth.clap")};
    Session::LoadReport report;
    rig.session->load(rack, 48000.0, report);
    RackController::Config config = rig.config();
    config.rig                    = true;
    RackController       controller(*rig.session, *rig.engine, rig.midi, rig.factory, config);
    const std::string    gig = (dir / "Gig.setlist").string();
    CHECK(controller.saveAs(gig) == RackController::SaveAsResult::Saved);   // a set to save into
    controller.takeNotices();
    const std::string rackPath = setRackPath(gig, controller.songRack()), rigPath = setRigPath(gig);
    const RackViewModel& view = controller.view();
    rig.factory.created.at("synth")->params = {ParameterInfo{7, nullptr, "Cutoff", 0.0, 1.0, 0.5},
                                               ParameterInfo{8, nullptr, "Hidden", 0.0, 1.0, 0.5, false}};
    rig.factory.created.at("synth")->values[7] = 0.25;
    double now  = 0.0;
    auto   tick = [&] { controller.tick(now += 0.05); };
    auto   send = [&](std::uint32_t word) {
        TimedMidiMessage m;
        m.captureTime       = 1'000'000'000 - 1'000'000;
        m.message.words[0]  = word;
        m.message.wordCount = 1;
        rig.engine->onMidi(7, m);
        runBlock(*rig.engine);
        tick();
    };
    tick();
    controller.takeNotices();
    CHECK(controller.rowParameters("keys").size() == 1);   // automatable ones only

    ControlLinkTarget cutoff;
    cutoff.row   = "keys";
    cutoff.param = 7;
    controller.learnControlLink(cutoff, ControlScope::Song);
    tick();
    CHECK(view.controlLinkLearning);
    send(0x20B04A7Fu);   // CC 74
    CHECK(!view.controlLinkLearning && view.controlLinks.size() == 1);
    CHECK(controller.takeNotices() ==
          std::vector<std::string>{"Control Link: From other apps (MIDI 2.0), CC 74, ch 1 \xE2\x86\x92 keys: Cutoff"});
    if (view.controlLinks.size() == 1) {
        const ControlLinkModel& l = view.controlLinks[0];
        CHECK(l.scope == ControlScope::Song && l.pickUp && !l.passOn && l.connected && l.targets.size() == 1);
        CHECK(l.targets[0].text == "keys: Cutoff" && l.targets[0].present && l.targets[0].target.bottom == 0.0 &&
              l.targets[0].target.top == 1.0 && l.targets[0].target.paramName == "Cutoff");
        // For the menu and dialog: the control as saved, the rows it acts on, the parameter's scale.
        CHECK(l.number == 74 && l.channel == 0 && l.setup() == rig.session->controlLinks(ControlScope::Song)[0]);
        CHECK(l.targets[0].rows == std::vector<std::string>{"keys"} && l.targets[0].minValue == 0.0 &&
              l.targets[0].maxValue == 1.0 && !l.targets[0].stepped);
    }
    CHECK(rig.session->modified());
    // Live, once the engine runs it: the parameter where it was compiled (0.25),
    // the control at the top (CC 74 at 127 was the learn), not yet moved since.
    runBlock(*rig.engine);
    tick();
    runBlock(*rig.engine);
    tick();
    if (view.controlLinks.size() == 1) {
        const ControlLinkModel& l = view.controlLinks[0];
        CHECK(l.targets[0].now == 0.25 && l.travel < 0.0 && !l.targets[0].waiting);
    }
    send(0x20B04A00u);   // CC 74 at 0: waiting for pick-up
    if (view.controlLinks.size() == 1)
        CHECK(view.controlLinks[0].travel == 0.0 && view.controlLinks[0].targets[0].waiting &&
              view.controlLinks[0].targets[0].now == 0.25);

    // The same control again: the link gets a second target.
    ControlLinkTarget gain;
    gain.kind = Kind::Gain;
    gain.row  = "keys";
    controller.learnControlLink(gain, ControlScope::Song);
    send(0x20B04A7Fu);
    CHECK(view.controlLinks.size() == 1 && view.controlLinks[0].targets.size() == 2);
    if (view.controlLinks[0].targets.size() == 2)
        CHECK(view.controlLinks[0].targets[1].text == "keys gain" && view.controlLinks[0].targets[1].target.bottom == -60.0 &&
              view.controlLinks[0].targets[1].minValue == -60.0 && view.controlLinks[0].targets[1].maxValue == 6.0);
    controller.takeNotices();

    // Learn again: the link's control changes, its targets stay.
    controller.relearnControlLink(ControlScope::Song, 0);
    tick();
    CHECK(view.controlLinkLearning && !view.controlLinks.empty() && view.controlLinks[0].learning);
    send(0x20B04B7Fu);   // CC 75
    CHECK(!view.controlLinkLearning && view.controlLinks.size() == 1 && view.controlLinks[0].number == 75 &&
          view.controlLinks[0].targets.size() == 2 && !view.controlLinks[0].learning);
    CHECK(controller.takeNotices() ==
          std::vector<std::string>{"Control Link: now From other apps (MIDI 2.0), CC 75, ch 1"});
    // Not to a control another link has.
    controller.learnControlLink(cutoff, ControlScope::Song);
    send(0x20B04C7Fu);   // CC 76: a second link
    controller.takeNotices();
    controller.relearnControlLink(ControlScope::Song, 1);
    send(0x20B04B7Fu);   // CC 75, the first's
    CHECK(controller.takeNotices().size() == 1 && view.controlLinks.size() == 2 && view.controlLinks[1].number == 76);
    controller.removeControlLink(ControlScope::Song, 1);
    controller.relearnControlLink(ControlScope::Song, 0);
    send(0x20B04A7Fu);   // back to CC 74
    controller.takeNotices();
    CHECK(view.controlLinks.size() == 1 && view.controlLinks[0].number == 74);

    // Add a target from the list: a parameter it has already is reset to its
    // whole range; one that isn't there is refused.
    ControlLinkSetup narrowed = rig.session->controlLinks(ControlScope::Song)[0];
    narrowed.targets[0].bottom = 0.3;
    controller.setControlLink(ControlScope::Song, 0, narrowed);
    controller.addControlLinkTarget(ControlScope::Song, 0, cutoff);
    CHECK(rig.session->controlLinks(ControlScope::Song)[0].targets.size() == 2 &&
          rig.session->controlLinks(ControlScope::Song)[0].targets[0].bottom == 0.0);
    ControlLinkTarget unknown = cutoff;
    unknown.param             = 99;
    controller.addControlLinkTarget(ControlScope::Song, 0, unknown);
    CHECK(controller.takeNotices().size() == 1 && rig.session->controlLinks(ControlScope::Song)[0].targets.size() == 2);

    // Refused: a key; a control a trigger answers; a row's gain for all songs.
    controller.learnControlLink(cutoff, ControlScope::Song);
    send(0x20903C64u);
    CHECK(controller.takeNotices().size() == 1 && view.controlLinks.size() == 1);
    TransportSettings transport;
    SequenceTrigger   start;
    start.action  = SequenceTrigger::Action::Start;
    start.control = SequenceTrigger::Control::Controller;
    start.number  = 80;
    transport.triggers = {start};
    rig.session->setTransportSettings(transport);
    controller.learnControlLink(cutoff, ControlScope::Song);
    send(0x20B0507Fu);
    std::vector<std::string> notices = controller.takeNotices();
    CHECK(notices.size() == 1 && notices[0].find("already the transport's") != std::string::npos);
    controller.learnControlLink(gain, ControlScope::AllSongs);
    tick();
    CHECK(!view.controlLinkLearning && controller.takeNotices().size() == 1);

    // To all songs: refused while it drives the row's gain; then, without it, moved.
    CHECK(!controller.setControlLinkScope(ControlScope::Song, 0, ControlScope::AllSongs));
    CHECK(controller.takeNotices().size() == 1);
    ControlLinkSetup edited = rig.session->controlLinks(ControlScope::Song)[0];
    edited.targets.pop_back();
    edited.pickUp = false;
    controller.setControlLink(ControlScope::Song, 0, edited);
    CHECK(controller.setControlLinkScope(ControlScope::Song, 0, ControlScope::AllSongs) == std::optional<std::size_t>{0});
    tick();
    CHECK(view.controlLinks.size() == 1 && view.controlLinks[0].scope == ControlScope::AllSongs &&
          !view.controlLinks[0].pickUp);
    if (!view.controlLinks.empty() && !view.controlLinks[0].targets.empty()) {
        const ControlLinkTargetModel& t = view.controlLinks[0].targets[0];
        CHECK(t.target.plugin == "synth" && t.target.row.empty() && t.text == "synth (every row): Cutoff" && t.present);
    }
    CHECK(rig.session->rigModified());
    // For all songs, a row's gain is refused as a target.
    controller.addControlLinkTarget(ControlScope::AllSongs, 0, gain);
    CHECK(controller.takeNotices().size() == 1 && rig.session->controlLinks(ControlScope::AllSongs)[0].targets.size() == 1);

    // A song's link on the same control overrides it.
    controller.learnControlLink(cutoff, ControlScope::Song);
    send(0x20B04A7Fu);
    CHECK(view.controlLinks.size() == 2 && view.controlLinks[1].overridden && !view.controlLinks[0].overridden);

    // Saved: the song's in the rack, all songs' in the rig.
    CHECK(controller.save());
    stagehand::Rack savedRack;
    stagehand::Rig  savedRig;
    CHECK(loadRack(rackPath, savedRack).ok && savedRack.controls.size() == 1 &&
          savedRack.controls[0].targets[0].row == "keys");
    CHECK(loadRig(rigPath, savedRig).ok && savedRig.controls.size() == 1 &&
          savedRig.controls[0].targets[0].plugin == "synth" && !savedRig.controls[0].pickUp);

    controller.removeControlLink(ControlScope::AllSongs, 0);
    controller.removeControlLink(ControlScope::Song, 0);
    tick();
    CHECK(view.controlLinks.empty());

    runBlock(*rig.engine);
    rig.engine->collect();
    fs::remove_all(dir);
}

// Made on screen (no hardware), its control changed on screen, and learned
// from the editor (docs/controls.md, milestones 4 and 5).
void testMadeControlLinks() {
    using Kind = ControlLinkTarget::Kind;
    Rig rig;
    rig.session->addOpenSource(MidiSourceRef{MidiSourceRef::Kind::Virtual, "MIDI 2.0 In"}, 7);
    Rack rack;
    rack.name  = "Song";
    rack.slots = {slot("keys", "synth", "/clap/Synth.clap")};
    Session::LoadReport report;
    rig.session->load(rack, 48000.0, report);
    RackController::Config config = rig.config();
    config.rig                    = true;
    RackController       controller(*rig.session, *rig.engine, rig.midi, rig.factory, config);
    const RackViewModel& view  = controller.view();
    auto*                synth = rig.factory.created.at("synth");
    synth->params = {ParameterInfo{7, nullptr, "Cutoff", 0.0, 1.0, 0.5}, ParameterInfo{8, nullptr, "Hidden", 0.0, 1.0, 0.5, false}};
    double now  = 0.0;
    auto   tick = [&] { controller.tick(now += 0.05); };
    auto   send = [&](std::uint32_t word) {
        TimedMidiMessage m;
        m.captureTime       = 1'000'000'000 - 1'000'000;
        m.message.words[0]  = word;
        m.message.wordCount = 1;
        rig.engine->onMidi(7, m);
        runBlock(*rig.engine);
        tick();
    };
    auto touch = [&](EventType type, std::uint32_t param) {
        Event e;
        e.type     = type;
        e.param.id = param;
        e.value    = 0.5;
        synth->emit.push_back(e);
        runBlock(*rig.engine);
        tick();
    };
    tick();
    controller.takeNotices();
    const std::vector<ControlLinkSetup>& links = rig.session->controlLinks(ControlScope::Song);

    // CC 11 from any input, channel 1, chosen from the list, driving Cutoff:
    // as a learned link would be, over the parameter's whole range.
    ControlLinkSetup control;
    control.number = 11;
    ControlLinkTarget cutoff;
    cutoff.row   = "keys";
    cutoff.param = 7;
    CHECK(controller.makeControlLink(control, cutoff, ControlScope::Song) == std::optional<std::size_t>{0});
    std::vector<std::string> notices = controller.takeNotices();
    CHECK(notices.size() == 1 && notices[0].find("CC 11") != std::string::npos &&
          notices[0].find("keys: Cutoff") != std::string::npos);
    CHECK(links.size() == 1 && links[0].source.kind == MidiSourceRef::Kind::All && links[0].number == 11 &&
          links[0].targets.size() == 1 && links[0].targets[0].paramName == "Cutoff" && links[0].targets[0].bottom == 0.0 &&
          links[0].targets[0].top == 1.0 && links[0].pickUp);
    tick();
    CHECK(view.controlLinks.size() == 1 && !view.controlLinkLearning);
    // The same control again: another target, a macro.
    ControlLinkTarget gain;
    gain.kind = Kind::Gain;
    gain.row  = "keys";
    CHECK(controller.makeControlLink(control, gain, ControlScope::Song) == std::optional<std::size_t>{0});
    CHECK(links.size() == 1 && links[0].targets.size() == 2 && links[0].targets[1].bottom == -60.0);
    // Refused: a parameter that can't be linked; a row's gain for all songs.
    ControlLinkTarget hidden = cutoff;
    hidden.param             = 8;
    control.number           = 12;
    CHECK(!controller.makeControlLink(control, hidden, ControlScope::Song));
    CHECK(!controller.makeControlLink(control, gain, ControlScope::AllSongs));
    CHECK(controller.takeNotices().size() == 3 && links.size() == 1);

    // What uses a control, for the list: another link in the same scope (not
    // the one being edited, not the other scope's), or a trigger.
    control.number = 11;
    CHECK(controller.controlUse(control, ControlScope::Song) == "another Control Link");
    CHECK(controller.controlUse(control, ControlScope::Song, 0).empty());
    CHECK(controller.controlUse(control, ControlScope::AllSongs).empty());
    TransportSettings transport;
    SequenceTrigger   start;
    start.action  = SequenceTrigger::Action::Start;
    start.control = SequenceTrigger::Control::Controller;
    start.number  = 80;
    transport.triggers = {start};
    rig.session->setTransportSettings(transport);
    ControlLinkSetup cc80;
    cc80.number = 80;
    CHECK(controller.controlUse(cc80, ControlScope::Song) == "the transport's");
    CHECK(!controller.makeControlLink(cc80, cutoff, ControlScope::Song));
    notices = controller.takeNotices();
    CHECK(notices.size() == 1 && notices[0].find("already the transport's") != std::string::npos && links.size() == 1);

    // Its control changed on screen: channel 2, CC 12; the targets stay.
    ControlLinkSetup moved = control;
    moved.channel          = 1;
    moved.number           = 12;
    CHECK(controller.setControlLinkControl(ControlScope::Song, 0, moved));
    CHECK(links.size() == 1 && links[0].channel == 1 && links[0].number == 12 && links[0].targets.size() == 2);
    CHECK(controller.takeNotices().size() == 1);
    // Refused: another link's control, a trigger's.
    control.number = 13;
    CHECK(controller.makeControlLink(control, cutoff, ControlScope::Song) == std::optional<std::size_t>{1});
    CHECK(!controller.setControlLinkControl(ControlScope::Song, 1, moved));
    CHECK(!controller.setControlLinkControl(ControlScope::Song, 1, cc80));
    CHECK(controller.takeNotices().size() == 3 && links.size() == 2 && links[1].number == 13);
    controller.removeControlLink(ControlScope::Song, 1);

    // Learn from the editor: the generic one opens (the synth has none of its
    // own); a parameter that can't be linked is said once and skipped; the
    // first that can is the target; then the hardware control is learned.
    ControlLinkTarget place;
    place.row = "keys";
    controller.learnControlLinkFromEditor(place, ControlScope::Song);
    tick();
    CHECK(view.controlLinkLearning && view.controlLinkTouching == "keys" && *rig.editorsOpen == 1);
    touch(EventType::ParamGestureBegin, 8);
    touch(EventType::ParamGestureBegin, 8);
    CHECK(controller.takeNotices().size() == 1 && view.controlLinkTouching == "keys");
    touch(EventType::ParamGestureBegin, 7);
    notices = controller.takeNotices();
    CHECK(notices.size() == 1 && notices[0].find("Cutoff") != std::string::npos);
    CHECK(view.controlLinkTouching.empty() && view.controlLinkLearning);
    send(0x20B04E7Fu);   // CC 78
    CHECK(!view.controlLinkLearning && links.size() == 2 && links[1].number == 78 && links[1].targets.size() == 1 &&
          links[1].targets[0].param == 7 && links[1].targets[0].row == "keys");
    controller.takeNotices();
    // A value the plugin changes itself counts as a touch too, and so does the generic editor's.
    controller.learnControlLinkFromEditor(place, ControlScope::Song);
    tick();
    CHECK(rig.engine->setParameter(synth, 7, 0.4));
    runBlock(*rig.engine);
    tick();
    CHECK(view.controlLinkTouching.empty() && view.controlLinkLearning);
    controller.cancelLearn();
    controller.learnControlLinkFromEditor(place, ControlScope::Song);
    tick();
    touch(EventType::ParamValue, 7);
    CHECK(view.controlLinkTouching.empty() && view.controlLinkLearning);
    controller.takeNotices();
    // Esc while waiting for the touch: nothing is armed after it.
    controller.cancelLearn();
    controller.learnControlLinkFromEditor(place, ControlScope::Song);
    tick();
    controller.cancelLearn();
    tick();
    CHECK(!view.controlLinkLearning && view.controlLinkTouching.empty());
    touch(EventType::ParamGestureBegin, 7);
    CHECK(!view.controlLinkLearning && controller.takeNotices().empty());
    // Another learn replaces it: a touch then does nothing.
    controller.learnControlLinkFromEditor(place, ControlScope::Song);
    tick();
    controller.learnControlLink(gain, ControlScope::Song);
    tick();
    CHECK(view.controlLinkTouching.empty() && view.controlLinkLearning);
    touch(EventType::ParamGestureBegin, 7);
    CHECK(view.controlLinkLearning && controller.takeNotices().empty());
    controller.cancelLearn();

    runBlock(*rig.engine);
    rig.engine->collect();
}

} // namespace

int main() {
    testViewModel();
    testMetersAndActivity();
    testEditorsAndRemove();
    testMidiEffects();
    testGenericEditor();
    testAddPlugin();
    testSave();
    testFiles();
    testTransport();
    testDock();
    testLearn();
    testTransportControls();
    testFindPlaybackDevice();
    testClickRouting();
    testRenameSequence();
    testRenameRow();
    testSetlist();
    testRigs();
    testPreparedRacks();
    testSetlistTriggers();
    testRigControls();
    testSwitchSettings();
    testSavingInASet();
    testLoopers();
    testKeepLoop();
    testLoopNumbers();
    testCheckSetlist();
    testHotplug();
    testPanic();
    testDspAndXruns();
    testAudioMenus();
    testNextRateShown();
    testControlLinks();
    testMadeControlLinks();
    return test::finish("RackController");
}
