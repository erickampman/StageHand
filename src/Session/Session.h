#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Engine/Engine.h"
#include "Midi/MidiInput.h"
#include "Plugin/HostServices.h"
#include "Plugin/PluginFactory.h"
#include "Session/Rack.h"
#include "Session/Rig.h"

namespace stagehand {

class PreparedRack;

// Runs a Rack: instantiates and activates its plugins, resolves each slot's MIDI
// source, and publishes the resulting EngineGraph. The rack can then be edited
// live (docs/rack-view.md, section 1), and capture() turns the running setup
// back into a Rack. The Session layer of docs/domain-model.md. Main thread.
//
// Loading a rack while another runs is safe by construction: the new graph is
// committed, and the old plugins stay alive (held by the old graph) until the
// audio thread has stopped them and Engine::collect() retires that graph. Edits
// rely on the same rule: a removed or disabled plugin keeps running in the
// current graph until the next one is adopted. At a switch the old rack's
// slots stay in the graph a while, draining (activate()), and leave it once
// they've rung out.
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
    // they all go into the committed graph. A plugin that can't be loaded
    // leaves a placeholder (kept, not run, saved back unchanged). A named device
    // that isn't open is opened; one that isn't connected leaves its slot
    // hearing nothing (no fallback to "all"), with a warning. Clears modified().
    // Returns false only if the rack produced no runnable slot at all. An empty
    // rack is fine: it starts a session to add slots to.
    // The same as prepare(), prepareStep() until ready, then activate().
    bool load(const Rack& rack, double sampleRate, LoadReport& report);

    // load() in two steps (docs/setlists.md, section 2), so a rack can be made
    // ready while another plays. prepare() takes the rack and builds its
    // sequence entries; nothing is created yet. Each prepareStep() then
    // creates, restores and activates one slot's plugin (CLAP: main thread),
    // and returns true once every slot is done. Nothing reaches the engine: a
    // prepared plugin is activated but never processed.
    std::unique_ptr<PreparedRack> prepare(const Rack& rack, double sampleRate);
    bool                          prepareStep(PreparedRack& prepared);
    // Make it the running rack, finishing its preparation first if need be:
    // sources and triggers resolve, and one graph is committed (with the rig's
    // effects, which stay loaded). load()'s rules and result otherwise.
    // The rack it replaces (docs/setlists.md, section 3): with RingOut its
    // slots stay in that graph, draining (SlotConfig::draining), so tails and
    // held notes ring on; with Cut they're muted (notes released, faded). Its
    // sequences end with note-offs. A rack still draining from an earlier
    // switch is muted, so at most one rack rings out. flush() removes each
    // slot once it's done.
    // The engine takes the switch as one (Engine::commitSwitch): the new
    // graph, sequences, tempo and signature together, at once or, with
    // `atNextBar` and the transport running, at the next bar line; the
    // transport stops there, or runs on with the new rack's At Start
    // sequences from the next bar (section 5).
    enum class OldRack { RingOut, Cut };
    struct SwitchOptions {
        OldRack oldRack       = OldRack::RingOut;
        bool    atNextBar     = false;
        bool    stopTransport = true;   // a new song from bar 1, unless the setlist keeps it running
    };
    bool activate(std::unique_ptr<PreparedRack> prepared, LoadReport& report, const SwitchOptions& how);
    bool activate(std::unique_ptr<PreparedRack> prepared, LoadReport& report);   // with the default options
    // The earlier racks' slots still sounding: no rack identity, just the
    // plugin, its runtime and filter.
    const std::vector<SlotConfig>& drainingSlots() const noexcept { return draining; }

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
    // A row's send to rig bus `bus` (docs/rig.md), after its fader; none = off.
    bool setSendDb(const std::string& id, std::size_t bus, std::optional<float> db);

