#include "Ui/RackUi.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "Application.hpp"
#include "NanoVG.hpp"
#include "Window.hpp"

#include "App/RackController.h"

namespace stagehand {

namespace dgl = DGL_NAMESPACE;

namespace {

// --- look --------------------------------------------------------------------------
// Logical pixels, multiplied by the window's scale factor when drawn. Stage-first:
// dark, high contrast, big enough to hit.

constexpr float kToolbarH   = 44.0f;
constexpr float kRowH       = 64.0f;
constexpr float kStatusH    = 30.0f;
constexpr float kInputW     = 330.0f;
constexpr float kPluginW    = 270.0f;
constexpr float kMinOutputW = 330.0f;
constexpr float kPad        = 12.0f;
constexpr float kMinDb      = -60.0f;   // one scale for fader and meters, so they line up
constexpr float kMaxDb      = 6.0f;

struct Rgb {
    int r, g, b;
};
constexpr Rgb kBg{22, 24, 30}, kRowA{31, 34, 42}, kRowB{35, 38, 47}, kBarBg{16, 18, 23}, kText{232, 234, 240},
    kDim{140, 146, 160}, kAccent{90, 160, 255}, kAmber{240, 170, 50}, kRed{232, 72, 60}, kGreen{80, 200, 120},
    kTrack{58, 62, 74}, kLine{48, 52, 62};

float dbToPos(float db) {
    return std::clamp((db - kMinDb) / (kMaxDb - kMinDb), 0.0f, 1.0f);
}

std::string dbText(float db) {
    char text[32];
    std::snprintf(text, sizeof text, "%+.1f dB", static_cast<double>(db));
    return text;
}

std::string noteName(int note) {   // MIDI 60 = C4
    static const char* const names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return names[note % 12] + std::to_string(note / 12 - 1);
}

std::string channelText(std::uint16_t mask) {
    if (mask == 0xFFFF) return "ch all";
    if (mask == 0) return "ch none";
    std::string text = "ch ";
    int         shown = 0;
    for (int c = 0; c < 16; ++c) {
        if (((mask >> c) & 1u) == 0) continue;
        if (shown == 3) return text + ",...";
        text += (shown++ ? "," : "") + std::to_string(c + 1);
    }
    return text;
}

} // namespace

// --- the widget --------------------------------------------------------------------

class RackWidget final : public dgl::NanoTopLevelWidget {
public:
    RackWidget(dgl::Window& window, RackController& rackController, const std::string& notice,
               const double& noticeUntil)
        : NanoTopLevelWidget(window), controller(rackController), noticeText(notice), noticeExpiry(noticeUntil) {
        loadSharedResources();   // DGL's built-in font
    }

protected:
    void onNanoDisplay() override;
    bool onMouse(const MouseEvent& ev) override;
    bool onMotion(const MotionEvent& ev) override;
    bool onScroll(const ScrollEvent& ev) override;
    bool onKeyboard(const KeyboardEvent& ev) override;

private:
    enum class Target { None, Fader, Mute, Editor, Clip, MasterClip, Save };
    struct Hit {
        Target target = Target::None;
        int    row    = -1;
    };

    // Geometry of the current frame, in logical pixels.
    float scaleFactor() { return static_cast<float>(getWindow().getScaleFactor()); }
    float width() { return static_cast<float>(getWidth()) / scaleFactor(); }
    float height() { return static_cast<float>(getHeight()) / scaleFactor(); }
    float outputX() { return kInputW + kPluginW; }
    float outputW() { return std::max(kMinOutputW, width() - outputX()); }
    float rowsTop() { return kToolbarH; }
    float rowsHeight() { return height() - kToolbarH - kStatusH; }
    float rowY(int row) { return rowsTop() + static_cast<float>(row) * kRowH - scrollY; }
    float faderX0() { return outputX() + kPad + 30.0f + 14.0f; }
    float faderX1() { return outputX() + outputW() - kPad - 76.0f - 22.0f; }
    float maxScroll();

    Hit hitTest(float x, float y);

