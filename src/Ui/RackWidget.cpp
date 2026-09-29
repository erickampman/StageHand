#include "Ui/RackWidget.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

#include "Core/Ump.h"

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
constexpr float kKeyBarX    = kPad + 16.0f;   // 128 keys at 1 logical pixel each
constexpr float kMenuItemH  = 26.0f;

constexpr UiColor kBg{22, 24, 30}, kRowA{31, 34, 42}, kRowB{35, 38, 47}, kBarBg{16, 18, 23}, kPanel{44, 48, 59},
    kText{232, 234, 240}, kDim{140, 146, 160}, kAccent{90, 160, 255}, kAmber{240, 170, 50}, kRed{232, 72, 60},
    kGreen{80, 200, 120}, kTrack{58, 62, 74}, kLine{48, 52, 62};

double nowSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

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

std::string maskText(const char* what, std::uint16_t mask) {
    if (mask == 0xFFFF) return std::string(what) + " all";
    std::string text  = what;
    int         shown = 0;
    for (int i = 0; i < 16; ++i) {
        if (((mask >> i) & 1u) == 0) continue;
        if (shown == 3) return text + ",...";
        text += (shown++ ? "," : " ") + std::to_string(i + 1);
    }
    return text;
}

// Toggle one channel or group. From "all", a click selects just that one; the
// last one can't be switched off (a row hearing nothing is a stage hazard).
std::uint16_t toggleBit(std::uint16_t mask, int bit) {
    const auto b = static_cast<std::uint16_t>(1u << bit);
    if (mask == 0xFFFF) return b;
    const auto next = static_cast<std::uint16_t>(mask ^ b);
    return next == 0 ? mask : next;
}

int velocity7(std::uint16_t v16) {
    return static_cast<int>(ump::scaleDown(v16, 16, 7));
}

std::uint16_t velocity16(int v7) {
    return static_cast<std::uint16_t>(ump::scaleUp(static_cast<std::uint32_t>(v7), 7, 16));
}

bool sameSource(const MidiSourceRef& a, const MidiSourceRef& b) {
    return a.kind == b.kind && (a.kind == MidiSourceRef::Kind::All || a.name == b.name);
}

const char* protocolText(SourceProtocol p) {
    return p == SourceProtocol::Midi2 ? "2.0" : p == SourceProtocol::Midi1 ? "1.0" : "mix";
}

} // namespace

RackWidget::RackWidget(dgl::Window& window, RackController& rackController)
    : NanoTopLevelWidget(window), controller(rackController) {
    loadSharedResources();   // DGL's built-in font (DejaVu Sans)
}

void RackWidget::showNotice(const std::string& text, double now) {
    notice      = text;
    noticeUntil = now + 4.0;
}

// --- geometry ------------------------------------------------------------------------

float RackWidget::scaleFactor() {
    return static_cast<float>(getWindow().getScaleFactor());
}
float RackWidget::width() {
    return static_cast<float>(getWidth()) / scaleFactor();
}
float RackWidget::height() {
    return static_cast<float>(getHeight()) / scaleFactor();
}
float RackWidget::outputX() const {
    return kInputW + kPluginW;
}
float RackWidget::outputW() {
    return std::max(kMinOutputW, width() - outputX());
}
float RackWidget::rowsHeight() {
    return height() - kToolbarH - kStatusH;
}
float RackWidget::rowY(std::size_t row) const {
    return kToolbarH + static_cast<float>(row) * kRowH - scrollY;
}
float RackWidget::faderX0() const {
    return outputX() + kPad + 30.0f + 14.0f;
}
float RackWidget::faderX1() {
    return outputX() + outputW() - kPad - 76.0f - 22.0f;
}
float RackWidget::maxScroll() {
    return std::max(0.0f, static_cast<float>(controller.view().rows.size()) * kRowH - rowsHeight());
}

// --- drawing helpers -----------------------------------------------------------------

void RackWidget::fill(UiColor c, int alpha) {
    fillColor(c.r, c.g, c.b, alpha);
}

void RackWidget::box(float x, float y, float w, float h, UiColor c, float radius) {
    beginPath();
    if (radius > 0.0f)
        roundedRect(x, y, w, h, radius);
    else
        rect(x, y, w, h);
    fill(c);
    NanoVG::fill();
}

void RackWidget::outline(float x, float y, float w, float h, UiColor c, float radius) {
    beginPath();
    roundedRect(x + 0.5f, y + 0.5f, w - 1.0f, h - 1.0f, radius);
    strokeColor(c.r, c.g, c.b);
    strokeWidth(1.0f);
    stroke();
}

void RackWidget::label(float x, float y, const std::string& text, float size, UiColor c, int align) {
    fontSize(size);
    fill(c);
    textAlign(align);
    this->text(x, y, text.c_str(), nullptr);
}

