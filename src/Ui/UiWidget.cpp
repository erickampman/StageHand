#include "Ui/UiWidget.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <string>

#ifndef __APPLE__
#include "Ui/StartupId.h"
#endif

// The brand's, embedded at build time (cmake/EmbedFile.cmake; resources/README.md).
extern const unsigned char kBrandLogo[];   // the Unlikelyware logo (PNG)
extern const unsigned int  kBrandLogoSize;
extern const unsigned char kBrandFont[];   // Karla, the wordmark's
extern const unsigned int  kBrandFontSize;

namespace stagehand {

namespace dgl = DGL_NAMESPACE;
using namespace ui;

namespace {

dgl::Color withAlpha(UiColor c, float a) {
    return dgl::Color(c.r, c.g, c.b, std::clamp(a, 0.0f, 1.0f));   // DGL's alpha is 0 to 1
}
UiColor lighter(UiColor c, float t) {   // toward white
    auto up = [t](int v) { return v + static_cast<int>(std::lround(static_cast<float>(255 - v) * t)); };
    return UiColor{up(c.r), up(c.g), up(c.b)};
}

} // namespace

double ui::nowSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

UiWidget::UiWidget(dgl::Window& window) : NanoTopLevelWidget(window) {
    loadSharedResources();   // DGL's built-in font (DejaVu Sans)
}

// --- geometry ------------------------------------------------------------------------

float UiWidget::scaleFactor() {
    return static_cast<float>(getWindow().getScaleFactor());
}
float UiWidget::width() {
    return static_cast<float>(getWidth()) / scaleFactor();
}
float UiWidget::height() {
    return static_cast<float>(getHeight()) / scaleFactor();
}

// --- drawing helpers -----------------------------------------------------------------

void UiWidget::fill(UiColor c, int alpha) {
    fillColor(c.r, c.g, c.b, alpha);
}

void UiWidget::gradientBox(float x, float y, float w, float h, UiColor left, UiColor right) {
    beginPath();
    rect(x, y, w, h);
    fillPaint(linearGradient(x, y, x + w, y, Color(left.r, left.g, left.b), Color(right.r, right.g, right.b)));
    NanoVG::fill();
}

void UiWidget::box(float x, float y, float w, float h, UiColor c, float radius) {
    beginPath();
    if (radius > 0.0f)
        roundedRect(x, y, w, h, radius);
    else
        rect(x, y, w, h);
    fill(c);
    NanoVG::fill();
}

void UiWidget::outline(float x, float y, float w, float h, UiColor c, float radius) {
    beginPath();
    roundedRect(x + 0.5f, y + 0.5f, w - 1.0f, h - 1.0f, radius);
    strokeColor(c.r, c.g, c.b);
    strokeWidth(1.0f);
    stroke();
}

// Empty text draws and measures nothing: DPF's NanoVG asserts on "" (an emptied text field).
void UiWidget::label(float x, float y, const std::string& text, float size, UiColor c, int align) {
    if (text.empty()) return;
    fontSize(size);
    fill(c);
    textAlign(align);
    this->text(x, y, text.c_str(), nullptr);
}

float UiWidget::labelWidth(const std::string& text, float size) {
    if (text.empty()) return 0.0f;
    fontSize(size);
    dgl::Rectangle<float> bounds;
    return textBounds(0, 0, text.c_str(), nullptr, bounds);
}

void UiWidget::button(float x, float y, float w, float h, const std::string& text, bool on, bool enabled,
                      UiColor onColor) {
    buttonFace(x, y, w, h, on, onColor);
    label(x + w / 2, y + h / 2, text, 13.0f, !enabled ? kDim : on ? kBarBg : kText, ALIGN_CENTER | ALIGN_MIDDLE);
}

// KeyboardEDO's buttons: a faint slate over whatever is behind, a fainter
// border, 3 px corners; lit, the colour itself.
void UiWidget::buttonFace(float x, float y, float w, float h, bool on, UiColor onColor) {
    if (on) {
        box(x, y, w, h, onColor, 3.0f);
        return;
    }
    beginPath();
    roundedRect(x, y, w, h, 3.0f);
    fillColor(withAlpha(kSlate, 0.30f));
    NanoVG::fill();
    beginPath();
    roundedRect(x + 0.5f, y + 0.5f, w - 1.0f, h - 1.0f, 3.0f);
    strokeColor(withAlpha(kSlate, 0.35f));
    strokeWidth(1.0f);
    stroke();
}

// PlainOldSynth's knob body (its src/UI/Knob.hpp): a glow bright at the
// centre fading out at the rim, a brighter core, a faint rim and a soft glint
// at the top left.
void UiWidget::sphere(float cx, float cy, float r, UiColor accent, float intensity) {
    beginPath();
    circle(cx, cy, r);
    fillPaint(radialGradient(cx, cy, r * 0.15f, r, withAlpha(accent, 0.80f * intensity), withAlpha(accent, 0.0f)));
    NanoVG::fill();
    beginPath();
    circle(cx, cy, r * 0.64f);
    fillPaint(radialGradient(cx, cy, 0.0f, r * 0.62f, withAlpha(lighter(accent, 0.35f), 0.85f * intensity),
                             withAlpha(accent, 0.0f)));
    NanoVG::fill();
    beginPath();
    circle(cx, cy, r);
    strokeColor(withAlpha(UiColor{255, 255, 255}, 0.14f * intensity));
    strokeWidth(1.2f);
    stroke();
    const float hx = cx - r * 0.30f, hy = cy - r * 0.34f, hr = r * 0.26f;
    beginPath();
    circle(hx, hy, hr);
    fillPaint(radialGradient(hx, hy, 0.0f, hr, withAlpha(UiColor{255, 255, 255}, 0.11f * intensity),
                             withAlpha(UiColor{255, 255, 255}, 0.0f)));
    NanoVG::fill();
}

void UiWidget::knob(float cx, float cy, float diameter, float value, UiColor accent) {
    constexpr float kPi = 3.14159265f;
    const float     r = diameter / 2, angle = 0.75f * kPi + 1.5f * kPi * std::clamp(value, 0.0f, 1.0f);
    sphere(cx, cy, r, accent);
    beginPath();
    circle(cx + std::cos(angle) * r * 0.72f, cy + std::sin(angle) * r * 0.72f, std::max(2.0f, r * 0.13f));
    fill(UiColor{244, 247, 250});
    NanoVG::fill();
}

// KeyboardEDO's slider (its src/app/Widgets.cpp), lying down: a recessed
// slot, lit from the rest to the value with a faint glow either side, the
// rest marked across it when it isn't an end, and the thumb a small sphere
// with a white dot. Dimmed (muted), it's grey.
void UiWidget::slider(float x0, float x1, float cy, float at, float rest, float thumb, UiColor accent, bool dim) {
    const UiColor colour = dim ? UiColor{104, 108, 116} : accent;
    const float   span = x1 - x0, half = std::max(2.0f, thumb * 0.15f);
    const float   v = x0 + std::clamp(at, 0.0f, 1.0f) * span, r = x0 + std::clamp(rest, 0.0f, 1.0f) * span;
    beginPath();
    roundedRect(x0 - half, cy - half, span + half * 2, half * 2, half);
    fill(kWell);
    NanoVG::fill();
    strokeColor(withAlpha(kSlate, 0.45f));
    strokeWidth(1.0f);
    stroke();

    const float lo = std::min(r, v), hi = std::max(r, v), glow = half * 1.8f;
    if (hi > lo) {
        beginPath();
        roundedRect(lo, cy - glow, hi - lo, glow * 2, std::min(glow, (hi - lo) / 2));
        fillColor(withAlpha(colour, 0.12f));
        NanoVG::fill();
        beginPath();
        roundedRect(lo, cy - half + 1.5f, hi - lo, half * 2 - 3.0f, std::min(half, (hi - lo) / 2));
        fillColor(withAlpha(colour, 0.80f));
        NanoVG::fill();
    }
    if (rest > 0.0f && rest < 1.0f) {
        const float tick = half + thumb * 0.22f;
        beginPath();
        moveTo(r, cy - tick);
        lineTo(r, cy + tick);
        strokeColor(withAlpha(kSlate, 0.9f));
        strokeWidth(1.5f);
        stroke();
    }

    const float radius = thumb / 2;
    beginPath();
    circle(v, cy, radius * 0.95f);
    fill(kSection);
    NanoVG::fill();
    sphere(v, cy, radius, colour, dim ? 0.7f : 1.0f);
    beginPath();
    circle(v, cy, std::max(2.0f, radius * 0.17f));
    fill(dim ? UiColor{150, 154, 160} : UiColor{244, 247, 250});
    NanoVG::fill();
}

// The plugins' dropdown (PlainOldSynth's src/UI/Dropdown.hpp): the choice in
// a dark box, a chevron at its right.
void UiWidget::dropdown(float x, float y, float w, float h, const std::string& text, UiColor ink, float size) {
    valueBox(x, y, w, h, "", ink, size);
    save();
    scissor(x, y, std::max(0.0f, w - 22.0f), h);
    label(x + 8.0f, y + h / 2, text, size, ink);
    restore();
    const float cx = x + w - 11.0f, cy = y + h / 2;
    beginPath();
    moveTo(cx - 4.0f, cy - 2.0f);
    lineTo(cx + 4.0f, cy - 2.0f);
    lineTo(cx, cy + 3.0f);
    closePath();
    fill(kDim);
    NanoVG::fill();
}

void UiWidget::valueBox(float x, float y, float w, float h, const std::string& text, UiColor ink, float size) {
    beginPath();
    roundedRect(x, y, w, h, 3.0f);
    fill(kWell);
    NanoVG::fill();
    beginPath();
    roundedRect(x + 0.5f, y + 0.5f, w - 1.0f, h - 1.0f, 3.0f);
    strokeColor(withAlpha(kSlate, 0.40f));
    strokeWidth(1.0f);
    stroke();
    if (!text.empty()) label(x + w / 2, y + h / 2, text, size, ink, ALIGN_CENTER | ALIGN_MIDDLE);
}

void UiWidget::section(float x, float y, float w, float h) {
    beginPath();
    roundedRect(x, y, w, h, 7.0f);
    fill(kSection);
    NanoVG::fill();
    beginPath();
    roundedRect(x + 0.5f, y + 0.5f, w - 1.0f, h - 1.0f, 7.0f);
    strokeColor(withAlpha(kSlate, 0.30f));
    strokeWidth(1.0f);
    stroke();
}

void UiWidget::tintedSection(float x, float y, float w, float h, UiColor left, UiColor right, UiColor stripe,
                             float stripeW) {
    beginPath();
    roundedRect(x, y, w, h, 7.0f);
    fillPaint(linearGradient(x, y, x + w, y, Color(left.r, left.g, left.b), Color(right.r, right.g, right.b)));
    NanoVG::fill();
    save();
    intersectScissor(x, y, stripeW, h);
    beginPath();
    roundedRect(x, y, w, h, 7.0f);
    fill(stripe);
    NanoVG::fill();
    restore();
    beginPath();
    roundedRect(x + 0.5f, y + 0.5f, w - 1.0f, h - 1.0f, 7.0f);
    strokeColor(withAlpha(kSlate, 0.30f));
    strokeWidth(1.0f);
    stroke();
}

// As KeyboardEDO's toolbar has it (its KeyboardApp::drawBrand): the logo, then
// both words on one baseline in Karla, a letter at a time so the spacing can
// be tightened (Karla's is generous). "Stage" in the icon's purple, "Hand" in
// its green.
float UiWidget::brand(float x, float cy, float h) {
    if (!brandTried) {
        brandTried = true;
        logo       = createImageFromMemory(kBrandLogo, kBrandLogoSize, IMAGE_GENERATE_MIPMAPS);
        brandFont  = createFontFromMemory("stagehand-brand", kBrandFont, kBrandFontSize, false);
    }
    float at = x;
    if (logo.isValid()) {
        const dgl::Size<uint> size = logo.getSize();
        const float           w    = size.getHeight() > 0 ? h * static_cast<float>(size.getWidth()) /
                                                     static_cast<float>(size.getHeight())
                                                          : h;
        beginPath();
        rect(at, cy - h / 2, w, h);
        fillPaint(imagePattern(at, cy - h / 2, w, h, 0.0f, logo, 1.0f));
        NanoVG::fill();
        region(at, cy - h / 2, w, h, [this](const Press&) {
            openInBrowser("https://www.unlikelyware.com");   // nothing sensible to do if no browser opens
        });
        at += w + 8.0f;
    }
    if (brandFont < 0) return at - x;
    save();
    fontFaceId(brandFont);
    const float size     = h * 0.82f;
    const float tracking = -0.06f * size;
    fontSize(size);
    textAlign(ALIGN_LEFT | ALIGN_MIDDLE);
    auto word = [&](const char* text, UiColor colour) {
        fill(colour);
        for (const char* c = text; *c != '\0'; ++c) {
            this->text(at, cy, c, c + 1);
            dgl::Rectangle<float> bounds;
            at += textBounds(0, 0, c, c + 1, bounds) + tracking;
        }
    };
    word("Stage", kPurple);
    word("Hand", kAccent);
    restore();
    return at - tracking - x;
}

void UiWidget::removeButton(float x, float y, float size) {
    buttonFace(x, y, size, size, false, kTrack);
    const float in = size * 0.3f;
    beginPath();
    moveTo(x + in, y + in);
    lineTo(x + size - in, y + size - in);
    moveTo(x + size - in, y + in);
    lineTo(x + in, y + size - in);
    strokeColor(kRed.r, kRed.g, kRed.b);
    strokeWidth(2.5f);
    lineCap(ROUND);
    stroke();
    lineCap(BUTT);
}

void UiWidget::midiButton(float x, float y, float w, float h, bool on, UiColor onColor) {
    buttonFace(x, y, w, h, on, onColor);
    const UiColor ink = on ? kBarBg : kText;
    const float   cx = x + w / 2, cy = y + h / 2, r = std::min(w, h) * 0.34f;
    // The plug's rim, its notch at the top, and five pins in a half circle below.
    beginPath();
    circle(cx, cy, r);
    strokeColor(ink.r, ink.g, ink.b);
    strokeWidth(1.5f);
    stroke();
    beginPath();
    rect(cx - r * 0.18f, cy - r, r * 0.36f, r * 0.3f);
    for (int i = 0; i < 5; ++i) {
        const float a = 3.14159265f * static_cast<float>(i) / 4.0f;   // 0 (right) round to pi (left), below
        circle(cx + std::cos(a) * r * 0.6f, cy + std::sin(a) * r * 0.6f - r * 0.1f, r * 0.15f);
    }
    fill(ink);
    NanoVG::fill();
}

void UiWidget::region(float x, float y, float w, float h, std::function<void(const Press&)> press,
                      std::function<void(float)> wheel, bool rightToo) {
    regions.push_back(Region{x, y + regionDy, w, h, std::move(press), std::move(wheel), rightToo});
}

void UiWidget::beginShift(float dy) {
    save();
    translate(0.0f, dy);
    regionDy += dy;
}

void UiWidget::endShift(float dy) {
    regionDy -= dy;
    restore();
}

void UiWidget::stepper(float x, float y, const std::string& value,
                       const std::function<void(int dir, bool big)>& change) {
    button(x, y, 22.0f, 22.0f, "-", false, true, kAccent);
    region(x, y, 22.0f, 22.0f, [change](const Press& p) { change(-1, (p.mod & dgl::kModifierShift) != 0); });
    valueBox(x + 26.0f, y, 88.0f, 22.0f, value, kText);
    region(x + 22.0f, y, 96.0f, 22.0f, nullptr, [change](float notches) { change(notches > 0 ? 1 : -1, false); });
    button(x + 118.0f, y, 22.0f, 22.0f, "+", false, true, kAccent);
    region(x + 118.0f, y, 22.0f, 22.0f, [change](const Press& p) { change(1, (p.mod & dgl::kModifierShift) != 0); });
}

// --- the frame -----------------------------------------------------------------------

void UiWidget::onNanoDisplay() {
    regions.clear();
    regionDy       = 0;
    const float sf = scaleFactor();
    scale(sf, sf);
    draw();
    if (field) {   // a click elsewhere keeps what's typed, and does nothing else
        region(0, 0, width(), height(), [this](const Press&) { endTextEdit(true); }, [](float) {}, true);
        drawTextField();
        return;
    }
    if (menu || overlayOpen()) {
        // A click anywhere else dismisses the overlay (and does nothing else).
        region(0, 0, width(), height(),
               [this](const Press&) {
                   if (!menu || !menu->overPanel) dismissOverlays();
                   menu.reset();
               },
               [](float) {}, true);
        drawOverlays();
        drawMenu();
    }
}

// --- text entry ------------------------------------------------------------------------

namespace {

bool continuation(char c) {
    return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
}

std::size_t previousChar(const std::string& s, std::size_t at) {
    if (at == 0) return 0;
    do --at;
    while (at > 0 && continuation(s[at]));
    return at;
}

std::size_t nextChar(const std::string& s, std::size_t at) {
    if (at >= s.size()) return s.size();
    do ++at;
    while (at < s.size() && continuation(s[at]));
    return at;
}

} // namespace

void UiWidget::editText(float x, float y, float w, float h, std::string text, std::size_t maxBytes,
                        std::function<void(const std::string&)> done) {
    menu.reset();
    field           = TextField{x, y, w, h, std::move(text), 0, true, maxBytes, std::move(done)};
    field->caret    = field->text.size();
    repaint();
}

void UiWidget::moveTextField(float x, float y, float w, float h) {
    if (!field) return;
    field->x = x;
    field->y = y;
    field->w = w;
    field->h = h;
}

void UiWidget::endTextEdit(bool keep) {
    if (!field) return;
    TextField ended = std::move(*field);
    field.reset();   // before `done`, which may start another
    if (keep && ended.done) ended.done(ended.text);
    repaint();
}

void UiWidget::drawTextField() {
    TextField& f = *field;
    box(f.x, f.y, f.w, f.h, kBarBg, 4.0f);
    outline(f.x, f.y, f.w, f.h, kAccent, 4.0f);
    const float tx = f.x + 6.0f, ty = f.y + f.h / 2;
    save();
    scissor(f.x + 2.0f, f.y, f.w - 4.0f, f.h);
    if (f.all && !f.text.empty()) box(tx - 1.0f, f.y + 4.0f, labelWidth(f.text, 14.0f) + 2.0f, f.h - 8.0f, kAccent, 2.0f);
    label(tx, ty, f.text, 14.0f, f.all && !f.text.empty() ? kBarBg : kText);
    if (!f.all && std::fmod(nowSeconds(), 1.0) < 0.6) {   // the caret, blinking
        const float cx = tx + (f.caret == 0 ? 0.0f : labelWidth(f.text.substr(0, f.caret), 14.0f));
        box(cx, f.y + 5.0f, 1.5f, f.h - 10.0f, kText);
    }
    restore();
    // A click in the field puts the caret at the nearest character boundary.
    region(f.x, f.y, f.w, f.h, [this, tx](const Press& p) {
        TextField&  t    = *field;
        std::size_t best = 0;
        float       dist = std::fabs(p.x - tx);
        for (std::size_t at = nextChar(t.text, 0); at <= t.text.size(); at = nextChar(t.text, at)) {
            const float d = std::fabs(p.x - tx - labelWidth(t.text.substr(0, at), 14.0f));
            if (d < dist) {
                dist = d;
                best = at;
            }
            if (at == t.text.size()) break;
        }
        t.caret = best;
        t.all   = false;
    });
}

bool UiWidget::onKeyboard(const KeyboardEvent& ev) {
    if (menuKey(ev)) return true;
    if (!field) return false;
    if (!ev.press) return true;
    TextField& f = *field;
    auto clearAll = [&f] {   // a key that replaces the selection
        if (!f.all) return;
        f.text.clear();
        f.caret = 0;
        f.all   = false;
    };
    const bool ctrl = (ev.mod & ui::kCommand) != 0;
    switch (ev.key) {
        case dgl::kKeyEnter: endTextEdit(true); return true;
        case dgl::kKeyEscape: endTextEdit(false); return true;
        case dgl::kKeyBackspace:
            if (f.all) clearAll();
            else if (f.caret > 0) {
                const std::size_t from = previousChar(f.text, f.caret);
                f.text.erase(from, f.caret - from);
                f.caret = from;
            }
            break;
        case dgl::kKeyDelete:
            if (f.all) clearAll();
            else f.text.erase(f.caret, nextChar(f.text, f.caret) - f.caret);
            break;
        case dgl::kKeyLeft:
            f.caret = f.all ? 0 : previousChar(f.text, f.caret);
            f.all   = false;
            break;
        case dgl::kKeyRight:
            f.caret = f.all ? f.text.size() : nextChar(f.text, f.caret);
            f.all   = false;
            break;
        case dgl::kKeyHome:
            f.caret = 0;
            f.all   = false;
            break;
        case dgl::kKeyEnd:
            f.caret = f.text.size();
            f.all   = false;
            break;
        default:
            if (ctrl && (ev.key == 'a' || ev.key == 'A' || ev.key == 0x01)) f.all = true;
            break;   // characters arrive through onCharacterInput
    }
    repaint();
    return true;   // every key is the field's while it's open
}

bool UiWidget::onCharacterInput(const CharacterInputEvent& ev) {
    if (menu && menu->search && !field) {   // filtering a long menu
        if (ev.character < 32 || ev.character == 127 || (ev.mod & (ui::kCommand | dgl::kModifierControl)) != 0) return false;
        if (menu->typed.size() < 64) menu->typed += ev.string;
        menu->scroll = 0;
        repaint();
        return true;
    }
    if (!field) return false;
    if (ev.character < 32 || ev.character == 127 || (ev.mod & (ui::kCommand | dgl::kModifierControl)) != 0) return true;
    TextField&        f   = *field;
    const std::string add = ev.string;
    if (f.all) {
        f.text.clear();
        f.caret = 0;
        f.all   = false;
    }
    if (f.text.size() + add.size() <= f.maxBytes) {
        f.text.insert(f.caret, add);
        f.caret += add.size();
    }
    repaint();
    return true;
}

// --- menus ---------------------------------------------------------------------------

void UiWidget::openMenu(Menu m) {
    if (!m.overPanel) dismissOverlays();
    m.x  = std::clamp(m.x, 4.0f, std::max(4.0f, width() - m.w - 4.0f));
    menu = std::move(m);
}

namespace {

bool containsFolded(const std::string& text, const std::string& part) {
    auto fold = [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
    return std::search(text.begin(), text.end(), part.begin(), part.end(),
                       [&](char a, char b) { return fold(a) == fold(b); }) != text.end();
}

} // namespace

std::vector<std::size_t> UiWidget::menuShown() const {
    std::vector<std::size_t> shown;
    if (!menu) return shown;
    for (std::size_t i = 0; i < menu->items.size(); ++i)
        if (!menu->search || menu->typed.empty() || containsFolded(menu->items[i].label, menu->typed)) shown.push_back(i);
    return shown;
}

void UiWidget::drawMenu() {
    if (!menu) return;
    Menu&                          m      = *menu;
    const std::vector<std::size_t> shown  = menuShown();
    const float                    titleH = (m.title.empty() ? 0.0f : 28.0f) + (m.search ? 28.0f : 0.0f);
    const float totalH = titleH + static_cast<float>(std::max<std::size_t>(shown.size(), m.search ? 1 : 0)) * kMenuItemH + 8.0f;
    const float bottom = height() - 8.0f;
    if (m.y + totalH > bottom) m.y = std::max(8.0f, bottom - totalH);   // flip up if there's room
    const float visibleH = std::min(totalH, bottom - m.y);
    m.scroll             = std::clamp(m.scroll, 0.0f, totalH - visibleH);

    box(m.x, m.y, m.w, visibleH, kPanel, 6.0f);
    outline(m.x, m.y, m.w, visibleH, kTrack, 6.0f);
    region(m.x, m.y, m.w, visibleH, nullptr, [this, maxScroll = totalH - visibleH](float notches) {
        menu->scroll = std::clamp(menu->scroll - notches * kMenuItemH, 0.0f, maxScroll);
    });
    if (!m.title.empty()) {
        save();
        scissor(m.x, m.y, m.w - 8.0f, 28.0f);
        label(m.x + 12.0f, m.y + 16.0f, m.title, 12.0f, kDim);
        restore();
    }
    if (m.search) {   // what's typed, as a field
        const float fy = m.y + titleH - 26.0f;
        box(m.x + 8.0f, fy, m.w - 16.0f, 22.0f, kBarBg, 4.0f);
        save();
        scissor(m.x + 8.0f, fy, m.w - 16.0f, 22.0f);
        if (m.typed.empty()) {
            label(m.x + 14.0f, fy + 11.0f, "Type to filter", 13.0f, kDim);
        } else {
            label(m.x + 14.0f, fy + 11.0f, m.typed, 13.0f, kText);
            if (std::fmod(nowSeconds(), 1.0) < 0.6)
                box(m.x + 15.0f + labelWidth(m.typed, 13.0f), fy + 4.0f, 1.5f, 14.0f, kText);
        }
        restore();
    }

    const float listTop = m.y + titleH, listBottom = m.y + visibleH;
    save();
    scissor(m.x, listTop, m.w, listBottom - listTop);
    if (m.search && shown.empty())
        label(m.x + 30.0f, listTop + 4.0f + kMenuItemH / 2 - m.scroll, "Nothing matches", 14.0f, kDim);
    for (std::size_t row = 0; row < shown.size(); ++row) {
        const MenuItem& item = m.items[shown[row]];
        const float     iy   = listTop + 4.0f + static_cast<float>(row) * kMenuItemH - m.scroll;
        if (iy + kMenuItemH < listTop || iy > listBottom) continue;
        if (item.checked) label(m.x + 12.0f, iy + kMenuItemH / 2, "\xE2\x9C\x93", 14.0f, kAccent);   // U+2713
        label(m.x + 30.0f, iy + kMenuItemH / 2, item.label, 14.0f,
              item.warn ? kAmber : !item.action ? kDim : item.danger ? kRed : kText);
        const float top = std::max(iy, listTop), bot = std::min(iy + kMenuItemH, listBottom);
        region(m.x, top, m.w, bot - top, [this, action = item.action](const Press&) {
            if (!action) return;   // a disabled item: the menu stays
            menu.reset();          // before the action, which may open another menu
            action();
        });
    }
    restore();
}

bool UiWidget::menuKey(const KeyboardEvent& ev) {
    if (!menu || !menu->search || !ev.press) return false;
    if (ev.key == dgl::kKeyBackspace) {
        menu->typed.erase(previousChar(menu->typed, menu->typed.size()));   // a whole UTF-8 character
        menu->scroll = 0;
    } else if (ev.key == dgl::kKeyEnter) {   // the first that can be chosen
        for (std::size_t i : menuShown())
            if (const auto action = menu->items[i].action) {
                menu.reset();
                action();
                break;
            }
    } else {
        return false;
    }
    repaint();
    return true;
}

// --- the browser -------------------------------------------------------------------

// `open` brings the browser forward on the Mac. GNOME on Wayland raises a
// window only with an activation token: xdg-open gets an X11 startup ID as one
// (Ui/StartupId.h), which the browser (an instance already running, too)
// activates with. Elsewhere the two variables are ignored.
bool UiWidget::openInBrowser(const std::string& target) {
    // Single-quoted for the shell, its own quotes escaped.
    std::string quoted = "'";
    for (const char c : target) quoted += c == '\'' ? std::string("'\\''") : std::string(1, c);
    quoted += "'";
#ifdef __APPLE__
    return std::system(("open " + quoted).c_str()) == 0;
#else
    const std::string id  = startupId(lastPressTime);
    const std::string env = id.empty() ? "" : "XDG_ACTIVATION_TOKEN='" + id + "' DESKTOP_STARTUP_ID='" + id + "' ";
    return std::system((env + "xdg-open " + quoted + " >/dev/null 2>&1 &").c_str()) == 0;
#endif
}

// --- input ---------------------------------------------------------------------------

bool UiWidget::onMouse(const MouseEvent& ev) {
    // DGL numbers buttons left 1, right 2, middle 3 (pugl swaps X11's 2 and 3).
    if (ev.button != dgl::kMouseButtonLeft && ev.button != dgl::kMouseButtonRight) return false;
    if (!ev.press) {
        if (ev.button != dgl::kMouseButtonLeft) return false;
        if (dragEnd) {
            const auto end = std::move(dragEnd);
            dragEnd        = nullptr;
            end();
        }
        drag = nullptr;
        repaint();
        return true;
    }
    lastPressTime  = ev.time;
    const float sf = scaleFactor();
    const Press p{static_cast<float>(ev.pos.getX()) / sf, static_cast<float>(ev.pos.getY()) / sf, ev.mod, ev.time,
                  ev.button};
    for (auto r = regions.rbegin(); r != regions.rend(); ++r) {   // topmost first
        if (p.x < r->x || p.x >= r->x + r->w || p.y < r->y || p.y >= r->y + r->h) continue;
        if (p.button == dgl::kMouseButtonRight && !r->rightToo) return false;   // only where they're wanted
        if (r->press) {
            const auto press = r->press;   // a copy: the handler may change what's drawn
            press(p);
        }
        repaint();
        return true;
    }
    return false;
}

bool UiWidget::onMotion(const MotionEvent& ev) {
    if (!drag) return false;
    const float sf = scaleFactor();
    drag(static_cast<float>(ev.pos.getX()) / sf, static_cast<float>(ev.pos.getY()) / sf, ev.mod);
    repaint();
    return true;
}

bool UiWidget::onScroll(const ScrollEvent& ev) {
    const float sf = scaleFactor();
    const float x = static_cast<float>(ev.pos.getX()) / sf, y = static_cast<float>(ev.pos.getY()) / sf;
    if (ev.delta.getY() == 0 && ev.delta.getX() != 0) {   // sideways: no region takes it
        swipeUnclaimed(x, y, static_cast<float>(ev.delta.getX()));
        repaint();
        return true;
    }
    const float notches = static_cast<float>(ev.delta.getY());
    // The topmost region under the pointer that takes the wheel gets it.
    for (auto r = regions.rbegin(); r != regions.rend(); ++r) {
        if (x < r->x || x >= r->x + r->w || y < r->y || y >= r->y + r->h || !r->wheel) continue;
        const auto wheel = r->wheel;
        wheel(notches);
        repaint();
        return true;
    }
    wheelUnclaimed(x, y, notches, ev.mod);
    repaint();
    return true;
}

} // namespace stagehand