    // --- the rig (docs/rig.md): what every song shares. It outlives load():
    // a rack load keeps the rig's effects running, so their tails ring on.
    // Its edits mark rigModified(), not modified(); without a rig loaded, the
    // graph has no buses and sends go nowhere.
    struct LoadedRig {
        Rig saved;   // settings as edited; effect state as of load
        struct Effect {
            std::shared_ptr<HostedPlugin> plugin;    // null: an empty slot, or one that didn't load
            std::string                   problem;   // why it didn't load, if it didn't
        };
        std::array<std::array<Effect, RigBus::kEffects>, kRigBuses> effects;
        std::array<std::shared_ptr<SlotRuntime>, kRigBuses>         returns;   // return level, meter
    };
    // Replace the rig: its effects are created, restored and activated at
    // `sampleRate`, and the graph is committed with them. An effect that can't
    // load leaves its slot empty but kept (saved back unchanged), with a
    // warning. Clears rigModified().
    void              loadRig(const Rig& rig, double sampleRate, LoadReport& report);
    bool              hasRig() const noexcept { return rigLoaded; }
    const LoadedRig&  rig() const noexcept { return rigState; }
    Rig               captureRig() const;   // as edited, each effect's state freshly saved
    bool              rigModified() const noexcept { return rigDirty; }
    void              markRigModified() noexcept { rigDirty = true; }
    void              markRigSaved() noexcept { rigDirty = false; }
    // Live (ramped; no commit).
    bool              setReturnDb(std::size_t bus, float db);
    bool              setBusMuted(std::size_t bus, bool muted);
    void              setMasterDb(float db);
    // Structural (at the next flush()): `plugin` null empties the slot. False,
    // with `problem` set, if the plugin can't be created; the slot is unchanged.
    bool              setBusEffect(std::size_t bus, std::size_t slot, const PluginDescriptor* plugin,
                                   std::string& problem);
    bool              setBusOutput(std::size_t bus, std::uint32_t firstOutput);

    // The rack's transport: applied to the engine at once (it's read per block)
    // and saved with the rack. setTempo clamps; setTimeSignature returns false
    // for an invalid one (numerator 1-32, denominator a power of two to 32).
    void setTempo(double bpm);
    bool setTimeSignature(int numerator, int denominator);

    // Structural: take effect at the next flush(), which commits one new graph
    // for any number of edits.
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
    // Sound `note` of sequence `id` once in the rows it plays into, for its own
    // length at the current tempo (kept between kMinAudition and kMaxAudition
    // seconds), as the editor adds or moves it. Playing or not. `hold`: until
    // endAuditions() instead (kHeldAudition at most, should that never come).
    bool auditionNote(const std::string& id, const SequenceNote& note, bool hold = false);
    static constexpr double kHeldAudition = 30.0;
    // End what auditionNote() started and is still sounding; `allNotesOff`: then
    // send a note-off for every key and All Notes Off (CC 123) on the
    // sequence's channel into its rows, which ends any note left on there,
    // whoever started it, even in a plugin that ignores CC 123.
    bool endAuditions(const std::string& id, bool allNotesOff = false);
    static constexpr double kMinAudition = 0.12;
    static constexpr double kMaxAudition = 1.0;

    // --- loopers (docs/looper.md): MIDI sources the rack owns, like sequences,
    // playing into their targets. Their settings are saved with the rack;
    // what they record never is: every load (a song switch included) makes
    // them empty. Edits are committed by flush().
    struct LooperEntry {
        LooperSetup                    setup;
        MidiSourceHandle               source = kInvalidMidiSource;   // its playback, never reused in a session
        std::shared_ptr<LooperRuntime> runtime;
        MidiSourceHandle               input = kInvalidMidiSource;    // setup.source resolved
        std::string                    inputProblem;                  // why it didn't resolve: it records nothing
    };
    const std::vector<LooperEntry>& loopers() const noexcept { return looperList; }
    const LooperEntry*              looper(const std::string& id) const noexcept;
    // A new one, empty; its id unique (from its name). Targets kept to rows
    // that exist. Unnamed: "Loop N", N one past the highest used in this rack
    // (a running tab: a deleted looper's number isn't used again).
    std::string addLooper(LooperSetup setup);
    bool        setLooper(const LooperSetup& setup);   // by id; what it recorded is kept
    bool        removeLooper(const std::string& id);   // its notes end
    bool        looperCommand(const std::string& id, LooperCommand command);   // at the next block
    // The one the looper switch drives (docs/looper.md, "Controls"); "" = none.
    void        setCurrentLooper(const std::string& id);
    const std::string& currentLooper() const noexcept { return currentLooperId; }
    // The looper switch, in the rig (the performer's, the same in every song):
    // none = off. Marks the rig modified.
    void        setLooperSwitch(const std::optional<SequenceTrigger>& trigger);
    // Why it doesn't act, if its input isn't connected.
    const std::string& looperSwitchProblem() const noexcept { return looperSwitchIssue; }

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