float RackWidget::labelWidth(const std::string& text, float size) {
    fontSize(size);
    dgl::Rectangle<float> bounds;
    return textBounds(0, 0, text.c_str(), nullptr, bounds);
}

void RackWidget::button(float x, float y, float w, float h, const std::string& text, bool on, bool enabled,
                        UiColor onColor) {
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
            const float greenEnd = dbToPos(-6.0f), amberEnd = dbToPos(0.0f);
            box(x0, by, w * std::min(level, greenEnd), 5.0f, kGreen);
            if (level > greenEnd) box(x0 + w * greenEnd, by, w * (std::min(level, amberEnd) - greenEnd), 5.0f, kAmber);
            if (level > amberEnd) box(x0 + w * amberEnd, by, w * (level - amberEnd), 5.0f, kRed);
        }
        if (hold[c] > kMinDb) box(x0 + w * dbToPos(hold[c]) - 1.0f, by, 2.0f, 5.0f, kText);
    }
}

void RackWidget::region(float x, float y, float w, float h, std::function<void(const Press&)> press,
                        std::function<void(float)> wheel) {
    regions.push_back(Region{x, y, w, h, std::move(press), std::move(wheel)});
}

// --- the frame -----------------------------------------------------------------------

void RackWidget::onNanoDisplay() {
    regions.clear();
    const float sf = scaleFactor();
    scale(sf, sf);
    box(0, 0, width(), height(), kBg);

    const RackViewModel& view = controller.view();
    scrollY = std::clamp(scrollY, 0.0f, maxScroll());

    scissor(0, kToolbarH, width(), rowsHeight());
    for (std::size_t i = 0; i < view.rows.size(); ++i) {
        const float y = rowY(i);
        if (y + kRowH < kToolbarH || y > kToolbarH + rowsHeight()) continue;
        drawRow(view.rows[i], i);
    }
    if (view.rows.empty())
        label(width() / 2, kToolbarH + rowsHeight() / 2, "This rack has no plugins yet. Add one with + Add plugin.",
              16.0f, kDim, ALIGN_CENTER | ALIGN_MIDDLE);
    resetScissor();

    // Drawn after the rows, so their regions cover rows scrolled under them.
    drawToolbar();
    drawStatus();

    if (menu || !filterId.empty()) {
        // A click anywhere else dismisses the overlay (and does nothing else).
        region(0, 0, width(), height(), [this](const Press&) {
            menu.reset();
            filterId.clear();
        }, [](float) {});
        drawFilterPanel();
        drawMenu();
    }
}

void RackWidget::drawToolbar() {
    const RackViewModel& view = controller.view();
    box(0, 0, width(), kToolbarH, kBarBg);
    region(0, 0, width(), kToolbarH, nullptr);

    const std::string name = view.rackName.empty() ? "Untitled rack" : view.rackName;
    save();
    scissor(kPad, 0, kInputW - kPad * 2, kToolbarH);
    label(kPad, kToolbarH / 2 - (view.modified ? 6.0f : 0.0f), name, 18.0f, kText);
    if (view.modified) label(kPad, kToolbarH / 2 + 12.0f, "unsaved changes", 11.0f, kAmber);
    restore();
    drawTransport(kInputW);

    const bool  canSave = !view.status.saveTarget.empty();
    const float saveX   = width() - kPad - 70.0f;
    button(saveX, 8.0f, 70.0f, 28.0f, "Save", false, canSave, kAccent);
    if (canSave) region(saveX, 8.0f, 70.0f, 28.0f, [this](const Press&) { controller.save(); });

    const float addX = saveX - 10.0f - 118.0f;
    button(addX, 8.0f, 118.0f, 28.0f, "+ Add plugin", false, true, kAccent);
    region(addX, 8.0f, 118.0f, 28.0f, [this, addX](const Press&) { openAddMenu(addX, kToolbarH); });
}

