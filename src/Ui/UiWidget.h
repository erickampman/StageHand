#pragma once

// The base of StageHand's own windows' widgets (internal to stagehand-ui): the
// look, drawing helpers, clickable regions, drags, menus and steppers shared by
// the rack window and the sequence editor windows.
//
// Immediate-mode: every draw records the clickable regions it drew, with their
// handlers, and input goes to the topmost region under the pointer. Overlays (a
// menu, a subclass's panel) are drawn last, so they're on top for input too.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "NanoVG.hpp"
#include "Window.hpp"

namespace stagehand {

struct UiColor {
    int r, g, b;
};

namespace ui {
constexpr UiColor kBg{22, 24, 30}, kRowA{31, 34, 42}, kRowB{35, 38, 47}, kBarBg{16, 18, 23}, kPanel{44, 48, 59},
    kText{232, 234, 240}, kDim{140, 146, 160}, kAccent{90, 160, 255}, kAmber{240, 170, 50}, kRed{232, 72, 60},
    kGreen{80, 200, 120}, kTrack{58, 62, 74}, kLine{48, 52, 62};
constexpr float kPad       = 12.0f;
constexpr float kMenuItemH = 26.0f;
double          nowSeconds();   // a steady clock
} // namespace ui

class UiWidget : public DGL_NAMESPACE::NanoTopLevelWidget {
public:
    explicit UiWidget(DGL_NAMESPACE::Window& window);

protected:
    struct Press {
        float         x = 0, y = 0;   // logical pixels
        std::uint32_t mod    = 0;
        std::uint32_t time   = 0;     // ms, for double-clicks
        std::uint32_t button = 1;     // DGL's numbering: kMouseButtonLeft (1) or kMouseButtonRight (2)
    };
    // A clickable area recorded during the last draw.
    struct Region {
        float                              x, y, w, h;
        std::function<void(const Press&)>  press;             // null: blocks what's below, does nothing
        std::function<void(float notches)> wheel;             // null: passes the wheel on
        bool                               rightToo = false;  // also gets right-clicks
    };
    struct MenuItem {
        std::string           label;
        std::function<void()> action;   // null: a disabled item
        bool                  checked = false;
        bool                  danger  = false;
    };
    struct Menu {
        float                 x = 0, y = 0, w = 260;
        std::string           title;
        std::vector<MenuItem> items;
        float                 scroll = 0;
    };

    // --- for subclasses
    virtual void draw() = 0;                  // everything but overlays, in logical pixels
    virtual bool overlayOpen() const { return false; }
    virtual void drawOverlays() {}            // drawn under the menu, above the rest
    virtual void dismissOverlays() {}         // a click elsewhere, or Escape
    // A wheel no region took (e.g. scroll a list).
    virtual void wheelUnclaimed(float /*x*/, float /*y*/, float /*notches*/, std::uint32_t /*mod*/) {}

    // --- geometry (logical pixels; the window's scale factor is applied when drawing)
    float scaleFactor();
    float width();
    float height();

    // --- drawing helpers
    void  fill(UiColor c, int alpha = 255);
    void  box(float x, float y, float w, float h, UiColor c, float radius = 0);
    void  outline(float x, float y, float w, float h, UiColor c, float radius = 0);
    void  label(float x, float y, const std::string& text, float size, UiColor c, int align = ALIGN_LEFT | ALIGN_MIDDLE);
    float labelWidth(const std::string& text, float size);
    void  button(float x, float y, float w, float h, const std::string& text, bool on, bool enabled, UiColor onColor);
    void  dotsButton(float x, float y, float w, float h);   // "•••"
    void  region(float x, float y, float w, float h, std::function<void(const Press&)> press,
                 std::function<void(float)> wheel = {}, bool rightToo = false);
    // [-] value [+]; `change` gets the direction and whether Shift made it a big step.
    void stepper(float x, float y, const std::string& value, const std::function<void(int dir, bool big)>& change);

    // --- text entry: one field at a time, drawn on top of everything. Typing
    // (UTF-8), Backspace, Delete, Left/Right, Home/End and Ctrl-A edit it; it
    // starts all selected, so typing replaces. Enter, or a click elsewhere,
    // calls `done` with the text; Escape leaves without it. While it's open it
    // takes every key (subclasses call UiWidget::onKeyboard first).
    void editText(float x, float y, float w, float h, std::string text, std::size_t maxBytes,
                  std::function<void(const std::string&)> done);
    void moveTextField(float x, float y, float w, float h);   // keep it on what it edits, as that moves
    bool editingText() const noexcept { return field.has_value(); }
    void endTextEdit(bool keep);

    // --- menus
    void openMenu(Menu m);
    void closeMenu() { menu.reset(); }
    bool menuOpen() const noexcept { return menu.has_value(); }

    // --- input
    // While the button is held: called on each motion. `dragEnd` (optional)
    // runs once on release.
    std::function<void(float x, float y, std::uint32_t mod)> drag;
    std::function<void()>                                    dragEnd;

    void onNanoDisplay() override;
    bool onMouse(const MouseEvent& ev) override;
    bool onMotion(const MotionEvent& ev) override;
    bool onScroll(const ScrollEvent& ev) override;
    bool onKeyboard(const KeyboardEvent& ev) override;
    bool onCharacterInput(const CharacterInputEvent& ev) override;

private:
    void drawMenu();
    void drawTextField();

    struct TextField {
        float                                   x = 0, y = 0, w = 0, h = 0;
        std::string                             text;
        std::size_t                             caret    = 0;      // a byte index, on a character boundary
        bool                                    all      = true;   // everything selected
        std::size_t                             maxBytes = 64;
        std::function<void(const std::string&)> done;
    };
    std::optional<TextField> field;

    std::vector<Region> regions;   // from the last draw
    std::optional<Menu> menu;
};

} // namespace stagehand
