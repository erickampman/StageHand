// RackController tests with fakes: the view model (rows, sources, status,
// protocol warning), meter ballistics and MIDI activity, editors, adding and
// removing plugins, and saving. No DGL, X11 or JACK.

#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "App/RackController.h"
#include "Check.h"
#include "FakePlugin.h"
#include "SessionFakes.h"
#include "Session/RackFile.h"
#include "Session/RigFile.h"
#include "Session/SetlistFile.h"

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
        c.audioStatus   = [] { return RackController::AudioStatus{48000.0, 256, 3, 4}; };
        c.quit          = [this] { ++quits; };
        c.virtualInputs = {{"MIDI 2.0 In", MidiProtocol::Midi2}, {"MIDI 1.0 In", MidiProtocol::Midi1}};
        c.saveTarget    = saveTarget;
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
    CHECK(view.status.saveTarget.empty() && view.status.noSaveReason == "Untitled: File > Save As keeps it");

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
    for (const auto [n, d, lights] : {std::tuple{7, 8, 7}, {9, 8, 3}, {12, 8, 4}, {3, 8, 3}, {5, 4, 5}, {2, 4, 2}}) {
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
    controller.newRack();
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
          rig.session->metronome().clickOutput == "Built-in Audio Analog Stereo");
    routing.devices.erase(routing.devices.begin());   // only the mains' device left
    controller.setClick(false);
    controller.setClickOutput("");
    controller.setClick(true);
    CHECK(routing.connected.empty() && rig.session->metronome().clickOutput.empty());
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
        CHECK(notices.size() == 1 && notices[0] == "Not saved: this rack is untitled; use Save As");
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

// New, Open and Save As: switching racks while running, the save-first rule,
// and what Save As will and won't replace.
void testFiles() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "stagehand-files-test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir / "notes.rack");   // exists, but isn't a rack

    Rack gig;
    gig.name     = "Gig";
    gig.slots    = {slot("keys", "synth", "/clap/Synth.clap")};
    Sequence arp;
    arp.id       = "arp";
    gig.sequences = {arp};
    const std::string gigPath = (dir / "Gig.rack").string();
    CHECK(saveRack(gigPath, gig).ok);

    Rig                 rig;
    Session::LoadReport report;
    Rack                untitled;
    untitled.name = "Untitled";
    rig.session->load(untitled, 48000.0, report);
    RackController       controller(*rig.session, *rig.engine, rig.midi, rig.factory, rig.config());
    const RackViewModel& view = controller.view();
    CHECK(controller.rackPath().empty() && !controller.unsavedUntitled());

    // Changes to an untitled rack are what the UI asks about.
    controller.addPlugin(installedPlugin("bass", "/clap/Bass.clap"), 0);
    CHECK(controller.unsavedUntitled());

    // Open: by the bundle or its rack.json. Editors close; the generation moves.
    controller.tick(0.5);
    controller.openEditor(view.rows[0].id);
    CHECK(*rig.editorsOpen == 1);
    const std::uint64_t before = controller.rackGeneration();
    CHECK(controller.openRack((dir / "Gig.rack" / "rack.json").string()));
    controller.tick(1.0);
    CHECK(*rig.editorsOpen == 0 && controller.rackGeneration() == before + 1);
    CHECK(controller.rackPath() == gigPath && view.rackName == "Gig" && !view.modified);
    CHECK(view.rows.size() == 1 && view.rows[0].id == "keys" && view.sequences.size() == 1);
    runBlock(*rig.engine);   // the old plugins go once the engine lets go of them
    rig.engine->collect();

    // Not a rack: nothing changes.
    controller.takeNotices();
    CHECK(!controller.openRack((dir / "notes.rack").string()));
    CHECK(controller.takeNotices().size() == 1 && controller.rackPath() == gigPath);

    // New saves the titled rack first (as quitting would), then starts over.
    controller.setMuted("keys", true);
    CHECK(controller.newRack());
    controller.tick(1.1);
    CHECK(controller.rackPath().empty() && view.rows.empty() && view.rackName == "Untitled");
    Rack reread;
    CHECK(loadRack(gigPath, reread).ok && reread.slots.size() == 1 && reread.slots[0].muted);

    // Save As: ".rack" added, the name follows the file. An existing rack needs
    // a yes; something that isn't a rack is never replaced.
    controller.addPlugin(installedPlugin("synth", "/clap/Synth.clap"), 0);
    CHECK(controller.saveAs((dir / "notes").string()) == RackController::SaveAsResult::Failed);
    CHECK(controller.saveAs(gigPath) == RackController::SaveAsResult::NeedsReplace);
    CHECK(controller.saveAs((dir / "Live").string()) == RackController::SaveAsResult::Saved);
    controller.tick(1.2);
    CHECK(controller.rackPath() == (dir / "Live.rack").string() && view.rackName == "Live" && !view.modified);
    CHECK(!controller.unsavedUntitled());
    CHECK(controller.saveAs(gigPath, true) == RackController::SaveAsResult::Saved);
    CHECK(loadRack(gigPath, reread).ok && reread.name == "Gig" && reread.slots.size() == 1 &&
          reread.slots[0].plugin.id == "synth");

    // Saving off (--no-save-rack): switching doesn't save, Save As refuses.
    RackController::Config off = rig.config(gigPath);
    off.savingOff              = true;
    RackController readOnly(*rig.session, *rig.engine, rig.midi, rig.factory, off);
    CHECK(readOnly.saveAs((dir / "Other").string()) == RackController::SaveAsResult::Failed);
    CHECK(readOnly.newRack() && !std::filesystem::exists(dir / "Other.rack"));
    CHECK(!readOnly.unsavedUntitled());

    rig.session->unload();
    runBlock(*rig.engine);
    rig.engine->collect();
    std::filesystem::remove_all(dir);
}

