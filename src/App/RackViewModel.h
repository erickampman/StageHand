#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Core/MidiFilter.h"
#include "Core/MidiMessage.h"
#include "Core/Sequence.h"
#include "Engine/SequenceSet.h"   // SequenceState
#include "Plugin/PortLayout.h"
#include "Session/Rack.h"

namespace stagehand {

// What the rack view draws (docs/rack-view.md, section 4): plain values, rebuilt
// by RackController each tick, with no pointers into the Session. Widgets read
// it and send intents back to the controller; they never edit it.

enum class RowState {
    Running,       // loaded, enabled, in the engine
    Disabled,      // loaded, not run
    Placeholder,   // the plugin couldn't be loaded; settings kept, saved back unchanged
};

// Which protocol a source delivers, for the row's badge and warning.
enum class SourceProtocol { Mixed, Midi1, Midi2 };

struct RowModel {
    std::string id;
    std::string name;          // the slot's display name
    std::string pluginName;    // from the plugin, or the saved id for a placeholder
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

    // Output side.
    float         gainDb      = 0.0f;
    bool          muted       = false;
    std::uint32_t firstOutput = 0;           // 0-based
    float         meterDb[2]  = {-60.0f, -60.0f};   // with ballistics; -60 = floor
    float         peakHoldDb[2] = {-60.0f, -60.0f};
    bool          clipped     = false;       // latched until cleared
};

struct SourceChoice {
    std::string    label;
    MidiSourceRef  ref;
    SourceProtocol protocol = SourceProtocol::Mixed;
    bool           hardware = false;
};

struct StatusModel {
    double        sampleRate    = 0.0;
    std::uint32_t bufferSize    = 0;
    std::uint64_t xruns         = 0;
    std::uint32_t outputs       = 2;   // for the output-pair choice
    std::uint64_t midiDropped   = 0;   // queue or buffer full, or no form a plugin takes
    MidiProtocol  preference    = MidiProtocol::Midi2;
    std::string   saveTarget;          // empty: not saving
    std::string   noSaveReason;
    float         outputDb[2]   = {-60.0f, -60.0f};   // master meter, with ballistics
    bool          outputClipped = false;
};

// The host transport (docs/sequences.md, section 1). Beats are quarter notes.
struct TransportModel {
    double tempoBpm     = 120.0;
    int    numerator    = 4;
    int    denominator  = 4;
    double beatPosition = 0.0;   // as of the last audio block
    int    beatInBar    = 0;     // 0-based, for the beat lights
    int    beatsInBar   = 4;     // lights to draw (a 6/8 bar is 3 beats; 5/8 rounds up to 3)
    bool   onBeat       = false; // the first part of a beat: the light is lit
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
    double                   noteOffOffsetMs = 0.0;
    std::size_t              noteCount = 0;
    std::vector<std::uint32_t> noteStarts;   // distinct start units, for the dock's mini-map
    std::uint32_t            lastNoteEnd = 0;  // the length can't go below this
    SequenceState            state    = SequenceState::Stopped;
    double                   position = 0.0;  // units into the loop
    std::vector<std::string> usedBy;          // names of the rows that play it
};

// One open MIDI input in the dock's inputs row.
struct InputChipModel {
    std::string    label;
    bool           isVirtual = false;
    SourceProtocol protocol  = SourceProtocol::Mixed;
    bool           active    = false;   // messages arrived a moment ago
};

struct RackViewModel {
    std::string               rackName;
    bool                      modified = false;
    TransportModel            transport;
    std::vector<RowModel>     rows;
    std::vector<SourceChoice> sources;   // for the source menu: All, virtual inputs, devices, sequences
    std::vector<InputChipModel>   inputs;      // the dock's inputs row
    std::vector<SequenceRowModel> sequences;   // the dock's sequence rows
    StatusModel               status;
};

} // namespace stagehand
