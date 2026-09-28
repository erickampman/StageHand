#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Core/MidiFilter.h"
#include "Core/MidiMessage.h"
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
    std::uint64_t midiDropped   = 0;   // queue or buffer full, or no form a plugin takes
    MidiProtocol  preference    = MidiProtocol::Midi2;
    std::string   saveTarget;          // empty: not saving
    std::string   noSaveReason;
    float         outputDb[2]   = {-60.0f, -60.0f};   // master meter, with ballistics
    bool          outputClipped = false;
};

struct RackViewModel {
    std::string               rackName;
    bool                      modified = false;
    std::vector<RowModel>     rows;
    std::vector<SourceChoice> sources;   // for the source menu: All, virtual inputs, devices
    StatusModel               status;
};

} // namespace stagehand
