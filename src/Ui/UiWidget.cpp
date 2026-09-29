#include "Ui/UiWidget.h"

#include <algorithm>
#include <chrono>

namespace stagehand {

namespace dgl = DGL_NAMESPACE;
using namespace ui;

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

void UiWidget::label(float x, float y, const std::string& text, float size, UiColor c, int align) {
    fontSize(size);
    fill(c);
    textAlign(align);
    this->text(x, y, text.c_str(), nullptr);
}

float UiWidget::labelWidth(const std::string& text, float size) {
    fontSize(size);
    dgl::Rectangle<float> bounds;
    return textBounds(0, 0, text.c_str(), nullptr, bounds);
}

void UiWidget::button(float x, float y, float w, float h, const std::string& text, bool on, bool enabled,
                      UiColor onColor) {
    box(x, y, w, h, on ? onColor : kTrack, 4.0f);
    label(x + w / 2, y + h / 2, text, 13.0f, !enabled ? kDim : on ? kBarBg : kText, ALIGN_CENTER | ALIGN_MIDDLE);
}

void UiWidget::dotsButton(float x, float y, float w, float h) {
    box(x, y, w, h, kTrack, 4.0f);
    for (int i = -1; i <= 1; ++i) {
        beginPath();
        circle(x + w / 2 + static_cast<float>(i) * 6.0f, y + h / 2, 2.0f);
        fill(kText);
        NanoVG::fill();
    }
}

void UiWidget::region(float x, float y, float w, float h, std::function<void(const Press&)> press,
                      std::function<void(float)> wheel, bool rightToo) {
    regions.push_back(Region{x, y, w, h, std::move(press), std::move(wheel), rightToo});
}

void UiWidget::stepper(float x, float y, const std::string& value,
                       const std::function<void(int dir, bool big)>& change) {
    button(x, y, 22.0f, 22.0f, "-", false, true, kAccent);
    region(x, y, 22.0f, 22.0f, [change](const Press& p) { change(-1, (p.mod & dgl::kModifierShift) != 0); });
    label(x + 70.0f, y + 11.0f, value, 13.0f, kText, ALIGN_CENTER | ALIGN_MIDDLE);
    region(x + 22.0f, y, 96.0f, 22.0f, nullptr, [change](float notches) { change(notches > 0 ? 1 : -1, false); });
    button(x + 118.0f, y, 22.0f, 22.0f, "+", false, true, kAccent);
    region(x + 118.0f, y, 22.0f, 22.0f, [change](const Press& p) { change(1, (p.mod & dgl::kModifierShift) != 0); });
}

// --- the frame -----------------------------------------------------------------------

void UiWidget::onNanoDisplay() {
    regions.clear();
    const float sf = scaleFactor();
    scale(sf, sf);
    draw();
    if (menu || overlayOpen()) {
        // A click anywhere else dismisses the overlay (and does nothing else).
        region(0, 0, width(), height(),
               [this](const Press&) {
                   menu.reset();
                   dismissOverlays();
               },
               [](float) {}, true);
        drawOverlays();
        drawMenu();
    }
}

// --- menus ---------------------------------------------------------------------------

void UiWidget::openMenu(Menu m) {
    dismissOverlays();
    m.x  = std::clamp(m.x, 4.0f, std::max(4.0f, width() - m.w - 4.0f));
    menu = std::move(m);
}

void UiWidget::drawMenu() {
    if (!menu) return;
    Menu&       m      = *menu;
    const float titleH = m.title.empty() ? 0.0f : 28.0f;
    const float totalH = titleH + static_cast<float>(m.items.size()) * kMenuItemH + 8.0f;
    const float bottom = height() - 8.0f;
    if (m.y + totalH > bottom) m.y = std::max(8.0f, bottom - totalH);   // flip up if there's room
    const float visibleH = std::min(totalH, bottom - m.y);
    m.scroll             = std::clamp(m.scroll, 0.0f, totalH - visibleH);

    box(m.x, m.y, m.w, visibleH, kPanel, 6.0f);
    outline(m.x, m.y, m.w, visibleH, kTrack, 6.0f);
    region(m.x, m.y, m.w, visibleH, nullptr, [this, maxScroll = totalH - visibleH](float notches) {
        menu->scroll = std::clamp(menu->scroll - notches * kMenuItemH, 0.0f, maxScroll);
    });
    if (titleH > 0.0f) {
        save();
        scissor(m.x, m.y, m.w - 8.0f, titleH);
        label(m.x + 12.0f, m.y + titleH / 2 + 2.0f, m.title, 12.0f, kDim);
        restore();
    }

    const float listTop = m.y + titleH, listBottom = m.y + visibleH;
    save();
    scissor(m.x, listTop, m.w, listBottom - listTop);
    for (std::size_t i = 0; i < m.items.size(); ++i) {
        const MenuItem& item = m.items[i];
        const float     iy   = listTop + 4.0f + static_cast<float>(i) * kMenuItemH - m.scroll;
        if (iy + kMenuItemH < listTop || iy > listBottom) continue;
        if (item.checked) label(m.x + 12.0f, iy + kMenuItemH / 2, "\xE2\x9C\x93", 14.0f, kAccent);   // U+2713
        label(m.x + 30.0f, iy + kMenuItemH / 2, item.label, 14.0f, !item.action ? kDim : item.danger ? kRed : kText);
        const float top = std::max(iy, listTop), bot = std::min(iy + kMenuItemH, listBottom);
        region(m.x, top, m.w, bot - top, [this, action = item.action](const Press&) {
            if (!action) return;   // a disabled item: the menu stays
            menu.reset();          // before the action, which may open another menu
            action();
        });
    }
    restore();
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
