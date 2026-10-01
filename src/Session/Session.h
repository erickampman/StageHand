#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
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

    // The MIDI inputs open now, in the order they were opened (the dock's chips).
    struct OpenInput {
        MidiSourceRef    ref;
        MidiSourceHandle handle = kInvalidMidiSource;
    };
    const std::vector<OpenInput>& openInputs() const noexcept { return inputs; }

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
    void                     setRackName(std::string rackName) { name = std::move(rackName); }
    double                   loadedSampleRate() const noexcept { return sampleRate; }   // of the last load()
    double                   tempoBpm() const noexcept { return tempo; }
    int                      timeSigNumerator() const noexcept { return sigNumerator; }
    int                      timeSigDenominator() const noexcept { return sigDenominator; }

    // --- editing (docs/rack-view.md, section 1). Each returns false for an
    // unknown id and marks the session modified. A placeholder's settings can be
    // edited too; they're saved, and used if its plugin turns up.

    // Live: written to the slot's SlotRuntime and ramped by the audio thread.
    // No graph commit, no allocation.
    bool setGainDb(const std::string& id, float db);
    bool setMuted(const std::string& id, bool muted);

    // The rack's transport: applied to the engine at once (it's read per block)
    // and saved with the rack. setTempo clamps; setTimeSignature returns false
    // for an invalid one (numerator 1-32, denominator a power of two to 32).
    void setTempo(double bpm);
    bool setTimeSignature(int numerator, int denominator);

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
    // once no graph holds it (Engine::collect()). It leaves every sequence's targets.
    bool removeSlot(const std::string& id);

    // --- sequences (docs/sequences.md). A sequence is a MIDI source the rack
    // owns. It plays into the rows listed in its `targets` (on top of their own
    // live input). Edits are committed by flush(), like structural edits, and
    // playback carries on through them.
    struct SequenceEntry {
        Sequence                         sequence;
        MidiSourceHandle                 source = kInvalidMidiSource;   // never reused in a session
        std::shared_ptr<SequenceRuntime> runtime;
        // sequence.triggers' inputs, resolved when they change; invalid for one
        // that isn't connected (it hears nothing) or listens to any input.
        std::vector<MidiSourceHandle>    triggerSources;
    };
    const std::vector<SequenceEntry>& sequences() const noexcept { return sequenceList; }
    const SequenceEntry*              sequence(const std::string& id) const noexcept;
    // A new, empty sequence (MIDI 2.0 in 12-EDO unless told otherwise); returns its id.
    std::string addSequence(MidiProtocol protocol = MidiProtocol::Midi2);
    // Replace a sequence's content and settings (matched by id). New triggers
    // open their devices if needed; one that isn't connected hears nothing.
    // Targets are kept to rows that exist, each once. Refused, with
    // `problem` set, if it would change the protocol of a sequence that has
    // notes: that's a conversion, not an edit.
    bool setSequence(const Sequence& sequence, std::string& problem);
    // Its rows simply stop hearing it.
    bool removeSequence(const std::string& id);
    bool startSequence(const std::string& id);   // at its launch boundary
    bool stopSequence(const std::string& id);    // at its launch boundary
    void stopAllSequences();                     // now, with note-offs
    SequenceState sequenceState(const std::string& id) const noexcept;

    // --- the transport (docs/sequences.md, section 7): Start puts beat 1 at the
    // next block; Stop ends every sequence and returns to bar 1. Its MIDI
    // control is saved with the rack and published with the sequences.
    void startTransport();
    void stopTransport();
    bool transportRunning() const noexcept;   // as of the last audio block
    const TransportSettings& transportSettings() const noexcept { return transportSetup; }
    // The beat lights' count and the click, applied to the engine at once (the
    // click's output device is the app's business: RackController routes it).
    const MetronomeSettings& metronome() const noexcept { return metronomeSetup; }
    void setMetronome(const MetronomeSettings& settings);
    void setTransportSettings(const TransportSettings& settings);
    // transportSettings().triggers' inputs, resolved (as SequenceEntry::triggerSources).
    const std::vector<MidiSourceHandle>& transportTriggerHandles() const noexcept { return transportTriggerSources; }
    // Why start on first note's input didn't resolve; empty if it did.
    const std::string& startOnNoteIssue() const noexcept { return startOnNoteProblem; }
    double        sequencePosition(const std::string& id) const noexcept;   // units into the loop

    // Commit the graph if a structural edit is pending. Call once per UI tick.
    void flush();
    bool flushPending() const noexcept { return graphDirty || sequencesDirty; }

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
    // Fills entry.triggerSources; returns why triggers didn't resolve, if any.
    std::vector<std::string> resolveTriggers(SequenceEntry& entry);
    std::vector<std::string> resolveTriggerList(const std::vector<SequenceTrigger>& triggers,
                                                std::vector<MidiSourceHandle>&      handles);
    std::vector<std::string> resolveTransport();
    void             commitGraph();
    void             commitSequences();
    SequenceEntry*   findSequence(const std::string& id) noexcept;
    static float     targetGainOf(const RackSlot& saved) noexcept;

    PluginFactory& factory;
    HostServices&  host;
    MidiInput&     midi;
    Engine&        engine;

    std::string                             name;
    std::vector<Slot>                       loaded;
    std::map<std::string, MidiSourceHandle> openSources;   // "device:<id>" / "virtual:<name>"
    std::vector<OpenInput>                  inputs;        // the same, in order
    double                                  sampleRate     = 0.0;
    double                                  tempo          = 120.0;
    int                                     sigNumerator   = 4;
    int                                     sigDenominator = 4;
    bool                                    graphDirty     = false;
    bool                                    sequencesDirty = false;
    std::vector<SequenceEntry>              sequenceList;
    MidiSourceHandle                        nextSequenceSource = kSequenceSourceBase + 1;
    TransportSettings                       transportSetup;
    MetronomeSettings                       metronomeSetup;
    void                                    applyMetronome();
    std::vector<MidiSourceHandle>           transportTriggerSources;
    MidiSourceHandle                        startOnNoteHandle = kInvalidMidiSource;
    std::string                             startOnNoteProblem;
    bool                                    dirty      = false;
};

} // namespace stagehand
