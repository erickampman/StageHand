#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "App/RackViewModel.h"
#include "Engine/Engine.h"
#include "Midi/MidiInput.h"
#include "Plugin/PluginFactory.h"
#include "Session/Session.h"

namespace stagehand {

// An open plugin editor, as the controller sees it. The app supplies these
// (EditorWindow on X11), so the controller stays free of windowing and testable.
class EditorHandle {
public:
    virtual ~EditorHandle() = default;
    virtual bool isOpen() const = 0;   // false once the user closed it
};

// Between the rack view and the Session (docs/rack-view.md, section 5): applies
// the view's intents, owns the editor windows and saving, and rebuilds the
// RackViewModel each tick. Main thread only. No DGL, X11 or JACK types.
class RackController {
public:
    struct AudioStatus {
        double        sampleRate = 0.0;
        std::uint32_t bufferSize = 0;
        std::uint64_t xruns      = 0;
        std::uint32_t outputs    = 2;   // output channels the backend registered
    };

    struct Config {
        // Opens an editor window for a plugin. Null result: no editor (the
        // opener reports why).
        std::function<std::unique_ptr<EditorHandle>(HostedPlugin&, const std::string& title)> openEditor;
        std::function<AudioStatus()> audioStatus;
        std::function<void()>        quit;   // e.g. MainLoop::requestQuit

        // StageHand's own virtual inputs (not listed by MidiInput::enumerate()).
        std::vector<std::pair<std::string, MidiProtocol>> virtualInputs;
        MidiProtocol protocolPreference = MidiProtocol::Midi2;

        // Where save() writes; empty = saving is off, and `noSaveReason` says why.
        std::string saveTarget;
        std::string noSaveReason;
    };

    RackController(Session& session, Engine& engine, MidiInput& midiInput, PluginFactory& pluginFactory,
                   Config config);
    ~RackController();   // closes the editors while the plugins are alive

    RackController(const RackController&)            = delete;
    RackController& operator=(const RackController&) = delete;

    // --- periodic (main loop) ---
    // Engine collect() and plugin idle(). Every few milliseconds: plugins rely
    // on it for main-thread callbacks and parameter flushes.
    void service();
    // Commit pending edits, notice closed editors, and rebuild the view model
    // (meters and activity included). About 30 times a second. `nowSeconds` is
    // any steady clock, for meter ballistics.
    void tick(double nowSeconds);

    const RackViewModel& view() const noexcept { return model; }
    // Messages for the user since the last call ("Saved to ...", problems).
    std::vector<std::string> takeNotices();

    // --- intents (docs/rack-view.md, section 4). Unknown ids are ignored. ---
    void setGainDb(const std::string& id, float db);
    void setMuted(const std::string& id, bool muted);
    void setEnabled(const std::string& id, bool enabled);
    void setFilter(const std::string& id, const MidiFilter& filter);
    void setSource(const std::string& id, const MidiSourceRef& source);
    void setFirstOutput(const std::string& id, std::uint32_t firstOutput);
    void setTempo(double bpm);
    void setTimeSignature(int numerator, int denominator);
    // Tap tempo: the average of the last four intervals. A gap of more than 2 s
    // starts over. `nowSeconds` is any steady clock.
    void tapTempo(double nowSeconds);
    void clearClip(const std::string& id);
    void clearOutputClip();

    // Plugins that can be added, scanned on first use (slow: loads each file).
    const std::vector<PluginDescriptor>& availablePlugins();
    // Returns the new slot's id, or "" (with a notice) on failure.
    std::string addPlugin(const PluginDescriptor& plugin, std::size_t position);
    void        removeSlot(const std::string& id);   // closes its editor first

    void openEditor(const std::string& id);
    void closeEditor(const std::string& id);
    void openAllEditors();
    void closeAllEditors();
    std::size_t openEditorCount() const;

