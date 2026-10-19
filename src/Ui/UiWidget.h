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
// StageHand's colours (docs/look.md): neutral charcoal panels, so the icon's
// three colours carry it. The accent is the icon's green (what's on, lit or
// chosen), purple the second (sequences, loopers, the sources), orange only
// in the brand; amber stays the warning colour.
constexpr UiColor kBg{20, 22, 26}, kRowA{30, 32, 37}, kRowB{34, 36, 42}, kBarBg{15, 16, 19}, kPanel{38, 41, 48},
    kText{232, 234, 240}, kDim{140, 145, 156}, kAccent{116, 201, 142}, kPurple{180, 160, 234}, kOrange{242, 166, 94},
    kAmber{240, 170, 50}, kRed{232, 72, 60}, kGreen{80, 200, 120}, kTrack{58, 62, 72}, kLine{46, 49, 56},
    // The house look's (KeyboardEDO's docs/decisions.md, "Look and feel"), in
    // StageHand's neutrals: sections a step up from the panel, value boxes and
    // slots darker, borders a faint slate.
    kSection{31, 34, 40}, kWell{14, 15, 18}, kSlate{110, 116, 128};
// The key StageHand's shortcuts and modified drags use (docs/macos.md, decision
// 7): Command on macOS, Control elsewhere; and the names the windows show.
#ifdef __APPLE__
constexpr std::uint32_t kCommand     = DGL_NAMESPACE::kModifierSuper;
constexpr const char*   kCommandName = "Cmd";
constexpr const char*   kAltName     = "Option";
#else
constexpr std::uint32_t kCommand     = DGL_NAMESPACE::kModifierControl;
constexpr const char*   kCommandName = "Ctrl";
constexpr const char*   kAltName     = "Alt";
#endif
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
        bool                  warn    = false;   // amber (a problem), enabled or not
    };
    struct Menu {
        float                 x = 0, y = 0, w = 260;
        std::string           title;
        std::vector<MenuItem> items;
        float                 scroll = 0;
        // A long list: typing filters it (the items containing what's typed,
        // in any case); Backspace takes a letter back, Enter chooses the first.
        bool                  search = false;
        std::string           typed;
        // Opened from a subclass's panel: the panel stays (a click outside the
        // menu closes only the menu).
        bool                  overPanel = false;
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
    // A box shading from `left` at its left edge to `right` at its right.
    void  gradientBox(float x, float y, float w, float h, UiColor left, UiColor right);
    void  label(float x, float y, const std::string& text, float size, UiColor c, int align = ALIGN_LEFT | ALIGN_MIDDLE);
    float labelWidth(const std::string& text, float size);
    void  button(float x, float y, float w, float h, const std::string& text, bool on, bool enabled, UiColor onColor);
    // A button's face without its text: faint slate with a border, or `onColor`
    // when on (its ink is then kBarBg).
    void  buttonFace(float x, float y, float w, float h, bool on, UiColor onColor);
    // The house look (KeyboardEDO's docs/decisions.md, "Look and feel"; the
    // plugins' PlainOldSynth): the knob's glowing sphere; a horizontal slider,
    // its thumb's centre at `at` from x0 (0) to x1 (1), lit from `rest`, the
    // thumb `thumb` across; a dropdown (text and a chevron in a dark box); a
    // value box (centred text in one); a section (a rounded box a step up from
    // the panel, for a group of controls; its caller writes the title).
    void  sphere(float cx, float cy, float r, UiColor accent, float intensity = 1.0f);
    void  slider(float x0, float x1, float cy, float at, float rest, float thumb, UiColor accent, bool dim);
    void  dropdown(float x, float y, float w, float h, const std::string& text, UiColor ink, float size = 13.0f);
    void  valueBox(float x, float y, float w, float h, const std::string& text, UiColor ink, float size = 13.0f);
    void  section(float x, float y, float w, float h);
    // A section shaded from `left` to `right`, with a `stripe`-coloured band
    // `stripeW` wide down its left edge, inside its rounding (a rack row).
    void  tintedSection(float x, float y, float w, float h, UiColor left, UiColor right, UiColor stripe, float stripeW);
    // The Unlikelyware logo, `h` high, and the two-colour "StageHand" wordmark
    // after it, centred on `cy`; a click on the logo opens unlikelyware.com.
    // Returns the width taken.
    float brand(float x, float cy, float h);
    // Opens `target` (a URL or a file) in the default browser, brought to the
    // front: on Linux with an activation token from the last click (GNOME on
    // Wayland raises no window without one). False if it couldn't be started.
    bool  openInBrowser(const std::string& target);
    void  removeButton(float x, float y, float size);       // a red X, for "remove this row"
    // MIDI control (learned triggers, a switch): a 5-pin DIN plug, drawn as
    // button() is, lit when `on`.
    void  midiButton(float x, float y, float w, float h, bool on, UiColor onColor);
    void  region(float x, float y, float w, float h, std::function<void(const Press&)> press,
                 std::function<void(float)> wheel = {}, bool rightToo = false);
    // Draw a part `dy` lower than its own coordinates say: drawing and the
    // regions it records move together. Pair each beginShift with an endShift.
    void  beginShift(float dy);
    void  endShift(float dy);
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
    std::vector<std::size_t> menuShown() const;   // the items a search menu shows (all, if it isn't one)
    bool menuKey(const KeyboardEvent& ev);        // Backspace and Enter in a search menu
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

    // The brand's, made on first use (the context exists by then).
    bool                   brandTried = false;
    DGL_NAMESPACE::NanoImage logo;
    int                    brandFont = -1;

    std::uint32_t lastPressTime = 0;   // the last click's X server time (ms), for openInBrowser

    std::vector<Region> regions;    // from the last draw
    float               regionDy = 0;   // beginShift()
    std::optional<Menu> menu;
};

} // namespace stagehand
