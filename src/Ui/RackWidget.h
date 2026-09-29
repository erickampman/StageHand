#pragma once

// The rack window's one widget (internal to stagehand-ui; RackUi is the public
// face). It draws the controller's RackViewModel and turns input into intents.
//
// Immediate-mode: every draw records the clickable regions it drew, with their
// handlers, and input goes to the topmost region under the pointer. Overlays (a
// menu, the filter panel) are drawn last, so they're on top for input too.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "NanoVG.hpp"
#include "Window.hpp"

#include "App/RackController.h"

namespace stagehand {

struct UiColor {
    int r, g, b;
};

class RackWidget final : public DGL_NAMESPACE::NanoTopLevelWidget {
public:
    RackWidget(DGL_NAMESPACE::Window& window, RackController& controller);

    // A status-bar message for a few seconds.
    void showNotice(const std::string& text, double now);

protected:
    void onNanoDisplay() override;
    bool onMouse(const MouseEvent& ev) override;
    bool onMotion(const MotionEvent& ev) override;
    bool onScroll(const ScrollEvent& ev) override;
    bool onKeyboard(const KeyboardEvent& ev) override;

private:
    struct Press {
        float         x = 0, y = 0;   // logical pixels
        std::uint32_t mod  = 0;
        std::uint32_t time = 0;       // ms, for double-clicks
    };
    // A clickable area recorded during the last draw.
    struct Region {
        float                               x, y, w, h;
        std::function<void(const Press&)>   press;   // null: blocks what's below, does nothing
        std::function<void(float notches)>  wheel;   // null: the wheel scrolls the rack
    };
    struct MenuItem {
        std::string           label;
        std::function<void()> action;             // null: a disabled item
        bool                  checked = false;
        bool                  danger  = false;
    };
    struct Menu {
        float                 x = 0, y = 0, w = 260;
        std::string           title;
        std::vector<MenuItem> items;
        float                 scroll = 0;
    };

    // --- geometry (logical pixels; the window's scale factor is applied when drawing)
    float scaleFactor();
    float width();
    float height();
    float outputX() const;
    float outputW();
    float rowsHeight();
    float rowY(std::size_t row) const;
    float faderX0() const;
    float faderX1();
    float maxScroll();

    // --- drawing helpers
    void  fill(UiColor c, int alpha = 255);
    void  box(float x, float y, float w, float h, UiColor c, float radius = 0);
    void  outline(float x, float y, float w, float h, UiColor c, float radius = 0);
    void  label(float x, float y, const std::string& text, float size, UiColor c,
                int align = ALIGN_LEFT | ALIGN_MIDDLE);
    float labelWidth(const std::string& text, float size);
    void  button(float x, float y, float w, float h, const std::string& text, bool on, bool enabled, UiColor onColor);
    void  meterBars(float x0, float x1, float y, const float db[2], const float hold[2]);
    void  region(float x, float y, float w, float h, std::function<void(const Press&)> press,
                 std::function<void(float)> wheel = {});

    // --- parts
    void drawToolbar();
    void drawTransport(float x);
    void drawRow(const RowModel& row, std::size_t index);
    void drawRowInput(const RowModel& row, float y);
    void drawRowPlugin(const RowModel& row, float y);
    void drawRowOutput(const RowModel& row, float y);
    void drawStatus();
    void drawMenu();
    void drawFilterPanel();
    // [-] value [+]; `change` gets the direction and whether Shift made it a big step.
    void stepper(float x, float y, const std::string& value, const std::function<void(int dir, bool big)>& change);

    // --- actions
    void openMenu(Menu m);
    void openSourceMenu(const RowModel& row, float x, float y);
    void openRowMenu(const RowModel& row, float x, float y);
    void openAddMenu(float x, float y);
    // Filter edits go through here, so the filter panel shows them before the
    // next model rebuild.
    const RowModel* rowById(const std::string& id) const;   // the current model's row, at event time
    MidiFilter filterOf(const RowModel& row) const;
    void       applyFilter(const std::string& id, const MidiFilter& filter);
    void       startKeyRangeDrag(const RowModel& row, float barX, const Press& p);

    RackController& controller;

    std::vector<Region>                                   regions;   // from the last draw
    std::function<void(float x, float y, std::uint32_t)>  drag;      // while the button is held
    std::optional<Menu>                                   menu;
    std::string                                           filterId;  // the row whose filter panel is open
    MidiFilter                                            panelFilter;

    float         scrollY = 0;
    std::string   lastFaderId;        // double-click on a fader
    std::uint32_t lastFaderTime = 0;
    std::uint32_t lastTempoTime = 0;  // double-click on the tempo...
    bool          tempoClicked  = false;   // ...once there's been a first click
    std::string   notice;
    double        noticeUntil = 0;
};

} // namespace stagehand
