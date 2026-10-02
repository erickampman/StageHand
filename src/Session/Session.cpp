#include "Session/Session.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace stagehand {

namespace {

std::string keyOf(const MidiSourceRef& ref) {
    switch (ref.kind) {
        case MidiSourceRef::Kind::All:     return "all";
        case MidiSourceRef::Kind::Device:  return "device:" + ref.name;
        case MidiSourceRef::Kind::Virtual: return "virtual:" + ref.name;
        case MidiSourceRef::Kind::Sequence: return "sequence:" + ref.name;
        case MidiSourceRef::Kind::None:    return "none";
    }
    return "all";
}

float dbToGain(float db) {
    return std::pow(10.0f, db / 20.0f);
}

// N, if `name` is "Loop N"; else 0.
std::uint32_t loopNumberOf(const std::string& name) {
    if (name.rfind("Loop ", 0) != 0 || name.size() == 5 || name.size() > 14) return 0;
    std::uint32_t n = 0;
    for (std::size_t i = 5; i < name.size(); ++i) {
        if (name[i] < '0' || name[i] > '9') return 0;
        n = n * 10 + static_cast<std::uint32_t>(name[i] - '0');
    }
    return n;
}

} // namespace

Session::Session(PluginFactory& pluginFactory, HostServices& hostServices, MidiInput& midiInput,
                 Engine& runningEngine)
    : factory(pluginFactory), host(hostServices), midi(midiInput), engine(runningEngine) {}

void Session::addOpenSource(const MidiSourceRef& ref, MidiSourceHandle handle) {
    if (ref.kind == MidiSourceRef::Kind::All || ref.kind == MidiSourceRef::Kind::Sequence ||
        ref.kind == MidiSourceRef::Kind::None || handle == kInvalidMidiSource)
        return;
    if (openSources.emplace(keyOf(ref), handle).second) inputs.push_back(OpenInput{ref, handle});
}

bool Session::load(const Rack& rack, double rate, LoadReport& report) {
    return activate(prepare(rack, rate), report);
}

std::unique_ptr<PreparedRack> Session::prepare(const Rack& rack, double rate) {
    auto prepared    = std::make_unique<PreparedRack>();
    prepared->source = rack;
    prepared->rate   = rate;
    prepared->slots.reserve(rack.slots.size());

    // Sequences first. A slot saved with the old "sequence:<id>" source becomes
    // one of that sequence's targets with no live input: it plays as before.
    std::vector<Sequence> sequencesIn = rack.sequences;
    for (const RackSlot& saved : rack.slots)
        if (saved.source.kind == MidiSourceRef::Kind::Sequence)
            for (Sequence& seq : sequencesIn)
                if (seq.id == saved.source.name &&
                    std::find(seq.targets.begin(), seq.targets.end(), saved.id) == seq.targets.end())
                    seq.targets.push_back(saved.id);
    for (const Sequence& seq : sequencesIn)
        prepared->sequences.push_back(
            SequenceEntry{seq, nextSequenceSource++, std::make_shared<SequenceRuntime>(), {}});
    // Loopers come empty: what they record is never saved (docs/looper.md).
    for (const LooperSetup& setup : rack.loopers)
        prepared->loopers.push_back(
            LooperEntry{setup, nextSequenceSource++, std::make_shared<LooperRuntime>(), kInvalidMidiSource, {}});
    return prepared;
}

bool Session::prepareStep(PreparedRack& prepared) {
    if (prepared.ready()) return true;
    const RackSlot& saved = prepared.source.slots[prepared.slots.size()];
    Slot            slot;
    slot.saved = saved;
    if (slot.saved.source.kind == MidiSourceRef::Kind::Sequence)
        slot.saved.source = MidiSourceRef{MidiSourceRef::Kind::None, {}};   // converted by prepare()
    const std::string where = "slot '" + saved.id + "': ";

    std::string                   problem;
    std::shared_ptr<HostedPlugin> plugin = createPlugin(saved.plugin, problem, prepared.scanned, prepared.haveScan);
    // State goes in before activation (domain-model.md, save/restore).
    if (plugin && !saved.state.empty() && !plugin->loadState(saved.state.data(), saved.state.size()))
        prepared.warnings.push_back(where + "the plugin rejected its saved state; it starts from its defaults");
    if (plugin && !plugin->activate(prepared.rate, kMaxPluginBlockFrames)) {
        problem = "the plugin failed to activate";
        plugin.reset();
    }
    if (!plugin) {
        slot.loadProblem = problem;
        prepared.warnings.push_back(where + problem + "; kept as a placeholder (not running)");
    } else {
        slot.plugin            = plugin;
        slot.runtime           = std::make_shared<SlotRuntime>(targetGainOf(saved), saved.muted);
        applySends(slot);
        slot.saved.plugin.path = plugin->descriptor().path;   // where it was actually found
    }
    prepared.slots.push_back(std::move(slot));
    return prepared.ready();
}

bool Session::activate(std::unique_ptr<PreparedRack> prepared, LoadReport& report) {
    return activate(std::move(prepared), report, SwitchOptions{});
}