// Tempo (drag up/down, Shift for fine; wheel; double-click for 120), Tap, time
// signature (a menu), and a light per beat of the bar.
void RackWidget::drawTransport(float x) {
    const TransportModel& t = controller.view().transport;
    char                  text[32];
    std::snprintf(text, sizeof text, "%.1f BPM", t.tempoBpm);
    box(x, 8.0f, 104.0f, 28.0f, kTrack, 4.0f);
    label(x + 52.0f, kToolbarH / 2, text, 14.0f, kText, ALIGN_CENTER | ALIGN_MIDDLE);
    region(
        x, 8.0f, 104.0f, 28.0f,
        [this](const Press& p) {
            if (tempoClicked && p.time - lastTempoTime < 400) {
                controller.setTempo(120.0);
                tempoClicked = false;
                return;
            }
            tempoClicked  = true;
            lastTempoTime = p.time;
            drag = [this, start = controller.view().transport.tempoBpm, startY = p.y](float, float y,
                                                                                        std::uint32_t mod) {
                const double perPixel = (mod & dgl::kModifierShift) != 0 ? 0.05 : 0.5;
                controller.setTempo(start + (startY - y) * perPixel);   // up is faster
            };
        },
        [this](float notches) { controller.setTempo(controller.view().transport.tempoBpm + notches); });

    button(x + 110.0f, 8.0f, 48.0f, 28.0f, "Tap", false, true, kAccent);
    region(x + 110.0f, 8.0f, 48.0f, 28.0f, [this](const Press& p) { controller.tapTempo(p.time / 1000.0); });

    std::snprintf(text, sizeof text, "%d/%d", t.numerator, t.denominator);
    button(x + 164.0f, 8.0f, 52.0f, 28.0f, text, false, true, kAccent);
    region(x + 164.0f, 8.0f, 52.0f, 28.0f, [this, x](const Press&) {
        Menu m;
        m.x     = x + 164.0f;
        m.y     = kToolbarH;
        m.w     = 140.0f;
        m.title = "Time signature";
        const TransportModel& now = controller.view().transport;
        for (const auto [n, d] : {std::pair{2, 4}, {3, 4}, {4, 4}, {5, 4}, {6, 8}, {7, 8}, {9, 8}, {12, 8}})
            m.items.push_back(MenuItem{std::to_string(n) + "/" + std::to_string(d),
                                       [this, n, d] { controller.setTimeSignature(n, d); },
                                       now.numerator == n && now.denominator == d, false});
        openMenu(std::move(m));
    });

    const int beats = std::clamp(t.beatsInBar, 1, 12);
    for (int b = 0; b < beats; ++b) {
        const bool lit = t.onBeat && b == t.beatInBar;
        beginPath();
        circle(x + 232.0f + static_cast<float>(b) * 15.0f, kToolbarH / 2, 5.0f);
        fill(!lit ? kTrack : b == 0 ? kAmber : kGreen);   // the downbeat stands out
        NanoVG::fill();
    }
}

void RackWidget::drawRow(const RowModel& row, std::size_t index) {
    const float y = rowY(index);
    box(0, y, width(), kRowH, index % 2 ? kRowB : kRowA);
    box(0, y + kRowH - 1.0f, width(), 1.0f, kLine);
    box(kInputW, y + 8.0f, 1.0f, kRowH - 16.0f, kLine);
    box(outputX(), y + 8.0f, 1.0f, kRowH - 16.0f, kLine);

    globalAlpha(row.state == RowState::Running ? 1.0f : 0.55f);
    drawRowInput(row, y);
    drawRowPlugin(row, y);
    if (row.state != RowState::Placeholder) drawRowOutput(row, y);
    globalAlpha(1.0f);
}

// Input: activity light, source (a menu), protocol badge; key range (drag its
// ends, or the middle to move it) and the filter chip (opens the filter panel).
void RackWidget::drawRowInput(const RowModel& row, float y) {
    beginPath();
    circle(kPad + 5.0f, y + 22.0f, 5.0f);
    fill(row.midiActive ? kGreen : kTrack);
    NanoVG::fill();

    const float sourceW = kInputW - kPad * 2 - 16.0f - 62.0f;
    save();
    scissor(kKeyBarX, y, sourceW, kRowH);
    const std::string source = row.sourceLabel + "  \xE2\x96\xBE";   // U+25BE small down triangle
    label(kKeyBarX, y + 22.0f, source, 14.0f, row.sourceConnected ? kText : kRed);
    restore();
    region(kKeyBarX - 4.0f, y + 8.0f, sourceW + 4.0f, 28.0f,
           [this, row, y](const Press&) { openSourceMenu(row, kKeyBarX, y + 36.0f); });

    button(kInputW - kPad - 36.0f, y + 13.0f, 36.0f, 18.0f, protocolText(row.sourceProtocol),
           row.sourceProtocol == SourceProtocol::Midi2, true, kAccent);
    if (row.protocolWarning) {   // MIDI 2.0 into a MIDI 1.0 plugin: per-note data is lost
        beginPath();
        circle(kInputW - kPad - 50.0f, y + 22.0f, 8.0f);
        fill(kAmber);
        NanoVG::fill();
        label(kInputW - kPad - 50.0f, y + 22.0f, "!", 13.0f, kBarBg, ALIGN_CENTER | ALIGN_MIDDLE);
    }

    const MidiFilter f = filterOf(row);
    box(kKeyBarX, y + 39.0f, 128.0f, 10.0f, kBarBg);
    box(kKeyBarX + f.noteLow, y + 39.0f, static_cast<float>(f.noteHigh - f.noteLow + 1), 10.0f, kAccent);
    region(kKeyBarX - 6.0f, y + 34.0f, 140.0f, 20.0f,
           [this, row](const Press& p) { startKeyRangeDrag(row, kKeyBarX, p); });

    std::string summary = noteName(f.noteLow) + "-" + noteName(f.noteHigh) + "  " + maskText("ch", f.channelMask);
    if (f.transpose != 0) summary += (f.transpose > 0 ? "  +" : "  ") + std::to_string(f.transpose);
    const float chipX = kKeyBarX + 136.0f, chipW = kInputW - kPad - chipX;
    const bool  open  = filterId == row.id;
    box(chipX, y + 35.0f, chipW, 18.0f, open ? kAccent : kTrack, 4.0f);
    save();
    scissor(chipX, y, chipW - 4.0f, kRowH);
    label(chipX + 6.0f, y + 44.0f, summary, 12.0f, open ? kBarBg : kText);
    restore();
    region(chipX, y + 33.0f, chipW, 22.0f, [this, row](const Press&) {
        menu.reset();
        if (filterId == row.id) {
            filterId.clear();
            return;
        }
        filterId    = row.id;
        panelFilter = row.filter;
    });
}