    // Drawing.
    void fill(Rgb c, int alpha = 255) { fillColor(c.r, c.g, c.b, alpha); }
    void box(float x, float y, float w, float h, Rgb c, float radius = 0.0f) {
        beginPath();
        radius > 0.0f ? roundedRect(x, y, w, h, radius) : rect(x, y, w, h);
        fill(c);
        NanoVG::fill();
    }
    void label(float x, float y, const std::string& text, float size, Rgb c, int align = ALIGN_LEFT | ALIGN_MIDDLE) {
        fontSize(size);
        fill(c);
        textAlign(align);
        this->text(x, y, text.c_str(), nullptr);
    }
    float labelWidth(const std::string& text, float size) {
        fontSize(size);
        dgl::Rectangle<float> bounds;
        return textBounds(0, 0, text.c_str(), nullptr, bounds);
    }
    void button(float x, float y, float w, float h, const std::string& text, bool on, bool enabled, Rgb onColor);
    void meterBars(float x0, float x1, float y, const float db[2], const float hold[2]);

    void drawToolbar();
    void drawRow(const RowModel& row, int index);
    void drawStatus();

    RackController& controller;
    const std::string& noticeText;
    const double&      noticeExpiry;

    float       scrollY = 0.0f;
    std::string dragId;              // the row whose fader is being dragged
    double      dragStartX  = 0.0;
    float       dragStartDb = 0.0f;
    std::string lastFaderId;         // double-click detection
    std::uint32_t lastFaderTime = 0;
};

float RackWidget::maxScroll() {
    const float content = static_cast<float>(controller.view().rows.size()) * kRowH;
    return std::max(0.0f, content - rowsHeight());
}

void RackWidget::button(float x, float y, float w, float h, const std::string& text, bool on, bool enabled,
                        Rgb onColor) {
    box(x, y, w, h, on ? onColor : kTrack, 4.0f);
    label(x + w / 2, y + h / 2, text, 13.0f, !enabled ? kDim : on ? kBarBg : kText, ALIGN_CENTER | ALIGN_MIDDLE);
}

// Two thin bars (L, R) on the fader's scale, with a peak-hold tick each.
void RackWidget::meterBars(float x0, float x1, float y, const float db[2], const float hold[2]) {
    const float w = x1 - x0;
    for (int c = 0; c < 2; ++c) {
        const float by = y + static_cast<float>(c) * 7.0f;
        box(x0, by, w, 5.0f, kBarBg);
        const float level = dbToPos(db[c]);
        if (db[c] > kMinDb) {
            // Green up to -6 dB, amber to 0, red above: drawn as three segments.
            const float greenEnd = dbToPos(-6.0f), amberEnd = dbToPos(0.0f);
            box(x0, by, w * std::min(level, greenEnd), 5.0f, kGreen);
            if (level > greenEnd) box(x0 + w * greenEnd, by, w * (std::min(level, amberEnd) - greenEnd), 5.0f, kAmber);
            if (level > amberEnd) box(x0 + w * amberEnd, by, w * (level - amberEnd), 5.0f, kRed);
        }
        if (hold[c] > kMinDb) box(x0 + w * dbToPos(hold[c]) - 1.0f, by, 2.0f, 5.0f, kText);
    }
}

void RackWidget::onNanoDisplay() {
    const float sf = scaleFactor();
    scale(sf, sf);
    box(0, 0, width(), height(), kBg);

    const RackViewModel& view = controller.view();
    scrollY = std::clamp(scrollY, 0.0f, maxScroll());

    scissor(0, rowsTop(), width(), rowsHeight());
    for (std::size_t i = 0; i < view.rows.size(); ++i) {
        const float y = rowY(static_cast<int>(i));
        if (y + kRowH < rowsTop() || y > rowsTop() + rowsHeight()) continue;
        drawRow(view.rows[i], static_cast<int>(i));
    }
    if (view.rows.empty())
        label(width() / 2, rowsTop() + rowsHeight() / 2, "This rack has no plugins yet.", 16.0f, kDim,
              ALIGN_CENTER | ALIGN_MIDDLE);
    resetScissor();

    drawToolbar();
    drawStatus();
}

void RackWidget::drawToolbar() {
    const RackViewModel& view = controller.view();
    box(0, 0, width(), kToolbarH, kBarBg);
    const std::string name = view.rackName.empty() ? "Untitled rack" : view.rackName;
    label(kPad, kToolbarH / 2, name, 18.0f, kText);
    if (view.modified) {
        const float x = kPad + labelWidth(name, 18.0f) + 12.0f;
        label(x, kToolbarH / 2, "unsaved changes", 13.0f, kAmber);
    }
    const bool canSave = !view.status.saveTarget.empty();
    button(width() - kPad - 70.0f, 8.0f, 70.0f, 28.0f, "Save", false, canSave, kAccent);
    label(width() - kPad - 70.0f - 12.0f, kToolbarH / 2, "Ctrl-Shift-S save    Ctrl-Q quit", 12.0f, kDim,
          ALIGN_RIGHT | ALIGN_MIDDLE);
}

void RackWidget::drawRow(const RowModel& row, int index) {
    const float y = rowY(index);
    box(0, y, width(), kRowH, index % 2 ? kRowB : kRowA);
    box(0, y + kRowH - 1.0f, width(), 1.0f, kLine);
    box(kInputW, y + 8.0f, 1.0f, kRowH - 16.0f, kLine);
    box(outputX(), y + 8.0f, 1.0f, kRowH - 16.0f, kLine);

    const bool running = row.state == RowState::Running;
    globalAlpha(running ? 1.0f : 0.5f);

    // Input: activity light, source, protocol badge; key range, channels, transpose.
    beginPath();
    circle(kPad + 5.0f, y + 22.0f, 5.0f);
    fill(row.midiActive ? kGreen : kTrack);
    NanoVG::fill();
    save();
    scissor(kPad + 16.0f, y, kInputW - kPad * 2 - 16.0f - 62.0f, kRowH);
    label(kPad + 16.0f, y + 22.0f, row.sourceLabel, 14.0f, row.sourceConnected ? kText : kRed);
    restore();
    const char* protocol = row.sourceProtocol == SourceProtocol::Midi2   ? "2.0"
                           : row.sourceProtocol == SourceProtocol::Midi1 ? "1.0"
                                                                         : "mix";
    button(kInputW - kPad - 36.0f, y + 13.0f, 36.0f, 18.0f, protocol, row.sourceProtocol == SourceProtocol::Midi2,
           true, kAccent);
    if (row.protocolWarning) {   // MIDI 2.0 into a MIDI 1.0 plugin: per-note data is lost
        beginPath();
        circle(kInputW - kPad - 50.0f, y + 22.0f, 8.0f);
        fill(kAmber);
        NanoVG::fill();
        label(kInputW - kPad - 50.0f, y + 22.0f, "!", 13.0f, kBarBg, ALIGN_CENTER | ALIGN_MIDDLE);
    }
    const float barX = kPad + 16.0f;   // 128 keys at 1 logical pixel each
    box(barX, y + 39.0f, 128.0f, 10.0f, kBarBg);
    box(barX + row.filter.noteLow, y + 39.0f, static_cast<float>(row.filter.noteHigh - row.filter.noteLow + 1), 10.0f,
        kAccent);
    std::string range = noteName(row.filter.noteLow) + "-" + noteName(row.filter.noteHigh) + "  " +
                        channelText(row.filter.channelMask);
    if (row.filter.transpose != 0)
        range += (row.filter.transpose > 0 ? "  +" : "  ") + std::to_string(row.filter.transpose);
    label(barX + 136.0f, y + 44.0f, range, 12.0f, kDim);

    // Plugin: slot name, plugin and vendor (or why it didn't load), editor button.
    const float px      = kInputW + kPad;
    const float editorX = kInputW + kPluginW - kPad - 70.0f;
    save();
    scissor(px, y, editorX - px - 8.0f, kRowH);
    label(px, y + 23.0f, row.name, 16.0f, kText);
    if (row.state == RowState::Placeholder) {
        label(px, y + 44.0f, "not loaded: " + row.problem, 12.0f, kRed);
    } else {
        std::string detail = row.pluginName + (row.vendor.empty() ? "" : " - " + row.vendor);
        if (row.state == RowState::Disabled) detail = "disabled - " + detail;
        label(px, y + 44.0f, detail, 12.0f, kDim);
    }
    restore();
    button(editorX, y + 19.0f, 70.0f, 26.0f, "Editor", row.editorOpen, row.hasEditor, kAccent);

    // Output: mute, gain fader over the meters, clip light, gain and output text.
    const float ox = outputX() + kPad;
    button(ox, y + 10.0f, 30.0f, 26.0f, "M", row.muted, true, kAmber);
    const float x0 = faderX0(), x1 = faderX1();
    box(x0, y + 17.0f, x1 - x0, 6.0f, kTrack, 3.0f);
    const float thumb = x0 + (x1 - x0) * dbToPos(row.gainDb);
    box(x0 + (x1 - x0) * dbToPos(0.0f) - 0.5f, y + 13.0f, 1.0f, 14.0f, kDim);   // 0 dB mark
    box(thumb - 5.0f, y + 9.0f, 10.0f, 22.0f, row.muted ? kDim : kText, 2.0f);
    meterBars(x0, x1, y + 37.0f, row.meterDb, row.peakHoldDb);
    box(x1 + 8.0f, y + 36.0f, 14.0f, 14.0f, row.clipped ? kRed : kBarBg, 2.0f);
    label(x1 + 30.0f, y + 21.0f, row.muted ? "muted" : dbText(row.gainDb), 13.0f, row.muted ? kAmber : kText);
    label(x1 + 30.0f, y + 44.0f, "out " + std::to_string(row.firstOutput + 1) + "-" +
                                     std::to_string(row.firstOutput + 2), 12.0f, kDim);
    globalAlpha(1.0f);
}

void RackWidget::drawStatus() {
    const StatusModel& s = controller.view().status;
    const float        y = height() - kStatusH;
    box(0, y, width(), kStatusH, kBarBg);

    char stats[160];
    std::snprintf(stats, sizeof stats, "%.0f Hz   %u frames   xruns %llu   MIDI %s", s.sampleRate, s.bufferSize,
                  static_cast<unsigned long long>(s.xruns), s.preference == MidiProtocol::Midi2 ? "2.0" : "1.0");
    std::string left = stats;
    if (s.midiDropped != 0) left += "   MIDI dropped " + std::to_string(s.midiDropped);
    label(kPad, y + kStatusH / 2, left, 12.0f, s.xruns != 0 || s.midiDropped != 0 ? kAmber : kDim);

    // Master meter at the right; the notice or save target between.
    const float mx1 = width() - kPad - 22.0f, mx0 = mx1 - 180.0f;
    const float hold[2] = {s.outputDb[0], s.outputDb[1]};
    meterBars(mx0, mx1, y + 9.0f, s.outputDb, hold);
    box(mx1 + 8.0f, y + 8.0f, 14.0f, 14.0f, s.outputClipped ? kRed : kTrack, 2.0f);

    const float tx = kPad + labelWidth(left, 12.0f) + 28.0f;
    save();
    scissor(tx, y, std::max(0.0f, mx0 - 12.0f - tx), kStatusH);
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (!noticeText.empty() && now < noticeExpiry)
        label(tx, y + kStatusH / 2, noticeText, 12.0f, kAccent);
    else if (!s.saveTarget.empty())
        label(tx, y + kStatusH / 2, "Saves to " + s.saveTarget, 12.0f, kDim);
    else
        label(tx, y + kStatusH / 2, "Not saving: " + s.noSaveReason, 12.0f, kAmber);
    restore();
}

// --- input -------------------------------------------------------------------------

RackWidget::Hit RackWidget::hitTest(float x, float y) {
    if (y < kToolbarH) {
        const float sx = width() - kPad - 70.0f;
        if (x >= sx && x <= sx + 70.0f && y >= 8.0f && y <= 36.0f) return {Target::Save, -1};
        return {};
    }
    if (y >= height() - kStatusH) {
        const float cx = width() - kPad - 14.0f;
        if (x >= cx - 4.0f && x <= cx + 18.0f) return {Target::MasterClip, -1};
        return {};
    }
    const int row = static_cast<int>(std::floor((y - rowsTop() + scrollY) / kRowH));
    if (row < 0 || row >= static_cast<int>(controller.view().rows.size())) return {};
    const float ry = y - rowY(row);   // within the row

    const float editorX = kInputW + kPluginW - kPad - 70.0f;
    if (x >= editorX && x <= editorX + 70.0f && ry >= 19.0f && ry <= 45.0f) return {Target::Editor, row};
    const float ox = outputX() + kPad;
    if (x >= ox && x <= ox + 30.0f && ry >= 10.0f && ry <= 36.0f) return {Target::Mute, row};
    if (x >= faderX0() - 8.0f && x <= faderX1() + 8.0f && ry >= 5.0f && ry <= 33.0f) return {Target::Fader, row};
    if (x >= faderX1() + 4.0f && x <= faderX1() + 26.0f && ry >= 32.0f && ry <= 54.0f) return {Target::Clip, row};
    return {};
}

bool RackWidget::onMouse(const MouseEvent& ev) {
    if (ev.button != 1) return false;
    const float sf = scaleFactor();
    const float x = static_cast<float>(ev.pos.getX()) / sf, y = static_cast<float>(ev.pos.getY()) / sf;
    if (!ev.press) {
        dragId.clear();
        return true;
    }

    const RackViewModel& view = controller.view();
    const Hit            hit  = hitTest(x, y);
    const RowModel*      row  = hit.row >= 0 ? &view.rows[static_cast<std::size_t>(hit.row)] : nullptr;
    switch (hit.target) {
        case Target::Save: controller.save(); break;
        case Target::MasterClip: controller.clearOutputClip(); break;
        case Target::Clip: controller.clearClip(row->id); break;
        case Target::Mute: controller.setMuted(row->id, !row->muted); break;
        case Target::Editor:
            if (row->editorOpen)
                controller.closeEditor(row->id);
            else if (row->hasEditor)
                controller.openEditor(row->id);
            break;
        case Target::Fader:
            // Double-click resets to 0 dB. Otherwise the fader moves with the drag,
            // not to where it was clicked: a stray click on stage changes nothing.
            if (row->id == lastFaderId && ev.time - lastFaderTime < 400) {
                controller.setGainDb(row->id, 0.0f);
                lastFaderId.clear();
                break;
            }
            lastFaderId   = row->id;
            lastFaderTime = ev.time;
            dragId        = row->id;
            dragStartX    = x;
            dragStartDb   = row->gainDb;
            break;
        case Target::None: return false;
    }
    repaint();
    return true;
}

bool RackWidget::onMotion(const MotionEvent& ev) {
    if (dragId.empty()) return false;
    const float  x       = static_cast<float>(ev.pos.getX()) / scaleFactor();
    const float  perPx   = (kMaxDb - kMinDb) / (faderX1() - faderX0()) * ((ev.mod & dgl::kModifierShift) ? 0.1f : 1.0f);
    float        db      = dragStartDb + static_cast<float>(x - dragStartX) * perPx;
    db                   = std::clamp(std::round(db * 10.0f) / 10.0f, kMinDb, kMaxDb);
    if (std::fabs(db) < 0.35f && (ev.mod & dgl::kModifierShift) == 0) db = 0.0f;   // detent at unity
    controller.setGainDb(dragId, db);
    repaint();
    return true;
}

bool RackWidget::onScroll(const ScrollEvent& ev) {
    const float sf = scaleFactor();
    const Hit   hit = hitTest(static_cast<float>(ev.pos.getX()) / sf, static_cast<float>(ev.pos.getY()) / sf);
    if (hit.target == Target::Fader) {   // half a dB per notch
        const RowModel& row = controller.view().rows[static_cast<std::size_t>(hit.row)];
        const float     db  = std::clamp(row.gainDb + static_cast<float>(ev.delta.getY()) * 0.5f, kMinDb, kMaxDb);
        controller.setGainDb(row.id, std::round(db * 10.0f) / 10.0f);
    } else {
        scrollY = std::clamp(scrollY - static_cast<float>(ev.delta.getY()) * kRowH / 2.0f, 0.0f, maxScroll());
    }
    repaint();
    return true;
}

bool RackWidget::onKeyboard(const KeyboardEvent& ev) {
    if (!ev.press || (ev.mod & dgl::kModifierControl) == 0) return false;
    // With Control held, X11 may report the control character instead of the letter.
    const bool q = ev.key == 'q' || ev.key == 'Q' || ev.key == 0x11;
    const bool s = ev.key == 's' || ev.key == 'S' || ev.key == 0x13;
    if (q) {
        controller.quit();
        return true;
    }
    if (s && (ev.mod & dgl::kModifierShift) != 0) {
        controller.save();
        return true;
    }
    return false;
}

// --- RackUi ------------------------------------------------------------------------

struct RackUi::Impl {
    explicit Impl(RackController& rackController) : controller(rackController), app(true), window(app) {}