bool Session::activate(std::unique_ptr<PreparedRack> prepared, LoadReport& report, const SwitchOptions& how) {
    while (!prepareStep(*prepared)) {}
    PreparedRack& p = *prepared;
    for (const std::string& warning : p.warnings) report.warnings.push_back(warning);

    // Sources and triggers resolve now, not when prepared: a device may have
    // come or gone since.
    sequenceList = std::move(p.sequences);
    looperList   = std::move(p.loopers);   // the old ones' notes end with the switch
    loopNumber   = 0;
    for (const LooperEntry& entry : looperList) loopNumber = std::max(loopNumber, loopNumberOf(entry.setup.name));
    currentLooperId.clear();
    engine.setCurrentLooper(kInvalidMidiSource);
    for (LooperEntry& entry : looperList) {
        resolveLooper(entry);
        if (!entry.inputProblem.empty())
            report.warnings.push_back("looper '" + entry.setup.name + "': " + entry.inputProblem +
                                      "; it records nothing from it");
    }
    for (SequenceEntry& entry : sequenceList)
        for (const std::string& problem : resolveTriggers(entry))
            report.warnings.push_back("sequence '" + entry.sequence.id + "': a MIDI trigger won't act: " + problem);
    const Rack& rack = p.source;
    transportSetup   = rack.transport;
    metronomeSetup   = rack.metronome;
    applyMetronome();
    for (const std::string& problem : resolveTransport())
        report.warnings.push_back("transport: a MIDI control won't act: " + problem);
    for (Slot& slot : p.slots) {
        if (!slot.plugin) continue;
        resolveSource(slot);
        if (!slot.sourceProblem.empty())   // no fallback to "all"
            report.warnings.push_back("slot '" + slot.saved.id + "': " + slot.sourceProblem +
                                      "; this slot will hear nothing from it");
    }

    // The rack being replaced rings out (or is cut); one still draining fades out now.
    auto mute = [](SlotRuntime& runtime) {
        runtime.targetGain.store(0.0f, std::memory_order_relaxed);
        runtime.muted.store(true, std::memory_order_relaxed);
    };
    for (SlotConfig& earlier : draining) mute(*earlier.runtime);
    for (const Slot& slot : loaded) {
        if (!slot.plugin) continue;
        SlotConfig config = configOf(slot);
        config.sequenceSources.clear();   // its sequences end with the switch
        config.draining = true;
        if (how.oldRack == OldRack::Cut) mute(*config.runtime);
        draining.push_back(std::move(config));
    }
    loaded     = std::move(p.slots);
    name       = rack.name;
    tempo      = std::clamp(rack.tempoBpm, Engine::kMinTempo, Engine::kMaxTempo);   // the engine's at the switch
    const bool powerOfTwo = rack.timeSigDenominator > 0 && (rack.timeSigDenominator & (rack.timeSigDenominator - 1)) == 0;
    const bool validSig   = rack.timeSigNumerator >= 1 && rack.timeSigNumerator <= 32 && powerOfTwo &&
                          rack.timeSigDenominator <= 32;
    sigNumerator   = validSig ? rack.timeSigNumerator : 4;
    sigDenominator = validSig ? rack.timeSigDenominator : 4;
    sampleRate = p.rate;
    dirty      = false;
    Engine::SwitchPlan plan;
    plan.atNextBar     = how.atNextBar;
    plan.stopTransport = how.stopTransport;
    plan.tempoBpm      = tempo;
    plan.numerator     = sigNumerator;
    plan.denominator   = sigDenominator;
    engine.commitSwitch(buildGraph(), buildSequences(), plan);   // the one commit: the new slots and the rig's buses
    graphDirty = sequencesDirty = false;
    return std::any_of(loaded.begin(), loaded.end(),
                       [](const Slot& s) { return s.plugin != nullptr; });
}

void PreparedRack::idle() {
    for (const Session::Slot& slot : slots)
        if (slot.plugin) slot.plugin->idle();
}

Rack Session::capture() const {
    Rack rack;
    rack.name               = name;
    rack.tempoBpm           = tempo;
    rack.timeSigNumerator   = sigNumerator;
    rack.timeSigDenominator = sigDenominator;
    for (const SequenceEntry& entry : sequenceList) rack.sequences.push_back(entry.sequence);
    for (const LooperEntry& entry : looperList) rack.loopers.push_back(entry.setup);   // the container, not its notes
    rack.transport = transportSetup;
    rack.metronome = metronomeSetup;
    for (const Slot& slot : loaded) {
        RackSlot saved = slot.saved;
        if (slot.plugin) {
            std::vector<std::byte> state;
            if (slot.plugin->saveState(state)) saved.state = std::move(state);
        }
        rack.slots.push_back(std::move(saved));
    }
    return rack;
}

const Session::Slot* Session::slot(const std::string& id) const noexcept {
    for (const Slot& s : loaded)
        if (s.saved.id == id) return &s;
    return nullptr;
}

