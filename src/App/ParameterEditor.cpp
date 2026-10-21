#include "App/ParameterEditor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "App/RackController.h"

namespace stagehand {

namespace {

// Ticks after a set() before the plugin's own value is shown again: it takes
// the change at its next block, a few milliseconds away (33 ms ticks).
constexpr int kQuietTicks = 3;

ParameterEditor::Kind kindOf(const ParameterInfo& info) {
    if (!info.isStepped) return ParameterEditor::Kind::Slider;
    const double steps = std::round(info.maxValue - info.minValue);
    if (steps == 1.0) return ParameterEditor::Kind::Switch;
    if (steps >= 1.0 && steps < ParameterEditor::kMenuSteps) return ParameterEditor::Kind::Menu;
    return ParameterEditor::Kind::Slider;
}

} // namespace

ParameterEditor::ParameterEditor(RackController& rackController, HostedPlugin& plugin)
    : controller(rackController), target(plugin) {
    for (const ParameterInfo& info : plugin.parameters()) {
        if (info.isHidden) continue;
        Row row;
        row.info  = info;
        row.kind  = kindOf(info);
        row.value = plugin.paramValue(info.id);
        row.text  = textFor(info, row.value);
        list.push_back(std::move(row));
    }
    quietTicks.assign(list.size(), 0);
}

bool ParameterEditor::editable(const HostedPlugin& plugin) {
    const std::vector<ParameterInfo> params = plugin.parameters();
    return std::any_of(params.begin(), params.end(), [](const ParameterInfo& p) { return !p.isHidden; });
}

std::string ParameterEditor::textFor(const ParameterInfo& info, double value) const {
    if (std::string text; target.paramValueText(info.id, value, text) && !text.empty()) return text;
    char buffer[32];
    if (info.isStepped)
        std::snprintf(buffer, sizeof buffer, "%lld", static_cast<long long>(std::llround(value)));
    else
        std::snprintf(buffer, sizeof buffer, "%.4g", value);
    return buffer;
}

double ParameterEditor::snap(const ParameterInfo& info, double value) const {
    value = std::clamp(value, std::min(info.minValue, info.maxValue), std::max(info.minValue, info.maxValue));
    return info.isStepped ? std::round(value) : value;
}

void ParameterEditor::sync() {
    for (std::size_t i = 0; i < list.size(); ++i) {
        if (quietTicks[i] > 0) {
            --quietTicks[i];
            continue;
        }
        if (i == dragging) continue;
        Row&         row   = list[i];
        const double value = target.paramValue(row.info.id);
        if (value == row.value) continue;
        row.value = value;
        row.text  = textFor(row.info, value);
        ++changes;
    }
}

void ParameterEditor::set(std::size_t index, double value) {
    if (index >= list.size() || list[index].info.isReadOnly) return;
    Row& row = list[index];
    value    = snap(row.info, value);
    quietTicks[index] = kQuietTicks;
    if (value == row.value) return;
    row.value = value;
    row.text  = textFor(row.info, value);
    ++changes;
    controller.setParameter(target, row.info.id, value);
}

void ParameterEditor::reset(std::size_t index) {
    if (index < list.size()) set(index, list[index].info.defaultValue);
}

void ParameterEditor::beginDrag(std::size_t index) {
    dragging = index;
}

void ParameterEditor::endDrag() {
    if (dragging < quietTicks.size()) quietTicks[dragging] = kQuietTicks;
    dragging = SIZE_MAX;
}

std::vector<std::pair<double, std::string>> ParameterEditor::choices(std::size_t index) const {
    std::vector<std::pair<double, std::string>> out;
    if (index >= list.size()) return out;
    const ParameterInfo& info = list[index].info;
    const double         low  = std::ceil(std::min(info.minValue, info.maxValue));
    const double         high = std::floor(std::max(info.minValue, info.maxValue));
    for (double v = low; v <= high && out.size() <= static_cast<std::size_t>(kMenuSteps); v += 1.0)
        out.emplace_back(v, textFor(info, v));
    return out;
}

bool ParameterEditor::parseValue(const std::string& text, double& value) {
    const char* start = text.c_str();
    char*       end   = nullptr;
    const double parsed = std::strtod(start, &end);
    if (end == start || !std::isfinite(parsed)) return false;
    value = parsed;
    return true;
}

} // namespace stagehand