    RackController&             controller;
    dgl::Application            app;
    dgl::Window                 window;
    std::unique_ptr<RackWidget> widget;
    std::string                 notice;
    double                      noticeUntil = 0.0;
};

std::unique_ptr<RackUi> RackUi::open(RackController& controller) {
    if (std::getenv("DISPLAY") == nullptr) {
        std::fputs("No display (DISPLAY isn't set); running without the rack window.\n", stderr);
        return nullptr;
    }
    auto impl = std::make_unique<Impl>(controller);
    {
        // The NanoVG context is created in the widget's constructor, so the
        // window's GL context must be current (the spike's lesson).
        const dgl::Window::ScopedGraphicsContext context(impl->window);
        impl->widget = std::make_unique<RackWidget>(impl->window, controller, impl->notice, impl->noticeUntil);
    }

    const double sf      = impl->window.getScaleFactor();
    const auto   rows    = static_cast<float>(std::clamp<std::size_t>(controller.view().rows.size(), 3, 10));
    const float  minW    = kInputW + kPluginW + kMinOutputW;
    const float  minH    = kToolbarH + kRowH * 2.0f + kStatusH;
    const float  startH  = kToolbarH + kRowH * rows + kStatusH;
    const auto   px      = [sf](float logical) { return static_cast<unsigned>(std::lround(logical * sf)); };
    const std::string title = "StageHand - " + (controller.view().rackName.empty() ? std::string("Untitled rack")
                                                                                   : controller.view().rackName);
    impl->window.setTitle(title.c_str());
    impl->window.setResizable(true);
    impl->window.setGeometryConstraints(px(minW), px(minH));
    impl->window.setSize(px(minW + 60.0f), px(startH));
    impl->window.show();
    return std::unique_ptr<RackUi>(new RackUi(std::move(impl)));
}

RackUi::RackUi(std::unique_ptr<Impl> ui) : impl(std::move(ui)) {}

RackUi::~RackUi() {
    // DGL's teardown order: the widget (its NanoVG context) with the GL context
    // current, then close the window and put the Application in quitting state.
    {
        const dgl::Window::ScopedGraphicsContext context(impl->window);
        impl->widget.reset();
    }
    impl->window.close();
    impl->app.quit();
}

void RackUi::idle() {
    impl->app.idle();
}

void RackUi::refresh() {
    if (impl->window.isVisible()) impl->widget->repaint();
}

void RackUi::showNotice(const std::string& text) {
    impl->notice      = text;
    impl->noticeUntil = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count() + 4.0;
}

bool RackUi::closed() const {
    // Closing the last window puts DGL's Application in quitting state. Not
    // visibility: a minimized window isn't visible, and must not quit.
    return impl->app.isQuitting();
}

} // namespace stagehand