// Setlists (docs/setlists.md, milestone 1): making one, going through it,
// editing it (saved at once), missing racks skipped, Save As re-pointing the
// song, the last position, and leaving it by opening a rack on its own.
void testSetlist() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "stagehand-setlist-test";
    fs::remove_all(dir);
    for (const char* name : {"A", "B", "C"}) {
        Rack rack;
        rack.name  = name;
        rack.slots = {slot("keys", "synth", "/clap/Synth.clap")};
        CHECK(saveRack((dir / "Racks" / (std::string(name) + ".rack")).string(), rack).ok);
    }
    const std::string a = (dir / "Racks/A.rack").string(), b = (dir / "Racks/B.rack").string(),
                      c = (dir / "Racks/C.rack").string();
    const std::string lastPath = (dir / "state/last.json").string();

    Rig                 rig;
    Session::LoadReport report;
    Rack                untitled;
    untitled.name = "Untitled";
    rig.session->load(untitled, 48000.0, report);
    RackController::Config config = rig.config();
    config.lastPositionPath       = lastPath;
    RackController       controller(*rig.session, *rig.engine, rig.midi, rig.factory, config);
    const RackViewModel& view = controller.view();
    double               now  = 0.0;
    auto tick = [&] { controller.tick(now += 2.0); };   // past the once-a-second disk check

    // An untitled rack can't be a song.
    const std::string gig = (dir / "Gig").string();
    CHECK(controller.newSetlist(gig, true) == RackController::NewSetlistResult::Failed);
    CHECK(!controller.hasSetlist());

    // New setlist with this rack: it's song 1, and the file exists at once.
    CHECK(controller.openRack(a));
    CHECK(controller.newSetlist(gig, true) == RackController::NewSetlistResult::Created);
    const std::string file = (dir / "Gig.setlist").string();
    CHECK(controller.hasSetlist() && controller.setlistPath() == file && isSetlistFile(file));
    tick();
    CHECK(view.setlist.open && view.setlist.name == "Gig" && view.setlist.songs.size() == 1);
    CHECK(view.setlist.current == 0 && view.setlist.next == -1 && view.setlist.songs[0].name == "A");
    CHECK(controller.newSetlist(gig, false) == RackController::NewSetlistResult::NeedsReplace);

    // Adding: after the current song, without switching; a non-rack is refused.
    CHECK(controller.addRackToSetlist(b));
    CHECK(!controller.addRackToSetlist((dir / "Nothing").string()));
    tick();
    CHECK(view.setlist.songs.size() == 2 && view.setlist.songs[1].name == "B" && controller.rackPath() == a);
    Setlist saved;
    CHECK(loadSetlist(file, saved).ok && saved.songs.size() == 2 && saved.songs[1].rack == b);

    // Next and Previous, and the remembered position.
    CHECK(controller.nextSong() && controller.rackPath() == b);
    CHECK(!controller.nextSong());   // the end
    LastPosition last;
    CHECK(loadLastPosition(lastPath, last) && last.setlist == file && last.song == 1);
    CHECK(controller.previousSong() && controller.rackPath() == a && !controller.previousSong());

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
    CHECK(loadSetlist(file, saved).ok && saved.songs[0].name.empty());

    // Remove the playing song: the rack still plays; Next goes to the one after.
    CHECK(controller.removeSong(1));   // [B, A]
    tick();
    CHECK(view.setlist.current == -1 && view.setlist.next == 1 && controller.rackPath() == a);
    CHECK(fs::exists(a));   // the rack stays on disk
    CHECK(controller.nextSong() && view.setlist.current == -1);   // tick() hasn't run yet
    tick();
    CHECK(view.setlist.current == 1);

    // A missing rack is shown and skipped.
    CHECK(controller.addRackToSetlist(c));   // [B, A, C]
    fs::rename(c, (dir / "Racks/C-away.rack").string());
    CHECK(controller.previousSong() && controller.rackPath() == b);   // previous from A
    tick();
    CHECK(view.setlist.songs[2].missing && view.setlist.next == 1);
    CHECK(controller.nextSong() && !controller.nextSong() && !controller.goToSong(2));
    // Found again: pointed at where it went. A missing song can also be removed.
    CHECK(!controller.relinkSong(2, (dir / "Nothing").string()));
    CHECK(controller.relinkSong(2, (dir / "Racks/C-away.rack").string()) && controller.goToSong(2));
    CHECK(controller.goToSong(1));
    fs::rename((dir / "Racks/C-away.rack").string(), c);
    CHECK(controller.relinkSong(2, c));

    // Save As while on a song points the song at the new bundle.
    CHECK(controller.saveAs((dir / "Racks/A2").string()) == RackController::SaveAsResult::Saved);
    CHECK(loadSetlist(file, saved).ok && saved.songs[1].rack == (dir / "Racks/A2.rack").string());

    // A new rack added to the set is created on disk and played.
    CHECK(controller.addNewRackToSetlist((dir / "Racks/D").string()));
    CHECK(controller.rackPath() == (dir / "Racks/D.rack").string() && !rackBundleOf((dir / "Racks/D.rack").string()).empty());
    CHECK(!controller.addNewRackToSetlist((dir / "Racks/D").string()));   // exists: refused

    // Reopening at a song, as a start with no arguments does.
    CHECK(controller.openSetlist(file, 0) && controller.rackPath() == b);
    CHECK(controller.openSetlist(file, 9) && controller.rackPath() == c);   // clamped to the last

    // Opening a rack on its own leaves the set, and forgets it.
    CHECK(controller.openRack(a));
    tick();
    CHECK(!controller.hasSetlist() && !view.setlist.open);
    CHECK(loadLastPosition(lastPath, last) && last.setlist.empty());

    // So does a new rack outside the set: untitled, on its own.
    CHECK(controller.openSetlist(file, 0) && controller.hasSetlist());
    CHECK(controller.newRack() && !controller.hasSetlist() && controller.rackPath().empty());

    runBlock(*rig.engine);
    rig.engine->collect();
    fs::remove_all(dir);
}