// Plugin: slot name, plugin and vendor (or why it didn't load), the row menu,
// and the editor button.
void RackWidget::drawRowPlugin(const RowModel& row, float y) {
    const float px      = kInputW + kPad;
    const float editorX = kInputW + kPluginW - kPad - 70.0f;
    const float menuX   = editorX - 8.0f - 26.0f;
    save();
    scissor(px, y, menuX - px - 6.0f, kRowH);
    label(px, y + 23.0f, row.name, 16.0f, kText);
    if (row.state == RowState::Placeholder) {
        label(px, y + 44.0f, "not loaded: " + row.problem, 12.0f, kRed);
    } else {
        std::string detail = row.pluginName + (row.vendor.empty() ? "" : " - " + row.vendor);
        if (row.state == RowState::Disabled) detail = "disabled - " + detail;
        label(px, y + 44.0f, detail, 12.0f, kDim);
    }
    restore();

    box(menuX, y + 19.0f, 26.0f, 26.0f, kTrack, 4.0f);
    for (int i = -1; i <= 1; ++i) {   // three dots
        beginPath();
        circle(menuX + 13.0f + static_cast<float>(i) * 6.0f, y + 32.0f, 2.0f);
        fill(kText);
        NanoVG::fill();
    }
    region(menuX, y + 19.0f, 26.0f, 26.0f, [this, row, menuX, y](const Press&) { openRowMenu(row, menuX, y + 47.0f); });

    button(editorX, y + 19.0f, 70.0f, 26.0f, "Editor", row.editorOpen, row.hasEditor, kAccent);
    if (row.hasEditor)
        region(editorX, y + 19.0f, 70.0f, 26.0f, [this, id = row.id](const Press&) {
            if (const RowModel* now = rowById(id)) now->editorOpen ? controller.closeEditor(id) : controller.openEditor(id);
        });
}

// Output: mute, gain fader over the meters, clip light, gain and output text.
void RackWidget::drawRowOutput(const RowModel& row, float y) {
    const float ox = outputX() + kPad;
    button(ox, y + 10.0f, 30.0f, 26.0f, "M", row.muted, true, kAmber);
    region(ox, y + 10.0f, 30.0f, 26.0f, [this, id = row.id](const Press&) {
        if (const RowModel* now = rowById(id)) controller.setMuted(id, !now->muted);
    });

    const float x0 = faderX0(), x1 = faderX1();
    box(x0, y + 17.0f, x1 - x0, 6.0f, kTrack, 3.0f);
    box(x0 + (x1 - x0) * dbToPos(0.0f) - 0.5f, y + 13.0f, 1.0f, 14.0f, kDim);   // 0 dB mark
    const float thumb = x0 + (x1 - x0) * dbToPos(row.gainDb);
    box(thumb - 5.0f, y + 9.0f, 10.0f, 22.0f, row.muted ? kDim : kText, 2.0f);
    region(
        x0 - 8.0f, y + 5.0f, x1 - x0 + 16.0f, 28.0f,
        [this, id = row.id, span = x1 - x0](const Press& p) {
            const RowModel* now = rowById(id);
            if (now == nullptr) return;
            // Double-click resets to 0 dB. Otherwise the fader moves with the drag,
            // not to where it was clicked: a stray click on stage changes nothing.
            if (id == lastFaderId && p.time - lastFaderTime < 400) {
                controller.setGainDb(id, 0.0f);
                lastFaderId.clear();
                return;
            }
            lastFaderId   = id;
            lastFaderTime = p.time;
            drag = [this, id, gain = now->gainDb, span, startX = p.x](float x, float, std::uint32_t mod) {
                const bool  fine = (mod & dgl::kModifierShift) != 0;
                const float db   = gain + (x - startX) * (kMaxDb - kMinDb) / span * (fine ? 0.1f : 1.0f);
                float       out  = std::clamp(std::round(db * 10.0f) / 10.0f, kMinDb, kMaxDb);
                if (std::fabs(out) < 0.35f && !fine) out = 0.0f;   // detent at unity
                controller.setGainDb(id, out);
            };
        },
        [this, id = row.id](float notches) {   // half a dB per notch
            const RowModel* now = rowById(id);
            if (now == nullptr) return;
            const float db = std::clamp(now->gainDb + notches * 0.5f, kMinDb, kMaxDb);
            controller.setGainDb(id, std::round(db * 10.0f) / 10.0f);
        });

    meterBars(x0, x1, y + 37.0f, row.meterDb, row.peakHoldDb);
    box(x1 + 8.0f, y + 36.0f, 14.0f, 14.0f, row.clipped ? kRed : kBarBg, 2.0f);
    region(x1 + 4.0f, y + 32.0f, 22.0f, 22.0f, [this, id = row.id](const Press&) { controller.clearClip(id); });
    label(x1 + 30.0f, y + 21.0f, row.muted ? "muted" : dbText(row.gainDb), 13.0f, row.muted ? kAmber : kText);
    label(x1 + 30.0f, y + 44.0f,
          "out " + std::to_string(row.firstOutput + 1) + "-" + std::to_string(row.firstOutput + 2), 12.0f, kDim);
}

