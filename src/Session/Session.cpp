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
    }
    return "all";
}

float dbToGain(float db) {
    return std::pow(10.0f, db / 20.0f);
}

} // namespace

Session::Session(PluginFactory& pluginFactory, HostServices& hostServices, MidiInput& midiInput,
                 Engine& runningEngine)
    : factory(pluginFactory), host(hostServices), midi(midiInput), engine(runningEngine) {}

void Session::addOpenSource(const MidiSourceRef& ref, MidiSourceHandle handle) {
    if (ref.kind != MidiSourceRef::Kind::All && handle != kInvalidMidiSource) openSources[keyOf(ref)] = handle;
}

bool Session::load(const Rack& rack, double rate, LoadReport& report) {
    std::vector<Slot>             next;
    std::vector<PluginDescriptor> scanned;
    bool                          haveScan = false;

    // Sequences first: slots resolve "sequence:<id>" sources against them.
    sequenceList.clear();
    for (const Sequence& seq : rack.sequences)
        sequenceList.push_back(SequenceEntry{seq, nextSequenceSource++, std::make_shared<SequenceRuntime>()});

    for (const RackSlot& saved : rack.slots) {
        Slot slot;
        slot.saved = saved;
        const std::string where = "slot '" + saved.id + "': ";

        std::string                   problem;
        std::shared_ptr<HostedPlugin> plugin = createPlugin(saved, problem, scanned, haveScan);
        // State goes in before activation (domain-model.md, save/restore).
        if (plugin && !saved.state.empty() && !plugin->loadState(saved.state.data(), saved.state.size()))
            report.warnings.push_back(where + "the plugin rejected its saved state; it starts from its defaults");
        if (plugin && !plugin->activate(rate, kMaxPluginBlockFrames)) {
            problem = "the plugin failed to activate";
            plugin.reset();
        }
        if (!plugin) {
            slot.loadProblem = problem;
            report.warnings.push_back(where + problem + "; kept as a placeholder (not running)");
            next.push_back(std::move(slot));
            continue;
        }
        slot.plugin            = plugin;
        slot.runtime           = std::make_shared<SlotRuntime>(targetGainOf(saved));
        slot.saved.plugin.path = plugin->descriptor().path;   // where it was actually found

        // Only a running slot opens its device; a disabled one resolves when enabled.
        if (saved.enabled) {
            resolveSource(slot);
            if (!slot.sourceProblem.empty())   // no fallback to "all"
                report.warnings.push_back(where + slot.sourceProblem + "; this slot will hear nothing from it");
        }
        next.push_back(std::move(slot));
    }

    loaded     = std::move(next);   // the old plugins live on in the old graph until it's collected
    name       = rack.name;
    setTempo(rack.tempoBpm);
    if (!setTimeSignature(rack.timeSigNumerator, rack.timeSigDenominator)) setTimeSignature(4, 4);
    sampleRate = rate;
    dirty      = false;
    commitSequences();
    commitGraph();
    return std::any_of(loaded.begin(), loaded.end(),
                       [](const Slot& s) { return s.plugin && s.saved.enabled; });
}

Rack Session::capture() const {
    Rack rack;
    rack.name               = name;
    rack.tempoBpm           = tempo;
    rack.timeSigNumerator   = sigNumerator;
    rack.timeSigDenominator = sigDenominator;
    for (const SequenceEntry& entry : sequenceList) rack.sequences.push_back(entry.sequence);
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
    if (s->runtime) s->runtime->targetGain.store(targetGainOf(s->saved), std::memory_order_relaxed);
    dirty = true;
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

bool Session::setEnabled(const std::string& id, bool enabled) {
    Slot* s = find(id);
    if (s == nullptr) return false;
    s->saved.enabled = enabled;
    // A slot loaded disabled hasn't resolved its source yet.
    if (enabled && s->plugin && s->source == kInvalidMidiSource && s->sourceProblem.empty()) resolveSource(*s);
    graphDirty = dirty = true;
    return true;
}

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
    slot.runtime = std::make_shared<SlotRuntime>(targetGainOf(slot.saved));
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
    graphDirty = dirty = true;
    return true;
}

