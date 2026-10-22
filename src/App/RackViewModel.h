#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "Core/MidiFilter.h"
#include "uw/ump/MidiMessage.h"
#include "Core/Sequence.h"
#include "Engine/SequenceSet.h"   // SequenceState
#include "uw/plugin/PortLayout.h"
#include "Core/UwNames.h"
#include "Session/Rack.h"
#include "Session/Rig.h"
#include "Session/Setlist.h"

namespace stagehand {

// What the rack view draws (docs/rack-view.md, section 4): plain values, rebuilt
// by RackController each tick, with no pointers into the Session. Widgets read
// it and send intents back to the controller; they never edit it. Comparable, so
// the rack window repaints only when it changes.

enum class RowState {
    Running,       // loaded, in the engine (muted or not)
    Placeholder,   // the plugin couldn't be loaded; settings kept, saved back unchanged
};

// Which protocol a source delivers, for the row's badge and warning.
enum class SourceProtocol { Mixed, Midi1, Midi2 };

// One of a row's MIDI effects (docs/midi-effects.md), in order.
struct MidiEffectModel {
    std::string id;
    std::string name;
    std::string problem;            // why it didn't load: the row's MIDI passes it by
    bool        bypassed   = false;
    bool        hasEditor  = false;
    bool        editorOpen = false;

    bool operator==(const MidiEffectModel&) const = default;
};

struct RowModel {
    std::string id;
    std::string name;          // the slot's display name
    std::string pluginName;    // from the plugin, or the saved id for a placeholder
    std::string pluginId;      // the saved one
    std::string vendor;
    RowState    state = RowState::Running;
    std::string problem;       // placeholder: why it didn't load

    bool hasEditor  = false;
    bool editorOpen = false;

    // Input side.
    MidiSourceRef   source;
    std::string     sourceLabel;             // "All inputs", "KeyLab 88", "KeyLab 88 (not connected)"
    bool            sourceConnected = true;
    SourceProtocol  sourceProtocol  = SourceProtocol::Mixed;
    NoteDialectMask pluginDialects  = 0;     // of the slot's note port
    // A MIDI 2.0 source feeds a plugin that takes only MIDI 1.0: per-note data
    // such as pitch 7.9 is lost in translation.
    bool            protocolWarning = false;
    MidiFilter      filter;                  // channels, groups, ranges, transpose
    bool            midiActive      = false; // MIDI reached the plugin in the last moment
    std::vector<MidiEffectModel> midiEffects;   // in front of the plugin, in order

    // Output side.
    float         gainDb      = 0.0f;
    bool          muted       = false;
    std::uint32_t firstOutput = 0;           // 0-based
    float         meterDb[2]  = {-60.0f, -60.0f};   // with ballistics; -60 = floor
    float         peakHoldDb[2] = {-60.0f, -60.0f};
    bool          clipped     = false;       // latched until cleared
    // Its sends to the rig's buses A and B (docs/rig.md), after the fader; none = off.
    std::array<std::optional<float>, kRigBuses> sendDb;
    int           loadPercent = 0;           // its plugin's share of the period, the worst of the last second

    bool operator==(const RowModel&) const = default;
};

// The rig (docs/rig.md): the effect buses every song shares, and the master.
struct BusEffectModel {
    std::string name;              // the plugin's, or the saved id when it didn't load; "" = empty slot
    std::string problem;           // why it didn't load (the slot passes audio on)
    bool        hasEditor  = false;
    bool        editorOpen = false;

    bool operator==(const BusEffectModel&) const = default;
};
struct BusModel {
    std::string                                   name;   // "A", "B"
    std::array<BusEffectModel, RigBus::kEffects>  effects;
    float                                         returnDb    = 0.0f;
    bool                                          muted       = false;
    std::uint32_t                                 firstOutput = 0;
    float                                         meterDb[2]    = {-60.0f, -60.0f};   // the return's, with ballistics
    float                                         peakHoldDb[2] = {-60.0f, -60.0f};
    int                                           loadPercent   = 0;   // its effects', as a row's

