#pragma once

#include <cstdint>
#include <vector>

#include "Core/MidiMessage.h"

namespace stagehand {

class HostedPlugin;
struct SlotRuntime;

// How a control's travel spreads over a target's range (docs/controls.md, 3).
enum class ControlCurve : std::uint8_t {
    Linear,
    SlowStart,   // little change at first, more toward the top (the travel squared)
    FastStart,   // the reverse (its square root)
};

// A control link (docs/controls.md) compiled for the audio thread: a
// controller, and the targets it drives. Published with the sequences
// (TransportControl::links), so a song's links switch with its graph.
struct CompiledControlLink {
    // The source: a controller, from an input or any (kInvalidMidiSource).
    MidiSourceHandle source = kInvalidMidiSource;
    std::uint8_t     group = 0, channel = 0, number = 0;
    bool             pickUp = true;    // else jump
    bool             passOn = false;   // else consumed: it goes nowhere else

    struct Target {
        enum class Kind : std::uint8_t {
            Parameter,   // a plugin's parameter: a row's or a bus effect's
            Gain,        // a row's link level, after its fader
            Send,        // a row's send to bus `param` (0 or 1), after its send level
            Return,      // a bus's link level, after its return
            Master,      // the master's link level
        };
        Kind kind = Kind::Parameter;
        // What it acts on, found in the current graph by these pointers (they're
        // compared, never followed): the plugin for Parameter; the row's
        // SlotRuntime for Gain and Send, the bus's for Return.
        const HostedPlugin* plugin  = nullptr;
        const SlotRuntime*  runtime = nullptr;
        std::uint32_t       param   = 0;   // Parameter: its id; Send: the bus
        // At the control's bottom and at its top (top below bottom reverses it):
        // the parameter's plain value, or dB for a level.
        double       bottom  = 0.0;
        double       top     = 1.0;
        ControlCurve curve   = ControlCurve::Linear;
        bool         stepped = false;   // a stepped parameter: the nearest step
        double       initial = 0.0;     // Parameter: its value when compiled (paramValue())
        // What it was compiled from, for showing it (Engine::linkStates()):
        // opaque to the engine (the Session's scope, link and target).
        std::uint64_t origin = 0;

        // --- audio thread: where the parameter is, and picking up
        double current  = 0.0;
        bool   picked   = false;   // pick-up: the control has reached it
        bool   hasLast  = false;   // the control's last mapped value, while waiting
        double last     = 0.0;

        // The value for a control at `travel` (0 to 1).
        double map(double travel) const noexcept;
        // Pick-up: the control's mapped value `mapped` against `now` (where the
        // target is). True once it has reached it (within 2% of the range, or
        // crossed it since the last move).
        bool pickUpAt(double mapped, double now) noexcept;
        // The target changed some other way: it's `value` now, and pick-up waits again.
        void moved(double value) noexcept;
        bool sameTarget(const Target& other) const noexcept;
    };
    std::vector<Target> targets;
    double              travel = -1.0;   // audio thread: the control's last position (0 to 1), -1 not moved yet

    // Is `message` this link's controller? `travel`: its value, 0 to 1 (7-bit,
    // or MIDI 2.0's 32-bit).
    bool matches(MidiSourceHandle from, const MidiMessage& message, double& travel) const noexcept;
    bool sameSource(const CompiledControlLink& other) const noexcept;
};

// A link level in dB as a gain (-60 dB and below: silence).
float linkLevelGain(double db) noexcept;

} // namespace stagehand