void RackWidget::drawStatus() {
    const StatusModel& s = controller.view().status;
    const float        y = height() - kStatusH;
    box(0, y, width(), kStatusH, kBarBg);
    region(0, y, width(), kStatusH, nullptr);

    char stats[160];
    std::snprintf(stats, sizeof stats, "%.0f Hz   %u frames   xruns %llu   MIDI %s", s.sampleRate, s.bufferSize,
                  static_cast<unsigned long long>(s.xruns), s.preference == MidiProtocol::Midi2 ? "2.0" : "1.0");
    std::string left = stats;
    if (s.midiDropped != 0) left += "   MIDI dropped " + std::to_string(s.midiDropped);
    label(kPad, y + kStatusH / 2, left, 12.0f, s.xruns != 0 || s.midiDropped != 0 ? kAmber : kDim);

    // Master meter at the right; the notice or save target between.
    const float mx1 = width() - kPad - 22.0f, mx0 = mx1 - 180.0f;
    meterBars(mx0, mx1, y + 9.0f, s.outputDb, s.outputDb);
    box(mx1 + 8.0f, y + 8.0f, 14.0f, 14.0f, s.outputClipped ? kRed : kTrack, 2.0f);
    region(mx1 + 4.0f, y + 4.0f, 22.0f, 22.0f, [this](const Press&) { controller.clearOutputClip(); });

    const float tx = kPad + labelWidth(left, 12.0f) + 28.0f;
    save();
    scissor(tx, y, std::max(0.0f, mx0 - 12.0f - tx), kStatusH);
    if (!notice.empty() && nowSeconds() < noticeUntil)
        label(tx, y + kStatusH / 2, notice, 12.0f, kAccent);
    else if (!s.saveTarget.empty())
        label(tx, y + kStatusH / 2, "Saves to " + s.saveTarget, 12.0f, kDim);
    else
        label(tx, y + kStatusH / 2, "Not saving: " + s.noSaveReason, 12.0f, kAmber);
    restore();
}

// --- menus ---------------------------------------------------------------------------

void RackWidget::openMenu(Menu m) {
    filterId.clear();
    m.x  = std::clamp(m.x, 4.0f, std::max(4.0f, width() - m.w - 4.0f));
    menu = std::move(m);
}

void RackWidget::openSourceMenu(const RowModel& row, float x, float y) {
    controller.refreshSources();   // devices come and go
    Menu m;
    m.x     = x;
    m.y     = y;
    m.w     = 300.0f;
    m.title = "MIDI source for " + row.name;
    for (const SourceChoice& choice : controller.view().sources) {
        std::string text = choice.label;
        if (choice.protocol != SourceProtocol::Mixed) text += std::string("   (") + protocolText(choice.protocol) + ")";
        m.items.push_back(MenuItem{text, [this, id = row.id, ref = choice.ref] { controller.setSource(id, ref); },
                                   sameSource(choice.ref, row.source), false});
    }
    openMenu(std::move(m));
}

