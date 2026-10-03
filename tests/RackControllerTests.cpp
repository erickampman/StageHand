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

void testSave() {
    const std::filesystem::path dir = scratchDir("stagehand-controller-test");
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
    const std::filesystem::path dir = scratchDir("stagehand-files-test");
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
    const fs::path dir = scratchDir("stagehand-setlist-test");
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
    // It's A again: the same rack (one copy per path), so only the position moves.
    const std::uint64_t generation = controller.rackGeneration();
    CHECK(controller.nextSong() && view.setlist.current == 1 && controller.rackGeneration() == generation);

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

    // A new rack added to the set is created on disk and played: a copy of
    // the song playing (A2, with an edit not yet saved), named for its bundle.
    controller.setTempo(97.0);
    CHECK(controller.addNewRackToSetlist((dir / "Racks/D").string()));
    for (int i = 0; i < 4 && controller.rackPath() != (dir / "Racks/D.rack").string(); ++i) tick();   // its plugin prepared
    CHECK(controller.rackPath() == (dir / "Racks/D.rack").string() && !rackBundleOf((dir / "Racks/D.rack").string()).empty());
    Rack copied;
    CHECK(loadRack((dir / "Racks/D.rack").string(), copied).ok && copied.name == "D" && copied.tempoBpm == 97.0 &&
          copied.slots.size() == 1 && copied.slots[0].id == "keys");
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
    const fs::path dir = scratchDir("stagehand-rigs-test");
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
    CHECK(controller.openSetlist(file, 0) && controller.rackPath() == a && !controller.switchPending());
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
    CHECK(controller.nextSong() && controller.rackPath() == b && !controller.switchPending());
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
    CHECK(controller.previousSong() && controller.rackPath() == a);
    CHECK(rig.session->slot("row1") != nullptr && rig.session->slot("row1")->saved.gainDb == -6.0f);

    // A jump to a rack not prepared waits for it while the current one plays;
    // a second jump replaces the first.
    CHECK(controller.goToSong(3) && controller.switchPending() && controller.rackPath() == a);
    tick();
    CHECK(controller.rackPath() == a && view.setlist.waiting == 3);
    CHECK(view.status.preparing == "Preparing song 4 (D): 1/2 plugins");
    CHECK(controller.goToSong(2) && controller.switchPending());
    tick();   // C: one plugin, then the switch
    CHECK(controller.rackPath() == c && !controller.switchPending() && view.setlist.current == 2);
    CHECK(view.setlist.waiting == -1);

    // A reprise: B before and after C is one prepared copy, ready for both at once.
    CHECK(controller.addRackToSetlist(b));   // [A, B, C, B, D]
    for (int i = 0; i < 6 && !controller.songPrepared(1); ++i) tick();
    CHECK(controller.songPrepared(1) && controller.songPrepared(3));
    CHECK(controller.goToSong(3) && controller.rackPath() == b && view.setlist.current == 3);
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
    CHECK(controller.openSetlist(file, 0) && controller.rackPath() == racks[0]);

    // Learn Next on CC 80, and Program Change on channel 1; saved at once.
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
    Setlist saved;
    CHECK(loadSetlist(file, saved).ok && saved.triggers.size() == 2 &&
          saved.triggers[0].action == SetlistTrigger::Action::Next && saved.triggers[0].number == 80);
    controller.takeNotices();

    // The pedal goes to the next song (B is prepared by now).
    send(0x20B0507Fu);
    send(0x20B05000u);
    tick();
    CHECK(controller.rackPath() == racks[1] && view.setlist.current == 1);

    // Two presses in one tick: two songs on, in one switch. C's own Start on
    // the same pedal is reported, and the setlist's wins.
    controller.takeNotices();
    const std::uint64_t generation = controller.rackGeneration();
    send(0x20B0507Fu);
    send(0x20B05000u);
    send(0x20B0507Fu);
    send(0x20B05000u);
    for (int i = 0; i < 4 && (controller.switchPending() || controller.rackPath() == racks[1]); ++i) tick();
    CHECK(controller.rackPath() == racks[3] && controller.rackGeneration() == generation + 1);
    send(0x20C00200u);   // Program Change 2: the third song, C
    for (int i = 0; i < 4 && controller.rackPath() != racks[2]; ++i) tick();
    CHECK(controller.rackPath() == racks[2] && !rig.engine->transport().running);
    bool reported = false;
    for (const std::string& text : controller.takeNotices())
        reported = reported || text.find("is also the setlist's Next song; the setlist's wins") != std::string::npos;
    CHECK(reported);
    send(0x20C00900u);   // past the end: a notice, no switch
    tick();
    CHECK(controller.rackPath() == racks[2] && !controller.takeNotices().empty());

    // Removed: the pedal no longer acts. Closing the set removes them all.
    controller.removeSetlistTrigger(0);
    tick();
    CHECK(view.setlist.triggers.size() == 1 && rig.session->setlistTriggers().size() == 1);
    controller.closeSetlist();
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
    RackController::Config config = rig.config();
    config.defaultRigPath         = rigPath;
    RackController       controller(*rig.session, *rig.engine, rig.midi, rig.factory, config);
    const RackViewModel& view = controller.view();

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

    // A bus effect's editor stays open when the rack changes; emptying its slot closes it.
    controller.openBusEditor(0, 0);
    controller.tick(1.3);
    CHECK(*rig.editorsOpen == 1 && view.rig.buses[0].effects[0].editorOpen);
    CHECK(controller.newRack());
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
    for (const auto [name, bpm] : {std::pair{"A", 100.0}, {"B", 140.0}, {"C", 80.0}}) {
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
    CHECK(controller.openSetlist(file, 0));
    tick();
    tick();   // B prepared
    CHECK(view.setlist.switching == SwitchSettings{});

    // The defaults: keep running, at the next bar. A's tempo plays on until
    // the bar line (a bar of 4/4 at 100 BPM: 115200 frames, 1800 of
    // runBlock's 64), then B's tempo, with its At Start from that bar.
    controller.startTransport();
    for (int i = 0; i < 4; ++i) runBlock(*rig.engine);
    CHECK(controller.nextSong() && controller.rackPath() == racks[1]);
    runBlock(*rig.engine);
    CHECK(rig.engine->transport().running && rig.engine->transport().tempoBpm == 100.0);   // waiting for the bar
    int blocks = 0;
    for (; blocks < 2000 && rig.engine->transport().tempoBpm != 140.0; ++blocks) runBlock(*rig.engine);
    CHECK(blocks > 1700 && blocks < 1800 && rig.engine->transport().running);   // at the line, 5 blocks in already
    CHECK(rig.session->sequenceState("intro") == SequenceState::Starting ||
          rig.session->sequenceState("intro") == SequenceState::Playing);

    // Stop, and Cut: saved at once; the next switch stops the transport and mutes the old song.
    SwitchSettings settings;
    settings.transport = SwitchSettings::Transport::Stop;
    settings.when      = SwitchSettings::When::Now;   // as soon as it's ready
    settings.oldRack   = SwitchSettings::OldRack::Cut;
    controller.setSwitchSettings(settings);
    Setlist saved;
    CHECK(loadSetlist(file, saved).ok && saved.switching == settings);
    tick();
    CHECK(view.setlist.switching == settings);
    tick();   // C prepared
    const std::shared_ptr<SlotRuntime> old = rig.session->slot("keys")->runtime;
    CHECK(controller.nextSong() && controller.rackPath() == racks[2]);
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
// rack; closing the set saves them too.
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
    CHECK(controller.openSetlist(file, 0));
    tick();

    // Edit A, switch: nothing is written; A's edit is kept, and shown.
    controller.setGainDb("keys", -6.0f);
    CHECK(controller.nextSong() && controller.rackPath() == b);
    tick();
    CHECK(gainOnDisk(a) == 0.0f && controller.unsavedRackCount() == 1 && view.otherUnsaved == 1 && !view.modified);
    CHECK(view.setlist.songs[0].unsaved && view.setlist.songs[2].unsaved && !view.setlist.songs[1].unsaved);

    // The reprise plays the edit (its prepared copy came from memory), and it's unsaved there.
    for (int i = 0; i < 3 && !controller.songPrepared(2); ++i) tick();
    CHECK(controller.nextSong() && controller.rackPath() == a);
    CHECK(rig.session->slot("keys")->saved.gainDb == -6.0f && rig.session->modified() &&
          controller.unsavedRackCount() == 0);

    // Edit A again and B, switching between: then one Save writes both.
    controller.setGainDb("keys", -9.0f);
    for (int i = 0; i < 4 && !controller.songPrepared(1); ++i) tick();
    CHECK(controller.previousSong() && controller.rackPath() == b);
    controller.setGainDb("keys", -3.0f);
    tick();
    CHECK(view.otherUnsaved == 1 && view.modified);   // "2 songs unsaved"
    CHECK(controller.save());
    CHECK(gainOnDisk(a) == -9.0f && gainOnDisk(b) == -3.0f && controller.unsavedRackCount() == 0 &&
          !rig.session->modified());

    // Edited again and left: closing the set saves what it kept.
    controller.setGainDb("keys", -1.0f);
    for (int i = 0; i < 4 && !controller.songPrepared(2); ++i) tick();
    CHECK(controller.nextSong() && controller.rackPath() == a && controller.unsavedRackCount() == 1);
    CHECK(gainOnDisk(b) == -3.0f);   // not written by the switch
    controller.closeSetlist();
    CHECK(controller.unsavedRackCount() == 0 && gainOnDisk(b) == -1.0f);

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
    const std::string rackPath = (dir / "Song.rack").string();
    RackController::Config config = rig.config(rackPath);
    config.defaultRigPath         = (dir / "Default.rig").string();
    RackController       controller(*rig.session, *rig.engine, rig.midi, rig.factory, config);
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
    CHECK(loadRig(config.defaultRigPath, savedRig).ok && savedRig.looperSwitch &&
          savedRig.looperSwitch->number == 82);

    // Another rack, then this one again: its looper is back, empty.
    CHECK(controller.newRack());
    tick();
    CHECK(view.loopers.empty());
    CHECK(controller.openRack(rackPath));
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
    std::ofstream((dir / "Racks/D.rack/rack.json").string()) << "{ not json";
    Setlist set;
    set.songs = {SetlistSong{a, ""}, SetlistSong{b, ""}, SetlistSong{a, ""}, SetlistSong{c, ""}, SetlistSong{d, ""}};
    const std::string file = (dir / "Gig.setlist").string();
    CHECK(saveSetlist(file, set).ok);

    Rig                 rig;
    Session::LoadReport report;
    rig.session->load(Rack{}, 48000.0, report);
    RackController       controller(*rig.session, *rig.engine, rig.midi, rig.factory, rig.config());
    const RackViewModel& view = controller.view();
    double               now  = 0.0;
    auto tick = [&] { controller.tick(now += 2.0); };
    CHECK(controller.openSetlist(file, 0) && controller.rackPath() == a);
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
    CHECK(controller.rackPath() == a && controller.songPrepared(1));   // nothing else changed

    // Switching to B: what the switch reports replaces what the check found.
    CHECK(controller.nextSong() && controller.rackPath() == b);
    tick();
    CHECK(view.setlist.songs[1].problems.size() == 2 && has(view.setlist.songs[1].problems, "hear nothing"));
    // A new check starts afresh; closing the set forgets it.
    CHECK(controller.checkSetlist());
    controller.closeSetlist();
    CHECK(!controller.checkingSetlist() && !controller.checkSetlist());
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
    RackController::Config config = rig.config((dir / "Song.rack").string());
    config.defaultRigPath         = (dir / "Default.rig").string();
    RackController       controller(*rig.session, *rig.engine, rig.midi, rig.factory, config);
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
    CHECK(loadRig(config.defaultRigPath, savedRig).ok && savedRig.panic && savedRig.panic->number == 83 &&
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
    const std::string rackPath    = (dir / "Song.rack").string();
    RackController::Config config = rig.config(rackPath);
    config.defaultRigPath         = (dir / "Default.rig").string();
    RackController       controller(*rig.session, *rig.engine, rig.midi, rig.factory, config);
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
    CHECK(loadRig(config.defaultRigPath, savedRig).ok && savedRig.controls.size() == 1 &&
          savedRig.controls[0].targets[0].plugin == "synth" && !savedRig.controls[0].pickUp);

    controller.removeControlLink(ControlScope::AllSongs, 0);
    controller.removeControlLink(ControlScope::Song, 0);
    tick();
    CHECK(view.controlLinks.empty());

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
    testControlLinks();
    return test::finish("RackController");
}
