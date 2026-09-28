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
    sampleRate = rate;
    dirty      = false;
    commitGraph();
    return std::any_of(loaded.begin(), loaded.end(),
                       [](const Slot& s) { return s.plugin && s.saved.enabled; });
}

Rack Session::capture() const {
    Rack rack;
    rack.name = name;
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
    if (graphDirty) commitGraph();
}

void Session::unload() {
    engine.commit(nullptr);
    graphDirty = false;
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