Session::Slot* Session::find(const std::string& id) noexcept {
    return const_cast<Slot*>(std::as_const(*this).slot(id));
}

// --- live edits --------------------------------------------------------------------

float Session::targetGainOf(const RackSlot& saved) noexcept {
    return saved.muted ? 0.0f : dbToGain(saved.gainDb);
}

bool Session::setGainDb(const std::string& id, float db) {
    Slot* s = find(id);
    if (s == nullptr) return false;
    s->saved.gainDb = db;
    if (s->runtime) s->runtime->targetGain.store(targetGainOf(s->saved), std::memory_order_relaxed);
    dirty = true;
    return true;
}

bool Session::setMuted(const std::string& id, bool muted) {
    Slot* s = find(id);
    if (s == nullptr) return false;
    s->saved.muted = muted;
    if (s->runtime) {
        s->runtime->targetGain.store(targetGainOf(s->saved), std::memory_order_relaxed);
        s->runtime->muted.store(muted, std::memory_order_relaxed);
    }
    dirty = true;
    return true;
}

bool Session::setSendDb(const std::string& id, std::size_t bus, std::optional<float> db) {
    Slot* s = find(id);
    if (s == nullptr || bus >= kRigBuses) return false;
    s->saved.sendDb[bus] = db;
    if (s->runtime) applySends(*s);
    dirty = true;
    return true;
}

void Session::applySends(const Slot& slot) {
    for (std::size_t b = 0; b < kRigBuses; ++b) {
        const std::optional<float>& db = slot.saved.sendDb[b];
        slot.runtime->targetSend[b].store(db ? dbToGain(*db) : 0.0f, std::memory_order_relaxed);
    }
}

// --- the rig (docs/rig.md) -------------------------------------------------------------

std::shared_ptr<HostedPlugin> Session::createEffect(const RigEffect& effect, double rate, std::string& problem) {
    std::vector<PluginDescriptor> scanned;
    bool                          haveScan = false;
    std::shared_ptr<HostedPlugin> plugin   = createPlugin(effect.plugin, problem, scanned, haveScan);
    if (!plugin) return nullptr;
    if (!effect.state.empty() && !plugin->loadState(effect.state.data(), effect.state.size()))
        problem = "it rejected its saved state; it starts from its defaults";   // still usable
    if (!plugin->activate(rate, kMaxPluginBlockFrames)) {
        problem = "plugin '" + effect.plugin.id + "' failed to activate";
        return nullptr;
    }
    return plugin;
}

void Session::loadRig(const Rig& rig, double rate, LoadReport& report) {
    LoadedRig next;
    next.saved = rig;
    for (std::size_t b = 0; b < kRigBuses; ++b) {
        for (std::size_t e = 0; e < RigBus::kEffects; ++e) {
            const std::optional<RigEffect>& effect = rig.buses[b].effects[e];
            if (!effect) continue;
            std::string problem;
            next.effects[b][e].plugin = createEffect(*effect, rate, problem);
            if (!problem.empty()) {
                next.effects[b][e].problem = problem;
                report.warnings.push_back(std::string("rig bus ") + static_cast<char>('A' + b) + ", effect " +
                                          std::to_string(e + 1) + ": " + problem +
                                          (next.effects[b][e].plugin ? "" : "; its slot passes audio on"));
            }
        }
        next.returns[b] = std::make_shared<SlotRuntime>(0.0f);
    }
    rigState  = std::move(next);   // the old effects live on in the old graph until it's collected
    setLooperSwitch(rigState.saved.looperSwitch);   // resolved; sent with the sequences
    rigLoaded = true;
    rigRate   = rate;
    rigDirty  = false;
    for (std::size_t b = 0; b < kRigBuses; ++b) applyReturn(b);
    engine.setMasterGain(dbToGain(rigState.saved.masterDb));
    commitGraph();
}

Rig Session::captureRig() const {
    Rig rig = rigState.saved;
    for (std::size_t b = 0; b < kRigBuses; ++b)
        for (std::size_t e = 0; e < RigBus::kEffects; ++e)
            if (const auto& plugin = rigState.effects[b][e].plugin; plugin && rig.buses[b].effects[e]) {
                std::vector<std::byte> state;
                if (plugin->saveState(state)) rig.buses[b].effects[e]->state = std::move(state);
            }
    return rig;
}

void Session::applyReturn(std::size_t bus) {
    const RigBus& b = rigState.saved.buses[bus];
    if (rigState.returns[bus])
        rigState.returns[bus]->targetGain.store(b.muted ? 0.0f : dbToGain(b.returnDb), std::memory_order_relaxed);
}

bool Session::setReturnDb(std::size_t bus, float db) {
    if (!rigLoaded || bus >= kRigBuses) return false;
    rigState.saved.buses[bus].returnDb = db;
    applyReturn(bus);
    rigDirty = true;
    return true;
}

bool Session::setBusMuted(std::size_t bus, bool muted) {
    if (!rigLoaded || bus >= kRigBuses) return false;
    rigState.saved.buses[bus].muted = muted;
    applyReturn(bus);
    rigDirty = true;
    return true;
}

