#pragma once

#include <array>
#include <cmath>
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
#include "uw/audio/AuxRouting.h"
#include "Engine/Engine.h"
#include "uw/midi/MidiInput.h"
#include "uw/plugin/PluginFactory.h"
#include "Core/UwNames.h"
#include "Session/RackFile.h"
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
    virtual void setTitle(const std::string&) {}

    std::string title;   // as last set; the controller retitles when it changes
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
        // Opens StageHand's generic editor (docs/generic-editor.md) for a
        // plugin with no editor of its own; only when one is asked for (an
        // Editor button), never by itself. Null: none (no rack window).
        std::function<std::unique_ptr<EditorHandle>(HostedPlugin&, const std::string& title)> openGenericEditor;
        std::function<AudioStatus()> audioStatus;
        std::function<void()>        quit;   // e.g. MainLoop::requestQuit

        // StageHand's own virtual inputs (not listed by MidiInput::enumerate()).
        std::vector<std::pair<std::string, MidiProtocol>> virtualInputs;
        MidiProtocol protocolPreference = MidiProtocol::Midi2;

        // Where the click's outputs are connected (the backend); null: nowhere.
        AuxRouting* clickRouting = nullptr;

        // `savingOff` (--no-save): nothing is ever saved.
        bool savingOff = false;

        // Where the last set and song are remembered, for reopening there
        // (docs/documents.md, "Reopening"); empty = not remembered (tests).
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

        // Whether the untitled set the controller starts with has a rig (an
        // empty one). False: no rig at all (tests), so no buses, and sends go
        // nowhere, until a set with a rig is opened.
        bool rig = false;
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
    // The xrun count shown starts again from 0 (after a soundcheck, say).
    void resetXruns();

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
    // Swap the row's plugin, keeping the row (Session::replacePlugin); closes
    // its editor first. A notice if it can't be.
    void        replacePlugin(const std::string& id, const PluginDescriptor& plugin);
    // Trimmed, at most kMaxNameBytes; an empty one names the row for its plugin.
    void        renameSlot(const std::string& id, const std::string& name);

    // A row's MIDI effects (docs/midi-effects.md). Added at the end (a notice
    // if it can't be); removing one closes its editor first.
    void addMidiEffect(const std::string& id, const PluginDescriptor& plugin);
    void removeMidiEffect(const std::string& id, const std::string& effect);
    void moveMidiEffect(const std::string& id, const std::string& effect, int by);
    void setMidiEffectBypassed(const std::string& id, const std::string& effect, bool bypassed);
    void openMidiEffectEditor(const std::string& id, const std::string& effect);
    void closeMidiEffectEditor(const std::string& id, const std::string& effect);

    // A row's editor: the plugin's own, or the generic one for a plugin
    // without one (and parameters to show). openAllEditors() opens only the
    // plugins' own.
    void openEditor(const std::string& id);
    void closeEditor(const std::string& id);
    // A parameter set from StageHand's own controls (the generic editor): to
    // the running plugin at its next block; the rack (or the rig, for a bus
    // effect) is modified.
    void setParameter(HostedPlugin& plugin, std::uint32_t id, double value);
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
    void setSequenceSwing(const std::string& id, double swing);   // 0.5 (straight) to 0.75
    void setSequenceSlop(const std::string& id, double ms);       // 0 to 50
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
    // Panic (docs/panic.md): all notes off and pedals up in every row, at the
    // next block; a second within 2 s also cuts the sound. Its control is
    // learned like the looper switch's and kept in the rig.
    void        panic();
    void        learnPanic();
    void        removePanic();

    // --- control links (docs/controls.md) ---
    // A plugin's parameters, for choosing one from a list: a row's, or a bus
    // effect's. Empty for a placeholder or an empty slot.
    struct ParameterChoice {
        std::uint32_t id = 0;
        std::string   name;
        double        minValue = 0.0, maxValue = 1.0;
        bool          stepped  = false;
    };
    std::vector<ParameterChoice> rowParameters(const std::string& rowId) const;
    std::vector<ParameterChoice> effectParameters(std::size_t bus, std::size_t effect) const;
    // Learn: the next controller moved drives `target`, over the parameter's
    // whole range (a level: -60 to 0 dB). A link in `scope` on that control
    // takes it as another target (a macro); otherwise it's a new link. For all
    // songs, a row's parameter becomes that plugin's in every row; a row's gain
    // or send can't be. A note, or a control a trigger uses, is refused (a notice).
    void learnControlLink(ControlLinkTarget target, ControlScope scope);
    // Replace link `index` as edited (mode, pass on, targets' ranges and
    // curves, targets removed; none left removes it).
    void setControlLink(ControlScope scope, std::size_t index, ControlLinkSetup link);
    void removeControlLink(ControlScope scope, std::size_t index);
    // Move a link between this song and all songs: where it is now (its
    // index in `to`: at the end, or merged into a link there on the same
    // control), or none (with a notice) if it can't go: a row's gain or send,
    // or (to this song) a plugin no row has.
    std::optional<std::size_t> setControlLinkScope(ControlScope scope, std::size_t index, ControlScope to);
    // Learn again: the next controller moved becomes link `index`'s control
    // (refused, with a notice, as learnControlLink() refuses, and if another
    // link in its scope has that control).
    void relearnControlLink(ControlScope scope, std::size_t index);
    // One more target for link `index`, over the parameter's whole range (a
    // level: -60 to 0 dB), as learnControlLink() makes one; a target it has
    // already is reset to that.
    void addControlLinkTarget(ControlScope scope, std::size_t index, ControlLinkTarget target);
    // Made on screen, no hardware needed (docs/controls.md, milestone 5): a
    // link on `control` (its source, group, channel and number; its other
    // fields are ignored) that drives `target`, as learnControlLink() makes
    // one once that control moves; a link in `scope` on that control takes it
    // as another target. Refused, with a notice, as learning refuses. Its
    // index in `scope`, or none.
    std::optional<std::size_t> makeControlLink(const ControlLinkSetup& control, ControlLinkTarget target,
                                               ControlScope scope);
    // Link `index`'s control, chosen on screen: as Learn again sets it, and
    // refused as Learn again refuses (a notice). False if refused.
    bool setControlLinkControl(ControlScope scope, std::size_t index, const ControlLinkSetup& control);
    // What already answers a control, for marking it in a list: a trigger
    // ("the transport's", "Panic's"...), or "another Control Link" in
    // `scope` (link `except` aside); "" if nothing does.
    std::string controlUse(const ControlLinkSetup& control, ControlScope scope,
                           std::optional<std::size_t> except = std::nullopt) const;
    // Learn from the editor (docs/controls.md, milestone 4): `place` is a
    // row's plugin (`row`) or a bus effect (`bus`, `effect`); its editor
    // opens (the generic one if it has none), and the first parameter touched
    // there becomes the target; then the next controller moved drives it, as
    // learnControlLink(). cancelLearn() stops it.
    void learnControlLinkFromEditor(ControlLinkTarget place, ControlScope scope);
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

    // --- the set (docs/documents.md): the one document, always open. The
    // controller starts with an untitled set whose one song is the running
    // rack. Racks are kept by their bundle in the set's package (for an
    // untitled set, a path under "Racks/" that's never on disk). ---
    // The set's package; empty for an untitled set (Save As names it).
    const std::string& setPath() const noexcept { return setFile; }
    // The running rack's name in the set ("So what").
    std::string        songRack() const;
    bool               savingOff() const noexcept { return config.savingOff; }
    // Anything Save would write: the song list, the running rack, other songs'
    // racks edited, the rig. Close and Quit ask first when there is.
    bool unsavedChanges() const;
    // Quitting without saving (the user said so): saveForQuit() then does nothing.
    void discardChanges() noexcept { discarding = true; }
    // Replace the set with a new, untitled one: one empty song, and a copy of
    // the rig in use. Edits to the set left are dropped (the UI asks first).
    void newSet();
    // Replace the set with the one at `chosen`, at `song` (clamped): a set
    // package; or an older `.setlist` file or a lone `.rack` bundle, made into
    // an untitled set (their files left alone). False (with a notice, nothing
    // changed) if it can't be read.
    bool openSet(const std::string& chosen, std::size_t song = 0);
    // The whole set to setPath(): the song list, the rig if it changed, every
    // rack with edits (the running one's and those kept in memory), and racks
    // no song uses are removed. False (with a notice) if untitled, saving is
    // off, or the write failed (everything stays unsaved).
    bool save();
    // Racks switched away from with edits (and every rack of an untitled set
    // but the running one): kept in memory until the set is saved.
    std::size_t unsavedRackCount() const noexcept { return unsavedRacks.size(); }
    // Save to a new package (".setlist" added if missing), which becomes the
    // set's file. An existing set there is replaced only with `replace`;
    // anything else there never is.
    enum class SaveAsResult { Saved, Failed, NeedsReplace };
    SaveAsResult saveAs(const std::string& chosen, bool replace = false);
    // At quit: a titled set with changes is saved, unless saving is off or
    // discardChanges() was called. False if the save failed.
    bool saveForQuit();
    // Changes whenever another rack replaces the running one: windows tied to
    // its sequences must close (their ids may mean something else now).
    std::uint64_t rackGeneration() const noexcept { return generation; }
    void quit();

    // --- songs (docs/setlists.md, sections 1 to 5). Edits mark the set
    // edited; Save writes them. ---
    // Switch to a song's rack, with one graph commit; editors, MIDI learn and
    // sequence windows close. The rack left keeps its edits in memory.
    bool goToSong(std::size_t index);
    bool switchPending() const noexcept { return waitingSong.has_value(); }
    // Ready to switch to at once: prepared, or the rack playing.
    bool songPrepared(std::size_t index) const;
    bool nextSong();       // skipping missing songs; false at the end
    bool previousSong();
    // Edits. "After the current song": where the next song goes.
    // A copy of the song playing, as a rack of its own ("So what 2"), after
    // it; it's played.
    bool newSong();
    // A copy of the rack bundle at `chosen` (a bundle, or its rack.json), as a
    // rack of the set's own, after the current song; no switch.
    bool importRack(const std::string& chosen);
    bool addThisRackAgain();                                   // the current song's rack, after it (a reprise)
    bool moveSong(std::size_t index, int delta);               // -1 up, +1 down
    bool renameSong(std::size_t index, const std::string& name);   // "" = the rack's own name
    // A set keeps at least one song: false for the last one. Removing the song
    // playing goes to the next (else the previous) first. Its rack goes when
    // the set is saved, unless another song uses it.
    bool removeSong(std::size_t index);
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
    // A DSP load shown steadily: the worst of a second's takes, for the next second.
    struct LoadWindow {
        float  shown = 0.0f, pending = 0.0f;
        double since = -1.0;
        void   update(float peak, double now) noexcept;
        int    percent() const noexcept { return static_cast<int>(std::lround(shown * 100.0f)); }
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
        LoadWindow          load;
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
    // Make `next` the running rack (one graph commit), closing what belonged
    // to the one left, which keeps its edits in memory (unsavedRacks) if it has
    // some, or if it isn't on disk. `next` is taken only on success.
    // `how`: the setlist's switch settings for a live switch; opening a set
    // stops the transport and switches at once.
    bool           switchRack(std::unique_ptr<PreparedRack>& next, const std::string& path,
                              const std::vector<std::string>& warnings, const Session::SwitchOptions& how,
                              bool live);
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
    bool           setEdited();             // the song list changed: marked edited, the songs checked again
    // A song's rack, by its name in the set: its bundle (the key in prepared,
    // unsavedRacks and the like), whether it's there, and reading it (the
    // copy in memory if there is one).
    std::string    bundleOf(const std::string& rack) const;
    bool           rackThere(const std::string& bundle) const;
    RackFileResult readRack(const std::string& bundle, Rack& rack) const;
    std::set<std::string> rackNames() const;   // the set's, for a new one's name
    // Make the set running `next` (its file "" for untitled), with `racks` (by
    // name) held in memory and `rig` in use, at `start`. Everything of the set
    // left is dropped.
    bool           adoptSet(Set next, const std::string& file, const Rig& rig, std::map<std::string, Rack> racks,
                            std::size_t start, bool edited);
    // Write the set to `path` (from setFile, if it's another); on success it's
    // the set's file and nothing is unsaved.
    bool           writeSet(const std::string& path);
    void           useRig(const Rig& rig);   // into the Session (busEditors close)
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
    void           logXrun() const;   // a new xrun, with the song and the bar, on stderr
    void           buildControlLinks();   // model.controlLinks
    std::string    controlText(const ControlLinkSetup& link) const;   // "KeyLab 88, CC 74, ch 1"
    std::string    targetText(const ControlLinkTarget& target, bool& present) const;
    // Which trigger already answers this controller, if one does ("Transport", a sequence's name...).
    std::string    triggerOn(const MidiSourceRef& source, std::uint8_t group, std::uint8_t channel,
                             std::uint8_t number) const;
    // `target` as `scope` keeps it (for all songs, a row's parameter by its
    // plugin); false if it can't be in that scope.
    bool           targetFor(ControlScope scope, ControlLinkTarget& target, std::vector<ControlLinkTarget>& out) const;
    void           addLinkTarget(ControlLinkSetup link, ControlScope scope);   // merged with one on its control
    ControlLinkTarget linkTarget;    // learning: what the control will drive (from the editor: the place)
    void              takeTouches();  // learning from the editor: the parameter touched there
    std::string       placeText(const ControlLinkTarget& place) const;   // "Keys", "Bus A effect 1"
    bool              touchArmed = false;              // engine.armTouch(true) is in force
    std::optional<std::uint32_t> touchRefused;         // a parameter touched that can't be linked (said once)
    ControlScope      linkScope = ControlScope::Song;
    std::optional<std::size_t> linkRelearn;   // learning again: link (in linkScope) whose control it is
    // A target as learnControlLink() starts it: the parameter's whole range and
    // its name now, or -60 to 0 dB. False (with a notice) if it can't be in `scope`.
    bool              startTarget(ControlLinkTarget& target, ControlScope scope);
    // The plugins' parameters as last asked, forgotten when the graph changes
    // (the model's ranges, each tick).
    const std::vector<ParameterChoice>& parametersOf(const HostedPlugin* plugin) const;
    // The plugin whose parameter `target` is (all songs': the first row with it); null if none.
    const HostedPlugin*                 targetPlugin(const ControlLinkTarget& target) const;
    mutable std::map<const HostedPlugin*, std::vector<ParameterChoice>> parameterCache;
    mutable std::uint64_t                                               parameterCacheAt = ~std::uint64_t{0};
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
    // By slot id, and "<slot id>/<effect id>" for a row's MIDI effect.
    std::map<std::string, std::unique_ptr<EditorHandle>> editors;
    std::map<std::string, RowCache>                      rows;      // by slot id
    // An editor for `plugin`: its own, or the generic one if it has none (null
    // if neither). editable(): would there be one?
    std::unique_ptr<EditorHandle> openAnyEditor(HostedPlugin& plugin, const std::string& title);
    static std::string            midiEffectTitle(const std::string& row, const std::string& effect, bool bypassed);
    bool                          editable(const HostedPlugin& plugin) const;
    std::map<std::pair<std::size_t, std::size_t>, std::unique_ptr<EditorHandle>> busEditors;   // by (bus, slot)
    std::vector<std::string>                              keeping;   // loopers whose copy is on its way
    void                                                  takeKeptLoops();
    std::array<Ballistics, kRigBuses>                     busMeters;
    Ballistics                                            master;
    std::array<LoadWindow, kRigBuses>                     busLoads;
    LoadWindow                                            dspLoad;
    std::uint64_t                                         xrunsBase = 0;     // the backend's count at the last reset
    std::uint64_t                                         xrunsSeen = 0;     // the backend's count, as of the last tick
    double                                                xrunAt    = -1.0;  // when the last was seen (tick time)
    static constexpr double                               kXrunRecentSeconds = 5.0;
    std::vector<MidiSourceInfo>                           enumerated;
    std::uint64_t                                         midiChanges = 0;   // MidiInput::changes() as last handled
    std::vector<PluginDescriptor>                         available;
    std::vector<double>                                   taps;      // tap-tempo times
    std::map<MidiSourceHandle, std::pair<std::uint32_t, double>> inputActivity;   // last count, lit until
    std::string                                           learningId;   // the sequence being learned for
    SequenceTrigger::Action                               learningAction = SequenceTrigger::Action::Toggle;
    SetlistTrigger::Action                                setlistLearnAction = SetlistTrigger::Action::Next;
    std::string                                           target;          // the running rack's bundle
    std::uint64_t                                         generation = 0;  // rackGeneration()
    std::string                                           clickDevice;     // connected to; "" = nowhere
    bool                                                  clickMissing = false;
    std::vector<std::string>                              deviceNames;     // playback devices, for the menu
    bool                                                  scanned  = false;
    double                                                lastTick = -1.0;
    std::uint32_t                                         panicsSeen = 0;     // engine.panics().count, as of the last tick
    double                                                panicAt    = -1.0;  // when the last was seen (tick time)
    static constexpr double                               kPanicLitSeconds = 0.5;
    std::vector<std::string>                              notices;

    // The set. `song` is the song playing when `onSong` (only false when a
    // set's racks couldn't be read).
    Set                                                   setlist;
    std::string                                           setFile;   // its package; "" = untitled
    bool                                                  edited     = false;   // its song list, since saved
    bool                                                  discarding = false;   // discardChanges()
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
