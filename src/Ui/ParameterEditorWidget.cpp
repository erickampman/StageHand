#include "Ui/ParameterEditorWidget.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

namespace stagehand {

namespace dgl = DGL_NAMESPACE;
using namespace ui;

namespace {

constexpr float kHeaderH = 44.0f;   // the plugin's name
constexpr float kRowH    = 36.0f;   // one parameter
constexpr float kNameW   = 170.0f;
constexpr float kValueW  = 92.0f;

// Where `value` sits in `info`'s range, 0 to 1.
float positionOf(const ParameterInfo& info, double value) {
    const double span = info.maxValue - info.minValue;
    return span == 0.0 ? 0.0f : static_cast<float>(std::clamp((value - info.minValue) / span, 0.0, 1.0));
}

} // namespace

ParameterEditorWidget::ParameterEditorWidget(dgl::Window& window, ParameterEditor& parameterEditor, std::string plugin)
    : UiWidget(window), editor(parameterEditor), pluginName(std::move(plugin)) {}

float ParameterEditorWidget::wantedHeight() const {
    return kHeaderH + static_cast<float>(editor.rows().size()) * kRowH + kPad;
}

void ParameterEditorWidget::clampScroll() {
    const float content = static_cast<float>(editor.rows().size()) * kRowH + kPad;
    scrollY             = std::clamp(scrollY, 0.0f, std::max(0.0f, content - (height() - kHeaderH)));
}

void ParameterEditorWidget::wheelUnclaimed(float, float, float notches, std::uint32_t) {
    scrollY -= notches * kRowH;
    clampScroll();
    repaint();
}

void ParameterEditorWidget::draw() {
    box(0, 0, width(), height(), kBg);
    clampScroll();
    const auto& rows = editor.rows();
    save();
    scissor(0, kHeaderH, width(), height() - kHeaderH);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const float y = kHeaderH + static_cast<float>(i) * kRowH - scrollY;
        if (y + kRowH < kHeaderH || y > height()) continue;
        drawRow(i, y);
    }
    restore();

    box(0, 0, width(), kHeaderH, kBarBg);   // over rows scrolled under it
    region(0, 0, width(), kHeaderH, nullptr);
    label(kPad, kHeaderH / 2, pluginName, 16.0f, kText);
    label(width() - kPad, kHeaderH / 2, "double-click: default", 11.0f, kDim, ALIGN_RIGHT | ALIGN_MIDDLE);
}