void Session::setMasterDb(float db) {
    rigState.saved.masterDb = db;
    engine.setMasterGain(dbToGain(db));
    rigDirty = true;
}

bool Session::setBusEffect(std::size_t bus, std::size_t slot, const PluginDescriptor* plugin, std::string& problem) {
    if (!rigLoaded || bus >= kRigBuses || slot >= RigBus::kEffects) return false;
    LoadedRig::Effect        loaded;
    std::optional<RigEffect> saved;
    if (plugin != nullptr) {
        RigEffect effect;
        effect.plugin = *plugin;
        loaded.plugin = createEffect(effect, rigRate > 0.0 ? rigRate : sampleRate, problem);
        if (!loaded.plugin) return false;
        problem.clear();
        saved = std::move(effect);
    }
    rigState.effects[bus][slot]             = std::move(loaded);   // the old one lives on until its graph is collected
    rigState.saved.buses[bus].effects[slot] = std::move(saved);
    graphDirty = rigDirty = true;
    return true;
}

bool Session::setBusOutput(std::size_t bus, std::uint32_t firstOutput) {
    if (!rigLoaded || bus >= kRigBuses) return false;
    rigState.saved.buses[bus].firstOutput = firstOutput;
    graphDirty = rigDirty = true;
    return true;
}

void Session::setTempo(double bpm) {
    tempo = std::clamp(bpm, Engine::kMinTempo, Engine::kMaxTempo);
    engine.setTempo(tempo);
    dirty = true;
}

bool Session::setTimeSignature(int numerator, int denominator) {
    const bool powerOfTwo = denominator > 0 && (denominator & (denominator - 1)) == 0;
    if (numerator < 1 || numerator > 32 || !powerOfTwo || denominator > 32) return false;
    sigNumerator   = numerator;
    sigDenominator = denominator;
    engine.setTimeSignature(numerator, denominator);
    dirty = true;
    return true;
}

// --- structural edits --------------------------------------------------------------

bool Session::setFilter(const std::string& id, const MidiFilter& filter) {
    Slot* s = find(id);
    if (s == nullptr) return false;
    s->saved.filter = filter;
    graphDirty = dirty = true;
    return true;
}

bool Session::setFirstOutput(const std::string& id, std::uint32_t firstOutput) {
    Slot* s = find(id);
    if (s == nullptr) return false;
    s->saved.firstOutput = firstOutput;
    graphDirty = dirty = true;
    return true;
}

bool Session::setSource(const std::string& id, const MidiSourceRef& source, std::string& problem) {
    Slot* s = find(id);
    if (s == nullptr) return false;
    s->saved.source = source;
    resolveSource(*s);
    problem    = s->sourceProblem;
    graphDirty = dirty = true;
    return true;
}

std::string Session::addSlot(const PluginDescriptor& plugin, std::size_t index, std::string& problem) {
    if (sampleRate <= 0.0) {
        problem = "no rack is loaded (load one, even an empty one, first)";
        return {};
    }
    std::unique_ptr<HostedPlugin> created = factory.create(plugin, host);
    if (!created) {
        problem = "plugin '" + plugin.id + "' failed to load";
        return {};
    }
    if (!created->activate(sampleRate, kMaxPluginBlockFrames)) {
        problem = "plugin '" + plugin.id + "' failed to activate";
        return {};
    }

    Slot slot;
    slot.saved.name   = plugin.name.empty() ? plugin.id : plugin.name;
    slot.saved.plugin = plugin;
    const std::string base = slotIdFrom(slot.saved.name);
    slot.saved.id          = base;
    for (int n = 2; find(slot.saved.id) != nullptr; ++n) slot.saved.id = base + "-" + std::to_string(n);
    slot.plugin  = std::move(created);
    slot.runtime = std::make_shared<SlotRuntime>(targetGainOf(slot.saved), slot.saved.muted);
    applySends(slot);
    resolveSource(slot);   // "all": nothing to open

    const std::string id = slot.saved.id;
    loaded.insert(loaded.begin() + static_cast<std::ptrdiff_t>(std::min(index, loaded.size())), std::move(slot));
    graphDirty = dirty = true;
    return id;
}

bool Session::removeSlot(const std::string& id) {
    const auto it = std::find_if(loaded.begin(), loaded.end(), [&](const Slot& s) { return s.saved.id == id; });
    if (it == loaded.end()) return false;
    loaded.erase(it);   // the running graph still holds the plugin until flush() replaces it
    for (SequenceEntry& entry : sequenceList) {   // no sequence plays into it any more
        auto& targets = entry.sequence.targets;
        targets.erase(std::remove(targets.begin(), targets.end(), id), targets.end());
    }
    for (LooperEntry& entry : looperList) {   // nor any looper
        auto& targets = entry.setup.targets;
        targets.erase(std::remove(targets.begin(), targets.end(), id), targets.end());
    }
    graphDirty = dirty = true;
    return true;
}

