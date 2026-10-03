#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "App/BeatNow.h"
#include "App/RackViewModel.h"
#include "Audio/AuxRouting.h"
#include "Engine/Engine.h"
#include "Midi/MidiInput.h"
#include "Plugin/PluginFactory.h"
#include "Session/Session.h"
#include "Session/Setlist.h"

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

        // Where the last setlist and song are remembered, for reopening there
        // (docs/setlists.md, "Reopening"); empty = not remembered (tests).
        std::string lastPositionPath;

        // MIDI devices that appear while running (hotplug): opened, as the
        // hardware inputs are at start (all inputs listen to them), unless
        // --midi chose the inputs. `openNewSoftware` (--midi all): app and
        // software sources too.
        // From a block's start to its sound at the outputs (AudioBackend::
        // outputLatencySeconds()): the beat display draws what's heard.
        double outputLatencySeconds = 0.0;

        bool openNewInputs   = true;
        bool openNewSoftware = false;

        // The default rig (docs/rig.md), used by a rack on its own and a setlist
        // that names none; created empty if it isn't there. Empty = no rig at
        // all (tests): no buses, and sends go nowhere.
        std::string defaultRigPath;
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
    // The beat heard at `now` (nowNanoseconds()), between ticks: for the beat
    // display and the edge flash, drawn at the display's rate (App/BeatNow.h).
    BeatNow heardBeat(Nanoseconds now) const;
    // Messages for the user since the last call ("Saved to ...", problems).
    std::vector<std::string> takeNotices();

    // --- intents (docs/rack-view.md, section 4). Unknown ids are ignored. ---
    void setGainDb(const std::string& id, float db);
    void setMuted(const std::string& id, bool muted);
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

    // --- the rig (docs/rig.md): its edits mark it modified (Ctrl-S saves it
    // with the rack). Without a rig, they do nothing.
    void setSendDb(const std::string& id, std::size_t bus, std::optional<float> db);   // none = off
    void setReturnDb(std::size_t bus, float db);
    void setBusMuted(std::size_t bus, bool muted);
    void setBusOutput(std::size_t bus, std::uint32_t firstOutput);
    void setMasterDb(float db);
    // `plugin` null empties the slot; its editor closes first. A notice if the
    // plugin can't be loaded (the slot keeps what it had).
    void setBusEffect(std::size_t bus, std::size_t slot, const PluginDescriptor* plugin);
    // A bus effect's editor. Unlike a row's, it stays open through song
    // switches: the rig stays loaded.
    void openBusEditor(std::size_t bus, std::size_t slot);
    void closeBusEditor(std::size_t bus, std::size_t slot);

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
    // Sound these notes of the sequence once, in the rows it plays into (the
    // editor, as notes are added, clicked or moved), ending the ones it sounded
    // before: a note dragged to a new pitch stops sounding at the old one. A
    // few at most: a big selection moved at once would be a cluster, not a
    // check of the pitch.
    // `hold`: they sound until endAuditions() (Session::auditionNote).
    void auditionNotes(const std::string& id, const std::vector<SequenceNote>& notes, bool hold = false);
    // Now, whatever auditionNotes() left sounding; `allNotesOff` as Session::endAuditions.
    void endAuditions(const std::string& id, bool allNotesOff = false);
    static constexpr std::size_t kMaxAuditionNotes = 8;
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

    // --- loopers (docs/looper.md). A new one records `input` into `targets`
    // (rows' ids), quantized to a sixteenth, from the next bar, ended by a
    // press, and is the current looper. Returns its id.
    std::string addLooper(const MidiSourceRef& input, const std::vector<std::string>& targets);
    const LooperSetup* looper(const std::string& id) const;   // as the Session has it; null if none
    void        removeLooper(const std::string& id);
    void        selectLooper(const std::string& id);   // the current looper: the looper switch drives it
    // Leading and trailing spaces go; an empty name is refused (with a notice);
    // at most kMaxNameBytes. Saved with the rack.
    void        renameLooper(const std::string& id, const std::string& name);
    void        setLooperInput(const std::string& id, const MidiSourceRef& input);
    void        setLooperTarget(const std::string& id, const std::string& row, bool on);
    void        setLooperQuantize(const std::string& id, double beats);   // 0 = off
    void        setLooperStart(const std::string& id, bool atBar);        // and its end and stop
    void        setLooperLength(const std::string& id, std::uint32_t count, bool inBars);   // 0 = ended by a press
    void        setLooperCleanup(const std::string& id, bool on);   // fat fingers' slips left out
    // Its row's button: a tap of the looper switch (record, end and play, stop, play).
    void        tapLooper(const std::string& id);
    void        clearLooper(const std::string& id);
    void        undoLooper(const std::string& id);   // the last overdub pass
    // Convert to sequence (docs/looper.md): the loop as a normal sequence in
    // this rack, playing into the looper's rows, stopped, unsaved until
    // Ctrl-S. The looper plays on. It arrives at a tick or two later (the
    // audio thread makes the copy), with a notice.
    void        keepLooperAsSequence(const std::string& id);
    // The looper switch: learned like a trigger (the next note-on or
    // controller press), saved in the rig. A momentary switch, for hold.
    void        learnLooperSwitch();
    void        removeLooperSwitch();
    // Leading and trailing spaces go; an empty name is refused (with a notice);
    // at most kMaxNameBytes. The id stays: rows keep playing it.
    void renameSequence(const std::string& id, const std::string& name);
    static constexpr std::size_t kMaxNameBytes = 64;

    // --- the beat lights and the click (docs/sequences.md, section 7) ---
    void setMetronomeCount(bool everyNote);   // every note (6/8: six), or the counted beats (6/8: two)
    void setClick(bool on);
    void setClickLevel(float db);             // -60 to 0; also turns the click on
    void setClickOutput(const std::string& device);   // also turns the click on
    // Before bar 1 at a start: 0 (none), 0.5, 1 or 2 bars (docs/perform.md);
    // saved with the rack. It clicks whether the click is on or not.
    void setCountIn(double bars);
    // The window's edge flashes on each pulse (docs/perform.md, decision 13);
    // saved with the rack. The count-in flashes whatever it's set to.
    void setFlash(bool on);
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
    // To rackPath(), and the rig too if it has changes, and every other rack
    // with edits kept in memory (Ctrl-S saves everything; docs/setlists.md,
    // section 6). False (with a notice) if untitled, saving is off, or a write
    // failed (that rack stays unsaved).
    bool save();
    // Racks switched away from with unsaved edits: kept in memory, one copy per
    // rack path, which preparing, switching to or opening that rack uses (it's
    // unsaved again then). A live switch never saves (section 6).
    std::size_t unsavedRackCount() const noexcept { return unsavedRacks.size(); }
    // The rig in use: the setlist's, else the default (empty: none).
    const std::string& rigPath() const noexcept { return rigFile; }
    // Save to a new bundle (".rack" added if missing), which becomes the rack's
    // path and its name. An existing rack there is replaced only with `replace`;
    // anything else there never is.
    enum class SaveAsResult { Saved, Failed, NeedsReplace };
    SaveAsResult saveAs(const std::string& chosen, bool replace = false);
    // At quit: save a titled rack and every rack kept unsaved (unless saving is
    // off). False if a save failed.
    bool saveForQuit();
    // Changes whenever another rack replaces the running one: windows tied to
    // its sequences must close (their ids may mean something else now).
    std::uint64_t rackGeneration() const noexcept { return generation; }
    void quit();

    // --- the setlist (docs/setlists.md, section 1). Its edits are saved at
    // once (a setlist has no unsaved state); a rack's are saved as always. ---
    bool               hasSetlist() const noexcept { return setlistOpen; }
    const std::string& setlistPath() const noexcept { return setlistFile; }
    // A new setlist at `chosen` (".setlist" added), opened at once. With
    // `withThisRack`, the running rack is its first song: it must be saved
    // somewhere (an untitled rack can't be a song). An existing setlist is
    // replaced only with `replace`; anything else there never is.
    enum class NewSetlistResult { Created, Failed, NeedsReplace };
    NewSetlistResult newSetlist(const std::string& chosen, bool withThisRack, bool replace = false);
    // Open a setlist and go to `song` (clamped; a missing song: the next one
    // there is). A set with no songs opens with the running rack left as is.
    bool openSetlist(const std::string& chosen, std::size_t song = 0);
    void closeSetlist();   // the running rack stays open, on its own
    // Switch to a song's rack (the running rack is saved first, as Open does),
    // with one graph commit; editors, MIDI learn and sequence windows close.
    // A rack that isn't prepared yet (section 2) is prepared first, one plugin
    // per tick() while the current one plays, and the switch happens when it's
    // ready (true then too; switchPending() meanwhile). Only the last request
    // counts. Another appearance of the rack playing is the same rack: only
    // the position moves. False, with a notice, if its rack is missing or
    // won't load.
    bool goToSong(std::size_t index);
    bool switchPending() const noexcept { return waitingSong.has_value(); }
    // Ready to switch to at once: prepared, or the rack playing.
    bool songPrepared(std::size_t index) const;
    bool nextSong();       // skipping missing songs; false at the end
    bool previousSong();
    // Edits, all saved to the setlist file at once. "After the current song":
    // after the song playing, or where its song was if that was removed.
    bool addRackToSetlist(const std::string& chosen);          // after the current song; no switch
    bool addNewRackToSetlist(const std::string& chosen);       // a copy of the song playing (else empty) saved there; goes to it
    bool addThisRackAgain();                                   // the current song's rack, after it
    bool moveSong(std::size_t index, int delta);               // -1 up, +1 down
    bool renameSong(std::size_t index, const std::string& name);   // "" = the rack's own name
    bool removeSong(std::size_t index);                        // the rack stays on disk
    bool relinkSong(std::size_t index, const std::string& chosen);   // point a (missing) song at another rack
    // Check setlist (milestone 7, edge case 9): each song's rack is read and
    // prepared once, in turn (one plugin per tick, after preparing ahead, as
    // the status bar shows), then dropped, and what reaching it would report
    // is kept: a rack that can't be read, plugins missing or failing, MIDI
    // inputs not connected, triggers clashing with the setlist's. The playing
    // rack is checked as it runs. The song list shows those songs and their
    // problems in amber until the next check, or until switching to the song
    // (its switch's warnings then replace them); a notice sums up. False if
    // there's no setlist; a check under way starts again.
    bool checkSetlist();
    bool checkingSetlist() const noexcept { return check.has_value(); }
    // Its MIDI triggers (section 4), saved to the setlist at once. Learned as
    // the transport's are: Next and Previous from the next note-on or
    // controller press, Song from the next Program Change (program n then goes
    // to song n + 1). A new one replaces the trigger for its action and any
    // other use of its control. They win over a rack's triggers on the same
    // control; a rack that has one gets a warning when it's switched to.
    void learnSetlistTrigger(SetlistTrigger::Action action);
    // How switching behaves (section 5), saved at once: the transport kept
    // running or stopped, at once or at the next bar (while it runs), the old
    // rack rung out or cut.
    void setSwitchSettings(const SwitchSettings& settings);
    void removeSetlistTrigger(std::size_t index);   // index into the setlist's triggers

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
    template <typename Change>
    void           editLooper(const std::string& id, Change change);
    void           buildLoopers();   // model.loopers
    bool           replaceRack(const Rack& rack, const std::string& path, const std::vector<std::string>& warnings);
    // Save the running rack as quitting would, close what belongs to it, and
    // make `next` the running rack (one graph commit). `next` is taken only on
    // success; false (nothing changed) if the save failed.
    // `how`: the setlist's switch settings for a song; Open and New (not live
    // actions) stop the transport and switch at once.
    // `live`: a song switch, which never saves; the rack left keeps its edits
    // in memory (unsavedRacks). Otherwise it's saved first, as at quit, and a
    // failed save changes nothing.
    bool           switchRack(std::unique_ptr<PreparedRack>& next, const std::string& path,
                              const std::vector<std::string>& warnings, const Session::SwitchOptions& how,
                              bool live);
    bool           saveUnsavedRacks();   // every rack in unsavedRacks; a notice for each that fails
    bool           saveCaptured(const Rack& captured);   // to target, as save() does
    // goToSong(); `now`: finish preparing it at once rather than over ticks.
    bool           goTo(std::size_t index, bool now);
    // Read the rack at `bundle` and start preparing it (its plugins come at
    // the next ticks). False if it can't be read: with a notice if `report`.
    bool           startPreparing(const std::string& bundle, bool report);
    // Each tick: keep the next and previous songs' racks prepared, and the one
    // a switch waits for (first), one plugin per tick; switch when it's ready.
    void           prepareAhead();
    // MIDI devices came or went (MidiInput::changes() moved): new ones opened
    // (config.openNewInputs), Session::syncMidi(), the source menus refreshed.
    void           midiChanged();
    void           checkStep();   // checkSetlist(), a step a tick
    void           forgetChecks();   // the set changed: its checks were for another
    void           refreshSongs(double now);   // songCache and songBundles
    int            neighbour(int step) const;  // where Next (1) or Previous (-1) goes, by songCache; -1: none
    // Open the rack bundle at `bundle` (with replaceRack()); false, with a notice, if it can't.
    bool           loadRackAt(const std::string& bundle);
    bool           writeSetlist();          // to setlistFile; a notice if it fails
    // Use the rig at `path` ("" = the default): the one in use is saved first
    // if it has changes, as a rack is when switching. A missing default rig is
    // created empty. Nothing happens if it's already in use.
    void           useRig(const std::string& path);
    bool           saveRigIfModified();     // false (with a notice) if a save failed
    void           rememberPosition();      // config.lastPositionPath
    void           buildSetlist();             // model.setlist
    std::size_t    insertionPoint() const;  // where "after the current song" is
    int            nextPlayable(std::size_t from, int step) const;   // -1: none
    void           setMetronome(const MetronomeSettings& settings);
    // Connect the click to its saved output, if it's on and the device is
    // there; `report`: say so when it isn't (once, until it connects).
    void           routeClick(bool report);
    void           buildDock(double now);
    void           buildRig(double now, double dt);   // model.rig
    void           finishLearn(const Engine::Learned& learned);
    // The setlist's triggers that fired since the last tick, all at once
    // (pressing Next three times goes three songs on), then one switch.
    void           takeSetlistRequests();
    // Where Next (1) or Previous (-1) goes from `from` (a song if `atSong`,
    // else the gap where a removed song was), skipping missing racks; -1: none.
    int            stepFrom(std::size_t from, bool atSong, int step) const;
    void           applySetlistTriggers();   // to the Session; notices for inputs that aren't there
    // The rack's triggers that the setlist's override (edge case 6), as warnings.
    std::vector<std::string> triggerConflicts(const std::string& rackName, const TransportSettings& transport,
                                              const std::vector<const Sequence*>& sequences) const;
    std::string    setlistTriggerText(const SetlistTrigger& trigger) const;
    std::string    triggerText(const SequenceTrigger& trigger) const;

    Session&       session;
    Engine&        engine;
    MidiInput&     midi;
    PluginFactory& factory;
    Config         config;

    RackViewModel                                        model;
    std::map<std::string, std::unique_ptr<EditorHandle>> editors;   // by slot id
    std::map<std::string, RowCache>                      rows;      // by slot id
    std::map<std::pair<std::size_t, std::size_t>, std::unique_ptr<EditorHandle>> busEditors;   // by (bus, slot)
    std::vector<std::string>                              keeping;   // loopers whose copy is on its way
    void                                                  takeKeptLoops();
    std::array<Ballistics, kRigBuses>                     busMeters;
    Ballistics                                            master;
    std::vector<MidiSourceInfo>                           enumerated;
    std::uint64_t                                         midiChanges = 0;   // MidiInput::changes() as last handled
    std::vector<PluginDescriptor>                         available;
    std::vector<double>                                   taps;      // tap-tempo times
    std::map<MidiSourceHandle, std::pair<std::uint32_t, double>> inputActivity;   // last count, lit until
    std::string                                           learningId;   // the sequence being learned for
    SequenceTrigger::Action                               learningAction = SequenceTrigger::Action::Toggle;
    SetlistTrigger::Action                                setlistLearnAction = SetlistTrigger::Action::Next;
    std::string                                           target;          // the rack's bundle; "" = untitled
    std::string                                           rigFile;         // the rig in use; "" = none
    std::uint64_t                                         generation = 0;  // rackGeneration()
    std::string                                           clickDevice;     // connected to; "" = nowhere
    bool                                                  clickMissing = false;
    std::vector<std::string>                              deviceNames;     // playback devices, for the menu
    bool                                                  scanned  = false;
    double                                                lastTick = -1.0;
    std::vector<std::string>                              notices;

    // The open setlist. `song` is the song playing when `onSong`; otherwise
    // where the next song is (its song was removed, or nothing is played yet).
    bool                                                  setlistOpen = false;
    Setlist                                               setlist;
    std::string                                           setlistFile;
    std::size_t                                           song   = 0;
    bool                                                  onSong = false;
    // The songs as the view shows them (names, missing racks): checked on
    // disk at most once a second, and again after any change to the set.
    std::vector<SongModel>                                songCache;
    std::vector<std::string>                              songBundles;   // each song's bundle; "" if missing
    double                                                missingCheckedAt = -1.0;

    // Prepared racks (docs/setlists.md, section 2), one per rack bundle, which
    // every appearance of the rack shares (edge case 4): the next and previous
    // songs', and the one a switch waits for.
    struct Ahead {
        std::unique_ptr<PreparedRack> rack;
        std::vector<std::string>      warnings;   // from reading its rack.json
    };
    std::map<std::string, Ahead>                          prepared;
    std::set<std::string>                                 unpreparable;   // unreadable; not retried until the set changes
    std::optional<std::size_t>                            waitingSong;    // a switch waiting for its rack
    std::map<std::string, Rack>                           unsavedRacks;   // by bundle; unsavedRackCount()
    std::string                                           preparingText;  // model.status.preparing
    // Check setlist: the racks still to check (by bundle, in song order), the
    // one being prepared, and what's been found, by bundle.
    struct Check {
        std::vector<std::string>      bundles;
        std::size_t                   next = 0;
        std::string                   bundle;
        std::unique_ptr<PreparedRack> rack;
        std::vector<std::string>      found;
    };
    std::optional<Check>                                  check;
    std::map<std::string, std::vector<std::string>>       songProblems;
    bool                                                  ticking = false;
};

} // namespace stagehand