    bool operator==(const BusModel&) const = default;
};
struct RigModel {
    bool                             loaded   = false;   // no rig: no buses, and sends go nowhere
    std::string                      name;
    bool                             modified = false;   // Ctrl-S saves it with the rack
    float                            masterDb = 0.0f;
    std::array<BusModel, kRigBuses>  buses;

    bool operator==(const RigModel&) const = default;
};

struct SourceChoice {
    std::string    label;
    MidiSourceRef  ref;
    SourceProtocol protocol = SourceProtocol::Mixed;
    bool           hardware = false;

    bool operator==(const SourceChoice&) const = default;
};

struct StatusModel {
    double        sampleRate    = 0.0;
    std::uint32_t bufferSize    = 0;
    std::uint64_t xruns         = 0;   // since StageHand started, or the count was reset
    int           xrunSecondsAgo = -1; // since the last one; -1: none counted
    bool          xrunRecent    = false;   // in the last few seconds: shown red
    int           dspPercent    = 0;   // DSP load (docs/panic.md): the average, about a second
    int           dspPeakPercent = 0;  // and the worst block of the last second
    std::uint32_t outputs       = 2;   // for the output-pair choice
    std::uint64_t midiDropped   = 0;   // queue or buffer full, or no form a plugin takes
    MidiProtocol  preference    = MidiProtocol::Midi2;
    std::string   saveTarget;          // the rack's bundle; empty: untitled, or saving is off
    std::string   noSaveReason;        // shown instead, when saveTarget is empty
    std::string   preparing;           // "Preparing song 4 (So what): 2/5 plugins"; "" when idle
    float         outputDb[2]   = {-60.0f, -60.0f};   // master meter, with ballistics
    bool          outputClipped = false;

    bool operator==(const StatusModel&) const = default;
};

// The host transport (docs/sequences.md, section 1). Beats are quarter notes.
struct TransportModel {
    double tempoBpm     = 120.0;
    int    numerator    = 4;
    int    denominator  = 4;
    double beatPosition = 0.0;   // as of the last audio block
    int    beatInBar    = 0;     // 0-based, for the beat lights
    int    beatsInBar   = 4;     // lights to draw: counted beats (6/8: 2 dotted beats; 7/8: 7 eighths)
    bool   onBeat       = false; // the first part of a beat: the light is lit (never while stopped)
    bool   running      = false; // Start/Stop (docs/sequences.md, section 7)
    int    countIn      = 0;     // counting in: the pulse sounding, counted down (4, 3, 2, 1); 0: not (docs/perform.md)
    // Its MIDI control: triggers described as a sequence row's are; start on
    // first note ("" when off, else the input: "All inputs", "KeyLab 88").
    std::vector<std::string> triggers;
    bool                     learning       = false;
    SequenceTrigger::Action  learningAction = SequenceTrigger::Action::Toggle;
    std::string              startOnNote;
    bool                     startOnNoteConnected = true;

    // The lights' count and the click.
    bool                     everyNote      = false;
    bool                     click          = false;
    float                    clickLevelDb   = -12.0f;
    double                   countInBars    = 0.0;     // 0, 0.5, 1 or 2
    bool                     flash          = false;   // the window's edge flashes on each pulse
    std::string              clickOutput;              // chosen device; "" = none
    bool                     clickConnected = false;   // to it, now
    std::vector<std::string> playbackDevices;          // as of refreshPlaybackDevices()
    std::string              mainDevice;               // where the main outputs go ("" if unknown)

    // Panic (docs/panic.md): lit for a moment after one; its learned control,
    // in the rig ("" when none), as the looper switch's is shown.
    bool                     panicLit       = false;
    std::string              panicControl;
    bool                     panicConnected = true;
    bool                     panicLearning  = false;