void Session::flush() {
    const auto done = std::remove_if(draining.begin(), draining.end(), [](const SlotConfig& c) {
        return c.runtime->drained.load(std::memory_order_relaxed);
    });
    if (done != draining.end()) {
        draining.erase(done, draining.end());   // the plugin goes when the graph that held it is collected
        graphDirty = true;
    }
    if (sequencesDirty) commitSequences();
    if (graphDirty) commitGraph();
}

void Session::unload() {
    engine.switchNow();   // a switch waiting for its bar line can't hold this up
    engine.commitSequences(nullptr);   // releases any sounding sequence notes
    engine.commit(nullptr);
    draining.clear();
    graphDirty = sequencesDirty = false;
}

// --- sequences ---------------------------------------------------------------------

const Session::SequenceEntry* Session::sequence(const std::string& id) const noexcept {
    for (const SequenceEntry& entry : sequenceList)
        if (entry.sequence.id == id) return &entry;
    return nullptr;
}

Session::SequenceEntry* Session::findSequence(const std::string& id) noexcept {
    return const_cast<SequenceEntry*>(std::as_const(*this).sequence(id));
}

std::string Session::addSequence(MidiProtocol protocol) {
    Sequence seq;
    seq.id = "seq";
    for (int n = 2; findSequence(seq.id) != nullptr; ++n) seq.id = "seq-" + std::to_string(n);
    seq.name     = "Sequence " + std::to_string(sequenceList.size() + 1);
    seq.protocol = protocol;
    sequenceList.push_back(SequenceEntry{seq, nextSequenceSource++, std::make_shared<SequenceRuntime>(), {}});
    sequencesDirty = dirty = true;
    return seq.id;
}

bool Session::setSequence(const Sequence& updated, std::string& problem) {
    SequenceEntry* entry = findSequence(updated.id);
    if (entry == nullptr) {
        problem = "there's no sequence '" + updated.id + "'";
        return false;
    }
    if (updated.protocol != entry->sequence.protocol && !entry->sequence.notes.empty()) {
        problem = "a sequence's MIDI protocol can change only while it has no notes";
        return false;
    }
    const bool newTriggers = updated.triggers != entry->sequence.triggers;
    const bool newTargets  = updated.targets != entry->sequence.targets;
    entry->sequence        = updated;
    if (newTriggers) resolveTriggers(*entry);
    if (newTargets) {   // only rows that exist; each once
        std::vector<std::string> kept;
        for (const std::string& id : entry->sequence.targets)
            if (find(id) != nullptr && std::find(kept.begin(), kept.end(), id) == kept.end()) kept.push_back(id);
        entry->sequence.targets = std::move(kept);
        graphDirty              = true;   // which rows hear it is part of the graph
    }
    sequencesDirty = dirty = true;
    return true;
}

bool Session::removeSequence(const std::string& id) {
    const auto it = std::find_if(sequenceList.begin(), sequenceList.end(),
                                 [&](const SequenceEntry& e) { return e.sequence.id == id; });
    if (it == sequenceList.end()) return false;
    sequenceList.erase(it);   // the engine releases its notes when it adopts the new set
    graphDirty = sequencesDirty = dirty = true;   // its rows stop hearing it
    return true;
}

bool Session::startSequence(const std::string& id) {
    const SequenceEntry* entry = sequence(id);
    if (entry == nullptr) return false;
    flush();   // a sequence added or edited this tick must be in the engine first
    return engine.sendSequenceCommand(entry->source, SequenceCommand::Start);
}

bool Session::stopSequence(const std::string& id) {
    const SequenceEntry* entry = sequence(id);
    return entry != nullptr && engine.sendSequenceCommand(entry->source, SequenceCommand::Stop);
}

bool Session::endAuditions(const std::string& id, bool allNotesOff) {
    const SequenceEntry* entry = sequence(id);
    if (entry == nullptr) return false;
    if (!allNotesOff) return engine.endAuditions(entry->source);
    const Sequence&     seq  = entry->sequence;
    const std::uint32_t head = (std::uint32_t(seq.group & 0x0F) << 24) | ((0xB0u | (seq.channel & 0x0F)) << 16) |
                               (123u << 8);   // controller 123, value 0
    MidiMessage cc;
    if (seq.protocol == MidiProtocol::Midi1) {
        cc.words[0]  = 0x20000000u | head;
        cc.wordCount = 1;
    } else {
        cc.words[0]  = 0x40000000u | head;
        cc.words[1]  = 0;
        cc.wordCount = 2;
    }
    return engine.endAuditions(entry->source, &cc);
}

bool Session::auditionNote(const std::string& id, const SequenceNote& note, bool hold) {
    const SequenceEntry* entry = sequence(id);
    if (entry == nullptr) return false;
    const Sequence&        seq          = entry->sequence;
    const double           unitsPerBeat = seq.rate.unitsPerBeat() > 0.0 ? seq.rate.unitsPerBeat() : 4.0;
    const double           seconds      = note.length / unitsPerBeat * 60.0 / std::max(1.0, tempo);
    const AuditionMessages m            = auditionMessages(seq, note);
    return engine.auditionNote(entry->source, m.on, m.off,
                               hold ? kHeldAudition : std::clamp(seconds, kMinAudition, kMaxAudition));
}