// Rigs (docs/rig.md): the default one created on first use, a setlist's own
// used while it's open (and the default after), and Ctrl-S saving the rig
// with the rack. Switching rigs saves the one in use if it has changes.
// (`Rig` here is the test harness; the rig model is stagehand::Rig.)
void testRigs() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "stagehand-rigs-test";
    fs::remove_all(dir);
    Rack song;
    song.name  = "Song";
    song.slots = {slot("keys", "synth", "/clap/Synth.clap")};
    const std::string songPath = (dir / "Song.rack").string();
    CHECK(saveRack(songPath, song).ok);
    stagehand::Rig stage;
    stage.buses[0].returnDb     = -3.0f;
    const std::string stagePath = (dir / "Stage.rig").string();
    CHECK(saveRig(stagePath, stage).ok);
    Setlist set;
    set.songs = {SetlistSong{songPath, ""}};
    set.rig   = stagePath;
    const std::string setPath = (dir / "Gig.setlist").string();
    CHECK(saveSetlist(setPath, set).ok);

    Rig                 rig;
    Session::LoadReport report;
    Rack                untitled;
    untitled.name = "Untitled";
    rig.session->load(untitled, 48000.0, report);
    RackController::Config config = rig.config();
    config.defaultRigPath         = (dir / "data" / "Default.rig").string();
    RackController controller(*rig.session, *rig.engine, rig.midi, rig.factory, config);

    // Started on its own: the default rig, created empty where the app keeps it.
    CHECK(controller.rigPath() == config.defaultRigPath && rig.session->hasRig());
    CHECK(!rigBundleOf(config.defaultRigPath).empty());

    // A setlist's own rig while it's open.
    CHECK(controller.openSetlist(setPath));
    CHECK(controller.rigPath() == stagePath && rig.session->rig().saved.buses[0].returnDb == -3.0f);

    // Ctrl-S saves the rig with the rack.
    CHECK(rig.session->setReturnDb(0, -10.0f) && rig.session->rigModified());
    CHECK(controller.save() && !rig.session->rigModified());
    stagehand::Rig onDisk;
    CHECK(loadRig(stagePath, onDisk).ok && onDisk.buses[0].returnDb == -10.0f);

    // Leaving the set: the default rig again, the setlist's saved first.
    CHECK(rig.session->setReturnDb(0, -12.0f));
    controller.closeSetlist();
    CHECK(controller.rigPath() == config.defaultRigPath && rig.session->rig().saved.buses[0].returnDb == 0.0f);
    CHECK(loadRig(stagePath, onDisk).ok && onDisk.buses[0].returnDb == -12.0f);

    runBlock(*rig.engine);
    rig.engine->collect();
    fs::remove_all(dir);
}

} // namespace

int main() {
    testViewModel();
    testMetersAndActivity();
    testEditorsAndRemove();
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
    testSetlist();
    testRigs();
    return test::finish("RackController");
}
