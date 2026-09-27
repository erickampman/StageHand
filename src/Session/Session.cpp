#include "Session/Session.h"

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

bool Session::load(const Rack& rack, double sampleRate, LoadReport& report) {
    std::vector<Slot>             next;
    std::vector<SlotConfig>       configs;
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
        if (plugin && !plugin->activate(sampleRate, kMaxPluginBlockFrames)) {
            problem = "the plugin failed to activate";
            plugin.reset();
        }
        if (!plugin) {
            report.warnings.push_back(where + problem + "; kept as a placeholder (not running)");
            next.push_back(std::move(slot));
            continue;
        }
        slot.plugin            = plugin;
        slot.saved.plugin.path = plugin->descriptor().path;   // where it was actually found

        if (saved.enabled) {
            SlotConfig config;
            config.plugin                = plugin;
            config.midiFilter            = saved.filter;
            config.midiFilter.allSources = saved.source.kind == MidiSourceRef::Kind::All;
            config.midiFilter.source     = kInvalidMidiSource;
            if (!config.midiFilter.allSources) {
                std::string sourceProblem;
                config.midiFilter.source = resolve(saved.source, sourceProblem);
                if (config.midiFilter.source == kInvalidMidiSource)   // no fallback to "all"
                    report.warnings.push_back(where + sourceProblem + "; this slot will hear nothing from it");
            }
            config.notePort    = saved.notePort;
            config.gain        = dbToGain(saved.gainDb);
            config.firstOutput = saved.firstOutput;
            configs.push_back(std::move(config));
        }
        next.push_back(std::move(slot));
    }

    const bool runnable = !configs.empty();
    engine.commit(std::make_unique<EngineGraph>(std::move(configs)));
    loaded = std::move(next);   // the old plugins live on in the old graph until it's collected
    name   = rack.name;
    return runnable;
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

void Session::unload() {
    engine.commit(nullptr);
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