void Session::setTransportSettings(const TransportSettings& settings) {
    transportSetup = settings;
    resolveTransport();
    sequencesDirty = dirty = true;
}

void Session::setMetronome(const MetronomeSettings& settings) {
    metronomeSetup = settings;
    applyMetronome();
    dirty = true;
}

void Session::applyMetronome() {
    engine.setClick(metronomeSetup.click, metronomeSetup.everyNote, dbToGain(metronomeSetup.clickLevelDb));
}

void Session::startTransport() {
    flush();   // sequences added or edited this tick must be in the engine first
    engine.sendTransportCommand(Engine::TransportCommand::Start);
}

void Session::stopTransport() {
    engine.sendTransportCommand(Engine::TransportCommand::Stop);
}

bool Session::transportRunning() const noexcept {
    return engine.transport().running;
}

void Session::stopAllSequences() {
    engine.sendSequenceCommand(kInvalidMidiSource, SequenceCommand::StopNow);
}

SequenceState Session::sequenceState(const std::string& id) const noexcept {
    const SequenceEntry* entry = sequence(id);
    return entry != nullptr ? entry->runtime->state.load(std::memory_order_relaxed) : SequenceState::Stopped;
}

double Session::sequencePosition(const std::string& id) const noexcept {
    const SequenceEntry* entry = sequence(id);
    return entry != nullptr ? entry->runtime->position.load(std::memory_order_relaxed) : 0.0;
}

std::unique_ptr<SequenceSet> Session::buildSequences() const {
    std::vector<CompiledSequence> compiled;
    compiled.reserve(sequenceList.size());
    // A trigger whose input didn't resolve hears nothing.
    auto compileTriggers = [](const std::vector<SequenceTrigger>& triggers, const std::vector<MidiSourceHandle>& handles,
                              std::vector<CompiledSequence::Trigger>& out) {
        for (std::size_t i = 0; i < triggers.size() && i < handles.size(); ++i) {
            const SequenceTrigger& t = triggers[i];
            if (handles[i] == kInvalidMidiSource && t.source.kind != MidiSourceRef::Kind::All) continue;
            out.push_back(CompiledSequence::Trigger{handles[i], t.action, t.control, t.group, t.channel, t.number});
        }
    };
    for (const SequenceEntry& entry : sequenceList) {
        CompiledSequence c = CompiledSequence::compile(entry.sequence, entry.source, entry.runtime);
        compileTriggers(entry.sequence.triggers, entry.triggerSources, c.triggers);
        compiled.push_back(std::move(c));
    }
    TransportControl transport;
    compileTriggers(transportSetup.triggers, transportTriggerSources, transport.triggers);
    if (const auto& t = rigState.saved.looperSwitch;
        rigLoaded && t && (looperSwitchSource != kInvalidMidiSource || t->source.kind == MidiSourceRef::Kind::All)) {
        transport.hasLooperSwitch = true;
        transport.looperSwitch    = CompiledSequence::Trigger{looperSwitchSource, t->action, t->control, t->group,
                                                           t->channel, t->number};
    }
    for (std::size_t i = 0; i < setlistControls.size() && i < setlistSources.size(); ++i) {
        const SetlistTrigger& t = setlistControls[i];
        if (setlistSources[i] == kInvalidMidiSource && t.source.kind != MidiSourceRef::Kind::All) continue;
        transport.setlist.push_back(CompiledSetlistTrigger{setlistSources[i], t.action, t.control, t.group, t.channel,
                                                           t.number});
    }
    // An input that isn't connected: start on first note waits for nothing.
    transport.startOnNote       = transportSetup.startOnNote && startOnNoteProblem.empty();
    transport.startOnNoteSource = startOnNoteHandle;
    std::vector<CompiledLooper> loopers;
    for (const LooperEntry& entry : looperList) {
        CompiledLooper l;
        l.source      = entry.source;
        l.runtime     = entry.runtime;
        // An input that isn't connected: a handle no live input has, so it records nothing.
        l.input       = entry.setup.source.kind == MidiSourceRef::Kind::All ? kInvalidMidiSource
                        : entry.input != kInvalidMidiSource                 ? entry.input
                                                                            : kSequenceSourceBase;
        l.channelMask = entry.setup.channelMask;
        l.quantize    = entry.setup.quantize;
        l.atBar       = entry.setup.atBar;
        l.fixedLength = entry.setup.fixedLength;
        l.fixedInBars = entry.setup.fixedInBars;
        loopers.push_back(std::move(l));
    }
    return std::make_unique<SequenceSet>(std::move(compiled), std::move(transport), std::move(loopers));
}

void Session::commitSequences() {
    engine.commitSequences(buildSequences());
    sequencesDirty = false;
}

