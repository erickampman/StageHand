#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "Core/UwNames.h"
#include "uw/plugin/HostedPlugin.h"

namespace stagehand {

class RackController;

// The model behind a generic editor window (docs/generic-editor.md): a
// plugin's parameters as rows to draw, their values read back from the plugin
// each tick, and the window's changes sent to the running plugin through the
// controller. For a plugin with no editor of its own. No DGL: the window turns
// mouse and keys into these calls, and tests call them directly. Main thread.
class ParameterEditor {
public:
    ParameterEditor(RackController& controller, HostedPlugin& plugin);

    // A stepped parameter with two values is a switch; with up to kMenuSteps a
    // menu of its values; anything else a slider (snapping, if stepped).
    enum class Kind { Slider, Switch, Menu };
    static constexpr int kMenuSteps = 24;

    struct Row {
        ParameterInfo info;
        Kind          kind  = Kind::Slider;
        double        value = 0.0;
        std::string   text;   // the value as the plugin writes it, else the number
    };
    // Every parameter but the hidden ones, in the plugin's order.
    const std::vector<Row>& rows() const noexcept { return list; }
    HostedPlugin&           plugin() noexcept { return target; }
    // Bumped whenever a row's value or text changes: the window repaints then.
    std::uint64_t           version() const noexcept { return changes; }

    // Values from the plugin (its own changes, a control link's), except the
    // row being dragged and one set in the last few ticks (the plugin takes a
    // change at its next block). Call each tick.
    void sync();

    // Set row `row` to `value`: clamped to its range, snapped if stepped, and
    // sent to the plugin. A read-only row isn't changed.
    void set(std::size_t row, double value);
    void reset(std::size_t row);   // to its default
    void beginDrag(std::size_t row);
    void endDrag();

    // A Menu row's values, low to high, each with its text.
    std::vector<std::pair<double, std::string>> choices(std::size_t row) const;
    std::string                                 textFor(const ParameterInfo& info, double value) const;
    // The value typed for row `row` ("31", "0.5"); false if it isn't a number.
    static bool parseValue(const std::string& text, double& value);

    // Does `plugin` have anything a generic editor would show?
    static bool editable(const HostedPlugin& plugin);

private:
    double snap(const ParameterInfo& info, double value) const;

    RackController&  controller;
    HostedPlugin&    target;
    std::vector<Row> list;
    std::vector<int> quietTicks;   // per row: ticks left before sync() reads it again
    std::size_t      dragging = SIZE_MAX;
    std::uint64_t    changes  = 0;
};

} // namespace stagehand