void Session::flush() {
    if (sequencesDirty) commitSequences();
    if (graphDirty) commitGraph();
}

void Session::unload() {
    engine.commitSequences(nullptr);   // releases any sounding sequence notes
    engine.commit(nullptr);
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
    sequenceList.push_back(SequenceEntry{seq, nextSequenceSource++, std::make_shared<SequenceRuntime>()});
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
    entry->sequence = updated;
    sequencesDirty = dirty = true;
    return true;
}

bool Session::removeSequence(const std::string& id) {
    const auto it = std::find_if(sequenceList.begin(), sequenceList.end(),
                                 [&](const SequenceEntry& e) { return e.sequence.id == id; });
    if (it == sequenceList.end()) return false;
    sequenceList.erase(it);   // the engine releases its notes when it adopts the new set
    for (Slot& slot : loaded)
        if (slot.saved.source.kind == MidiSourceRef::Kind::Sequence && slot.saved.source.name == id) {
            resolveSource(slot);   // now unresolved: hears nothing, with the reason
            graphDirty = true;
        }
    sequencesDirty = dirty = true;
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

void Session::commitSequences() {
    std::vector<CompiledSequence> compiled;
    compiled.reserve(sequenceList.size());
    for (const SequenceEntry& entry : sequenceList)
        compiled.push_back(CompiledSequence::compile(entry.sequence, entry.source, entry.runtime));
    engine.commitSequences(std::make_unique<SequenceSet>(std::move(compiled)));
    sequencesDirty = false;
}

// --- helpers -----------------------------------------------------------------------

void Session::commitGraph() {
    std::vector<SlotConfig> configs;
    for (const Slot& slot : loaded) {
        if (!slot.plugin || !slot.saved.enabled) continue;
        SlotConfig config;
        config.plugin                = slot.plugin;
        config.runtime               = slot.runtime;
        config.midiFilter            = slot.saved.filter;
        config.midiFilter.allSources = slot.saved.source.kind == MidiSourceRef::Kind::All;
        config.midiFilter.source     = slot.source;   // invalid when unresolved: hears nothing
        config.notePort              = slot.saved.notePort;
        config.firstOutput           = slot.saved.firstOutput;
        configs.push_back(std::move(config));
    }
    engine.commit(std::make_unique<EngineGraph>(std::move(configs)));
    graphDirty = false;
}

void Session::resolveSource(Slot& slot) {
    slot.source = kInvalidMidiSource;
    slot.sourceProblem.clear();
    if (slot.saved.source.kind != MidiSourceRef::Kind::All)
        slot.source = resolve(slot.saved.source, slot.sourceProblem);
}

std::shared_ptr<HostedPlugin> Session::createPlugin(const RackSlot& saved, std::string& problem,
                                                    std::vector<PluginDescriptor>& scanned, bool& haveScan) {
    if (!saved.plugin.path.empty())
        if (std::unique_ptr<HostedPlugin> plugin = factory.create(saved.plugin, host)) return plugin;

    if (!haveScan) {
        scanned  = factory.scan();
        haveScan = true;
    }
    for (const PluginDescriptor& found : scanned)
        if (found.format == saved.plugin.format && found.id == saved.plugin.id)
            if (std::unique_ptr<HostedPlugin> plugin = factory.create(found, host)) return plugin;

    problem = "plugin '" + saved.plugin.id + "' isn't installed or failed to load";
    return nullptr;
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
            openSources[key] = handle;
            return handle;
        }
        problem = "MIDI device '" + ref.name + "' isn't connected";
    } else {
        problem = "there's no virtual input '" + ref.name + "'";
    }
    return kInvalidMidiSource;
}

} // namespace stagehand