std::vector<std::string> Session::setSetlistTriggers(const std::vector<SetlistTrigger>& triggers) {
    std::vector<std::string> problems;
    setlistControls = triggers;
    setlistSources.clear();
    for (const SetlistTrigger& t : triggers) {
        MidiSourceHandle handle = kInvalidMidiSource;   // All: any input
        if (t.source.kind != MidiSourceRef::Kind::All) {
            std::string problem;
            handle = resolve(t.source, problem);
            if (handle == kInvalidMidiSource) problems.push_back(problem);
        }
        setlistSources.push_back(handle);
    }
    sequencesDirty = true;
    return problems;
}

// --- loopers (docs/looper.md) ----------------------------------------------------------

const Session::LooperEntry* Session::looper(const std::string& id) const noexcept {
    for (const LooperEntry& entry : looperList)
        if (entry.setup.id == id) return &entry;
    return nullptr;
}

Session::LooperEntry* Session::findLooper(const std::string& id) noexcept {
    return const_cast<LooperEntry*>(std::as_const(*this).looper(id));
}

void Session::resolveLooper(LooperEntry& entry) {
    entry.input = kInvalidMidiSource;
    entry.inputProblem.clear();
    if (entry.setup.source.kind != MidiSourceRef::Kind::All) entry.input = resolve(entry.setup.source, entry.inputProblem);
}

std::string Session::addLooper(LooperSetup setup) {
    if (setup.name.empty()) {
        for (const LooperEntry& entry : looperList)   // one renamed "Loop 7" since counts too
            loopNumber = std::max(loopNumber, loopNumberOf(entry.setup.name));
        setup.name = "Loop " + std::to_string(++loopNumber);
    }
    const std::string base = slotIdFrom(setup.name);
    setup.id               = base;
    for (int n = 2; findLooper(setup.id) != nullptr; ++n) setup.id = base + "-" + std::to_string(n);
    std::erase_if(setup.targets, [this](const std::string& row) { return find(row) == nullptr; });
    LooperEntry entry{std::move(setup), nextSequenceSource++, std::make_shared<LooperRuntime>(), kInvalidMidiSource, {}};
    resolveLooper(entry);
    looperList.push_back(std::move(entry));
    graphDirty = sequencesDirty = dirty = true;
    return looperList.back().setup.id;
}

bool Session::setLooper(const LooperSetup& setup) {
    LooperEntry* entry = findLooper(setup.id);
    if (entry == nullptr) return false;
    const bool newInput   = setup.source != entry->setup.source;
    entry->setup          = setup;
    std::erase_if(entry->setup.targets, [this](const std::string& row) { return find(row) == nullptr; });
    if (newInput) resolveLooper(*entry);
    graphDirty = sequencesDirty = dirty = true;   // targets are part of the graph
    return true;
}

bool Session::removeLooper(const std::string& id) {
    const auto it = std::find_if(looperList.begin(), looperList.end(),
                                 [&](const LooperEntry& e) { return e.setup.id == id; });
    if (it == looperList.end()) return false;
    if (id == currentLooperId) setCurrentLooper({});
    looperList.erase(it);   // the engine ends its notes when it adopts the new set
    graphDirty = sequencesDirty = dirty = true;
    return true;
}

bool Session::looperCommand(const std::string& id, LooperCommand command) {
    const LooperEntry* entry = looper(id);
    if (entry == nullptr) return false;
    flush();   // a looper just added must be in the engine first
    return engine.sendLooperCommand(entry->source, command);
}

void Session::setCurrentLooper(const std::string& id) {
    const LooperEntry* entry = looper(id);
    currentLooperId          = entry != nullptr ? id : std::string{};
    engine.setCurrentLooper(entry != nullptr ? entry->source : kInvalidMidiSource);
}

void Session::setLooperSwitch(const std::optional<SequenceTrigger>& trigger) {
    if (trigger != rigState.saved.looperSwitch) rigDirty = true;
    rigState.saved.looperSwitch = trigger;
    looperSwitchSource          = kInvalidMidiSource;
    looperSwitchIssue.clear();
    if (trigger && trigger->source.kind != MidiSourceRef::Kind::All)
        looperSwitchSource = resolve(trigger->source, looperSwitchIssue);
    sequencesDirty = true;
}

// --- helpers -----------------------------------------------------------------------

SlotConfig Session::configOf(const Slot& slot) const {
    SlotConfig config;
    config.plugin                = slot.plugin;
    config.runtime               = slot.runtime;
    config.midiFilter            = slot.saved.filter;
    config.midiFilter.allSources = slot.saved.source.kind == MidiSourceRef::Kind::All;
    config.midiFilter.source     = slot.source;   // invalid when unresolved or None: hears no live input
    for (const SequenceEntry& entry : sequenceList)   // the sequences that play into it
        if (std::find(entry.sequence.targets.begin(), entry.sequence.targets.end(), slot.saved.id) !=
            entry.sequence.targets.end())
            config.sequenceSources.push_back(entry.source);
    for (const LooperEntry& entry : looperList)   // and the loopers
        if (std::find(entry.setup.targets.begin(), entry.setup.targets.end(), slot.saved.id) != entry.setup.targets.end())
            config.sequenceSources.push_back(entry.source);
    config.notePort              = slot.saved.notePort;
    config.firstOutput           = slot.saved.firstOutput;
    return config;
}

