#include "App/RackController.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

#include "Core/Ump.h"
#include "Session/RackFile.h"

namespace stagehand {

namespace {

// learningId while learning the transport's trigger: no sequence id can look like it.
const std::string kTransportLearn = "\x01transport";

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
    target = config.saveTarget;
    routeClick(true);
    refreshSources();
    tick(0.0);
}

RackController::~RackController() {
    editors.clear();
}

// --- periodic ----------------------------------------------------------------------

void RackController::service() {
    engine.collect();
    for (const Session::Slot& slot : session.slots())
        if (slot.plugin) slot.plugin->idle();
}

void RackController::tick(double now) {
    if (Engine::Learned learned; !learningId.empty() && engine.takeLearned(learned)) finishLearn(learned);
    session.flush();

    for (auto it = editors.begin(); it != editors.end();)
        it = it->second->isOpen() ? std::next(it) : editors.erase(it);

    const double dt = lastTick < 0.0 ? 0.0 : std::max(0.0, now - lastTick);
    lastTick        = now;

    model.rackName = session.rackName();
    model.modified = session.modified();

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
    transport.clickOutput     = metronome.clickOutput;
    transport.clickConnected  = !clickDevice.empty();
    transport.playbackDevices = deviceNames;
    model.rows.clear();
    for (const Session::Slot& slot : session.slots()) model.rows.push_back(rowFor(slot, now, dt));
    // Forget rows that are gone.
    for (auto it = rows.begin(); it != rows.end();)
        it = session.slot(it->first) != nullptr ? std::next(it) : rows.erase(it);

    buildDock(now);

    StatusModel& status = model.status;
    if (config.audioStatus) {
        const AudioStatus audio = config.audioStatus();
        status.sampleRate       = audio.sampleRate;
        status.bufferSize       = audio.bufferSize;
        status.xruns            = audio.xruns;
        status.outputs          = audio.outputs;
    }
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
        row.state      = saved.enabled ? RowState::Running : RowState::Disabled;
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

    if (slot.runtime) {
        const LevelMeter::Reading reading = slot.runtime->meter.take();
        cache.meter.update(reading, now, dt);
        row.clipped = reading.clipped;

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
    for (const Session::OpenInput& input : session.openInputs()) {
        InputChipModel chip;
        chip.label     = sourceLabel(input.ref);
        chip.isVirtual = input.ref.kind == MidiSourceRef::Kind::Virtual;
        chip.protocol  = sourceProtocolOf(input.ref);
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
        for (const Session::Slot& slot : session.slots())
            if (slot.saved.source.kind == MidiSourceRef::Kind::Sequence && slot.saved.source.name == seq.id)
                row.usedBy.push_back(slot.saved.name);
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
}

std::vector<std::string> RackController::takeNotices() {
    return std::exchange(notices, {});
}

// --- sources -----------------------------------------------------------------------

void RackController::refreshSources() {
    enumerated = midi.enumerate();

    model.sources.clear();
    model.sources.push_back(SourceChoice{"All inputs", MidiSourceRef{}, SourceProtocol::Mixed, false});
    for (const auto& [name, protocol] : config.virtualInputs)
        model.sources.push_back(SourceChoice{name, MidiSourceRef{MidiSourceRef::Kind::Virtual, name},
                                             sourceProtocolOf(MidiSourceRef{MidiSourceRef::Kind::Virtual, name}),
                                             false});
    // Hardware first: it's what a player picks.
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
    for (const Session::SequenceEntry& entry : session.sequences()) {   // last: the rack's own sources
        const MidiSourceRef ref{MidiSourceRef::Kind::Sequence, entry.sequence.id};
        model.sources.push_back(SourceChoice{"Sequence: " + entry.sequence.name, ref, sourceProtocolOf(ref), false});
    }
}

std::string RackController::sourceLabel(const MidiSourceRef& ref) const {
    switch (ref.kind) {
        case MidiSourceRef::Kind::All:     return "All inputs";
        case MidiSourceRef::Kind::Virtual: return ref.name;
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

void RackController::setEnabled(const std::string& id, bool enabled) {
    session.setEnabled(id, enabled);
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

void RackController::clearOutputClip() {
    engine.outputMeter().clearClip();
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

std::string RackController::addSequence(MidiProtocol protocol) {
    const std::string id = session.addSequence(protocol);
    refreshSources();   // it's a source now
    return id;
}

void RackController::removeSequence(const std::string& id) {
    if (id == learningId) cancelLearn();
    if (session.removeSequence(id)) refreshSources();
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

void RackController::setSequenceProtocol(const std::string& id, MidiProtocol protocol) {
    editSequence(id, [protocol](Sequence& seq) { seq.protocol = protocol; });   // the Session refuses it with notes
    refreshSources();                                                           // the menu shows protocols
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

void RackController::setClick(bool on) {
    MetronomeSettings m = session.metronome();
    m.click             = on;
    if (on && m.clickOutput.empty()) notice("Click: choose its output in the same menu (not the main outputs' device)");
    setMetronome(m);
}

void RackController::setClickLevel(float db) {
    MetronomeSettings m = session.metronome();
    m.clickLevelDb      = std::clamp(db, -60.0f, 0.0f);
    setMetronome(m);
}

void RackController::setClickOutput(const std::string& device) {
    MetronomeSettings m = session.metronome();
    m.clickOutput       = device;
    clickMissing        = false;   // a new choice: report afresh
    setMetronome(m);
}

void RackController::refreshPlaybackDevices() {
    deviceNames.clear();
    if (config.clickRouting == nullptr) return;
    for (const PlaybackDevice& d : config.clickRouting->playbackDevices()) deviceNames.push_back(d.name);
    model.transport.playbackDevices = deviceNames;
}

// Never the main outputs by default: a click the audience hears is worse than
// none. A missing device leaves it unconnected, and it connects when it appears.
void RackController::routeClick(bool report) {
    AuxRouting* const        routing = config.clickRouting;
    const MetronomeSettings& m       = session.metronome();
    if (routing == nullptr) return;
    if (!m.click || m.clickOutput.empty()) {
        if (!clickDevice.empty()) routing->disconnectAux();
        clickDevice.clear();
        clickMissing = false;
        return;
    }
    const std::vector<PlaybackDevice> devices = routing->playbackDevices();
    const int                         i       = findPlaybackDevice(m.clickOutput, devices);
    if (i < 0) {
        if (!clickDevice.empty()) routing->disconnectAux();
        clickDevice.clear();
        if (report && !clickMissing)
            notice("Click: its output '" + m.clickOutput + "' isn't connected; the click is silent until it is");
        clickMissing = true;
        return;
    }
    const PlaybackDevice& device = devices[static_cast<std::size_t>(i)];
    if (device.name == clickDevice) return;
    if (!routing->connectAux(device)) {
        notice("Click: couldn't connect to " + device.name);
        return;
    }
    if (clickMissing || device.name != m.clickOutput) notice("Click: playing on " + device.name);
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
    const Session::SequenceEntry* entry = transport ? nullptr : session.sequence(id);
    if (!transport && entry == nullptr) return;   // removed meanwhile
    const std::string who = transport ? std::string("Transport") : entry->sequence.name;
    const auto input = std::find_if(session.openInputs().begin(), session.openInputs().end(),
                                    [&](const Session::OpenInput& in) { return in.handle == learned.source; });
    if (input == session.openInputs().end()) {
        notice(who + ": couldn't tell which input that came from; try again");
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
    if (config.savingOff || target.empty()) {
        notice(config.savingOff ? "Not saved: saving is off (--no-save-rack)" : "Not saved: this rack is untitled; use Save As");
        return false;
    }
    const RackFileResult result = saveRack(target, session.capture());
    if (!result.ok) {
        notice("Couldn't save the rack: " + result.error);
        return false;
    }
    session.markSaved();
    notice("Saved the rack to " + target);
    return true;
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
    session.markSaved();
    notice("Saved the rack to " + target);
    return SaveAsResult::Saved;
}

bool RackController::saveForQuit() {
    return config.savingOff || target.empty() || save();
}

bool RackController::unsavedUntitled() const {
    return target.empty() && !config.savingOff && session.modified();
}

bool RackController::newRack() {
    Rack rack;
    rack.name = "Untitled";
    return replaceRack(rack, {}, {});
}

bool RackController::openRack(const std::string& chosen) {
    const std::string bundle = rackBundleOf(chosen);
    if (bundle.empty()) {
        notice("Couldn't open " + chosen + ": it isn't a rack (no rack.json)");
        return false;
    }
    Rack                 rack;
    const RackFileResult result = loadRack(bundle, rack);
    if (!result.ok) {
        notice("Couldn't open " + bundle + ": " + result.error);
        return false;
    }
    return replaceRack(rack, bundle, result.warnings);
}

// Switch racks: save the old one as quitting would, close what belongs to it,
// and load the new one at the running sample rate.
bool RackController::replaceRack(const Rack& rack, const std::string& path, const std::vector<std::string>& warnings) {
    if (!saveForQuit()) return false;   // the notice says why
    cancelLearn();
    closeAllEditors();
    rows.clear();
    taps.clear();

    session.stopTransport();   // a new song starts from bar 1
    Session::LoadReport report;
    session.load(rack, session.loadedSampleRate(), report);
    target = path;
    routeClick(true);
    ++generation;
    for (const std::string& warning : warnings) notice(warning);
    for (const std::string& warning : report.warnings) notice(warning);
    notice(path.empty() ? "New rack" : "Opened " + path);
    refreshSources();
    tick(std::max(lastTick, 0.0));
    return true;
}

void RackController::quit() {
    if (config.quit) config.quit();
}

} // namespace stagehand