    bool operator==(const TransportModel&) const = default;
};

// One sequence in the sources dock (docs/sequences.md, section 4).
struct SequenceRowModel {
    std::string              id;
    std::string              name;
    MidiProtocol             protocol = MidiProtocol::Midi2;
    int                      edo      = 12;
    SequenceRate             rate;
    std::uint32_t            length   = 16;
    LaunchQuantize           launch   = LaunchQuantize::Bar;
    bool                     repeat   = true;
    bool                     atStart  = false;   // starts with the transport
    double                   noteOffOffsetMs = 0.0;
    std::size_t              noteCount = 0;
    std::vector<std::uint32_t> noteStarts;   // distinct start units, for the dock's mini-map
    std::uint32_t            lastNoteEnd = 0;  // the length can't go below this
    SequenceState            state    = SequenceState::Stopped;
    double                   position = 0.0;  // units into the loop
    std::vector<std::string> usedBy;          // names of the rows it plays into (its targets)
    std::vector<std::string> targetIds;       // the same rows, by id (Sequence::targets)
    // MIDI triggers, described ("Play/Stop: KeyLab 88, note C1, ch 10"), in
    // Sequence::triggers order; `learning` while MIDI learn waits for one.
    std::vector<std::string> triggers;
    bool                     learning = false;
    SequenceTrigger::Action  learningAction = SequenceTrigger::Action::Toggle;

    bool operator==(const SequenceRowModel&) const = default;
};

// One open MIDI input in the dock's inputs row.
struct InputChipModel {
    std::string    label;
    bool           isVirtual = false;
    SourceProtocol protocol  = SourceProtocol::Mixed;
    bool           active    = false;   // messages arrived a moment ago
    bool           connected = true;    // false: its device left (it's heard again when it's back)

    bool operator==(const InputChipModel&) const = default;
};

// A looper's row in the dock, under the sequences (docs/looper.md).
struct LooperRowModel {
    std::string              id;
    std::string              name;
    std::string              inputLabel;          // what it records: "All inputs", "KeyLab 88"
    bool                     inputConnected = true;
    std::vector<std::string> targetIds;           // the rows it plays into, in rack order
    std::vector<std::string> targetNames;
    LooperState              state   = LooperState::Empty;
    bool                     waiting = false;     // a start, play or stop waits for its beat or bar
    bool                     current = false;     // the one the looper switch drives
    double                   lengthBeats = 0.0;   // 0 until the first recording ends
    double                   position    = 0.0;   // beats into the loop, while it plays
    std::uint32_t            notes    = 0;
    std::uint32_t            capacity = 0;
    std::uint32_t            dropped  = 0;        // past the cap
    std::uint32_t            passes   = 0;        // the recording, then each overdub kept (undo while > 1)
    double                   quantize    = 0.25;  // in beats; 0 = off
    bool                     atBar       = true;
    std::uint32_t            fixedLength = 0;     // 0 = ended by a press
    bool                     fixedInBars = true;
    bool                     cleanup     = true;  // fat fingers' slips left out

    bool operator==(const LooperRowModel&) const = default;
};

// The open setlist (docs/setlists.md), for the setlist row and its song list.
struct SongModel {
    std::string name;      // the song's name in the set, else its rack's
    std::string rackName;  // the rack bundle's name
    bool        missing = false;   // no rack at its path: skipped by Next and Previous
    bool        prepared = false;  // a switch to it is instant (docs/setlists.md, section 2)
    bool        unsaved  = false;  // its rack has edits not yet saved (section 6)
    std::vector<std::string> problems;   // what Check setlist found (milestone 7); empty: none, or not checked