std::unique_ptr<EngineGraph> Session::buildGraph() const {
    std::vector<SlotConfig> configs;
    for (const Slot& slot : loaded)
        if (slot.plugin) configs.push_back(configOf(slot));
    configs.insert(configs.end(), draining.begin(), draining.end());   // ringing out after them
    std::vector<BusConfig> buses;
    if (rigLoaded)
        for (std::size_t b = 0; b < kRigBuses; ++b) {
            BusConfig bus;
            for (std::size_t e = 0; e < RigBus::kEffects; ++e) bus.effects[e] = rigState.effects[b][e].plugin;
            bus.runtime     = rigState.returns[b];
            bus.firstOutput = rigState.saved.buses[b].firstOutput;
            buses.push_back(std::move(bus));
        }
    return std::make_unique<EngineGraph>(std::move(configs), std::move(buses));
}

void Session::commitGraph() {
    engine.commit(buildGraph());
    graphDirty = false;
}

void Session::resolveSource(Slot& slot) {
    slot.source = kInvalidMidiSource;
    slot.sourceProblem.clear();
    if (slot.saved.source.kind == MidiSourceRef::Kind::Sequence) {   // load() converts these; never kept
        slot.saved.source = MidiSourceRef{MidiSourceRef::Kind::None, {}};
        return;
    }
    if (slot.saved.source.kind != MidiSourceRef::Kind::All && slot.saved.source.kind != MidiSourceRef::Kind::None)
        slot.source = resolve(slot.saved.source, slot.sourceProblem);
}

std::shared_ptr<HostedPlugin> Session::createPlugin(const PluginDescriptor& wanted, std::string& problem,
                                                    std::vector<PluginDescriptor>& scanned, bool& haveScan) {
    if (!wanted.path.empty())
        if (std::unique_ptr<HostedPlugin> plugin = factory.create(wanted, host)) return plugin;

    if (!haveScan) {
        scanned  = factory.scan();
        haveScan = true;
    }
    for (const PluginDescriptor& found : scanned)
        if (found.format == wanted.format && found.id == wanted.id)
            if (std::unique_ptr<HostedPlugin> plugin = factory.create(found, host)) return plugin;

    problem = "plugin '" + wanted.id + "' isn't installed or failed to load";
    return nullptr;
}

std::vector<std::string> Session::resolveTriggers(SequenceEntry& entry) {
    return resolveTriggerList(entry.sequence.triggers, entry.triggerSources);
}

std::vector<std::string> Session::resolveTriggerList(const std::vector<SequenceTrigger>& triggers,
                                                     std::vector<MidiSourceHandle>&      handles) {
    std::vector<std::string> problems;
    handles.clear();
    for (const SequenceTrigger& t : triggers) {
        MidiSourceHandle handle = kInvalidMidiSource;   // All: any input
        if (t.source.kind == MidiSourceRef::Kind::Sequence) {
            problems.push_back("a sequence can't trigger one");
        } else if (t.source.kind != MidiSourceRef::Kind::All) {
            std::string problem;
            handle = resolve(t.source, problem);
            if (handle == kInvalidMidiSource) problems.push_back(problem);
        }
        handles.push_back(handle);
    }
    return problems;
}

// The transport's triggers and start-on-note input, resolved like a sequence's.
std::vector<std::string> Session::resolveTransport() {
    std::vector<std::string> problems = resolveTriggerList(transportSetup.triggers, transportTriggerSources);
    startOnNoteHandle                 = kInvalidMidiSource;   // any input
    startOnNoteProblem.clear();
    const MidiSourceRef& from = transportSetup.startOnNoteSource;
    if (transportSetup.startOnNote && from.kind != MidiSourceRef::Kind::All) {
        startOnNoteHandle = resolve(from, startOnNoteProblem);
        if (startOnNoteHandle == kInvalidMidiSource) problems.push_back("start on first note: " + startOnNoteProblem);
    }
    return problems;
}

MidiSourceHandle Session::resolve(const MidiSourceRef& ref, std::string& problem) {
    if (ref.kind == MidiSourceRef::Kind::Sequence) {
        if (const SequenceEntry* entry = sequence(ref.name)) return entry->source;
        problem = "there's no sequence '" + ref.name + "'";
        return kInvalidMidiSource;
    }
    const std::string key = keyOf(ref);
    if (const auto it = openSources.find(key); it != openSources.end()) return it->second;
    if (ref.kind == MidiSourceRef::Kind::Device) {
        if (const MidiSourceHandle handle = midi.open(ref.name); handle != kInvalidMidiSource) {
            addOpenSource(ref, handle);
            return handle;
        }
        problem = "MIDI device '" + ref.name + "' isn't connected";
    } else {
        problem = "there's no virtual input '" + ref.name + "'";
    }
    return kInvalidMidiSource;
}

} // namespace stagehand