    // --- sequences (the sources dock) ---
    const Sequence* sequence(const std::string& id) const;   // as the Session has it; null if none
    std::string addSequence(MidiProtocol protocol);   // returns its id
    void        removeSequence(const std::string& id);
    // Play/Stop: start if stopped, stop if playing; a pending start or stop is
    // cancelled.
    void togglePlay(const std::string& id);
    void stopAllSequences();
    void setSequenceRate(const std::string& id, SequenceRate rate);
    // Refused, with a notice, if notes lie past the new end: the editor deletes
    // notes, the length control never does.
    void setSequenceLength(const std::string& id, std::uint32_t length);
    void setSequenceLaunch(const std::string& id, LaunchQuantize launch);
    void setSequenceRepeat(const std::string& id, bool repeat);
    // The editor's edits (the editor window, docs/sequences.md section 5).
    // Notes are replaced whole; the rest changes one setting and leaves the notes.
    void setSequenceNotes(const std::string& id, std::vector<SequenceNote> notes);
    void setSequenceProtocol(const std::string& id, MidiProtocol protocol);   // only while it has no notes
    void setSequenceTuning(const std::string& id, int edo, int referenceNote);
    void setSequenceChannel(const std::string& id, int channel);   // 0-based
    void setSequenceGroup(const std::string& id, int group);       // 0-based
    void setSequenceNoteOffOffset(const std::string& id, double ms);

    // Refresh the source menu (MidiInput::enumerate()); call when it opens.
    void refreshSources();

    bool save();   // to Config::saveTarget; true if saved
    void quit();

private:
    // A meter's display state between ticks.
    struct Ballistics {
        float  displayDb[LevelMeter::kChannels] = {kFloorDb, kFloorDb};
        float  holdDb[LevelMeter::kChannels]    = {kFloorDb, kFloorDb};
        double holdUntil[LevelMeter::kChannels] = {};
        void   update(const LevelMeter::Reading& reading, double now, double dt) noexcept;
    };
    // Per-row state kept between ticks, by slot id. Plugin facts are queried once
    // per plugin, not every tick.
    struct RowCache {
        const HostedPlugin* plugin          = nullptr;
        NoteDialectMask     dialects        = 0;
        bool                hasEditor       = false;
        Ballistics          meter;
        std::uint32_t       lastMidiEvents  = 0;
        double              midiActiveUntil = 0.0;
    };

    static constexpr float  kFloorDb        = -60.0f;   // the meter's bottom
    static constexpr float  kFallDbPerSec   = 24.0f;
    static constexpr double kPeakHoldSec    = 1.5;
    static constexpr double kMidiLightSec   = 0.15;

    RowModel       rowFor(const Session::Slot& slot, double now, double dt);
    std::string    sourceLabel(const MidiSourceRef& ref) const;
    SourceProtocol sourceProtocolOf(const MidiSourceRef& ref) const;
    void           notice(std::string text) { notices.push_back(std::move(text)); }
    // The model's row for `id`, patched right away by edits, so input arriving
    // before the next tick() builds on them (e.g. several wheel notches).
    RowModel*      modelRow(const std::string& id);
    // Apply `change` to a copy of the sequence and hand it to the Session.
    template <typename Change>
    void           editSequence(const std::string& id, Change change);
    void           buildDock(double now);

    Session&       session;
    Engine&        engine;
    MidiInput&     midi;
    PluginFactory& factory;
    Config         config;

    RackViewModel                                        model;
    std::map<std::string, std::unique_ptr<EditorHandle>> editors;   // by slot id
    std::map<std::string, RowCache>                      rows;      // by slot id
    Ballistics                                            master;
    std::vector<MidiSourceInfo>                           enumerated;
    std::vector<PluginDescriptor>                         available;
    std::vector<double>                                   taps;      // tap-tempo times
    std::map<MidiSourceHandle, std::pair<std::uint32_t, double>> inputActivity;   // last count, lit until
    bool                                                  scanned  = false;
    double                                                lastTick = -1.0;
    std::vector<std::string>                              notices;
};

} // namespace stagehand