void ParameterEditorWidget::drawRow(std::size_t index, float y) {
    const ParameterEditor::Row& row = editor.rows()[index];
    const bool  readOnly = row.info.isReadOnly;
    const float cy       = y + kRowH / 2;
    box(0, y, width(), kRowH, index % 2 == 0 ? kRowA : kRowB);

    save();
    scissor(kPad, y, kNameW - 8.0f, kRowH);
    label(kPad, cy, row.info.name, 13.0f, readOnly ? kDim : kText);
    restore();

    const float cx0    = kPad + kNameW;                         // the control
    const float valueX = width() - kPad - kValueW;              // the value, typed into
    const float cx1    = valueX - 12.0f;
    const ParameterInfo info = row.info;

    // The value box: click to type a number.
    valueBox(valueX, y + 6.0f, kValueW, kRowH - 12.0f, row.text, readOnly ? kDim : kText, 12.0f);
    if (!readOnly)
        region(valueX, y + 4.0f, kValueW, kRowH - 8.0f, [this, index, valueX, y](const Press&) {
            const auto& rows = editor.rows();
            if (index >= rows.size()) return;
            char start[32];
            std::snprintf(start, sizeof start, rows[index].info.isStepped ? "%.0f" : "%.6g", rows[index].value);
            editText(valueX, y + 6.0f, kValueW, kRowH - 12.0f, start, 24, [this, index](const std::string& typed) {
                if (double v = 0; ParameterEditor::parseValue(typed, v)) editor.set(index, v);
                repaint();
            });
        });

    // A double-click on the control resets the row to its default.
    auto doubleClicked = [this, index](const Press& p) {
        const bool twice = lastClicked == index && p.time - lastClickTime < 400;
        lastClicked      = twice ? SIZE_MAX : index;
        lastClickTime    = p.time;
        if (twice) editor.reset(index);
        return twice;
    };

    switch (row.kind) {
        case ParameterEditor::Kind::Switch: {
            const bool on = row.value > (info.minValue + info.maxValue) / 2;
            button(cx0, y + 6.0f, 72.0f, kRowH - 12.0f, row.text, on, !readOnly, kAccent);
            if (!readOnly)
                region(cx0, y + 4.0f, 72.0f, kRowH - 8.0f, [this, index, info, on](const Press&) {
                    editor.set(index, on ? info.minValue : info.maxValue);
                    repaint();
                });
            break;
        }
        case ParameterEditor::Kind::Menu: {
            const float w = std::min(200.0f, cx1 - cx0);
            dropdown(cx0, y + 6.0f, w, kRowH - 12.0f, row.text, readOnly ? kDim : kText, 13.0f);
            if (!readOnly)
                region(cx0, y + 4.0f, w, kRowH - 8.0f,
                       [this, index, cx0, y](const Press&) { openChoices(index, cx0, y + kRowH - 4.0f); },
                       [this, index](float notches) {
                           const auto& rows = editor.rows();
                           if (index < rows.size()) editor.set(index, rows[index].value + (notches > 0 ? 1.0 : -1.0));
                           repaint();
                       });
            break;
        }
        case ParameterEditor::Kind::Slider: {
            const float rest = info.minValue < 0.0 && info.maxValue > 0.0 ? positionOf(info, 0.0) : 0.0f;
            slider(cx0 + 8.0f, cx1 - 8.0f, cy, positionOf(info, row.value), rest, 16.0f, kAccent, readOnly);
            if (readOnly) break;
            const float span = std::max(1.0f, cx1 - cx0 - 16.0f);
            region(
                cx0, y + 4.0f, cx1 - cx0, kRowH - 8.0f,
                [this, index, info, span, doubleClicked](const Press& p) {
                    if (doubleClicked(p)) {
                        repaint();
                        return;
                    }
                    // It moves with the drag, not to where it was clicked (as the
                    // rack's faders do); Shift is finer.
                    const auto& rows = editor.rows();
                    if (index >= rows.size()) return;
                    editor.beginDrag(index);
                    drag = [this, index, info, span, start = rows[index].value, startX = p.x](float x, float,
                                                                                              std::uint32_t mod) {
                        const double fine = (mod & dgl::kModifierShift) != 0 ? 0.1 : 1.0;
                        editor.set(index, start + (x - startX) / span * (info.maxValue - info.minValue) * fine);
                        repaint();
                    };
                    dragEnd = [this] { editor.endDrag(); };
                },
                [this, index, info](float notches) {   // a step, or a hundredth of the range
                    const auto& rows = editor.rows();
                    if (index >= rows.size()) return;
                    const double step = info.isStepped ? 1.0 : (info.maxValue - info.minValue) / 100.0;
                    editor.set(index, rows[index].value + (notches > 0 ? step : -step));
                    repaint();
                });
            break;
        }
    }
}

void ParameterEditorWidget::openChoices(std::size_t index, float x, float y) {
    const auto& rows = editor.rows();
    if (index >= rows.size()) return;
    Menu m;
    m.w     = 220.0f;
    m.x     = std::min(x, width() - m.w - 8.0f);
    m.y     = y;
    m.title = rows[index].info.name;
    for (const auto& [value, text] : editor.choices(index))
        m.items.push_back(MenuItem{text, [this, index, value = value] {
                                       editor.set(index, value);
                                       repaint();
                                   },
                                   value == rows[index].value, false});
    openMenu(std::move(m));
}

} // namespace stagehand