    // --- the setlist's MIDI triggers (docs/setlists.md, section 4): published
    // with every rack's sequences and matched before the rack's own triggers,
    // and kept through rack loads. Not the rack's: no modified(). Inputs
    // resolve as a sequence's triggers do; returns why some won't act.
    // Committed at the next flush().
    std::vector<std::string>             setSetlistTriggers(const std::vector<SetlistTrigger>& triggers);
    const std::vector<SetlistTrigger>&   setlistTriggers() const noexcept { return setlistControls; }
    const std::vector<MidiSourceHandle>& setlistTriggerHandles() const noexcept { return setlistSources; }

    // Commit the graph if a structural edit is pending, or a draining slot has
    // finished (it's left out). Call once per UI tick.
    void flush();
    bool flushPending() const noexcept { return graphDirty || sequencesDirty; }

    // Unsaved changes: edits here, or a plugin reporting a state change (CLAP
    // state.mark_dirty, via HostServices::stateChanged()).
    bool modified() const noexcept { return dirty; }
    void markModified() noexcept { dirty = true; }
    void markSaved() noexcept { dirty = false; }

    // Stop running the rack: commits an empty graph (draining slots go too). Plugins stay alive until
    // the Session is destroyed or another rack is loaded; follow the engine's
    // shutdown order (wait for Engine::committedGraphAdopted(), stop the
    // backend) before destroying the Session.
    void unload();

private:
    Slot* find(const std::string& id) noexcept;
    // The saved path first (a hint), then a scan (done at most once per load).
    std::shared_ptr<HostedPlugin> createPlugin(const PluginDescriptor& wanted, std::string& problem,
                                               std::vector<PluginDescriptor>& scanned, bool& haveScan);
    // Created, restored and activated at `rate`; null, with `problem`, if not.
    std::shared_ptr<HostedPlugin> createEffect(const RigEffect& effect, double rate, std::string& problem);
    void             applySends(const Slot& slot);
    void             applyReturn(std::size_t bus);
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
    std::unique_ptr<EngineGraph> buildGraph() const;      // every slot, the draining ones and the rig's buses
    std::unique_ptr<SequenceSet> buildSequences() const;  // the sequences and the transport's and setlist's control
    SlotConfig       configOf(const Slot& slot) const;   // for the graph; the sequences that play into it included
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
    bool                                    rigLoaded  = false;
    bool                                    rigDirty   = false;
    double                                  rigRate    = 0.0;   // the rig's effects' sample rate
    LoadedRig                               rigState;
    std::vector<SlotConfig>                 draining;   // drainingSlots()
    std::vector<SetlistTrigger>             setlistControls;
    std::vector<LooperEntry>                looperList;
    std::uint32_t                           loopNumber = 0;   // the highest "Loop N" used (addLooper())
    std::string                             currentLooperId;
    MidiSourceHandle                        looperSwitchSource = kInvalidMidiSource;
    std::string                             looperSwitchIssue;
    void                                    resolveLooper(LooperEntry& entry);
    LooperEntry*                            findLooper(const std::string& id) noexcept;
    std::vector<MidiSourceHandle>           setlistSources;   // setlistControls' inputs, resolved
};

// A rack made ready to switch to (Session::prepare()): its plugins created,
// restored and activated at the sample rate it was prepared for, never
// processed. It costs memory, not CPU. Dropping it destroys its plugins.
class PreparedRack {
public:
    const Rack&  rack() const noexcept { return source; }
    double       sampleRate() const noexcept { return rate; }
    std::size_t  pluginCount() const noexcept { return source.slots.size(); }   // the steps, placeholders included
    std::size_t  preparedCount() const noexcept { return slots.size(); }
    bool         ready() const noexcept { return slots.size() == source.slots.size(); }
    // The created plugins' main-thread work (HostedPlugin::idle()), as for running ones.
    void         idle();

private:
    friend class Session;

    Rack                                 source;
    double                               rate = 0.0;
    std::vector<Session::Slot>           slots;       // the first preparedCount() of source.slots
    std::vector<Session::SequenceEntry>  sequences;   // triggers resolve at activate()
    std::vector<Session::LooperEntry>    loopers;     // empty; inputs resolve at activate()
    std::vector<std::string>             warnings;    // from creating the plugins
    std::vector<PluginDescriptor>        scanned;     // the plugin scan, done at most once
    bool                                 haveScan = false;
};

} // namespace stagehand
