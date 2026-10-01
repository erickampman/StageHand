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
#include "Audio/AuxRouting.h"
#include "Engine/Engine.h"
#include "Midi/MidiInput.h"
#include "Plugin/PluginFactory.h"
#include "Session/Session.h"

namespace stagehand {

// Which of `devices` a saved click output means (docs/sequences.md, section 7),
// or -1: the same name; else the same name without the " 2" PipeWire adds to
// tell two alike apart; else the same card under another profile ("Scarlett
// 2i2 USB Analog Stereo" -> "Scarlett 2i2 USB Pro"), if exactly one matches and
// it's the same kind (analog, or digital/HDMI: never HDMI for analog, which
// could be the PA).
int findPlaybackDevice(const std::string& saved, const std::vector<PlaybackDevice>& devices);

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

        // Where the click's outputs are connected (the backend); null: nowhere.
        AuxRouting* clickRouting = nullptr;

        // The rack's bundle, where save() writes; empty = untitled (Save As
        // names it). `savingOff` (--no-save-rack): nothing is ever saved.
        std::string saveTarget;
        bool        savingOff = false;
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
    // A new sequence, playing into `target` (a row's id; "" = no row yet).
    // Returns its id.
    std::string addSequence(MidiProtocol protocol, const std::string& target = {});
    // A new row for `plugin` (at the bottom), played by sequence `id`. Returns
    // the row's id, or "" (with a notice) if the plugin couldn't be added.
    std::string addRowForSequence(const std::string& id, const PluginDescriptor& plugin);
    // Where a sequence plays (docs/sequences.md, "Where a sequence plays"): add
    // or remove a row from its targets. Rows keep their own live input.
    void        setSequenceTarget(const std::string& id, const std::string& row, bool on);
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
    // MIDI learn (docs/sequences.md, section 4): the next note-on or controller
    // press from any input becomes a trigger that does `action` to the
    // sequence. It replaces the sequence's trigger for that action and any
    // other use of that control on the same sequence. One learn at a time; a
    // new one replaces it.
    void learnTrigger(const std::string& id, SequenceTrigger::Action action);
    void cancelLearn();
    void removeTrigger(const std::string& id, std::size_t index);   // index into Sequence::triggers
    void setSequenceAtStart(const std::string& id, bool atStart);
    // Leading and trailing spaces go; an empty name is refused (with a notice);
    // at most kMaxNameBytes. The id stays: rows keep playing it.
    void renameSequence(const std::string& id, const std::string& name);
    static constexpr std::size_t kMaxNameBytes = 64;

    // --- the beat lights and the click (docs/sequences.md, section 7) ---
    void setMetronomeCount(bool everyNote);   // every note (6/8: six), or the counted beats (6/8: two)
    void setClick(bool on);
    void setClickLevel(float db);             // -60 to 0; also turns the click on
    void setClickOutput(const std::string& device);   // also turns the click on
    void testClick();                         // one blip now, on the click's output (connected for it if need be)
    void refreshPlaybackDevices();            // for the menu; call when it opens

    // --- the transport (docs/sequences.md, section 7) ---
    void startTransport();
    void stopTransport();
    void toggleTransport();
    // MIDI learn for the transport's own triggers, with the sequences' rules.
    void learnTransportTrigger(SequenceTrigger::Action action);
    void removeTransportTrigger(std::size_t index);   // index into TransportSettings::triggers
    // Start on first note: off, or from `source` (All = any input).
    void setStartOnNote(bool on, const MidiSourceRef& source = {});

    // Refresh the source menu (MidiInput::enumerate()); call when it opens.
    void refreshSources();

    // --- the rack file (docs/rack-view.md, "Files") ---
    // Where Save writes: the rack's bundle; empty for an untitled rack (a
    // no-argument start, a --plugin session, or after New).
    const std::string& rackPath() const noexcept { return target; }
    bool               savingOff() const noexcept { return config.savingOff; }
    // An untitled rack with changes that switching racks or quitting would
    // lose: the UI asks first.
    bool unsavedUntitled() const;
    // Replace the running rack with a new, empty one, or with the rack at
    // `chosen` (a bundle, or its rack.json). A titled rack is saved first, as
    // at quit (unless saving is off); if that fails, nothing changes. Editors
    // close, MIDI learn is cancelled. False (with a notice) if nothing changed.
    bool newRack();
    bool openRack(const std::string& chosen);
    bool save();   // to rackPath(); false (with a notice) if untitled or saving is off
    // Save to a new bundle (".rack" added if missing), which becomes the rack's
    // path and its name. An existing rack there is replaced only with `replace`;
    // anything else there never is.
    enum class SaveAsResult { Saved, Failed, NeedsReplace };
    SaveAsResult saveAs(const std::string& chosen, bool replace = false);
    // At quit: save a titled rack (unless saving is off). False if a save failed.
    bool saveForQuit();
    // Changes whenever another rack replaces the running one: windows tied to
    // its sequences must close (their ids may mean something else now).
    std::uint64_t rackGeneration() const noexcept { return generation; }
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
    bool           replaceRack(const Rack& rack, const std::string& path, const std::vector<std::string>& warnings);
    void           setMetronome(const MetronomeSettings& settings);
    // Connect the click to its saved output, if it's on and the device is
    // there; `report`: say so when it isn't (once, until it connects).
    void           routeClick(bool report);
    void           buildDock(double now);
    void           finishLearn(const Engine::Learned& learned);
    std::string    triggerText(const SequenceTrigger& trigger) const;

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
    std::string                                           learningId;   // the sequence being learned for
    SequenceTrigger::Action                               learningAction = SequenceTrigger::Action::Toggle;
    std::string                                           target;          // the rack's bundle; "" = untitled
    std::uint64_t                                         generation = 0;  // rackGeneration()
    std::string                                           clickDevice;     // connected to; "" = nowhere
    bool                                                  clickMissing = false;
    std::vector<std::string>                              deviceNames;     // playback devices, for the menu
    bool                                                  scanned  = false;
    double                                                lastTick = -1.0;
    std::vector<std::string>                              notices;
};

} // namespace stagehand
