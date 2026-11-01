#pragma once

// A sequence's editor window (docs/sequences.md, section 5): settings down the
// left, and the grid, one row per EDO step (or note number) over black-key
// bands and one column per unit. Mouse and keys become SequenceEditor calls
// (the notes) and controller intents (the settings). Internal to stagehand-ui.

#include <cstdint>
#include <string>

#include "App/RackController.h"
#include "App/SequenceEditor.h"
#include "Ui/UiWidget.h"

namespace stagehand {

class SequenceEditorWidget final : public UiWidget {
public:
    SequenceEditorWidget(DGL_NAMESPACE::Window& window, RackController& controller, SequenceEditor& editor);

protected:
    void draw() override;
    void wheelUnclaimed(float x, float y, float notches, std::uint32_t mod) override;
    void swipeUnclaimed(float x, float y, float notches) override;
    bool onKeyboard(const KeyboardEvent& ev) override;

private:
    // --- geometry (logical pixels)
    float canvasX() const;
    float canvasY() const;
    float canvasW();
    float canvasH();
    float unitWidth();                 // the zoom: fitted to the window until the wheel zooms
    float rowY(int pitch) const;       // top of a pitch's row
    std::int64_t unitAt(float x);      // may be outside the sequence
    int          pitchAt(float y) const;
    void         clampScroll();

    // --- parts
    void drawStrip(const Sequence& seq);
    void drawRuler(const Sequence& seq);
    void drawGrid(const Sequence& seq);
    void drawVelocityReadout(const Sequence& seq);
    void settingRow(float y, const char* name);

    // --- input
    void pressCanvas(const Press& p);

    RackController& controller;
    SequenceEditor& editor;

    float         scrollX = 0, scrollY = 0;
    float         zoom    = 0;          // 0: fit the sequence's length to the width
    bool          centred = false;      // the first draw scrolls the reference pitch into view
    // A rubber band being dragged (canvas coordinates), while `banding`.
    bool          banding = false;
    float         bandX0 = 0, bandY0 = 0, bandX1 = 0, bandY1 = 0;
    // A Ctrl-drag (velocity), and the note it grabbed: its readout follows it.
    bool          velocityDrag  = false;
    std::uint32_t velocityStart = 0;
    int           velocityPitch = 0;
    // Double-click on a note.
    std::uint32_t lastClickTime  = 0;
    std::int64_t  lastClickUnit  = -1;
    int           lastClickPitch = 0;
    bool          clicked        = false;
};

} // namespace stagehand
