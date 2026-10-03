#include "App/RackController.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include "Core/Ump.h"
#include "Session/RackFile.h"
#include "Session/RigFile.h"
#include "Session/SetlistFile.h"

namespace stagehand {

namespace {

// learningId while learning the transport's or the setlist's trigger: no sequence id can look like these.
const std::string kTransportLearn = "\x01transport";
const std::string kSetlistLearn   = "\x01setlist";
const std::string kLooperLearn    = "\x01looper";
const std::string kPanicLearn     = "\x01panic";
const std::string kLinkLearn      = "\x01link";

SourceProtocol toSourceProtocol(MidiProtocol p) {
    return p == MidiProtocol::Midi2 ? SourceProtocol::Midi2 : SourceProtocol::Midi1;
}

// "Scarlett 2i2 USB 2" -> "Scarlett 2i2 USB": PipeWire's number for a second alike device.
std::string withoutNumber(const std::string& name) {
    std::size_t end = name.size();
    while (end > 0 && std::isdigit(static_cast<unsigned char>(name[end - 1]))) --end;
    return end < name.size() && end > 0 && name[end - 1] == ' ' ? name.substr(0, end - 1) : name;
}

// The card without its profile: "Built-in Audio Analog Stereo" -> "Built-in Audio".
std::string cardOf(const std::string& name) {
    const std::string plain = withoutNumber(name);
    std::size_t       cut   = plain.size();
    for (const char* profile : {" Analog", " Digital", " Pro", " Multichannel", " Mono", " Stereo", " Surround"}) {
        for (std::size_t at = plain.find(profile); at != std::string::npos; at = plain.find(profile, at + 1)) {
            const std::size_t after = at + std::strlen(profile);
            if (after == plain.size() || plain[after] == ' ' || plain[after] == '(') {
                cut = std::min(cut, at);
                break;
            }
        }
    }
    return plain.substr(0, cut);
}

bool digitalOut(const std::string& name) {
    return name.find("Digital") != std::string::npos || name.find("HDMI") != std::string::npos ||
           name.find("IEC958") != std::string::npos;
}

} // namespace

int findPlaybackDevice(const std::string& saved, const std::vector<PlaybackDevice>& devices) {
    if (saved.empty()) return -1;
    for (std::size_t i = 0; i < devices.size(); ++i)
        if (devices[i].name == saved) return static_cast<int>(i);
    for (std::size_t i = 0; i < devices.size(); ++i)
        if (withoutNumber(devices[i].name) == withoutNumber(saved)) return static_cast<int>(i);
    int found = -1;
    for (std::size_t i = 0; i < devices.size(); ++i) {
        if (cardOf(devices[i].name) != cardOf(saved) || digitalOut(devices[i].name) != digitalOut(saved)) continue;
        if (found >= 0) return -1;   // two candidates: don't guess
        found = static_cast<int>(i);
    }
    return found;
}

RackController::RackController(Session& runningSession, Engine& runningEngine, MidiInput& midiInput,
                               PluginFactory& pluginFactory, Config appConfig)
    : session(runningSession), engine(runningEngine), midi(midiInput), factory(pluginFactory),
      config(std::move(appConfig)) {
    target      = config.saveTarget;
    midiChanges = midi.changes();   // what's open now was opened by the app
    useRig({});   // the default rig, until a setlist names another
    routeClick(true);
    refreshSources();
    tick(0.0);
}

RackController::~RackController() {
    editors.clear();
    busEditors.clear();
}

// --- periodic ----------------------------------------------------------------------

void RackController::service() {
    engine.collect();
    for (const Session::Slot& slot : session.slots())
        if (slot.plugin) slot.plugin->idle();
    for (const SlotConfig& slot : session.drainingSlots()) slot.plugin->idle();
    if (session.hasRig())   // the rig's effects too
        for (const auto& bus : session.rig().effects)
            for (const Session::LoadedRig::Effect& effect : bus)
                if (effect.plugin) effect.plugin->idle();
    for (const auto& [bundle, ahead] : prepared) ahead.rack->idle();
}

BeatNow RackController::heardBeat(Nanoseconds now) const {
    return beatNow(engine.transport(), now, config.outputLatencySeconds, session.metronome().everyNote);
}

void RackController::tick(double now) {
    ticking = true;
    if (Engine::Learned learned; !learningId.empty() && engine.takeLearned(learned)) finishLearn(learned);
    if (const std::uint64_t changes = midi.changes(); changes != midiChanges) {
        midiChanges = changes;
        midiChanged();
    }
    refreshSongs(now);
    takeSetlistRequests();
    prepareAhead();   // before flush(): a switch commits the new rack's graph, and nothing else is pending
    if (preparingText.empty()) checkStep();   // one plugin a tick: preparing ahead first
    session.flush();

    for (auto it = editors.begin(); it != editors.end();)
        it = it->second->isOpen() ? std::next(it) : editors.erase(it);
    for (auto it = busEditors.begin(); it != busEditors.end();)
        it = it->second->isOpen() ? std::next(it) : busEditors.erase(it);

    const double dt = lastTick < 0.0 ? 0.0 : std::max(0.0, now - lastTick);
    lastTick        = now;

    model.rackName     = session.rackName();
    model.modified     = session.modified();
    model.otherUnsaved = unsavedRacks.size();
    buildSetlist();

    TransportModel&         transport = model.transport;
    const Engine::Transport clock     = engine.transport();
    transport.tempoBpm     = session.tempoBpm();
    transport.numerator    = session.timeSigNumerator();
    transport.denominator  = session.timeSigDenominator();
    transport.beatPosition = clock.beatPosition;
    // The lights (and the click) count metronomePulse()s: the counted beats,
    // or every note if the rack says so. A light stays lit for 0.15 of a
    // quarter note, or of the pulse if that's longer, so a quick eighth still
    // shows.
    const MetronomePulse pulse =
        metronomePulse(transport.numerator, transport.denominator, session.metronome().everyNote);
    const double beatLen  = pulse.beats;   // in quarter notes
    const double intoBar  = std::max(0.0, clock.beatPosition - clock.barStartBeats);
    const int    beat     = static_cast<int>(std::floor(intoBar / beatLen + 1e-9));
    transport.beatsInBar  = pulse.perBar;
    transport.beatInBar   = std::clamp(beat, 0, std::max(0, transport.beatsInBar - 1));
    transport.running     = clock.running;
    transport.onBeat      = clock.running && intoBar - beat * beatLen < 0.15 * std::max(beatLen, 1.0);
    // Counting in (docs/perform.md): the countdown, and the lights (amber)
    // counting the pulses where they'd fall in the bars before bar 1.
    transport.countIn = clock.countIn;
    if (clock.countIn > 0 && !clock.running) {
        const int k         = static_cast<int>(std::floor(clock.countInBeat / beatLen + 1e-9));   // the pulse sounding
        const int perBar    = std::max(pulse.perBar, 1);
        transport.beatInBar = (perBar - clock.countIn % perBar) % perBar;   // its place in its bar
        transport.onBeat    = clock.countInBeat - k * beatLen < 0.15 * std::max(beatLen, 1.0);
    }
    const TransportSettings& settings = session.transportSettings();
    transport.triggers.clear();
    for (std::size_t i = 0; i < settings.triggers.size(); ++i) {
        std::string text = triggerText(settings.triggers[i]);
        const auto& handles = session.transportTriggerHandles();
        if (i < handles.size() && handles[i] == kInvalidMidiSource &&
            settings.triggers[i].source.kind != MidiSourceRef::Kind::All)
            text += " (not connected)";
        transport.triggers.push_back(std::move(text));
    }
    transport.learning             = learningId == kTransportLearn;
    transport.learningAction       = learningAction;
    transport.startOnNote          = settings.startOnNote ? sourceLabel(settings.startOnNoteSource) : std::string{};
    transport.startOnNoteConnected = session.startOnNoteIssue().empty();
    if (config.clickRouting != nullptr && config.clickRouting->takeDevicesChanged()) routeClick(true);   // hot-plug
    const MetronomeSettings& metronome = session.metronome();
    transport.everyNote       = metronome.everyNote;
    transport.click           = metronome.click;
    transport.clickLevelDb    = metronome.clickLevelDb;
    transport.countInBars     = metronome.countInBars;
    transport.flash           = metronome.flash;
    transport.clickOutput     = session.clickOutput();
    transport.clickConnected  = !clickDevice.empty();
    transport.playbackDevices = deviceNames;
    if (const Engine::Panics panics = engine.panics(); panics.count != panicsSeen) {   // the button or the control
        panicsSeen = panics.count;
        panicAt    = now;
        notice(panics.cut ? "Panic: sound cut" : "Panic: all notes off");
    }
    transport.panicLit = panicAt >= 0.0 && now - panicAt < kPanicLitSeconds;
    if (const auto& control = session.rig().saved.panic) {
        const std::string text = triggerText(*control);
        transport.panicControl = text.substr(text.find(": ") + 2);   // without the action
    } else {
        transport.panicControl.clear();
    }
    transport.panicConnected = session.panicControlProblem().empty();
    transport.panicLearning  = learningId == kPanicLearn;
    model.controlLinkLearning = learningId == kLinkLearn;
    model.rows.clear();
    for (const Session::Slot& slot : session.slots()) model.rows.push_back(rowFor(slot, now, dt));
    // Forget rows that are gone.
    for (auto it = rows.begin(); it != rows.end();)
        it = session.slot(it->first) != nullptr ? std::next(it) : rows.erase(it);

    takeKeptLoops();
    buildDock(now);
    buildRig(now, dt);
    buildControlLinks();

    StatusModel& status = model.status;
    if (config.audioStatus) {
        const AudioStatus audio = config.audioStatus();
        status.sampleRate       = audio.sampleRate;
        status.bufferSize       = audio.bufferSize;
        status.outputs          = audio.outputs;
        if (audio.xruns != xrunsSeen) {   // a new one (or more): when, and where, in the log
            if (audio.xruns > xrunsSeen) logXrun();
            xrunsSeen = audio.xruns;
            xrunAt    = now;
        }
        xrunsBase    = std::min(xrunsBase, audio.xruns);   // a backend that started again counts from 0
        status.xruns = audio.xruns - xrunsBase;
    }
    status.xrunSecondsAgo = status.xruns != 0 && xrunAt >= 0.0 ? static_cast<int>(now - xrunAt) : -1;
    status.xrunRecent     = status.xrunSecondsAgo >= 0 && now - xrunAt < kXrunRecentSeconds;
    const Engine::DspLoad load = engine.takeDspLoad();
    dspLoad.update(load.peak, now);
    status.dspPercent     = static_cast<int>(std::lround(load.average * 100.0f));
    status.dspPeakPercent = dspLoad.percent();
    const Engine::Stats stats = engine.stats();
    status.midiDropped  = stats.midiQueueFull + stats.slotEventsFull + stats.unsupportedDialect;
    status.preference   = config.protocolPreference;
    status.saveTarget   = config.savingOff ? std::string{} : target;
    status.noSaveReason = config.savingOff ? "Not saving: saving is off (--no-save-rack)"
                          : target.empty() ? "Untitled: File > Save As keeps it"
                                           : "";
    const LevelMeter::Reading output = engine.outputMeter().take();
    master.update(output, now, dt);
    for (std::size_t c = 0; c < LevelMeter::kChannels; ++c) status.outputDb[c] = master.displayDb[c];
    status.outputClipped = output.clipped;
    status.preparing     = preparingText;
    ticking              = false;
}

void RackController::Ballistics::update(const LevelMeter::Reading& reading, double now, double dt) noexcept {
    for (std::size_t c = 0; c < LevelMeter::kChannels; ++c) {
        const float peak = reading.peak[c];
        const float db   = peak > 0.0f ? std::max(kFloorDb, 20.0f * std::log10(peak)) : kFloorDb;
        displayDb[c]     = std::max(db, std::max(kFloorDb, displayDb[c] - kFallDbPerSec * static_cast<float>(dt)));
        if (db >= holdDb[c]) {
            holdDb[c]    = db;
            holdUntil[c] = now + kPeakHoldSec;
        } else if (now >= holdUntil[c]) {
            holdDb[c] = displayDb[c];   // released: falls with the meter
        }
    }
}

RowModel RackController::rowFor(const Session::Slot& slot, double now, double dt) {
    const RackSlot& saved = slot.saved;
    RowCache&       cache = rows[saved.id];
    if (cache.plugin != slot.plugin.get()) {   // new, or re-created: query it once
        cache        = RowCache{};
        cache.plugin = slot.plugin.get();
        if (slot.plugin) {
            const PortLayout layout = slot.plugin->portLayout();
            if (saved.notePort < layout.noteInputs.size())
                cache.dialects = layout.noteInputs[saved.notePort].supportedDialects;
            cache.hasEditor = slot.plugin->hasEditor();
        }
    }

    RowModel row;
    row.id   = saved.id;
    row.name = saved.name;
    if (slot.plugin) {
        row.pluginName = slot.plugin->descriptor().name;
        row.vendor     = slot.plugin->descriptor().vendor;
        row.state      = RowState::Running;
    } else {
        row.pluginName = saved.plugin.id;
        row.state      = RowState::Placeholder;
        row.problem    = slot.loadProblem;
    }
    row.hasEditor  = cache.hasEditor;
    const auto ed  = editors.find(saved.id);
    row.editorOpen = ed != editors.end() && ed->second->isOpen();

    row.source          = saved.source;
    row.sourceConnected = slot.sourceProblem.empty();
    row.sourceLabel     = sourceLabel(saved.source) + (row.sourceConnected ? "" : " (not connected)");
    row.sourceProtocol  = sourceProtocolOf(saved.source);
    row.pluginDialects  = cache.dialects;
    row.protocolWarning = row.sourceProtocol == SourceProtocol::Midi2 && cache.dialects != 0 &&
                          (cache.dialects & NoteDialect::Midi2) == 0;
    row.filter          = saved.filter;

    row.gainDb      = saved.gainDb;
    row.muted       = saved.muted;
    row.firstOutput = saved.firstOutput;
    row.sendDb      = saved.sendDb;

    if (slot.runtime) {
        const LevelMeter::Reading reading = slot.runtime->meter.take();
        cache.meter.update(reading, now, dt);
        row.clipped = reading.clipped;
        cache.load.update(slot.runtime->load.take(), now);
        row.loadPercent = cache.load.percent();

        const std::uint32_t events = slot.runtime->midiEvents.load(std::memory_order_relaxed);
        if (events != cache.lastMidiEvents) {
            cache.lastMidiEvents  = events;
            cache.midiActiveUntil = now + kMidiLightSec;
        }
        row.midiActive = now < cache.midiActiveUntil;
    }
    for (std::size_t c = 0; c < LevelMeter::kChannels; ++c) {
        row.meterDb[c]    = cache.meter.displayDb[c];
        row.peakHoldDb[c] = cache.meter.holdDb[c];
    }
    return row;
}

void RackController::buildDock(double now) {
    model.inputs.clear();
    // Devices first, then the virtual inputs (as in the source menus).
    std::vector<const Session::OpenInput*> ordered;
    for (const Session::OpenInput& input : session.openInputs())
        if (input.ref.kind != MidiSourceRef::Kind::Virtual) ordered.push_back(&input);
    for (const Session::OpenInput& input : session.openInputs())
        if (input.ref.kind == MidiSourceRef::Kind::Virtual) ordered.push_back(&input);
    for (const Session::OpenInput* openInput : ordered) {
        const Session::OpenInput& input = *openInput;
        InputChipModel chip;
        chip.label     = sourceLabel(input.ref);
        chip.isVirtual = input.ref.kind == MidiSourceRef::Kind::Virtual;
        chip.protocol  = sourceProtocolOf(input.ref);
        chip.connected = session.inputConnected(input.ref);
        auto& [lastCount, litUntil] = inputActivity[input.handle];
        if (const std::uint32_t count = engine.sourceActivity(input.handle); count != lastCount) {
            lastCount = count;
            litUntil  = now + kMidiLightSec;
        }
        chip.active = now < litUntil;
        model.inputs.push_back(std::move(chip));
    }

    model.sequences.clear();
    for (const Session::SequenceEntry& entry : session.sequences()) {
        const Sequence&  seq = entry.sequence;
        SequenceRowModel row;
        row.id              = seq.id;
        row.name            = seq.name;
        row.protocol        = seq.protocol;
        row.edo             = seq.edo;
        row.rate            = seq.rate;
        row.length          = seq.length;
        row.launch          = seq.launch;
        row.repeat          = seq.repeat;
        row.atStart         = seq.atStart;
        row.noteOffOffsetMs = seq.noteOffOffsetMs;
        row.noteCount       = seq.notes.size();
        for (const SequenceNote& note : seq.notes) {
            row.noteStarts.push_back(note.start);
            row.lastNoteEnd = std::max(row.lastNoteEnd, note.start + note.length);
        }
        std::sort(row.noteStarts.begin(), row.noteStarts.end());
        row.noteStarts.erase(std::unique(row.noteStarts.begin(), row.noteStarts.end()), row.noteStarts.end());
        row.state    = session.sequenceState(seq.id);
        row.position = session.sequencePosition(seq.id);
        for (const Session::Slot& slot : session.slots())   // in rack order
            if (std::find(seq.targets.begin(), seq.targets.end(), slot.saved.id) != seq.targets.end()) {
                row.usedBy.push_back(slot.saved.name);
                row.targetIds.push_back(slot.saved.id);
            }
        for (std::size_t i = 0; i < seq.triggers.size(); ++i) {
            std::string text = triggerText(seq.triggers[i]);
            if (i < entry.triggerSources.size() && entry.triggerSources[i] == kInvalidMidiSource &&
                seq.triggers[i].source.kind != MidiSourceRef::Kind::All)
                text += " (not connected)";
            row.triggers.push_back(std::move(text));
        }
        row.learning       = learningId == seq.id;
        row.learningAction = learningAction;
        model.sequences.push_back(std::move(row));
    }
    buildLoopers();
}

void RackController::buildLoopers() {
    // Always a current looper while there are any (a new rack's: its first).
    const auto& loopers = session.loopers();
    if (!loopers.empty() && session.looper(session.currentLooper()) == nullptr)
        session.setCurrentLooper(loopers.front().setup.id);
    model.loopers.clear();
    for (const Session::LooperEntry& entry : loopers) {
        const LooperSetup& setup = entry.setup;
        LooperRuntime&     rt    = *entry.runtime;
        LooperRowModel     row;
        row.id             = setup.id;
        row.name           = setup.name;
        row.inputLabel     = sourceLabel(setup.source) + (entry.inputProblem.empty() ? "" : " (not connected)");
        row.inputConnected = entry.inputProblem.empty();
        for (const Session::Slot& slot : session.slots())   // in rack order
            if (std::find(setup.targets.begin(), setup.targets.end(), slot.saved.id) != setup.targets.end()) {
                row.targetIds.push_back(slot.saved.id);
                row.targetNames.push_back(slot.saved.name);
            }
        row.state       = rt.state.load(std::memory_order_relaxed);
        row.waiting     = rt.waiting.load(std::memory_order_relaxed);
        row.current     = setup.id == session.currentLooper();
        row.lengthBeats = rt.length.load(std::memory_order_relaxed);
        row.position    = rt.position.load(std::memory_order_relaxed);
        row.notes       = rt.noteCount.load(std::memory_order_relaxed);
        row.capacity    = static_cast<std::uint32_t>(rt.capacity);
        row.dropped     = rt.dropped.load(std::memory_order_relaxed);
        row.passes      = rt.passes.load(std::memory_order_relaxed);
        row.quantize    = setup.quantize;
        row.atBar       = setup.atBar;
        row.fixedLength = setup.fixedLength;
        row.fixedInBars = setup.fixedInBars;
        row.cleanup     = setup.cleanup;
        model.loopers.push_back(std::move(row));
    }
    const auto& looperSwitch = session.rig().saved.looperSwitch;
    if (looperSwitch) {
        const std::string text = triggerText(*looperSwitch);
        model.looperSwitch     = text.substr(text.find(": ") + 2);   // without the action
    } else {
        model.looperSwitch.clear();
    }
    model.looperSwitchConnected = session.looperSwitchProblem().empty();
    model.looperSwitchLearning  = learningId == kLooperLearn;
}

// --- loopers (docs/looper.md) ----------------------------------------------------------

template <typename Change>
void RackController::editLooper(const std::string& id, Change change) {
    const Session::LooperEntry* entry = session.looper(id);
    if (entry == nullptr) return;
    LooperSetup setup = entry->setup;
    change(setup);
    session.setLooper(setup);
}

std::string RackController::addLooper(const MidiSourceRef& input, const std::vector<std::string>& targets) {
    LooperSetup setup;
    setup.source  = input;
    setup.targets = targets;
    const std::string id = session.addLooper(setup);
    session.setCurrentLooper(id);
    return id;
}

const LooperSetup* RackController::looper(const std::string& id) const {
    const Session::LooperEntry* entry = session.looper(id);
    return entry != nullptr ? &entry->setup : nullptr;
}

void RackController::removeLooper(const std::string& id) {
    session.removeLooper(id);
}

void RackController::selectLooper(const std::string& id) {
    if (session.looper(id) != nullptr) session.setCurrentLooper(id);
}

void RackController::renameLooper(const std::string& id, const std::string& name) {
    const std::size_t first = name.find_first_not_of(" \t");
    if (first == std::string::npos) {
        notice("A looper needs a name");
        return;
    }
    std::string text = name.substr(first, name.find_last_not_of(" \t") - first + 1);
    if (text.size() > kMaxNameBytes) text.resize(kMaxNameBytes);
    editLooper(id, [&text](LooperSetup& s) { s.name = text; });
}

void RackController::setLooperInput(const std::string& id, const MidiSourceRef& input) {
    editLooper(id, [&input](LooperSetup& s) { s.source = input; });
    if (const Session::LooperEntry* entry = session.looper(id); entry != nullptr && !entry->inputProblem.empty())
        notice(entry->setup.name + ": " + entry->inputProblem + "; it records nothing from it");
}

void RackController::setLooperTarget(const std::string& id, const std::string& row, bool on) {
    editLooper(id, [&row, on](LooperSetup& s) {
        std::erase(s.targets, row);
        if (on) s.targets.push_back(row);
    });
}

void RackController::setLooperQuantize(const std::string& id, double beats) {
    editLooper(id, [beats](LooperSetup& s) { s.quantize = std::max(0.0, beats); });
}

void RackController::setLooperStart(const std::string& id, bool atBar) {
    editLooper(id, [atBar](LooperSetup& s) { s.atBar = atBar; });
}

void RackController::setLooperLength(const std::string& id, std::uint32_t count, bool inBars) {
    editLooper(id, [count, inBars](LooperSetup& s) {
        s.fixedLength = std::min<std::uint32_t>(count, 1024);
        s.fixedInBars = inBars;
    });
}

void RackController::setLooperCleanup(const std::string& id, bool on) {
    editLooper(id, [on](LooperSetup& s) { s.cleanup = on; });
}

void RackController::tapLooper(const std::string& id) {
    session.looperCommand(id, LooperCommand::Press);
    session.looperCommand(id, LooperCommand::Release);
}

void RackController::clearLooper(const std::string& id) {
    session.looperCommand(id, LooperCommand::Clear);
}

void RackController::undoLooper(const std::string& id) {
    session.looperCommand(id, LooperCommand::Undo);
}

void RackController::keepLooperAsSequence(const std::string& id) {
    if (session.looper(id) == nullptr) return;
    if (std::find(keeping.begin(), keeping.end(), id) == keeping.end()) keeping.push_back(id);
    session.looperCommand(id, LooperCommand::Snapshot);
}

void RackController::takeKeptLoops() {
    for (auto it = keeping.begin(); it != keeping.end();) {
        const Session::LooperEntry* entry = session.looper(*it);
        LooperRuntime::Snapshot     loop;
        if (entry != nullptr && !entry->runtime->takeSnapshot(loop)) {
            ++it;   // not made yet
            continue;
        }
        if (entry != nullptr) {
            const LooperSetup& setup = entry->setup;
            if (loop.length <= 0.0) {
                notice(setup.name + ": nothing to keep yet (record a loop first)");
            } else {
                Sequence seq = loopToSequence(loop, setup.quantize, setup.atBar, setup.cleanup);
                seq.id       = session.addSequence(seq.protocol);
                seq.name     = setup.name;
                seq.targets  = setup.targets;
                std::string problem;
                if (session.setSequence(seq, problem))
                    notice("Kept " + setup.name + " as a sequence (" + std::to_string(seq.notes.size()) +
                           (seq.notes.size() == 1 ? " note" : " notes") + "); Ctrl-S saves it");
                else
                    notice("Couldn't keep " + setup.name + ": " + problem);
            }
        }
        it = keeping.erase(it);
    }
}

void RackController::learnLooperSwitch() {
    learningId = kLooperLearn;
    engine.armLearn();
}

void RackController::removeLooperSwitch() {
    session.setLooperSwitch(std::nullopt);
}

void RackController::panic() {
    engine.panic();
}

void RackController::learnPanic() {
    learningId = kPanicLearn;
    engine.armLearn();
}

void RackController::removePanic() {
    session.setPanicControl(std::nullopt);
}

std::vector<std::string> RackController::takeNotices() {
    return std::exchange(notices, {});
}

// --- control links (docs/controls.md) --------------------------------------------------------

namespace {

std::vector<RackController::ParameterChoice> choicesOf(const HostedPlugin* plugin) {
    std::vector<RackController::ParameterChoice> out;
    if (plugin == nullptr) return out;
    for (const ParameterInfo& p : plugin->parameters())
        if (p.isAutomatable) out.push_back({p.id, p.name, p.minValue, p.maxValue, p.isStepped});
    return out;
}

const char* busName(int bus) {
    return bus == 0 ? "A" : "B";
}

} // namespace

std::vector<RackController::ParameterChoice> RackController::rowParameters(const std::string& rowId) const {
    const Session::Slot* s = session.slot(rowId);
    return parametersOf(s != nullptr ? s->plugin.get() : nullptr);
}

std::vector<RackController::ParameterChoice> RackController::effectParameters(std::size_t bus, std::size_t effect) const {
    if (!session.hasRig() || bus >= kRigBuses || effect >= RigBus::kEffects) return {};
    return parametersOf(session.rig().effects[bus][effect].plugin.get());
}

const std::vector<RackController::ParameterChoice>& RackController::parametersOf(const HostedPlugin* plugin) const {
    static const std::vector<ParameterChoice> none;
    if (plugin == nullptr) return none;
    if (engine.graphCommits() != parameterCacheAt) {   // plugins came or went: a pointer may be another's now
        parameterCache.clear();
        parameterCacheAt = engine.graphCommits();
    }
    auto known = parameterCache.find(plugin);
    if (known == parameterCache.end()) known = parameterCache.emplace(plugin, choicesOf(plugin)).first;
    return known->second;
}

const HostedPlugin* RackController::targetPlugin(const ControlLinkTarget& t) const {
    if (t.kind != ControlLinkTarget::Kind::Parameter) return nullptr;
    if (t.bus >= 0)
        return session.hasRig() && t.bus < static_cast<int>(kRigBuses) && t.effect >= 0 &&
                       t.effect < static_cast<int>(RigBus::kEffects)
                   ? session.rig().effects[t.bus][t.effect].plugin.get()
                   : nullptr;
    if (!t.row.empty()) {
        const Session::Slot* s = session.slot(t.row);
        return s != nullptr ? s->plugin.get() : nullptr;
    }
    for (const Session::Slot& s : session.slots())   // all songs': the first row with it
        if (s.plugin && s.saved.plugin.id == t.plugin) return s.plugin.get();
    return nullptr;
}

bool RackController::startTarget(ControlLinkTarget& target, ControlScope scope) {
    using Kind = ControlLinkTarget::Kind;
    std::vector<ControlLinkTarget> kept;
    if (ControlLinkTarget check = target; !targetFor(scope, check, kept)) {
        notice(target.kind == Kind::Gain || target.kind == Kind::Send
                   ? "Control Link: a row's gain or send can't be for all songs"
                   : "Control Link: that plugin isn't in this song");
        return false;
    }
    if (target.kind == Kind::Parameter) {   // its whole range, and its name as it is now
        const std::vector<ParameterChoice>& choices = parametersOf(targetPlugin(target));
        const auto p = std::find_if(choices.begin(), choices.end(), [&](const ParameterChoice& c) { return c.id == target.param; });
        if (p == choices.end()) {
            notice("Control Link: that parameter isn't there to link");
            return false;
        }
        target.paramName = p->name;
        target.bottom    = p->minValue;
        target.top       = p->maxValue;
    } else {
        target.bottom = -60.0;
        target.top    = 0.0;
    }
    return true;
}

void RackController::learnControlLink(ControlLinkTarget target, ControlScope scope) {
    if (!startTarget(target, scope)) return;
    linkTarget  = std::move(target);
    linkScope   = scope;
    linkRelearn.reset();
    learningId  = kLinkLearn;
    engine.armLearn();
}

void RackController::relearnControlLink(ControlScope scope, std::size_t index) {
    if (index >= session.controlLinks(scope).size()) return;
    linkScope   = scope;
    linkRelearn = index;
    learningId  = kLinkLearn;
    engine.armLearn();
}

void RackController::addControlLinkTarget(ControlScope scope, std::size_t index, ControlLinkTarget target) {
    const std::vector<ControlLinkSetup>& links = session.controlLinks(scope);
    if (index >= links.size() || !startTarget(target, scope)) return;
    ControlLinkSetup link = links[index];
    link.targets          = {std::move(target)};
    addLinkTarget(std::move(link), scope);   // into link `index`: it's the one on that control
}

void RackController::setControlLink(ControlScope scope, std::size_t index, ControlLinkSetup link) {
    std::vector<ControlLinkSetup> links = session.controlLinks(scope);
    if (index >= links.size()) return;
    if (link.targets.empty()) links.erase(links.begin() + static_cast<std::ptrdiff_t>(index));
    else links[index] = std::move(link);
    session.setControlLinks(scope, std::move(links));
}

void RackController::removeControlLink(ControlScope scope, std::size_t index) {
    std::vector<ControlLinkSetup> links = session.controlLinks(scope);
    if (index >= links.size()) return;
    links.erase(links.begin() + static_cast<std::ptrdiff_t>(index));
    session.setControlLinks(scope, std::move(links));
}

std::optional<std::size_t> RackController::setControlLinkScope(ControlScope scope, std::size_t index, ControlScope to) {
    const std::vector<ControlLinkSetup>& links = session.controlLinks(scope);
    if (index >= links.size()) return std::nullopt;
    if (scope == to) return index;
    ControlLinkSetup link = links[index];
    for (ControlLinkTarget t : link.targets) {
        std::vector<ControlLinkTarget> kept;
        if (targetFor(to, t, kept)) continue;
        bool present = true;
        notice("Control Link: " + targetText(t, present) +
               (to == ControlScope::AllSongs ? " can't be for all songs" : " isn't in this song"));
        return std::nullopt;
    }
    removeControlLink(scope, index);
    addLinkTarget(link, to);
    const std::vector<ControlLinkSetup>& moved = session.controlLinks(to);
    const auto at = std::find_if(moved.begin(), moved.end(), [&](const ControlLinkSetup& l) { return l.sameControl(link); });
    if (at == moved.end()) return std::nullopt;
    return static_cast<std::size_t>(at - moved.begin());
}

bool RackController::targetFor(ControlScope scope, ControlLinkTarget& target, std::vector<ControlLinkTarget>& out) const {
    using Kind = ControlLinkTarget::Kind;
    const bool rowParameter = target.kind == Kind::Parameter && target.bus < 0;
    if (scope == ControlScope::AllSongs) {
        if (target.kind == Kind::Gain || target.kind == Kind::Send) return false;
        if (rowParameter && !target.row.empty()) {   // a row's plugin: that plugin, in every row
            const Session::Slot* s = session.slot(target.row);
            if (s == nullptr) return false;
            target.plugin = s->saved.plugin.id;
            target.row.clear();
        }
        out.push_back(target);
        return true;
    }
    if (rowParameter && !target.plugin.empty()) {   // a plugin's: each row of this song with it
        for (const Session::Slot& s : session.slots())
            if (s.saved.plugin.id == target.plugin) {
                ControlLinkTarget own = target;
                own.plugin.clear();
                own.row = s.saved.id;
                out.push_back(std::move(own));
            }
        return !out.empty();
    }
    out.push_back(target);
    return true;
}

void RackController::addLinkTarget(ControlLinkSetup link, ControlScope scope) {
    std::vector<ControlLinkTarget> targets;
    for (ControlLinkTarget t : link.targets) targetFor(scope, t, targets);
    if (targets.empty()) return;
    std::vector<ControlLinkSetup> links = session.controlLinks(scope);
    auto same = std::find_if(links.begin(), links.end(), [&](const ControlLinkSetup& l) { return l.sameControl(link); });
    if (same == links.end()) {
        link.targets = std::move(targets);
        links.push_back(std::move(link));
    } else {   // a macro: one more target (or a new range for one it has)
        for (ControlLinkTarget& t : targets) {
            auto own = std::find_if(same->targets.begin(), same->targets.end(),
                                    [&](const ControlLinkTarget& o) { return o.sameTarget(t); });
            if (own != same->targets.end()) *own = std::move(t);
            else same->targets.push_back(std::move(t));
        }
    }
    session.setControlLinks(scope, std::move(links));
}

std::string RackController::triggerOn(const MidiSourceRef& source, std::uint8_t group, std::uint8_t channel,
                                      std::uint8_t number) const {
    auto overlaps = [&](const MidiSourceRef& other) {
        return source.kind == MidiSourceRef::Kind::All || other.kind == MidiSourceRef::Kind::All || other == source;
    };
    auto answers = [&](const SequenceTrigger& t) {
        return t.control == SequenceTrigger::Control::Controller && t.group == group && t.channel == channel &&
               t.number == number && overlaps(t.source);
    };
    for (const SequenceTrigger& t : session.transportSettings().triggers)
        if (answers(t)) return "the transport's";
    for (const Session::SequenceEntry& e : session.sequences())
        for (const SequenceTrigger& t : e.sequence.triggers)
            if (answers(t)) return "'" + e.sequence.name + "''s";
    if (setlistOpen)
        for (const SetlistTrigger& t : setlist.triggers)
            if (t.action != SetlistTrigger::Action::Song && t.control == SequenceTrigger::Control::Controller &&
                t.group == group && t.channel == channel && t.number == number && overlaps(t.source))
                return "the setlist's";
    if (const auto& t = session.rig().saved.looperSwitch; t && answers(*t)) return "the looper switch";
    if (const auto& t = session.rig().saved.panic; t && answers(*t)) return "Panic's";
    return {};
}

std::string RackController::controlText(const ControlLinkSetup& link) const {
    SequenceTrigger t;
    t.source  = link.source;
    t.control = SequenceTrigger::Control::Controller;
    t.group   = link.group;
    t.channel = link.channel;
    t.number  = link.number;
    const std::string text = triggerText(t);
    return text.substr(text.find(": ") + 2);   // without the action
}

std::string RackController::targetText(const ControlLinkTarget& t, bool& present) const {
    using Kind = ControlLinkTarget::Kind;
    const std::string param = t.paramName.empty() ? "parameter " + std::to_string(t.param) : t.paramName;
    auto rowName = [&](const std::string& id) {
        const Session::Slot* s = session.slot(id);
        present = s != nullptr && s->plugin != nullptr;
        return s != nullptr ? s->saved.name : id;
    };
    present = true;
    switch (t.kind) {
        case Kind::Parameter:
            if (t.bus >= 0) {
                std::string effect = std::string("Bus ") + busName(t.bus) + " effect " + std::to_string(t.effect + 1);
                const HostedPlugin* plugin =
                    session.hasRig() && t.bus < static_cast<int>(kRigBuses) && t.effect >= 0 &&
                            t.effect < static_cast<int>(RigBus::kEffects)
                        ? session.rig().effects[t.bus][t.effect].plugin.get()
                        : nullptr;
                present = plugin != nullptr;
                if (plugin != nullptr) effect += " (" + plugin->descriptor().name + ")";
                return effect + ": " + param;
            }
            if (!t.plugin.empty()) {   // all songs': every row with that plugin
                std::string pluginName = t.plugin;
                present                = false;
                for (const Session::Slot& s : session.slots())
                    if (s.plugin && s.saved.plugin.id == t.plugin) {
                        pluginName = s.plugin->descriptor().name;
                        present    = true;
                        break;
                    }
                return pluginName + " (every row): " + param;
            }
            return rowName(t.row) + ": " + param;
        case Kind::Gain:   return rowName(t.row) + " gain";
        case Kind::Send:   return rowName(t.row) + " send " + busName(t.bus);
        case Kind::Return: present = session.hasRig(); return std::string("Bus ") + busName(t.bus) + " return";
        case Kind::Master: return "Master";
    }
    return {};
}

void RackController::buildControlLinks() {
    using Kind = ControlLinkTarget::Kind;
    model.controlLinks.clear();
    model.controlLinkLearnTarget = learningId == kLinkLearn && !linkRelearn ? linkTarget : ControlLinkTarget{};
    const std::vector<Engine::LinkState> live = engine.linkStates();
    const std::vector<ControlLinkSetup>& own  = session.controlLinks(ControlScope::Song);
    for (const ControlScope scope : {ControlScope::Song, ControlScope::AllSongs}) {
        const std::vector<ControlLinkSetup>& links = session.controlLinks(scope);
        for (std::size_t i = 0; i < links.size(); ++i) {
            const ControlLinkSetup& link = links[i];
            ControlLinkModel        m;
            m.scope      = scope;
            m.index      = i;
            m.control    = controlText(link);
            m.connected  = session.controlLinkProblem(scope, i).empty();
            m.pickUp     = link.pickUp;
            m.passOn     = link.passOn;
            m.overridden = scope == ControlScope::AllSongs &&
                           std::any_of(own.begin(), own.end(), [&](const ControlLinkSetup& o) { return o.sameControl(link); });
            m.source     = link.source;
            m.group      = link.group;
            m.channel    = link.channel;
            m.number     = link.number;
            m.learning   = learningId == kLinkLearn && linkRelearn == i && linkScope == scope;
            for (std::size_t j = 0; j < link.targets.size(); ++j) {
                const ControlLinkTarget& t = link.targets[j];
                ControlLinkTargetModel   target;
                target.target = t;
                target.text   = targetText(t, target.present);
                if (t.kind == Kind::Gain || t.kind == Kind::Send || (t.kind == Kind::Parameter && !t.row.empty()))
                    target.rows.push_back(t.row);
                else if (t.kind == Kind::Parameter && t.bus < 0)
                    for (const Session::Slot& s : session.slots())
                        if (s.saved.plugin.id == t.plugin) target.rows.push_back(s.saved.id);
                if (t.kind == Kind::Parameter) {
                    target.minValue = std::min(t.bottom, t.top);
                    target.maxValue = std::max(t.bottom, t.top);
                    for (const ParameterChoice& p : parametersOf(targetPlugin(t)))
                        if (p.id == t.param) {
                            target.minValue = p.minValue;
                            target.maxValue = p.maxValue;
                            target.stepped  = p.stepped;
                        }
                } else {   // the fader's scale
                    target.minValue = -60.0;
                    target.maxValue = 6.0;
                }
                const std::uint64_t origin = linkOrigin(scope, i, j);
                const auto state = std::find_if(live.begin(), live.end(), [&](const Engine::LinkState& l) { return l.origin == origin; });
                if (state != live.end()) {
                    target.now     = state->value;
                    target.waiting = state->waiting;
                    m.travel       = state->travel;
                }
                m.targets.push_back(std::move(target));
            }
            model.controlLinks.push_back(std::move(m));
        }
    }
}

// --- sources -----------------------------------------------------------------------

void RackController::midiChanged() {
    if (config.openNewInputs)
        for (const MidiSourceInfo& info : midi.enumerate()) {
            const MidiSourceRef ref{MidiSourceRef::Kind::Device, info.id};
            if ((!info.isHardware && !config.openNewSoftware) || session.sourceOpen(ref)) continue;
            const MidiSourceHandle handle = midi.open(info.id);
            if (handle == kInvalidMidiSource) continue;
            session.addOpenSource(ref, handle);
            notice("MIDI device '" + info.id + "' connected");
        }
    for (const std::string& change : session.syncMidi()) notice(change);
    refreshSources();
}

void RackController::refreshSources() {
    enumerated = midi.enumerate();

    model.sources.clear();
    model.sources.push_back(SourceChoice{"All inputs", MidiSourceRef{}, SourceProtocol::Mixed, false});
    // A row that only plays the sequences aimed at it.
    model.sources.push_back(SourceChoice{sourceLabel(MidiSourceRef{MidiSourceRef::Kind::None, {}}),
                                         MidiSourceRef{MidiSourceRef::Kind::None, {}}, SourceProtocol::Mixed, false});
    // Hardware first: it's what a player picks; then software, then the
    // virtual inputs other apps send to.
    std::vector<const MidiSourceInfo*> ordered;
    for (const MidiSourceInfo& info : enumerated)
        if (info.isHardware) ordered.push_back(&info);
    for (const MidiSourceInfo& info : enumerated)
        if (!info.isHardware) ordered.push_back(&info);
    for (const MidiSourceInfo* info : ordered) {
        const MidiSourceRef ref{MidiSourceRef::Kind::Device, info->id};
        model.sources.push_back(SourceChoice{info->name.empty() ? info->id : info->name, ref,
                                             sourceProtocolOf(ref), info->isHardware});
    }
    for (const auto& [name, protocol] : config.virtualInputs) {
        const MidiSourceRef ref{MidiSourceRef::Kind::Virtual, name};
        model.sources.push_back(SourceChoice{sourceLabel(ref), ref, sourceProtocolOf(ref), false});
    }
    // Sequences aren't here: a sequence chooses its rows (Sequence::targets).
}

std::string RackController::sourceLabel(const MidiSourceRef& ref) const {
    switch (ref.kind) {
        case MidiSourceRef::Kind::All:     return "All inputs";
        case MidiSourceRef::Kind::None:    return "No live input";
        case MidiSourceRef::Kind::Virtual:
            // A port other apps send to, not a protocol setting: say so. The
            // port keeps its name ("MIDI 2.0 In"), so racks and connections don't change.
            for (const auto& [name, protocol] : config.virtualInputs)
                if (name == ref.name)
                    return std::string("From other apps (") + (protocol == MidiProtocol::Midi2 ? "MIDI 2.0" : "MIDI 1.0") + ")";
            return ref.name;
        case MidiSourceRef::Kind::Sequence:
            if (const Session::SequenceEntry* entry = session.sequence(ref.name)) return entry->sequence.name;
            return ref.name;
        case MidiSourceRef::Kind::Device:
            for (const MidiSourceInfo& info : enumerated)
                if (info.id == ref.name && !info.name.empty()) return info.name;
            return ref.name;
    }
    return ref.name;
}

SourceProtocol RackController::sourceProtocolOf(const MidiSourceRef& ref) const {
    // A sequence sends in its own protocol; the input preference doesn't apply.
    if (ref.kind == MidiSourceRef::Kind::Sequence) {
        const Session::SequenceEntry* entry = session.sequence(ref.name);
        return entry == nullptr ? SourceProtocol::Mixed : toSourceProtocol(entry->sequence.protocol);
    }
    // With the preference off, every input arrives as MIDI 1.0 (MidiInput).
    if (config.protocolPreference == MidiProtocol::Midi1) return SourceProtocol::Midi1;
    switch (ref.kind) {
        case MidiSourceRef::Kind::All:
        case MidiSourceRef::Kind::None:
        case MidiSourceRef::Kind::Sequence: return SourceProtocol::Mixed;
        case MidiSourceRef::Kind::Virtual:
            for (const auto& [name, protocol] : config.virtualInputs)
                if (name == ref.name) return toSourceProtocol(protocol);
            return SourceProtocol::Mixed;
        case MidiSourceRef::Kind::Device:
            for (const MidiSourceInfo& info : enumerated)
                if (info.id == ref.name) return toSourceProtocol(info.protocol);
            return SourceProtocol::Mixed;
    }
    return SourceProtocol::Mixed;
}

// --- intents -----------------------------------------------------------------------

RowModel* RackController::modelRow(const std::string& id) {
    for (RowModel& row : model.rows)
        if (row.id == id) return &row;
    return nullptr;
}

void RackController::setGainDb(const std::string& id, float db) {
    if (session.setGainDb(id, db))
        if (RowModel* row = modelRow(id)) row->gainDb = db;
}

void RackController::setMuted(const std::string& id, bool muted) {
    if (session.setMuted(id, muted))
        if (RowModel* row = modelRow(id)) row->muted = muted;
}

void RackController::setFilter(const std::string& id, const MidiFilter& filter) {
    if (session.setFilter(id, filter))
        if (RowModel* row = modelRow(id)) row->filter = filter;
}

void RackController::setFirstOutput(const std::string& id, std::uint32_t firstOutput) {
    if (session.setFirstOutput(id, firstOutput))
        if (RowModel* row = modelRow(id)) row->firstOutput = firstOutput;
}

void RackController::setSource(const std::string& id, const MidiSourceRef& source) {
    std::string problem;
    if (session.setSource(id, source, problem) && !problem.empty())
        notice("'" + id + "': " + problem + "; it hears nothing until that's fixed");
}

void RackController::setTempo(double bpm) {
    session.setTempo(std::round(bpm * 10.0) / 10.0);
    model.transport.tempoBpm = session.tempoBpm();
}

void RackController::setTimeSignature(int numerator, int denominator) {
    if (!session.setTimeSignature(numerator, denominator)) return;
    model.transport.numerator   = numerator;
    model.transport.denominator = denominator;
}

void RackController::tapTempo(double now) {
    if (!taps.empty() && now - taps.back() > 2.0) taps.clear();   // a new count
    taps.push_back(now);
    if (taps.size() > 5) taps.erase(taps.begin());                // four intervals
    if (taps.size() < 2) return;
    const double interval = (taps.back() - taps.front()) / static_cast<double>(taps.size() - 1);
    if (interval > 0.0) setTempo(60.0 / interval);
}

void RackController::clearClip(const std::string& id) {
    if (const Session::Slot* slot = session.slot(id); slot != nullptr && slot->runtime)
        slot->runtime->meter.clearClip();
}

void RackController::resetXruns() {
    xrunsBase = xrunsSeen;
    xrunAt    = -1.0;
}

// "xrun: song 4 (So what), bar 17" on stderr, so a gig's xruns can be found in the log.
void RackController::logXrun() const {
    const SetlistModel& set  = model.setlist;
    std::string         song = set.open && set.current >= 0 && set.current < static_cast<int>(set.songs.size())
                                   ? "song " + std::to_string(set.current + 1) + " (" +
                                         set.songs[static_cast<std::size_t>(set.current)].name + ")"
                                   : session.rackName().empty() ? std::string("untitled rack")
                                                                : session.rackName();
    const Engine::Transport t = engine.transport();
    std::fprintf(stderr, "xrun: %s, %s\n", song.c_str(),
                 t.running ? ("bar " + std::to_string(t.barNumber + 1)).c_str() : "stopped");
}

void RackController::LoadWindow::update(float peak, double now) noexcept {
    pending = std::max(pending, peak);
    if (since < 0.0) since = now;
    if (now - since < 1.0) return;
    shown   = pending;
    pending = 0.0f;
    since   = now;
}

void RackController::clearOutputClip() {
    engine.outputMeter().clearClip();
}

// --- the rig (docs/rig.md) -------------------------------------------------------------

void RackController::setSendDb(const std::string& id, std::size_t bus, std::optional<float> db) {
    if (session.setSendDb(id, bus, db))
        if (RowModel* row = modelRow(id)) row->sendDb[bus] = db;
}

void RackController::setReturnDb(std::size_t bus, float db) {
    if (session.setReturnDb(bus, db)) model.rig.buses[bus].returnDb = db;   // patched now, as a row's gain is
}

void RackController::setBusMuted(std::size_t bus, bool muted) {
    if (session.setBusMuted(bus, muted)) model.rig.buses[bus].muted = muted;
}

void RackController::setBusOutput(std::size_t bus, std::uint32_t firstOutput) {
    if (session.setBusOutput(bus, firstOutput)) model.rig.buses[bus].firstOutput = firstOutput;
}

void RackController::setMasterDb(float db) {
    if (!session.hasRig()) return;
    session.setMasterDb(db);
    model.rig.masterDb = db;
}

void RackController::setBusEffect(std::size_t bus, std::size_t slot, const PluginDescriptor* plugin) {
    if (!session.hasRig() || bus >= kRigBuses || slot >= RigBus::kEffects) return;
    std::string problem;
    closeBusEditor(bus, slot);   // while the old effect is certainly alive
    if (!session.setBusEffect(bus, slot, plugin, problem))
        notice("Couldn't load " + (plugin->name.empty() ? plugin->id : plugin->name) + ": " + problem);
}

void RackController::openBusEditor(std::size_t bus, std::size_t slot) {
    if (!session.hasRig() || bus >= kRigBuses || slot >= RigBus::kEffects || !config.openEditor) return;
    const std::shared_ptr<HostedPlugin>& plugin = session.rig().effects[bus][slot].plugin;
    if (!plugin) return;
    const auto key = std::make_pair(bus, slot);
    if (const auto it = busEditors.find(key); it != busEditors.end() && it->second->isOpen()) return;
    const std::string title = "Bus " + std::string(1, static_cast<char>('A' + bus)) + " " + std::to_string(slot + 1) +
                              " - " + plugin->descriptor().name + " (StageHand)";
    if (std::unique_ptr<EditorHandle> editor = config.openEditor(*plugin, title)) busEditors[key] = std::move(editor);
}

void RackController::closeBusEditor(std::size_t bus, std::size_t slot) {
    busEditors.erase(std::make_pair(bus, slot));
}

void RackController::buildRig(double now, double dt) {
    RigModel& m = model.rig;
    m           = {};
    if (!session.hasRig()) return;
    const Session::LoadedRig& rig = session.rig();
    m.loaded   = true;
    m.name     = rig.saved.name;
    m.modified = session.rigModified();
    m.masterDb = rig.saved.masterDb;
    for (std::size_t b = 0; b < kRigBuses; ++b) {
        BusModel&     bus   = m.buses[b];
        const RigBus& saved = rig.saved.buses[b];
        bus.name            = std::string(1, static_cast<char>('A' + b));
        bus.returnDb        = saved.returnDb;
        bus.muted           = saved.muted;
        bus.firstOutput     = saved.firstOutput;
        for (std::size_t e = 0; e < RigBus::kEffects; ++e) {
            BusEffectModel&                     effect = bus.effects[e];
            const Session::LoadedRig::Effect&   loaded = rig.effects[b][e];
            if (loaded.plugin) {
                effect.name      = loaded.plugin->descriptor().name.empty() ? loaded.plugin->descriptor().id
                                                                            : loaded.plugin->descriptor().name;
                effect.hasEditor = loaded.plugin->hasEditor();
            } else if (saved.effects[e]) {
                effect.name = saved.effects[e]->plugin.name.empty() ? saved.effects[e]->plugin.id
                                                                    : saved.effects[e]->plugin.name;
            }
            effect.problem    = loaded.problem;
            const auto open   = busEditors.find(std::make_pair(b, e));
            effect.editorOpen = open != busEditors.end() && open->second->isOpen();
        }
        if (const std::shared_ptr<SlotRuntime>& runtime = rig.returns[b]) {
            busMeters[b].update(runtime->meter.take(), now, dt);
            busLoads[b].update(runtime->load.take(), now);
            bus.loadPercent = busLoads[b].percent();
            for (std::size_t c = 0; c < LevelMeter::kChannels; ++c) {
                bus.meterDb[c]    = busMeters[b].displayDb[c];
                bus.peakHoldDb[c] = busMeters[b].holdDb[c];
            }
        }
    }
}

const std::vector<PluginDescriptor>& RackController::availablePlugins() {
    if (!scanned) {
        available = factory.scan();
        std::sort(available.begin(), available.end(),
                  [](const PluginDescriptor& a, const PluginDescriptor& b) { return a.name < b.name; });
        scanned = true;
    }
    return available;
}

std::string RackController::addPlugin(const PluginDescriptor& plugin, std::size_t position) {
    std::string       problem;
    const std::string id = session.addSlot(plugin, position, problem);
    if (id.empty()) notice("Couldn't add " + (plugin.name.empty() ? plugin.id : plugin.name) + ": " + problem);
    return id;
}

void RackController::removeSlot(const std::string& id) {
    closeEditor(id);   // while the plugin is certainly alive
    session.removeSlot(id);
}

void RackController::openEditor(const std::string& id) {
    const Session::Slot* slot = session.slot(id);
    if (slot == nullptr || !slot->plugin || !config.openEditor) return;
    if (const auto it = editors.find(id); it != editors.end() && it->second->isOpen()) return;
    const std::string title = slot->saved.name + " - " + slot->plugin->descriptor().name + " (StageHand)";
    if (std::unique_ptr<EditorHandle> editor = config.openEditor(*slot->plugin, title))
        editors[id] = std::move(editor);
}

void RackController::closeEditor(const std::string& id) {
    editors.erase(id);
}

void RackController::closeAllEditors() {
    editors.clear();
}

void RackController::openAllEditors() {
    for (const Session::Slot& slot : session.slots()) openEditor(slot.saved.id);
}

std::size_t RackController::openEditorCount() const {
    return static_cast<std::size_t>(std::count_if(editors.begin(), editors.end(),
                                                  [](const auto& e) { return e.second->isOpen(); }));
}

// --- sequences ---------------------------------------------------------------------

template <typename Change>
void RackController::editSequence(const std::string& id, Change change) {
    const Session::SequenceEntry* entry = session.sequence(id);
    if (entry == nullptr) return;
    Sequence seq = entry->sequence;
    change(seq);
    std::string problem;
    if (!session.setSequence(seq, problem)) notice(problem);
}

const Sequence* RackController::sequence(const std::string& id) const {
    const Session::SequenceEntry* entry = session.sequence(id);
    return entry != nullptr ? &entry->sequence : nullptr;
}

std::string RackController::addSequence(MidiProtocol protocol, const std::string& target) {
    const std::string id = session.addSequence(protocol);
    if (!target.empty()) setSequenceTarget(id, target, true);
    return id;
}

std::string RackController::addRowForSequence(const std::string& id, const PluginDescriptor& plugin) {
    const std::string row = addPlugin(plugin, session.slots().size());
    if (!row.empty()) setSequenceTarget(id, row, true);
    return row;
}

void RackController::setSequenceTarget(const std::string& id, const std::string& row, bool on) {
    editSequence(id, [&](Sequence& seq) {
        auto& t = seq.targets;
        const auto it = std::find(t.begin(), t.end(), row);
        if (on && it == t.end()) t.push_back(row);
        if (!on && it != t.end()) t.erase(it);
    });
}

void RackController::removeSequence(const std::string& id) {
    if (id == learningId) cancelLearn();
    session.removeSequence(id);
}

void RackController::togglePlay(const std::string& id) {
    switch (session.sequenceState(id)) {
        case SequenceState::Stopped:
        case SequenceState::Stopping: session.startSequence(id); break;   // a pending stop is cancelled
        case SequenceState::Starting:
        case SequenceState::Playing: session.stopSequence(id); break;     // a pending start is cancelled
    }
}

void RackController::stopAllSequences() {
    session.stopAllSequences();
}

void RackController::setSequenceRate(const std::string& id, SequenceRate rate) {
    editSequence(id, [rate](Sequence& seq) { seq.rate = rate; });
}

void RackController::setSequenceLength(const std::string& id, std::uint32_t length) {
    const Session::SequenceEntry* entry = session.sequence(id);
    if (entry == nullptr) return;
    length = std::clamp<std::uint32_t>(length, 1, 4096);
    std::uint32_t lastEnd = 0;
    for (const SequenceNote& note : entry->sequence.notes) lastEnd = std::max(lastEnd, note.start + note.length);
    if (length < lastEnd) {
        notice(entry->sequence.name + ": notes reach unit " + std::to_string(lastEnd) +
               "; delete them in the editor to make it shorter");
        return;
    }
    editSequence(id, [length](Sequence& seq) { seq.length = length; });
}

void RackController::setSequenceLaunch(const std::string& id, LaunchQuantize launch) {
    editSequence(id, [launch](Sequence& seq) { seq.launch = launch; });
}

void RackController::setSequenceRepeat(const std::string& id, bool repeat) {
    editSequence(id, [repeat](Sequence& seq) { seq.repeat = repeat; });
}

void RackController::setSequenceNotes(const std::string& id, std::vector<SequenceNote> notes) {
    editSequence(id, [&notes](Sequence& seq) { seq.notes = std::move(notes); });
}

void RackController::endAuditions(const std::string& id, bool allNotesOff) {
    session.endAuditions(id, allNotesOff);
}

void RackController::auditionNotes(const std::string& id, const std::vector<SequenceNote>& notes, bool hold) {
    session.endAuditions(id);
    for (std::size_t i = 0; i < notes.size() && i < kMaxAuditionNotes; ++i) session.auditionNote(id, notes[i], hold);
}

void RackController::setSequenceProtocol(const std::string& id, MidiProtocol protocol) {
    editSequence(id, [protocol](Sequence& seq) { seq.protocol = protocol; });   // the Session refuses it with notes
}

void RackController::setSequenceTuning(const std::string& id, int edo, int referenceNote) {
    editSequence(id, [=](Sequence& seq) {
        seq.edo           = std::clamp(edo, 1, 128);
        seq.referenceNote = std::clamp(referenceNote, 0, 127);
    });
}

void RackController::setSequenceChannel(const std::string& id, int channel) {
    editSequence(id, [channel](Sequence& seq) { seq.channel = static_cast<std::uint8_t>(std::clamp(channel, 0, 15)); });
}

void RackController::setSequenceGroup(const std::string& id, int group) {
    editSequence(id, [group](Sequence& seq) { seq.group = static_cast<std::uint8_t>(std::clamp(group, 0, 15)); });
}

void RackController::setSequenceNoteOffOffset(const std::string& id, double ms) {
    editSequence(id, [ms](Sequence& seq) { seq.noteOffOffsetMs = std::clamp(ms, -1000.0, 1000.0); });
}

void RackController::renameSequence(const std::string& id, const std::string& name) {
    const std::size_t first = name.find_first_not_of(" \t");
    if (first == std::string::npos) {
        notice("A sequence needs a name; it keeps its old one");
        return;
    }
    std::string trimmed = name.substr(first, name.find_last_not_of(" \t") - first + 1);
    if (trimmed.size() > kMaxNameBytes) {   // cut on a character boundary
        std::size_t end = kMaxNameBytes;
        while (end > 0 && (static_cast<unsigned char>(trimmed[end]) & 0xC0) == 0x80) --end;
        trimmed.resize(end);
    }
    editSequence(id, [&trimmed](Sequence& seq) { seq.name = trimmed; });
}

void RackController::setSequenceAtStart(const std::string& id, bool atStart) {
    editSequence(id, [atStart](Sequence& seq) { seq.atStart = atStart; });
}

void RackController::setMetronome(const MetronomeSettings& settings) {
    session.setMetronome(settings);
    routeClick(true);
}

void RackController::setMetronomeCount(bool everyNote) {
    MetronomeSettings m = session.metronome();
    m.everyNote         = everyNote;
    setMetronome(m);
}

// Turned on with no output chosen, the click takes a playback device other
// than the mains' (the first). If the mains' device is the only one, it stays
// silent until chosen: the menu says so.
void RackController::setClick(bool on) {
    MetronomeSettings m = session.metronome();
    m.click             = on;
    if (on && session.clickOutput().empty() && config.clickRouting != nullptr) {
        const std::string main = config.clickRouting->mainDevice();
        for (const PlaybackDevice& d : config.clickRouting->playbackDevices())
            if (d.name != main) {
                session.setClickOutput(d.name);   // the rig's: every song's
                break;
            }
        if (session.clickOutput().empty())
            notice("Click: silent until you choose its output (the only device plays the main outputs too)");
    }
    setMetronome(m);
    if (on && !session.transportRunning()) notice("Click: it sounds while the transport runs (▶ or Space)");
}

// Choosing a level means wanting the click: it's turned on (as setClick(true)).
void RackController::setClickLevel(float db) {
    MetronomeSettings m = session.metronome();
    m.clickLevelDb      = std::clamp(db, -60.0f, 0.0f);
    session.setMetronome(m);
    setClick(true);
}

// Where it plays is the rig's (every song's); whether this song clicks is the rack's.
void RackController::setCountIn(double bars) {
    MetronomeSettings m = session.metronome();
    m.countInBars       = bars <= 0.0 ? 0.0 : bars < 1.0 ? 0.5 : bars < 2.0 ? 1.0 : 2.0;
    setMetronome(m);
}

void RackController::setFlash(bool on) {
    MetronomeSettings m = session.metronome();
    m.flash             = on;
    setMetronome(m);
}

void RackController::setClickOutput(const std::string& device) {
    session.setClickOutput(device);
    MetronomeSettings m = session.metronome();
    m.click             = !device.empty();   // choosing where it plays means wanting it
    clickMissing        = false;   // a new choice: report afresh
    setMetronome(m);
}

void RackController::testClick() {
    const MetronomeSettings& m      = session.metronome();
    const std::string&       output = session.clickOutput();
    if (output.empty()) {
        notice("Click: choose its output first");
        return;
    }
    if (!m.click) {   // off: connect just for the test
        AuxRouting* const routing = config.clickRouting;
        const auto        devices = routing != nullptr ? routing->playbackDevices() : std::vector<PlaybackDevice>{};
        const int         i       = findPlaybackDevice(output, devices);
        if (i < 0 || !routing->connectAux(devices[static_cast<std::size_t>(i)])) {
            notice("Click: its output '" + output + "' isn't connected");
            return;
        }
        clickDevice = devices[static_cast<std::size_t>(i)].name;
    } else if (clickDevice.empty()) {
        notice("Click: its output '" + output + "' isn't connected");
        return;
    }
    engine.testClick();
    notice("Click: a test blip on " + clickDevice);
}

void RackController::refreshPlaybackDevices() {
    deviceNames.clear();
    if (config.clickRouting == nullptr) return;
    for (const PlaybackDevice& d : config.clickRouting->playbackDevices()) deviceNames.push_back(d.name);
    model.transport.playbackDevices = deviceNames;
    model.transport.mainDevice      = config.clickRouting->mainDevice();
}

// Never the main outputs by default: a click the audience hears is worse than
// none. A missing device leaves it unconnected, and it connects when it appears.
void RackController::routeClick(bool report) {
    AuxRouting* const        routing = config.clickRouting;
    const MetronomeSettings& m       = session.metronome();
    const std::string&       output  = session.clickOutput();
    if (routing == nullptr) return;
    if (!m.click || output.empty()) {
        if (!clickDevice.empty()) routing->disconnectAux();
        clickDevice.clear();
        clickMissing = false;
        return;
    }
    const std::vector<PlaybackDevice> devices = routing->playbackDevices();
    const int                         i       = findPlaybackDevice(output, devices);
    if (i < 0) {
        if (!clickDevice.empty()) routing->disconnectAux();
        clickDevice.clear();
        if (report && !clickMissing)
            notice("Click: its output '" + output + "' isn't connected; the click is silent until it is");
        clickMissing = true;
        return;
    }
    const PlaybackDevice& device = devices[static_cast<std::size_t>(i)];
    if (device.name == clickDevice) return;
    if (!routing->connectAux(device)) {
        notice("Click: couldn't connect to " + device.name);
        return;
    }
    if (clickMissing || device.name != output) notice("Click: playing on " + device.name);
    clickDevice  = device.name;
    clickMissing = false;
}

void RackController::startTransport() {
    session.startTransport();
}

void RackController::stopTransport() {
    session.stopTransport();
}

void RackController::toggleTransport() {
    if (session.transportRunning()) stopTransport();
    else startTransport();
}

void RackController::learnTransportTrigger(SequenceTrigger::Action action) {
    learningId     = kTransportLearn;
    learningAction = action;
    engine.armLearn();
}

void RackController::removeTransportTrigger(std::size_t index) {
    TransportSettings settings = session.transportSettings();
    if (index >= settings.triggers.size()) return;
    settings.triggers.erase(settings.triggers.begin() + static_cast<std::ptrdiff_t>(index));
    session.setTransportSettings(settings);
}

void RackController::setStartOnNote(bool on, const MidiSourceRef& source) {
    TransportSettings settings = session.transportSettings();
    settings.startOnNote       = on;
    settings.startOnNoteSource = on ? source : MidiSourceRef{};
    session.setTransportSettings(settings);
    if (on && !session.startOnNoteIssue().empty())
        notice("Start on first note: " + session.startOnNoteIssue() + "; it won't start until that's fixed");
}

void RackController::learnTrigger(const std::string& id, SequenceTrigger::Action action) {
    if (session.sequence(id) == nullptr) return;
    learningId     = id;
    learningAction = action;
    engine.armLearn();
}

void RackController::cancelLearn() {
    if (learningId.empty()) return;
    engine.cancelLearn();
    learningId.clear();
}

void RackController::removeTrigger(const std::string& id, std::size_t index) {
    editSequence(id, [index](Sequence& seq) {
        if (index < seq.triggers.size()) seq.triggers.erase(seq.triggers.begin() + static_cast<std::ptrdiff_t>(index));
    });
}

// The control MIDI learn heard becomes the sequence's trigger for the action.
void RackController::finishLearn(const Engine::Learned& learned) {
    const std::string id        = std::exchange(learningId, {});
    const bool        transport = id == kTransportLearn;
    const bool        forSet    = id == kSetlistLearn;
    const bool        forLooper = id == kLooperLearn;
    const bool        forPanic  = id == kPanicLearn;
    const bool        forLink   = id == kLinkLearn;
    const bool        ownTrigger = transport || forSet || forLooper || forPanic || forLink;
    const Session::SequenceEntry* entry = ownTrigger ? nullptr : session.sequence(id);
    if (forSet ? !setlistOpen : !ownTrigger && entry == nullptr) return;   // closed or removed meanwhile
    const std::string who = forSet      ? std::string("Setlist")
                            : forLooper ? std::string("Looper switch")
                            : forPanic  ? std::string("Panic")
                            : forLink   ? std::string("Control Link")
                            : transport ? std::string("Transport")
                                        : entry->sequence.name;
    const auto input = std::find_if(session.openInputs().begin(), session.openInputs().end(),
                                    [&](const Session::OpenInput& in) { return in.handle == learned.source; });
    if (input == session.openInputs().end()) {
        notice(who + ": couldn't tell which input that came from; try again");
        return;
    }
    if (forSet) {
        SetlistTrigger t;
        t.action  = setlistLearnAction;
        t.source  = input->ref;
        t.control = ump::isNoteOn(learned.message) ? SequenceTrigger::Control::Note : SequenceTrigger::Control::Controller;
        t.group   = ump::group(learned.message);
        t.channel = ump::channel(learned.message);
        if (t.action != SetlistTrigger::Action::Song)
            t.number = static_cast<std::uint8_t>((learned.message.words[0] >> 8) & 0x7F);   // note or controller
        std::erase_if(setlist.triggers, [&t](const SetlistTrigger& old) { return old.action == t.action || old.sameControl(t); });
        setlist.triggers.push_back(t);
        writeSetlist();
        applySetlistTriggers();
        notice(who + ": " + setlistTriggerText(t));
        std::vector<const Sequence*> sequences;
        for (const Session::SequenceEntry& e : session.sequences()) sequences.push_back(&e.sequence);
        for (const std::string& warning : triggerConflicts(session.rackName(), session.transportSettings(), sequences))
            notice(warning);
        return;
    }
    if (forLink) {   // docs/controls.md
        const MidiMessage& m = learned.message;
        if (!ump::isChannelVoice(m) || ump::status(m) != ump::Status::ControlChange) {
            notice(who + ": that was a key; move a knob, fader or pedal instead");
            return;
        }
        ControlLinkSetup link;
        link.source  = input->ref;
        link.group   = ump::group(m);
        link.channel = ump::channel(m);
        link.number  = static_cast<std::uint8_t>((m.words[0] >> 8) & 0x7F);
        if (const std::string user = triggerOn(link.source, link.group, link.channel, link.number); !user.empty()) {
            notice(who + ": CC " + std::to_string(link.number) + " is already " + user + "; choose another control");
            return;
        }
        if (linkRelearn) {   // another control for a link
            const std::size_t             index = *std::exchange(linkRelearn, std::nullopt);
            std::vector<ControlLinkSetup> links = session.controlLinks(linkScope);
            if (index >= links.size()) return;   // removed meanwhile
            for (std::size_t i = 0; i < links.size(); ++i)
                if (i != index && links[i].sameControl(link)) {
                    notice(who + ": " + controlText(link) + " has a Control Link of its own; choose another control");
                    return;
                }
            links[index].source  = link.source;
            links[index].group   = link.group;
            links[index].channel = link.channel;
            links[index].number  = link.number;
            session.setControlLinks(linkScope, std::move(links));
            notice(who + ": now " + controlText(link));
            return;
        }
        link.targets = {linkTarget};
        addLinkTarget(link, linkScope);
        bool present = true;
        notice(who + ": " + controlText(link) + " \xE2\x86\x92 " + targetText(linkTarget, present));
        return;
    }
    if (forLooper || forPanic) {   // in the rig: the performer's, the same in every song
        SequenceTrigger t;
        t.source  = input->ref;
        t.control = ump::isNoteOn(learned.message) ? SequenceTrigger::Control::Note : SequenceTrigger::Control::Controller;
        t.group   = ump::group(learned.message);
        t.channel = ump::channel(learned.message);
        t.number  = static_cast<std::uint8_t>((learned.message.words[0] >> 8) & 0x7F);
        if (forPanic)
            session.setPanicControl(t);
        else
            session.setLooperSwitch(t);
        const std::string text = triggerText(t);
        notice(who + ": " + text.substr(text.find(": ") + 2));
        return;
    }
    SequenceTrigger t;
    t.action  = learningAction;
    t.source  = input->ref;
    t.control = ump::isNoteOn(learned.message) ? SequenceTrigger::Control::Note : SequenceTrigger::Control::Controller;
    t.group   = ump::group(learned.message);
    t.channel = ump::channel(learned.message);
    t.number  = static_cast<std::uint8_t>((learned.message.words[0] >> 8) & 0x7F);   // note or controller
    auto replace = [&t](std::vector<SequenceTrigger>& triggers) {
        std::erase_if(triggers, [&t](const SequenceTrigger& old) { return old.action == t.action || old.sameControl(t); });
        triggers.push_back(t);
    };
    if (transport) {
        TransportSettings settings = session.transportSettings();
        replace(settings.triggers);
        session.setTransportSettings(settings);
    } else {
        editSequence(id, [&replace](Sequence& seq) { replace(seq.triggers); });
    }
    notice(who + ": " + triggerText(t));
}

std::string RackController::setlistTriggerText(const SetlistTrigger& t) const {
    if (t.action == SetlistTrigger::Action::Song) {
        std::string text = "Program Change: " + sourceLabel(t.source) + ", ch " + std::to_string(t.channel + 1);
        if (t.group != 0) text += " g" + std::to_string(t.group + 1);
        return text + " (program n: song n)";
    }
    SequenceTrigger as;   // the same words as a sequence's, after the action
    as.source  = t.source;
    as.control = t.control;
    as.group   = t.group;
    as.channel = t.channel;
    as.number  = t.number;
    const std::string text = triggerText(as);
    return (t.action == SetlistTrigger::Action::Next ? "Next song: " : "Previous song: ") + text.substr(text.find(": ") + 2);
}

std::string RackController::triggerText(const SequenceTrigger& t) const {
    static const char* const names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    std::string text = t.action == SequenceTrigger::Action::Start  ? "Start: "
                       : t.action == SequenceTrigger::Action::Stop ? "Stop: "
                                                                   : "Play/Stop: ";
    text += sourceLabel(t.source) + ", ";
    text += t.control == SequenceTrigger::Control::Note
                ? "note " + std::string(names[t.number % 12]) + std::to_string(t.number / 12 - 1)
                : "CC " + std::to_string(t.number);
    text += ", ch " + std::to_string(t.channel + 1);
    if (t.group != 0) text += " g" + std::to_string(t.group + 1);
    return text;
}

// --- saving and quitting -----------------------------------------------------------

bool RackController::save() {
    if (config.savingOff) {
        notice("Not saved: saving is off (--no-save-rack)");
        return false;
    }
    const bool others = saveUnsavedRacks();
    if (target.empty()) {
        notice("Not saved: this rack is untitled; use Save As");
        return false;
    }
    return saveCaptured(session.capture()) && others;
}

bool RackController::saveUnsavedRacks() {
    bool        ok    = true;
    std::size_t saved = 0;
    for (auto it = unsavedRacks.begin(); it != unsavedRacks.end();) {
        const RackFileResult result = saveRack(it->first, it->second);
        if (!result.ok) {
            notice("Couldn't save " + rackNameOf(it->first) + ": " + result.error + "; it stays unsaved");
            ok = false;
            ++it;
            continue;
        }
        it = unsavedRacks.erase(it);
        ++saved;
    }
    if (saved != 0) notice("Saved " + std::to_string(saved) + (saved == 1 ? " other song's rack" : " other songs' racks"));
    return ok;
}

bool RackController::saveCaptured(const Rack& captured) {
    const RackFileResult result = saveRack(target, captured);
    if (!result.ok) {
        notice("Couldn't save the rack: " + result.error);
        return false;
    }
    session.markSaved();
    notice("Saved the rack to " + target);
    return saveRigIfModified();
}

RackController::SaveAsResult RackController::saveAs(const std::string& chosen, bool replace) {
    if (config.savingOff) {
        notice("Not saved: saving is off (--no-save-rack)");
        return SaveAsResult::Failed;
    }
    const std::string path = saveAsPath(chosen);
    if (path.empty()) return SaveAsResult::Failed;
    switch (classifySaveTarget(path, target)) {
        case SaveTarget::NotARack:
            notice("Not saved: " + path + " exists and isn't a rack; choose another name");
            return SaveAsResult::Failed;
        case SaveTarget::OtherRack:
            if (!replace) return SaveAsResult::NeedsReplace;
            break;
        case SaveTarget::New:
        case SaveTarget::Current: break;
    }
    const std::string oldName = session.rackName();
    session.setRackName(rackNameOf(path));
    const RackFileResult result = saveRack(path, session.capture());
    if (!result.ok) {
        session.setRackName(oldName);
        notice("Couldn't save the rack: " + result.error);
        return SaveAsResult::Failed;
    }
    target = path;
    prepared.erase(path);   // a rack replaced there: its prepared copy is out of date
    session.markSaved();
    notice("Saved the rack to " + target);
    saveRigIfModified();
    if (setlistOpen && onSong && song < setlist.songs.size()) {   // the song now plays the new bundle
        setlist.songs[song].rack = path;
        writeSetlist();
    }
    return SaveAsResult::Saved;
}

bool RackController::saveForQuit() {
    if (config.savingOff) return true;
    if (!target.empty()) return save();
    const bool others = saveUnsavedRacks();
    return saveRigIfModified() && others;
}

// --- the rig (docs/rig.md) -------------------------------------------------------------

void RackController::useRig(const std::string& path) {
    const std::string wanted = path.empty() ? config.defaultRigPath : path;
    if (wanted.empty() || wanted == rigFile) return;
    if (!rigFile.empty()) saveRigIfModified();   // the one in use, as a rack is saved when switching

    Rig                  rig;
    const RackFileResult result = loadRig(wanted, rig);
    if (!result.ok) {
        if (wanted == config.defaultRigPath && rigBundleOf(wanted).empty()) {   // first use: an empty one
            rig      = Rig{};
            rig.name = rackNameOf(wanted);
            if (!config.savingOff && !saveRig(wanted, rig).ok) notice("Couldn't create the default rig at " + wanted);
        } else {
            notice("Couldn't open the rig: " + result.error + "; using the default rig");
            if (wanted != config.defaultRigPath) useRig({});
            return;
        }
    }
    for (const std::string& warning : result.warnings) notice(warning);
    Session::LoadReport report;
    busEditors.clear();   // the old rig's effects are on their way out
    session.loadRig(rig, session.loadedSampleRate(), report);
    for (const std::string& warning : report.warnings) notice(warning);
    rigFile = wanted;
    routeClick(true);   // the click plays where this rig says
}

bool RackController::saveRigIfModified() {
    if (config.savingOff || rigFile.empty() || !session.rigModified()) return true;
    const RackFileResult result = saveRig(rigFile, session.captureRig());
    if (!result.ok) {
        notice("Couldn't save the rig: " + result.error);
        return false;
    }
    session.markRigSaved();
    notice("Saved the rig to " + rigFile);
    return true;
}

bool RackController::unsavedUntitled() const {
    return target.empty() && !config.savingOff && session.modified();
}

bool RackController::newRack() {
    Rack rack;
    rack.name = "Untitled";
    if (!replaceRack(rack, {}, {})) return false;
    if (setlistOpen) closeSetlist();   // a rack opened on its own leaves the set
    return true;
}

bool RackController::openRack(const std::string& chosen) {
    const std::string bundle = rackBundleOf(chosen);
    if (bundle.empty()) {
        notice("Couldn't open " + chosen + ": it isn't a rack (no rack.json)");
        return false;
    }
    if (!loadRackAt(bundle)) return false;
    if (setlistOpen) closeSetlist();   // a rack opened on its own leaves the set
    return true;
}

bool RackController::loadRackAt(const std::string& bundle) {
    if (const auto kept = unsavedRacks.find(bundle); kept != unsavedRacks.end())
        return replaceRack(kept->second, bundle, {});   // its edits, still unsaved
    Rack                 rack;
    const RackFileResult result = loadRack(bundle, rack);
    if (!result.ok) {
        notice("Couldn't open " + bundle + ": " + result.error);
        return false;
    }
    return replaceRack(rack, bundle, result.warnings);
}

// Open and New: the rack prepared at once (they aren't live actions).
bool RackController::replaceRack(const Rack& rack, const std::string& path, const std::vector<std::string>& warnings) {
    std::unique_ptr<PreparedRack> next = session.prepare(rack, session.loadedSampleRate());
    if (!switchRack(next, path, warnings, Session::SwitchOptions{}, false)) return false;
    tick(std::max(lastTick, 0.0));
    return true;
}

bool RackController::switchRack(std::unique_ptr<PreparedRack>& next, const std::string& path,
                                 const std::vector<std::string>& warnings, const Session::SwitchOptions& how,
                                 bool live) {
    // The rack being left, captured once (each plugin's saveState(), in
    // memory): saved first unless the switch is live, kept in memory if it's
    // live and has edits, and in a setlist prepared again from it, since it's
    // likely the previous song now.
    const std::string oldPath = target;
    const bool        saving  = !live && !config.savingOff && !oldPath.empty();
    const bool        keep    = setlistOpen && !oldPath.empty() && oldPath != path;
    const bool        kept    = live && !oldPath.empty() && oldPath != path && session.modified();
    Rack              old;
    if (saving || keep || kept) old = session.capture();
    if (!live && (saving ? !saveCaptured(old) : !saveRigIfModified())) return false;   // the notice says why
    cancelLearn();
    closeAllEditors();
    rows.clear();
    taps.clear();

    Session::LoadReport report;
    session.activate(std::move(next), report, how);   // one commit; the rig's effects stay
    target = path;
    if (kept) unsavedRacks[oldPath] = old;
    if (const auto edited = unsavedRacks.find(path); edited != unsavedRacks.end()) {
        unsavedRacks.erase(edited);   // the running rack holds its edits now
        session.markModified();
    }
    routeClick(true);
    ++generation;   // sequence windows close
    for (const std::string& warning : warnings) notice(warning);
    for (const std::string& warning : report.warnings) notice(warning);
    if (setlistOpen && !path.empty() && songProblems.count(path) != 0) {   // checked: what it reports now
        std::vector<std::string>& problems = songProblems[path];
        problems                           = warnings;
        problems.insert(problems.end(), report.warnings.begin(), report.warnings.end());
    }
    notice(path.empty() ? "New rack" : "Opened " + path);
    refreshSources();
    prepared.erase(path);
    if (keep) prepared[oldPath] = Ahead{session.prepare(old, session.loadedSampleRate()), {}};
    return true;
}

void RackController::quit() {
    if (config.quit) config.quit();
}

// --- the setlist (docs/setlists.md) -----------------------------------------------------

namespace {

bool rackMissing(const SetlistSong& song) {
    return rackBundleOf(song.rack).empty();
}

std::string trimmed(const std::string& text) {
    const std::size_t first = text.find_first_not_of(" \t");
    if (first == std::string::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t") - first + 1);
}

} // namespace

RackController::NewSetlistResult RackController::newSetlist(const std::string& chosen, bool withThisRack,
                                                            bool replace) {
    const std::string path = setlistPathOf(chosen);
    if (path.empty()) return NewSetlistResult::Failed;
    if (withThisRack && target.empty()) {
        notice("Save the rack first (Save As): a song must be a rack on disk");
        return NewSetlistResult::Failed;
    }
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        if (!isSetlistFile(path)) {
            notice("Not created: " + path + " exists and isn't a setlist; choose another name");
            return NewSetlistResult::Failed;
        }
        if (!replace) return NewSetlistResult::NeedsReplace;
    }
    Setlist fresh;
    if (withThisRack) fresh.songs.push_back(SetlistSong{target, ""});
    const RackFileResult result = saveSetlist(path, fresh);
    if (!result.ok) {
        notice("Couldn't create the setlist: " + result.error);
        return NewSetlistResult::Failed;
    }
    if (setlistOpen && !config.savingOff) saveUnsavedRacks();   // the set being left
    setlistOpen = true;
    setlist     = std::move(fresh);
    setlistFile = resolveRackPath(path);
    prepared.clear();
    unpreparable.clear();
    waitingSong.reset();
    forgetChecks();
    useRig(setlist.rig);   // a new setlist names no rig: the default
    applySetlistTriggers();
    song        = 0;
    onSong      = withThisRack;
    rememberPosition();
    notice("New setlist " + setlistFile);
    return NewSetlistResult::Created;
}

bool RackController::openSetlist(const std::string& chosen, std::size_t start) {
    Setlist              loaded;
    const RackFileResult result = loadSetlist(chosen, loaded);
    if (!result.ok) {
        notice("Couldn't open the setlist: " + result.error);
        return false;
    }
    for (const std::string& warning : result.warnings) notice(warning);
    if (setlistOpen && !config.savingOff) saveUnsavedRacks();   // the set being left
    setlistOpen = true;
    setlist     = std::move(loaded);
    setlistFile = resolveRackPath(chosen);
    songCache.clear();
    prepared.clear();
    unpreparable.clear();
    waitingSong.reset();
    forgetChecks();
    useRig(setlist.rig);   // its own, else the default
    applySetlistTriggers();
    song        = std::min(start, setlist.songs.size());
    onSong      = false;
    if (setlist.songs.empty()) {
        rememberPosition();
        notice("Opened the setlist " + setlistFile + " (no songs yet)");
        return true;
    }
    song      = std::min(start, setlist.songs.size() - 1);
    int first = nextPlayable(song, 1);
    if (first < 0) first = nextPlayable(song, -1);
    if (first < 0) {
        rememberPosition();
        notice("Opened the setlist " + setlistFile + ", but none of its racks were found");
        return true;
    }
    goTo(static_cast<std::size_t>(first), true);   // not a live switch: no waiting
    return true;
}

void RackController::closeSetlist() {
    if (!setlistOpen) return;
    if (!config.savingOff) saveUnsavedRacks();   // leaving the set saves its racks, as quitting does
    useRig({});   // a rack on its own uses the default rig
    setlistOpen = false;
    setlist     = {};
    setlistFile.clear();
    song   = 0;
    onSong = false;
    prepared.clear();
    unpreparable.clear();
    waitingSong.reset();
    preparingText.clear();
    forgetChecks();
    if (learningId == kSetlistLearn) cancelLearn();
    applySetlistTriggers();   // none any more
    rememberPosition();
}

bool RackController::goToSong(std::size_t index) {
    return goTo(index, false);
}

bool RackController::goTo(std::size_t index, bool now) {
    waitingSong.reset();   // only the last request counts
    if (!setlistOpen || index >= setlist.songs.size()) return false;
    const SetlistSong& s      = setlist.songs[index];
    const std::string  bundle = rackBundleOf(s.rack);
    if (bundle.empty()) {
        notice("Song " + std::to_string(index + 1) + "'s rack is missing: " + s.rack);
        return false;
    }
    if (bundle != target) {
        auto it = prepared.find(bundle);
        if (it != prepared.end() && it->second.rack->sampleRate() != session.loadedSampleRate()) {
            prepared.erase(it);   // prepared for another sample rate
            it = prepared.end();
        }
        if (it == prepared.end()) {
            if (!startPreparing(bundle, true)) return false;
            it = prepared.find(bundle);
        }
        if (now)
            while (!session.prepareStep(*it->second.rack)) {}
        if (!it->second.rack->ready()) {   // the current rack plays on meanwhile
            waitingSong = index;
            return true;
        }
        Ahead ahead = std::move(it->second);
        prepared.erase(it);
        // A live switch follows the setlist's settings; opening the set doesn't.
        Session::SwitchOptions how;
        if (!now) {
            how.oldRack       = setlist.switching.oldRack == SwitchSettings::OldRack::Cut ? Session::OldRack::Cut
                                                                                         : Session::OldRack::RingOut;
            how.atNextBar     = setlist.switching.when == SwitchSettings::When::NextBar;
            how.stopTransport = setlist.switching.transport == SwitchSettings::Transport::Stop;
        }
        if (!switchRack(ahead.rack, bundle, ahead.warnings, how, !now)) {
            prepared[bundle] = std::move(ahead);   // still ready for another try
            return false;
        }
    }
    // else: another appearance of the rack playing, which is the same rack.
    song   = index;
    onSong = true;
    rememberPosition();
    if (!ticking) tick(std::max(lastTick, 0.0));
    return true;
}

bool RackController::songPrepared(std::size_t index) const {
    if (index >= songBundles.size() || songBundles[index].empty()) return false;
    if (songBundles[index] == target) return true;
    const auto it = prepared.find(songBundles[index]);
    return it != prepared.end() && it->second.rack->ready();
}

bool RackController::startPreparing(const std::string& bundle, bool report) {
    Rack           rack;
    RackFileResult result;
    if (const auto kept = unsavedRacks.find(bundle); kept != unsavedRacks.end()) {
        rack      = kept->second;   // the one in-memory copy, with its edits
        result.ok = true;
    } else {
        result = loadRack(bundle, rack);
    }
    if (!result.ok) {
        if (report) notice("Couldn't open " + bundle + ": " + result.error);
        unpreparable.insert(bundle);
        return false;
    }
    unpreparable.erase(bundle);
    std::vector<std::string>     warnings = result.warnings;   // shown at the switch
    std::vector<const Sequence*> sequences;
    for (const Sequence& seq : rack.sequences) sequences.push_back(&seq);
    for (std::string& warning : triggerConflicts(rack.name, rack.transport, sequences)) warnings.push_back(std::move(warning));
    prepared[bundle] = Ahead{session.prepare(rack, session.loadedSampleRate()), std::move(warnings)};
    return true;
}

void RackController::prepareAhead() {
    preparingText.clear();
    if (!setlistOpen) return;

    // What to keep ready, most urgent first: by the songs as last checked on disk.
    std::vector<std::pair<std::string, std::size_t>> wanted;   // bundle, a song using it
    auto want = [&](long index) {
        if (index < 0 || static_cast<std::size_t>(index) >= songBundles.size()) return;
        const std::string& bundle = songBundles[static_cast<std::size_t>(index)];
        if (bundle.empty() || bundle == target) return;
        for (const auto& w : wanted)
            if (w.first == bundle) return;
        wanted.emplace_back(bundle, static_cast<std::size_t>(index));
    };
    if (waitingSong) want(static_cast<long>(*waitingSong));
    want(neighbour(1));
    want(neighbour(-1));

    // Drop the rest, and racks prepared for another sample rate (the backend restarted).
    for (auto it = prepared.begin(); it != prepared.end();) {
        const bool used = std::any_of(wanted.begin(), wanted.end(), [&](const auto& w) { return w.first == it->first; });
        it = used && it->second.rack->sampleRate() == session.loadedSampleRate() ? std::next(it) : prepared.erase(it);
    }

    // One plugin per tick.
    for (const auto& [bundle, index] : wanted) {
        auto it = prepared.find(bundle);
        if (it == prepared.end()) {
            if (unpreparable.count(bundle) != 0 || !startPreparing(bundle, false)) continue;
            it = prepared.find(bundle);
        }
        PreparedRack& rack = *it->second.rack;
        if (rack.ready()) continue;
        session.prepareStep(rack);
        preparingText = "Preparing song " + std::to_string(index + 1) + " (" + rackNameOf(bundle) +
                        "): " + std::to_string(rack.preparedCount()) + "/" + std::to_string(rack.pluginCount()) +
                        " plugins";
        break;
    }

    // The switch waiting for its rack: once it's ready, or if something changed
    // under it (goTo() then switches, or says why it can't).
    if (waitingSong && *waitingSong < songBundles.size()) {
        const std::string& bundle = songBundles[*waitingSong];
        const auto         it     = prepared.find(bundle);
        if (bundle.empty() || bundle == target || unpreparable.count(bundle) != 0 ||
            (it != prepared.end() && it->second.rack->ready()))
            goTo(*waitingSong, false);
    }
}

bool RackController::checkSetlist() {
    if (!setlistOpen) return false;
    Check fresh;
    for (const SetlistSong& s : setlist.songs) {
        const std::string bundle = rackBundleOf(s.rack);   // a missing rack is shown as missing already
        if (!bundle.empty() && std::find(fresh.bundles.begin(), fresh.bundles.end(), bundle) == fresh.bundles.end())
            fresh.bundles.push_back(bundle);
    }
    songProblems.clear();
    check = std::move(fresh);
    return true;
}

void RackController::forgetChecks() {
    check.reset();
    songProblems.clear();
}

void RackController::checkStep() {
    if (!check) return;
    Check& c = *check;
    if (c.rack) {   // one plugin, then its problems once they're all made
        if (!session.prepareStep(*c.rack)) {
            preparingText = "Checking " + rackNameOf(c.bundle) + ": " + std::to_string(c.rack->preparedCount()) + "/" +
                            std::to_string(c.rack->pluginCount()) + " plugins";
            return;
        }
        c.found.insert(c.found.end(), c.rack->problems().begin(), c.rack->problems().end());
        c.rack.reset();   // dropped: a check never keeps a rack prepared
        songProblems[c.bundle] = std::move(c.found);
        c.found.clear();
        return;
    }
    if (c.next >= c.bundles.size()) {   // done
        std::size_t songs = 0;
        for (const std::string& bundle : songBundles)
            if (const auto it = songProblems.find(bundle); it != songProblems.end() && !it->second.empty()) ++songs;
        notice(songs == 0 ? std::string("Check setlist: no problems found")
                          : "Check setlist: " + std::to_string(songs) + (songs == 1 ? " song has" : " songs have") +
                                " problems (in amber in the song list)");
        check.reset();
        return;
    }
    c.bundle = c.bundles[c.next++];
    if (c.bundle == target) {   // playing: as it runs
        std::vector<std::string> problems = session.runningProblems();
        std::vector<const Sequence*> sequences;
        for (const Session::SequenceEntry& e : session.sequences()) sequences.push_back(&e.sequence);
        for (std::string& w : triggerConflicts(session.rackName(), session.transportSettings(), sequences))
            problems.push_back(std::move(w));
        songProblems[c.bundle] = std::move(problems);
        return;
    }
    Rack           rack;
    RackFileResult result;
    if (const auto kept = unsavedRacks.find(c.bundle); kept != unsavedRacks.end()) {
        rack      = kept->second;
        result.ok = true;
    } else {
        result = loadRack(c.bundle, rack);
    }
    if (!result.ok) {
        songProblems[c.bundle] = {"it can't be read: " + result.error};
        return;
    }
    c.found = result.warnings;
    std::vector<const Sequence*> sequences;
    for (const Sequence& seq : rack.sequences) sequences.push_back(&seq);
    for (std::string& w : triggerConflicts(rack.name, rack.transport, sequences)) c.found.push_back(std::move(w));
    for (std::string& w : session.sourceProblems(rack)) c.found.push_back(std::move(w));
    c.rack        = session.prepare(rack, session.loadedSampleRate());
    preparingText = "Checking " + rackNameOf(c.bundle);
}

// From the song a switch waits for, if one does: pressing Next while it's
// prepared goes on from there.
bool RackController::nextSong() {
    if (!setlistOpen) return false;
    const int to = stepFrom(waitingSong.value_or(song), waitingSong || onSong, 1);
    return to >= 0 && goToSong(static_cast<std::size_t>(to));
}

bool RackController::previousSong() {
    if (!setlistOpen) return false;
    const int to = stepFrom(waitingSong.value_or(song), waitingSong || onSong, -1);
    return to >= 0 && goToSong(static_cast<std::size_t>(to));
}

int RackController::stepFrom(std::size_t from, bool atSong, int step) const {
    if (step > 0) return nextPlayable(atSong ? from + 1 : from, 1);
    return from == 0 ? -1 : nextPlayable(from - 1, -1);
}

void RackController::takeSetlistRequests() {
    Engine::SetlistRequest     request;
    std::optional<std::size_t> to;
    while (engine.takeSetlistRequest(request)) {
        if (!setlistOpen) continue;
        const std::size_t from   = to ? *to : waitingSong.value_or(song);
        const bool        atSong = to || waitingSong || onSong;
        int               found  = -1;
        switch (request.action) {
            case SetlistTrigger::Action::Next:     found = stepFrom(from, atSong, 1); break;
            case SetlistTrigger::Action::Previous: found = stepFrom(from, atSong, -1); break;
            case SetlistTrigger::Action::Song:
                if (request.program < setlist.songs.size())
                    found = request.program;
                else
                    notice("Program Change " + std::to_string(request.program + 1) + ": the set has only " +
                           std::to_string(setlist.songs.size()) + " songs");
                break;
        }
        if (found >= 0) to = static_cast<std::size_t>(found);
    }
    if (to && (*to != song || !onSong || waitingSong)) goToSong(*to);
}

void RackController::learnSetlistTrigger(SetlistTrigger::Action action) {
    if (!setlistOpen) return;
    learningId         = kSetlistLearn;
    setlistLearnAction = action;
    engine.armLearn(action == SetlistTrigger::Action::Song);
}

void RackController::setSwitchSettings(const SwitchSettings& settings) {
    if (!setlistOpen || settings == setlist.switching) return;
    setlist.switching = settings;
    writeSetlist();
}

void RackController::removeSetlistTrigger(std::size_t index) {
    if (!setlistOpen || index >= setlist.triggers.size()) return;
    setlist.triggers.erase(setlist.triggers.begin() + static_cast<std::ptrdiff_t>(index));
    writeSetlist();
    applySetlistTriggers();
}

void RackController::applySetlistTriggers() {
    for (const std::string& problem : session.setSetlistTriggers(setlistOpen ? setlist.triggers
                                                                             : std::vector<SetlistTrigger>{}))
        notice("Setlist: a MIDI trigger won't act: " + problem);
}

std::vector<std::string> RackController::triggerConflicts(const std::string& rackName,
                                                          const TransportSettings& transport,
                                                          const std::vector<const Sequence*>& sequences) const {
    std::vector<std::string> warnings;
    if (!setlistOpen) return warnings;
    auto check = [&](const std::string& who, const std::vector<SequenceTrigger>& triggers) {
        for (const SequenceTrigger& t : triggers)
            for (const SetlistTrigger& s : setlist.triggers)
                if (s.sameControl(t)) {
                    const std::string text = setlistTriggerText(s);
                    warnings.push_back(rackName + ": " + who + "'s " + triggerText(t) + " is also the setlist's " +
                                       text.substr(0, text.find(':')) + "; the setlist's wins");
                }
    };
    check("the transport", transport.triggers);
    for (const Sequence* seq : sequences) check("sequence '" + seq->name + "'", seq->triggers);
    return warnings;
}

// The first song from `from` on (step 1) or back (step -1) whose rack is there.
int RackController::nextPlayable(std::size_t from, int step) const {
    for (auto i = static_cast<long>(from); i >= 0 && i < static_cast<long>(setlist.songs.size()); i += step)
        if (!rackMissing(setlist.songs[static_cast<std::size_t>(i)])) return static_cast<int>(i);
    return -1;
}

std::size_t RackController::insertionPoint() const {
    return std::min(onSong ? song + 1 : song, setlist.songs.size());
}

bool RackController::addRackToSetlist(const std::string& chosen) {
    if (!setlistOpen) return false;
    const std::string bundle = rackBundleOf(chosen);
    if (bundle.empty()) {
        notice("Not added: " + chosen + " isn't a rack (no rack.json)");
        return false;
    }
    const std::size_t at = insertionPoint();
    setlist.songs.insert(setlist.songs.begin() + static_cast<std::ptrdiff_t>(at), SetlistSong{bundle, ""});
    if (waitingSong && *waitingSong >= at) ++*waitingSong;
    notice("Added " + rackNameOf(bundle) + " as song " + std::to_string(at + 1));
    return writeSetlist();
}

bool RackController::addNewRackToSetlist(const std::string& chosen) {
    if (!setlistOpen) return false;
    const std::string path = saveAsPath(chosen);
    if (path.empty()) return false;
    if (classifySaveTarget(path, {}) != SaveTarget::New) {
        notice("Not added: " + path + " already exists (Add a rack... adds an existing one)");
        return false;
    }
    // A copy of the song playing (its rows, plugin states, sends, sequences,
    // loopers and settings, edits included: docs/rig.md); with none, empty.
    Rack fresh                  = onSong ? session.capture() : Rack{};
    fresh.name                  = rackNameOf(path);
    const RackFileResult result = saveRack(path, fresh);
    if (!result.ok) {
        notice("Couldn't create the rack: " + result.error);
        return false;
    }
    const std::size_t at = insertionPoint();
    setlist.songs.insert(setlist.songs.begin() + static_cast<std::ptrdiff_t>(at), SetlistSong{path, ""});
    if (!writeSetlist()) return false;
    return goToSong(at);
}

bool RackController::addThisRackAgain() {
    if (!setlistOpen || !onSong) return false;
    const SetlistSong again = setlist.songs[song];
    setlist.songs.insert(setlist.songs.begin() + static_cast<std::ptrdiff_t>(song + 1), again);
    if (waitingSong && *waitingSong > song) ++*waitingSong;
    notice("Added " + rackNameOf(again.rack) + " again as song " + std::to_string(song + 2));
    return writeSetlist();
}

bool RackController::moveSong(std::size_t index, int delta) {
    if (!setlistOpen || index >= setlist.songs.size()) return false;
    const long to = static_cast<long>(index) + delta;
    if (to < 0 || to >= static_cast<long>(setlist.songs.size())) return false;
    const auto other = static_cast<std::size_t>(to);
    std::swap(setlist.songs[index], setlist.songs[other]);
    if (onSong && song == index) song = other;   // the playing song moves with it
    else if (onSong && song == other) song = index;
    if (waitingSong == index) waitingSong = other;   // so does the one a switch waits for
    else if (waitingSong == other) waitingSong = index;
    return writeSetlist();
}

bool RackController::renameSong(std::size_t index, const std::string& name) {
    if (!setlistOpen || index >= setlist.songs.size()) return false;
    std::string text = trimmed(name);
    if (text.size() > kMaxNameBytes) text.resize(kMaxNameBytes);
    if (text == rackNameOf(setlist.songs[index].rack)) text.clear();   // just the rack's name
    setlist.songs[index].name = text;
    return writeSetlist();
}

bool RackController::removeSong(std::size_t index) {
    if (!setlistOpen || index >= setlist.songs.size()) return false;
    setlist.songs.erase(setlist.songs.begin() + static_cast<std::ptrdiff_t>(index));
    if (waitingSong == index) waitingSong.reset();
    else if (waitingSong && *waitingSong > index) --*waitingSong;
    if (onSong && index == song) onSong = false;   // the rack still plays; Next goes to the one that followed
    else if (index < song) --song;
    song = std::min(song, setlist.songs.size());
    rememberPosition();
    return writeSetlist();
}

bool RackController::relinkSong(std::size_t index, const std::string& chosen) {
    if (!setlistOpen || index >= setlist.songs.size()) return false;
    const std::string bundle = rackBundleOf(chosen);
    if (bundle.empty()) {
        notice("Not changed: " + chosen + " isn't a rack (no rack.json)");
        return false;
    }
    setlist.songs[index].rack = bundle;
    notice("Song " + std::to_string(index + 1) + " now plays " + bundle);
    return writeSetlist();
}

bool RackController::writeSetlist() {
    songCache.clear();   // checked again at the next tick
    songBundles.clear();
    unpreparable.clear();
    const RackFileResult result = saveSetlist(setlistFile, setlist);
    if (!result.ok) notice("Couldn't save the setlist: " + result.error);
    return result.ok;
}

void RackController::rememberPosition() {
    if (config.lastPositionPath.empty()) return;
    saveLastPosition(config.lastPositionPath, LastPosition{setlistOpen ? setlistFile : std::string{}, song});
}

void RackController::refreshSongs(double now) {
    if (!setlistOpen) {
        songCache.clear();
        songBundles.clear();
        return;
    }
    if (songCache.size() == setlist.songs.size() && now - missingCheckedAt < 1.0 && now >= missingCheckedAt) return;
    songCache.clear();
    songBundles.clear();
    for (const SetlistSong& s : setlist.songs) {
        SongModel sm;
        sm.rackName = rackNameOf(s.rack);
        sm.name     = s.name.empty() ? sm.rackName : s.name;
        songBundles.push_back(rackBundleOf(s.rack));
        sm.missing = songBundles.back().empty();
        songCache.push_back(std::move(sm));
    }
    missingCheckedAt = now;
}

int RackController::neighbour(int step) const {
    if (step > 0) {
        for (std::size_t i = onSong ? song + 1 : song; i < songCache.size(); ++i)
            if (!songCache[i].missing) return static_cast<int>(i);
    } else {
        for (std::size_t i = std::min(song, songCache.size()); i-- > 0;)
            if (!songCache[i].missing) return static_cast<int>(i);
    }
    return -1;
}

void RackController::buildSetlist() {
    SetlistModel& m = model.setlist;
    m               = {};
    if (!setlistOpen) return;
    m.open     = true;
    m.name     = setlistNameOf(setlistFile);
    m.songs    = songCache;
    for (std::size_t i = 0; i < m.songs.size(); ++i) m.songs[i].prepared = songPrepared(i);
    for (std::size_t i = 0; i < m.songs.size() && i < songBundles.size(); ++i)
        if (const auto it = songProblems.find(songBundles[i]); it != songProblems.end()) m.songs[i].problems = it->second;
    m.checking = check.has_value();
    m.current  = onSong ? static_cast<int>(song) : -1;
    m.next     = neighbour(1);
    m.previous = neighbour(-1);
    for (std::size_t i = 0; i < m.songs.size() && i < songBundles.size(); ++i)
        m.songs[i].unsaved = unsavedRacks.count(songBundles[i]) != 0 ||
                             (!songBundles[i].empty() && songBundles[i] == target && session.modified());
    m.waiting   = waitingSong ? static_cast<int>(*waitingSong) : -1;
    m.switching = setlist.switching;
    const auto& handles = session.setlistTriggerHandles();
    for (std::size_t i = 0; i < setlist.triggers.size(); ++i) {
        std::string text = setlistTriggerText(setlist.triggers[i]);
        if (i < handles.size() && handles[i] == kInvalidMidiSource &&
            setlist.triggers[i].source.kind != MidiSourceRef::Kind::All)
            text += " (not connected)";
        m.triggers.push_back(std::move(text));
    }
    m.learning       = learningId == kSetlistLearn;
    m.learningAction = setlistLearnAction;
}

} // namespace stagehand
