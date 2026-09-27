#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "Engine/Engine.h"
#include "Midi/MidiInput.h"
#include "Plugin/HostServices.h"
#include "Plugin/PluginFactory.h"
#include "Session/Rack.h"

namespace stagehand {

// Runs a Rack: instantiates and activates its plugins, resolves each slot's MIDI
// source, and publishes the resulting EngineGraph. capture() turns the running
// setup back into a Rack. The Session layer of docs/domain-model.md. Main thread.
//
// Loading a rack while another runs is safe by construction: the new graph is
// committed, and the old plugins stay alive (held by the old graph) until the
// audio thread has stopped them and Engine::collect() retires that graph.
class Session {
public:
    Session(PluginFactory& pluginFactory, HostServices& hostServices, MidiInput& midiInput,
            Engine& runningEngine);

    // Record an input the app already opened (the virtual inputs, the default
    // hardware), so rack sources resolve to it instead of opening it twice.
    void addOpenSource(const MidiSourceRef& ref, MidiSourceHandle handle);

    struct LoadReport {
        std::vector<std::string> warnings;   // missing plugins, unconnected devices, ...
    };

    // Replace what's running with `rack`. Every slot's plugin is created,
    // restored (loadState, before activation), and activated at `sampleRate`;
    // enabled slots go into the committed graph. A plugin that can't be loaded
    // leaves a placeholder (kept, not run, saved back unchanged). A named device
    // that isn't open is opened; one that isn't connected leaves its slot
    // hearing nothing (no fallback to "all"), with a warning. Returns false only
    // if the rack produced no runnable slot at all.
    bool load(const Rack& rack, double sampleRate, LoadReport& report);

    // The running setup as a Rack, each plugin's state freshly saved.
    // Placeholders keep what they were loaded with.
    Rack capture() const;

    // What's loaded, for editors and reporting. A placeholder has no plugin.
    struct Slot {
        RackSlot                      saved;    // as loaded (state as of load time)
        std::shared_ptr<HostedPlugin> plugin;   // null for a placeholder
    };
    const std::vector<Slot>& slots() const noexcept { return loaded; }
    const std::string&       rackName() const noexcept { return name; }

    // Stop running the rack: commits an empty graph. Plugins stay alive until
    // the Session is destroyed or another rack is loaded; follow the engine's
    // shutdown order (wait for Engine::committedGraphAdopted(), stop the
    // backend) before destroying the Session.
    void unload();

private:
    MidiSourceHandle resolve(const MidiSourceRef& ref, LoadReport& report);

    PluginFactory& factory;
    HostServices&  host;
    MidiInput&     midi;
    Engine&        engine;

    std::string                             name;
    std::vector<Slot>                       loaded;
    std::map<std::string, MidiSourceHandle> openSources;   // "device:<id>" / "virtual:<name>"
};

} // namespace stagehand