void RackWidget::openRowMenu(const RowModel& row, float x, float y) {
    Menu m;
    m.x     = x;
    m.y     = y;
    m.w     = 220.0f;
    m.title = row.name;
    const bool placeholder = row.state == RowState::Placeholder;
    const bool enabled     = row.state == RowState::Running;
    m.items.push_back(MenuItem{"Enabled",
                               placeholder ? std::function<void()>{}
                                           : [this, id = row.id, enabled] { controller.setEnabled(id, !enabled); },
                               row.state != RowState::Disabled, false});
    const std::uint32_t outputs = controller.view().status.outputs;
    if (!placeholder && outputs > 2)
        for (std::uint32_t o = 0; o + 1 < outputs; o += 2)
            m.items.push_back(MenuItem{"Output " + std::to_string(o + 1) + "-" + std::to_string(o + 2),
                                       [this, id = row.id, o] { controller.setFirstOutput(id, o); },
                                       row.firstOutput == o, false});
    // Removing asks again: this menu is replaced by a confirmation.
    m.items.push_back(MenuItem{"Remove...", [this, id = row.id, name = row.name, x, y] {
                                   Menu confirm;
                                   confirm.x     = x;
                                   confirm.y     = y;
                                   confirm.w     = 220.0f;
                                   confirm.title = "Remove " + name + "?";
                                   confirm.items.push_back(MenuItem{"Remove", [this, id] {
                                                                        if (filterId == id) filterId.clear();
                                                                        controller.removeSlot(id);
                                                                    },
                                                                    false, true});
                                   confirm.items.push_back(MenuItem{"Cancel", [] {}, false, false});
                                   openMenu(std::move(confirm));
                               },
                               false, true});
    openMenu(std::move(m));
}

void RackWidget::openAddMenu(float x, float y) {
    Menu m;
    m.x     = x;
    m.y     = y;
    m.w     = 360.0f;
    m.title = "Add a plugin (added at the bottom)";
    for (const PluginDescriptor& plugin : controller.availablePlugins()) {   // scans once; may take a moment
        const std::string text = plugin.name + (plugin.vendor.empty() ? "" : "  -  " + plugin.vendor);
        m.items.push_back(MenuItem{text, [this, plugin] {
                                       controller.addPlugin(plugin, controller.view().rows.size());
                                       scrollY = 1e9f;   // show the new row (clamped when drawn)
                                   },
                                   false, false});
    }
    if (m.items.empty()) m.items.push_back(MenuItem{"No CLAP plugins found", {}, false, false});
    openMenu(std::move(m));
}

void RackWidget::drawMenu() {
    if (!menu) return;
    Menu&       m      = *menu;
    const float titleH = m.title.empty() ? 0.0f : 28.0f;
    const float totalH = titleH + static_cast<float>(m.items.size()) * kMenuItemH + 8.0f;
    const float bottom = height() - 8.0f;
    if (m.y + totalH > bottom) m.y = std::max(kToolbarH, bottom - totalH);   // flip up if there's room
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
        label(m.x + 30.0f, iy + kMenuItemH / 2, item.label, 14.0f,
              !item.action ? kDim : item.danger ? kRed : kText);
        const float top = std::max(iy, listTop), bot = std::min(iy + kMenuItemH, listBottom);
        region(m.x, top, m.w, bot - top, [this, action = item.action](const Press&) {
            if (!action) return;   // a disabled item: the menu stays
            menu.reset();          // before the action, which may open another menu
            action();
        });
    }
    restore();
}

// --- the filter panel ------------------------------------------------------------------

const RowModel* RackWidget::rowById(const std::string& id) const {
    for (const RowModel& row : controller.view().rows)
        if (row.id == id) return &row;
    return nullptr;
}

MidiFilter RackWidget::filterOf(const RowModel& row) const {
    return filterId == row.id ? panelFilter : row.filter;
}

void RackWidget::applyFilter(const std::string& id, const MidiFilter& filter) {
    if (filterId == id) panelFilter = filter;
    controller.setFilter(id, filter);
}

// Drag an end of the key range, or its middle to move it. A press outside the
// range extends the nearer end to it.
void RackWidget::startKeyRangeDrag(const RowModel& row, float barX, const Press& p) {
    const MidiFilter start = filterOf(row);
    const int        note  = std::clamp(static_cast<int>(std::floor(p.x - barX)), 0, 127);
    enum class Mode { Low, High, Move };
    Mode mode = Mode::Move;
    if (note < start.noteLow || (std::abs(note - start.noteLow) <= 3 && note - start.noteLow <= start.noteHigh - note))
        mode = Mode::Low;
    else if (note > start.noteHigh || std::abs(note - start.noteHigh) <= 3)
        mode = Mode::High;

    drag = [this, id = row.id, start, note, mode, barX](float x, float, std::uint32_t) {
        const int  n = std::clamp(static_cast<int>(std::floor(x - barX)), 0, 127);
        MidiFilter f = start;
        if (mode == Mode::Low) {
            f.noteLow = static_cast<std::uint8_t>(std::min(n, static_cast<int>(start.noteHigh)));
        } else if (mode == Mode::High) {
            f.noteHigh = static_cast<std::uint8_t>(std::max(n, static_cast<int>(start.noteLow)));
        } else {
            const int d = std::clamp(n - note, -static_cast<int>(start.noteLow), 127 - static_cast<int>(start.noteHigh));
            f.noteLow   = static_cast<std::uint8_t>(start.noteLow + d);
            f.noteHigh  = static_cast<std::uint8_t>(start.noteHigh + d);
        }
        applyFilter(id, f);
    };
    if (mode != Mode::Move) drag(p.x, p.y, p.mod);   // the press itself sets that end
}

