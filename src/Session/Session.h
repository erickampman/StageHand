#pragma once

#include <cstddef>
#include <cstdint>
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
// source, and publishes the resulting EngineGraph. The rack can then be edited
// live (docs/rack-view.md, section 1), and capture() turns the running setup
// back into a Rack. The Session layer of docs/domain-model.md. Main thread.
//
// Loading a rack while another runs is safe by construction: the new graph is
// committed, and the old plugins stay alive (held by the old graph) until the
// audio thread has stopped them and Engine::collect() retires that graph. Edits
// rely on the same rule: a removed or disabled plugin keeps running in the
// current graph until the next one is adopted.
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
    // hearing nothing (no fallback to "all"), with a warning. Clears modified().
    // Returns false only if the rack produced no runnable slot at all. An empty
    // rack is fine: it starts a session to add slots to.
    bool load(const Rack& rack, double sampleRate, LoadReport& report);

    // The running setup as a Rack, including edits, each plugin's state freshly
    // saved. Placeholders keep what they were loaded with.
    Rack capture() const;

    // What's loaded, for the UI and reporting.
    struct Slot {
        RackSlot                      saved;    // settings as edited; plugin state as of load
        std::shared_ptr<HostedPlugin> plugin;   // null for a placeholder
        std::shared_ptr<SlotRuntime>  runtime;  // live gain, meter, MIDI activity; null for a placeholder
        MidiSourceHandle              source = kInvalidMidiSource;   // resolved saved.source
        std::string                   sourceProblem;                 // why it didn't resolve, if it didn't
        std::string                   loadProblem;                   // placeholder: why the plugin didn't load
    };
    const std::vector<Slot>& slots() const noexcept { return loaded; }
    const Slot*              slot(const std::string& id) const noexcept;
    const std::string&       rackName() const noexcept { return name; }

    // --- editing (docs/rack-view.md, section 1). Each returns false for an
    // unknown id and marks the session modified. A placeholder's settings can be
    // edited too; they're saved, and used if its plugin turns up.

    // Live: written to the slot's SlotRuntime and ramped by the audio thread.
    // No graph commit, no allocation.
    bool setGainDb(const std::string& id, float db);
    bool setMuted(const std::string& id, bool muted);

    // Structural: take effect at the next flush(), which commits one new graph
    // for any number of edits.
    bool setEnabled(const std::string& id, bool enabled);
    bool setFilter(const std::string& id, const MidiFilter& filter);   // source fields ignored
    bool setFirstOutput(const std::string& id, std::uint32_t firstOutput);
    // Resolves (opening a named device if needed). If it can't, the slot hears
    // nothing and `problem` says why; the choice is kept either way.
    bool setSource(const std::string& id, const MidiSourceRef& source, std::string& problem);

    // Create and activate `plugin` as a new slot at `index` (clamped to the
    // end), at the sample rate of the last load(). Returns its id, unique in the
    // rack and derived from the plugin's name, or "" with `problem` set.
    std::string addSlot(const PluginDescriptor& plugin, std::size_t index, std::string& problem);
    // Close the slot's editor first. Its plugin is deactivated and destroyed
    // once no graph holds it (Engine::collect()).
    bool removeSlot(const std::string& id);

    // Commit the graph if a structural edit is pending. Call once per UI tick.
    void flush();
    bool flushPending() const noexcept { return graphDirty; }

    // Unsaved changes: edits here, or a plugin reporting a state change (CLAP
    // state.mark_dirty, via HostServices::stateChanged()).
    bool modified() const noexcept { return dirty; }
    void markModified() noexcept { dirty = true; }
    void markSaved() noexcept { dirty = false; }

    // Stop running the rack: commits an empty graph. Plugins stay alive until
    // the Session is destroyed or another rack is loaded; follow the engine's
    // shutdown order (wait for Engine::committedGraphAdopted(), stop the
    // backend) before destroying the Session.
    void unload();

private:
    Slot* find(const std::string& id) noexcept;
    // The saved path first (a hint), then a scan (done at most once per load).
    std::shared_ptr<HostedPlugin> createPlugin(const RackSlot& saved, std::string& problem,
                                               std::vector<PluginDescriptor>& scanned, bool& haveScan);
    // An open input's handle, opening a named device if needed; invalid (with
    // `problem` set) if it can't be found.
    MidiSourceHandle resolve(const MidiSourceRef& ref, std::string& problem);
    void             resolveSource(Slot& slot);
    void             commitGraph();
    static float     targetGainOf(const RackSlot& saved) noexcept;

    PluginFactory& factory;
    HostServices&  host;
    MidiInput&     midi;
    Engine&        engine;

    std::string                             name;
    std::vector<Slot>                       loaded;
    std::map<std::string, MidiSourceHandle> openSources;   // "device:<id>" / "virtual:<name>"
    double                                  sampleRate = 0.0;
    bool                                    graphDirty = false;
    bool                                    dirty      = false;
};

} // namespace stagehand