    bool operator==(const SongModel&) const = default;
};
struct SetlistModel {
    bool                   open = false;
    std::string            name;          // the file's name
    std::vector<SongModel> songs;
    // The song playing, or -1: none yet (an empty set), or its song was removed
    // (the rack still plays; Next goes to the song that followed it).
    int                    current = -1;
    int                    next     = -1;  // where Next goes (missing songs skipped), or -1
    int                    previous = -1;  // where Previous goes, or -1
    int                    waiting  = -1;  // the song a switch waits for (its rack being prepared), or -1
    SwitchSettings         switching;      // section 5
    bool                   checking = false;   // Check setlist under way
    // Its MIDI triggers (section 4), as text ("Next song: Keys, CC 80, ch 1"), and MIDI learn for one.
    std::vector<std::string> triggers;
    bool                     learning       = false;
    SetlistTrigger::Action   learningAction = SetlistTrigger::Action::Next;

    bool operator==(const SetlistModel&) const = default;
};

// A control link (docs/controls.md) as the Control Links menu and dialog show it.
struct ControlLinkTargetModel {
    ControlLinkTarget target;        // as saved: its range, curve, and what it names
    std::string       text;          // "Keys: Cutoff", "Keys gain", "Bus A return", "Master"
    bool              present = true; // there now (else left out until it is)
    std::vector<std::string> rows;   // the rows it acts on (a row's; all songs': every row with its plugin)
    // The scale its range bar shows: the parameter's (as last seen, else its
    // own bottom and top), or the fader's in dB for a level.
    double                minValue = 0.0, maxValue = 1.0;
    bool                  stepped  = false;
    // Live (the engine's): where it is now, in the same units; none if it
    // isn't running (overridden, its input or plugin not there). Waiting: the
    // control has moved but not reached it yet (pick-up).
    std::optional<double> now;
    bool                  waiting = false;

    bool operator==(const ControlLinkTargetModel&) const = default;
};
struct ControlLinkModel {
    ControlScope  scope = ControlScope::Song;
    std::size_t   index = 0;          // in its scope's list
    std::string   control;            // "KeyLab 88, CC 74, ch 1"
    bool          connected  = true;  // its input is there
    bool          pickUp     = true;
    bool          passOn     = false;
    bool          overridden = false; // all songs', with a song's own link on the same control
    // The control, as saved (with `pickUp`, `passOn` and the targets', the link to edit).
    MidiSourceRef source;
    std::uint8_t  group = 0, channel = 0, number = 0;
    double        travel   = -1.0;    // live: the control's position (0 to 1), -1 not moved yet
    bool          learning = false;   // its control is being learned again
    std::vector<ControlLinkTargetModel> targets;

    ControlLinkSetup setup() const {   // as saved
        ControlLinkSetup link;
        link.source  = source;
        link.group   = group;
        link.channel = channel;
        link.number  = number;
        link.pickUp  = pickUp;
        link.passOn  = passOn;
        for (const ControlLinkTargetModel& t : targets) link.targets.push_back(t.target);
        return link;
    }

    bool operator==(const ControlLinkModel&) const = default;
};

struct RackViewModel {
    std::string               rackName;
    bool                      modified = false;
    std::size_t               otherUnsaved = 0;   // other racks with edits kept in memory (Ctrl-S saves them)
    TransportModel            transport;
    std::vector<RowModel>     rows;
    std::vector<SourceChoice> sources;   // for a row's input menu: All, None, devices, virtual inputs
    std::vector<InputChipModel>   inputs;      // the dock's inputs row
    std::vector<SequenceRowModel> sequences;   // the dock's sequence rows
    std::vector<LooperRowModel>   loopers;     // the dock's looper rows, under the sequences
    // The looper switch (in the rig): "" = none; else as a trigger is described.
    std::string                   looperSwitch;
    bool                          looperSwitchConnected = true;
    bool                          looperSwitchLearning  = false;
    StatusModel               status;
    SetlistModel              setlist;
    RigModel                  rig;
    // Control links (docs/controls.md): this song's, then all songs'.
    std::vector<ControlLinkModel> controlLinks;
    bool                          controlLinkLearning = false;
    ControlLinkTarget             controlLinkLearnTarget;   // while learning a new one: what it will drive

    bool operator==(const RackViewModel&) const = default;
};

} // namespace stagehand