void RackWidget::stepper(float x, float y, const std::string& value,
                         const std::function<void(int dir, bool big)>& change) {
    button(x, y, 22.0f, 22.0f, "-", false, true, kAccent);
    region(x, y, 22.0f, 22.0f, [change](const Press& p) { change(-1, (p.mod & dgl::kModifierShift) != 0); });
    label(x + 70.0f, y + 11.0f, value, 13.0f, kText, ALIGN_CENTER | ALIGN_MIDDLE);
    region(x + 22.0f, y, 96.0f, 22.0f, nullptr, [change](float notches) { change(notches > 0 ? 1 : -1, false); });
    button(x + 118.0f, y, 22.0f, 22.0f, "+", false, true, kAccent);
    region(x + 118.0f, y, 22.0f, 22.0f, [change](const Press& p) { change(1, (p.mod & dgl::kModifierShift) != 0); });
}

void RackWidget::drawFilterPanel() {
    if (filterId.empty()) return;
    const std::vector<RowModel>& rows = controller.view().rows;
    const auto it = std::find_if(rows.begin(), rows.end(), [&](const RowModel& r) { return r.id == filterId; });
    if (it == rows.end()) {   // removed
        filterId.clear();
        return;
    }
    const RowModel&   row = *it;
    const std::string id  = row.id;
    const MidiFilter  f   = panelFilter;

    constexpr float w = 490.0f, h = 246.0f;
    const float     ry = rowY(static_cast<std::size_t>(it - rows.begin()));
    float           y  = ry + kRowH + 4.0f;
    if (y + h > height() - kStatusH) y = ry - h - 4.0f;
    y             = std::clamp(y, kToolbarH + 4.0f, std::max(kToolbarH + 4.0f, height() - kStatusH - h - 4.0f));
    const float x = kPad;

    box(x, y, w, h, kPanel, 6.0f);
    outline(x, y, w, h, kTrack, 6.0f);
    region(x, y, w, h, nullptr, [](float) {});
    label(x + 16.0f, y + 22.0f, "Input filter: " + row.name, 14.0f, kText);
    button(x + w - 16.0f - 60.0f - 8.0f - 60.0f, y + 10.0f, 60.0f, 24.0f, "Reset", false, true, kAccent);
    region(x + w - 16.0f - 60.0f - 8.0f - 60.0f, y + 10.0f, 60.0f, 24.0f,
           [this, id](const Press&) { applyFilter(id, MidiFilter{}); });
    button(x + w - 16.0f - 60.0f, y + 10.0f, 60.0f, 24.0f, "Done", true, true, kAccent);
    region(x + w - 16.0f - 60.0f, y + 10.0f, 60.0f, 24.0f, [this](const Press&) { filterId.clear(); });

    // Channels and groups: "All", or one box each. From "All", a click picks just
    // that one; the last one stays on.
    auto maskRow = [&](float ry2, const char* name, std::uint16_t mask, bool channels) {
        label(x + 16.0f, ry2 + 10.0f, name, 13.0f, kDim);
        button(x + 100.0f, ry2, 40.0f, 20.0f, "All", mask == 0xFFFF, true, kAccent);
        region(x + 100.0f, ry2, 40.0f, 20.0f, [this, id, f, channels](const Press&) {
            MidiFilter next = f;
            (channels ? next.channelMask : next.groupMask) = 0xFFFF;
            applyFilter(id, next);
        });
        for (int i = 0; i < 16; ++i) {
            const float bx = x + 148.0f + static_cast<float>(i) * 20.5f;
            const bool  on = mask != 0xFFFF && ((mask >> i) & 1u) != 0;
            button(bx, ry2, 18.0f, 20.0f, std::to_string(i + 1), on, true, kAccent);
            region(bx, ry2, 18.0f, 20.0f, [this, id, f, channels, i](const Press&) {
                MidiFilter     next  = f;
                std::uint16_t& field = channels ? next.channelMask : next.groupMask;
                field                = toggleBit(field, i);
                applyFilter(id, next);
            });
        }
    };
    maskRow(y + 48.0f, "Channels", f.channelMask, true);
    maskRow(y + 78.0f, "Groups", f.groupMask, false);

    label(x + 16.0f, y + 124.0f, "Keys", 13.0f, kDim);
    stepper(x + 100.0f, y + 113.0f, noteName(f.noteLow) + " (" + std::to_string(f.noteLow) + ")",
            [this, id, f](int dir, bool big) {
                MidiFilter next = f;
                next.noteLow    = static_cast<std::uint8_t>(std::clamp(f.noteLow + dir * (big ? 12 : 1), 0, 127));
                next.noteHigh   = std::max(next.noteHigh, next.noteLow);
                applyFilter(id, next);
            });
    label(x + 256.0f, y + 124.0f, "to", 13.0f, kDim, ALIGN_CENTER | ALIGN_MIDDLE);
    stepper(x + 276.0f, y + 113.0f, noteName(f.noteHigh) + " (" + std::to_string(f.noteHigh) + ")",
            [this, id, f](int dir, bool big) {
                MidiFilter next = f;
                next.noteHigh   = static_cast<std::uint8_t>(std::clamp(f.noteHigh + dir * (big ? 12 : 1), 0, 127));
                next.noteLow    = std::min(next.noteLow, next.noteHigh);
                applyFilter(id, next);
            });

    label(x + 16.0f, y + 158.0f, "Velocity", 13.0f, kDim);
    const int vLow = velocity7(f.velocityLow), vHigh = velocity7(f.velocityHigh);
    stepper(x + 100.0f, y + 147.0f, std::to_string(vLow), [this, id, f, vLow, vHigh](int dir, bool big) {
        MidiFilter next  = f;
        const int  low   = std::clamp(vLow + dir * (big ? 10 : 1), 0, 127);
        next.velocityLow = velocity16(low);
        if (low > vHigh) next.velocityHigh = velocity16(low);
        applyFilter(id, next);
    });
    label(x + 256.0f, y + 158.0f, "to", 13.0f, kDim, ALIGN_CENTER | ALIGN_MIDDLE);
    stepper(x + 276.0f, y + 147.0f, std::to_string(vHigh), [this, id, f, vLow, vHigh](int dir, bool big) {
        MidiFilter next   = f;
        const int  high   = std::clamp(vHigh + dir * (big ? 10 : 1), 0, 127);
        next.velocityHigh = velocity16(high);
        if (high < vLow) next.velocityLow = velocity16(high);
        applyFilter(id, next);
    });

    label(x + 16.0f, y + 192.0f, "Transpose", 13.0f, kDim);
    const int t = f.transpose;
    stepper(x + 100.0f, y + 181.0f, t == 0 ? "0" : (t > 0 ? "+" : "") + std::to_string(t) + " st",
            [this, id, f](int dir, bool big) {
                MidiFilter next = f;
                next.transpose  = static_cast<std::int8_t>(std::clamp(f.transpose + dir * (big ? 12 : 1), -48, 48));
                applyFilter(id, next);
            });

    label(x + 16.0f, y + 226.0f,
          "Shift-click: an octave for keys and transpose, 10 for velocity. The wheel works on values.", 11.0f, kDim);
}

// --- input -------------------------------------------------------------------------

bool RackWidget::onMouse(const MouseEvent& ev) {
    if (ev.button != 1) return false;
    if (!ev.press) {
        drag = nullptr;
        return true;
    }
    const float sf = scaleFactor();
    const Press p{static_cast<float>(ev.pos.getX()) / sf, static_cast<float>(ev.pos.getY()) / sf, ev.mod, ev.time};
    for (auto r = regions.rbegin(); r != regions.rend(); ++r) {   // topmost first
        if (p.x < r->x || p.x >= r->x + r->w || p.y < r->y || p.y >= r->y + r->h) continue;
        if (r->press) {
            const auto press = r->press;   // a copy: the handler may change what's drawn
            press(p);
        }
        repaint();
        return true;
    }
    return false;
}

bool RackWidget::onMotion(const MotionEvent& ev) {
    if (!drag) return false;
    const float sf = scaleFactor();
    drag(static_cast<float>(ev.pos.getX()) / sf, static_cast<float>(ev.pos.getY()) / sf, ev.mod);
    repaint();
    return true;
}

bool RackWidget::onScroll(const ScrollEvent& ev) {
    const float sf = scaleFactor();
    const float x = static_cast<float>(ev.pos.getX()) / sf, y = static_cast<float>(ev.pos.getY()) / sf;
    const float notches = static_cast<float>(ev.delta.getY());
    for (auto r = regions.rbegin(); r != regions.rend(); ++r) {
        if (x < r->x || x >= r->x + r->w || y < r->y || y >= r->y + r->h) continue;
        if (r->wheel) {
            const auto wheel = r->wheel;
            wheel(notches);
            repaint();
            return true;
        }
        break;   // the topmost region takes no wheel: scroll the rack
    }
    if (menu || !filterId.empty()) return true;
    scrollY = std::clamp(scrollY - notches * kRowH / 2.0f, 0.0f, maxScroll());
    repaint();
    return true;
}

bool RackWidget::onKeyboard(const KeyboardEvent& ev) {
    if (!ev.press) return false;
    if (ev.key == dgl::kKeyEscape) {
        menu.reset();
        filterId.clear();
        repaint();
        return true;
    }
    if ((ev.mod & dgl::kModifierControl) == 0) return false;
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

} // namespace stagehand
