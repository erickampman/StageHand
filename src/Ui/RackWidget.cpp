#include "Ui/RackWidget.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include "Core/Ump.h"
#include "Session/RackFile.h"

namespace stagehand {

namespace dgl = DGL_NAMESPACE;
using namespace ui;

namespace {

// --- look --------------------------------------------------------------------------
// Logical pixels, multiplied by the window's scale factor when drawn. Stage-first:
// dark, high contrast, big enough to hit.

constexpr float kToolbarH   = 44.0f;
constexpr float kRowH       = 64.0f;
constexpr float kStatusH    = 30.0f;
constexpr float kInputW     = 330.0f;
constexpr float kTransportX = 280.0f;   // the toolbar's transport, left of the input column's edge
constexpr float kPluginW    = 270.0f;
constexpr float kMinOutputW = 330.0f;
constexpr float kMinDb      = -60.0f;   // one scale for fader and meters, so they line up
constexpr float kMaxDb      = 6.0f;
constexpr float kKeyBarX    = kPad + 16.0f;   // 128 keys at 1 logical pixel each
constexpr float kDockHeadH  = 28.0f;   // the sources dock: header, inputs row, sequence rows
constexpr float kInputsH    = 34.0f;
constexpr float kSeqRowH    = 44.0f;

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
    : UiWidget(window), controller(rackController) {}

void RackWidget::showNotice(const std::string& text, double now) {
    notice      = text;
    noticeUntil = now + 4.0;
}

// --- geometry ------------------------------------------------------------------------

float RackWidget::outputX() const {
    return kInputW + kPluginW;
}
float RackWidget::outputW() {
    return std::max(kMinOutputW, width() - outputX());
}
float RackWidget::rowsHeight() {
    return dockTop() - kToolbarH;
}
// The dock grows with its sequences, up to a little over half the space; past
// that its sequence rows scroll.
float RackWidget::dockHeight() {
    // At least one sequence row's height: an empty dock shows how to add one.
    const float rows = static_cast<float>(std::max<std::size_t>(controller.view().sequences.size(), 1));
    const float want = kDockHeadH + kInputsH + rows * kSeqRowH;
    const float most = std::max(kDockHeadH + kInputsH + kSeqRowH, (height() - kToolbarH - kStatusH) * 0.55f);
    return std::min(want, most);
}
float RackWidget::dockTop() {
    return height() - kStatusH - dockHeight();
}
float RackWidget::dockMaxScroll() {
    const float content = static_cast<float>(controller.view().sequences.size()) * kSeqRowH;
    return std::max(0.0f, content - (dockHeight() - kDockHeadH - kInputsH));
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

// --- the frame -----------------------------------------------------------------------

void RackWidget::draw() {
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
    drawDock();
    drawToolbar();
    drawStatus();
}

void RackWidget::drawToolbar() {
    const RackViewModel& view = controller.view();
    box(0, 0, width(), kToolbarH, kBarBg);
    region(0, 0, width(), kToolbarH, nullptr);

    const std::string name = view.rackName.empty() ? "Untitled rack" : view.rackName;
    save();
    scissor(kPad, 0, kTransportX - kPad * 2, kToolbarH);
    label(kPad, kToolbarH / 2 - (view.modified ? 6.0f : 0.0f), name, 18.0f, kText);
    if (view.modified) label(kPad, kToolbarH / 2 + 12.0f, "unsaved changes", 11.0f, kAmber);
    restore();
    drawTransport(kTransportX);

    const float fileX = width() - kPad - 70.0f;
    button(fileX, 8.0f, 70.0f, 28.0f, "File", false, true, kAccent);
    region(fileX, 8.0f, 70.0f, 28.0f, [this, fileX](const Press&) { openFileMenu(fileX - 150.0f, kToolbarH); });

    const float addX = fileX - 10.0f - 118.0f;
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

    // Start/Stop: a triangle or a square, green while running (Space too).
    const float playX = x + 164.0f;
    box(playX, 8.0f, 34.0f, 28.0f, t.running ? kGreen : kTrack, 4.0f);
    beginPath();
    if (t.running) {
        rect(playX + 11.0f, 16.0f, 12.0f, 12.0f);
    } else {
        moveTo(playX + 12.0f, 15.0f);
        lineTo(playX + 24.0f, 22.0f);
        lineTo(playX + 12.0f, 29.0f);
        closePath();
    }
    fill(t.running ? kBarBg : kText);
    NanoVG::fill();
    region(playX, 8.0f, 34.0f, 28.0f, [this](const Press&) { controller.toggleTransport(); });

    // Its MIDI control: lit when it has any, amber if start on first note's
    // input isn't connected, blinking amber while learning.
    const float midiX   = playX + 40.0f;
    const bool  blinkOn = std::fmod(ui::nowSeconds(), 0.5) < 0.25;
    const bool  mapped  = !t.triggers.empty() || !t.startOnNote.empty();
    button(midiX, 8.0f, 50.0f, 28.0f, "MIDI", t.learning ? blinkOn : mapped, true,
           t.learning || !t.startOnNoteConnected ? kAmber : kAccent);
    region(midiX, 8.0f, 50.0f, 28.0f, [this, midiX](const Press&) { openTransportMenu(midiX, kToolbarH); });

    const float sigX = midiX + 56.0f;
    std::snprintf(text, sizeof text, "%d/%d", t.numerator, t.denominator);
    button(sigX, 8.0f, 52.0f, 28.0f, text, false, true, kAccent);
    region(sigX, 8.0f, 52.0f, 28.0f, [this, sigX](const Press&) {
        Menu m;
        m.x     = sigX;
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

    const int   beats   = std::clamp(t.beatsInBar, 1, 12);
    const float spacing = beats > 8 ? 12.0f : 15.0f;
    for (int b = 0; b < beats; ++b) {
        const bool lit = t.onBeat && b == t.beatInBar;
        beginPath();
        circle(sigX + 68.0f + static_cast<float>(b) * spacing, kToolbarH / 2, 5.0f);
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
        closeMenu();
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
    if (!notice.empty() && ui::nowSeconds() < noticeUntil)
        label(tx, y + kStatusH / 2, notice, 12.0f, kAccent);
    else if (!s.saveTarget.empty())
        label(tx, y + kStatusH / 2, "Saves to " + s.saveTarget, 12.0f, kDim);
    else
        label(tx, y + kStatusH / 2, s.noSaveReason, 12.0f, kAmber);
    restore();
}

// --- the sources dock (docs/sequences.md, section 4) -----------------------------------

void RackWidget::drawDock() {
    const RackViewModel& view = controller.view();
    const float          top  = dockTop();
    box(0, top, width(), dockHeight(), kBarBg);
    box(0, top, width(), 1.0f, kTrack);
    region(0, top, width(), dockHeight(), nullptr, [this](float notches) {
        dockScroll = std::clamp(dockScroll - notches * kSeqRowH / 2.0f, 0.0f, dockMaxScroll());
    });

    // Header: title, + Sequence, Stop all.
    label(kPad, top + kDockHeadH / 2, "Sources", 13.0f, kDim);
    const float stopX = width() - kPad - 80.0f, addX = stopX - 8.0f - 110.0f;
    button(addX, top + 3.0f, 110.0f, 22.0f, "+ Sequence", false, true, kAccent);
    region(addX, top + 3.0f, 110.0f, 22.0f, [this, addX, top](const Press&) {
        Menu m;
        m.x     = addX;
        m.y     = top + kDockHeadH;
        m.w     = 220.0f;
        m.title = "New sequence";
        m.items.push_back(MenuItem{"MIDI 2.0 (EDO pitches)", [this] { controller.addSequence(MidiProtocol::Midi2); }});
        m.items.push_back(MenuItem{"MIDI 1.0 (note numbers)", [this] { controller.addSequence(MidiProtocol::Midi1); }});
        openMenu(std::move(m));
    });
    button(stopX, top + 3.0f, 80.0f, 22.0f, "Stop all", false, !view.sequences.empty(), kAccent);
    region(stopX, top + 3.0f, 80.0f, 22.0f, [this](const Press&) { controller.stopAllSequences(); });

    // Inputs: one chip per open input, lit by its messages.
    const float iy = top + kDockHeadH;
    label(kPad, iy + kInputsH / 2, "Inputs", 12.0f, kDim);
    float x = kPad + 56.0f;
    save();
    scissor(0, iy, width() - kPad, kInputsH);
    for (const InputChipModel& chip : view.inputs) {
        const std::string text = chip.label;
        const float       w    = labelWidth(text, 12.0f) + 30.0f;
        box(x, iy + 6.0f, w, 22.0f, kTrack, 11.0f);
        beginPath();
        circle(x + 11.0f, iy + 17.0f, 4.0f);
        fill(chip.active ? kGreen : kBarBg);
        NanoVG::fill();
        label(x + 21.0f, iy + 17.0f, text, 12.0f, kText);
        x += w + 8.0f;
    }
    if (view.inputs.empty()) label(x, iy + kInputsH / 2, "no MIDI inputs open", 12.0f, kDim);
    restore();

    // Sequence rows, scrolled if there are more than fit.
    const float rowsTop = iy + kInputsH, rowsH = dockHeight() - kDockHeadH - kInputsH;
    dockScroll          = std::clamp(dockScroll, 0.0f, dockMaxScroll());
    save();
    scissor(0, rowsTop, width(), rowsH);
    for (std::size_t i = 0; i < view.sequences.size(); ++i) {
        const float y = rowsTop + static_cast<float>(i) * kSeqRowH - dockScroll;
        if (y + kSeqRowH < rowsTop || y > rowsTop + rowsH) continue;
        drawSequenceRow(view.sequences[i], y);
    }
    restore();
    if (view.sequences.empty() && rowsH >= kSeqRowH / 2)
        label(kPad + 56.0f, rowsTop + kSeqRowH / 2, "No sequences yet. Add one with + Sequence.", 12.0f, kDim);
}

void RackWidget::drawSequenceRow(const SequenceRowModel& seq, float y) {
    box(0, y, width(), 1.0f, kLine);

    // Play/Stop: a triangle or a square; a pending start or stop blinks.
    const bool pending = seq.state == SequenceState::Starting || seq.state == SequenceState::Stopping;
    const bool running = seq.state == SequenceState::Playing || seq.state == SequenceState::Stopping;
    const bool blinkOn = std::fmod(ui::nowSeconds(), 0.5) < 0.25;
    box(kPad, y + 8.0f, 32.0f, 28.0f, pending ? (blinkOn ? kAmber : kTrack) : running ? kGreen : kTrack, 4.0f);
    beginPath();
    if (running) {
        rect(kPad + 10.0f, y + 16.0f, 12.0f, 12.0f);
    } else {
        moveTo(kPad + 11.0f, y + 15.0f);
        lineTo(kPad + 23.0f, y + 22.0f);
        lineTo(kPad + 11.0f, y + 29.0f);
        closePath();
    }
    fill(running || pending ? kBarBg : kText);
    NanoVG::fill();
    region(kPad, y + 8.0f, 32.0f, 28.0f, [this, id = seq.id](const Press&) { controller.togglePlay(id); });

    // Name, and what it is and who plays it.
    const float nameX = kPad + 44.0f, editX = 262.0f, mapX = 314.0f, mapW = 130.0f;
    save();
    scissor(nameX, y, editX - nameX - 10.0f, kSeqRowH);
    label(nameX, y + 16.0f, seq.name, 14.0f, kText);
    std::string detail = seq.protocol == MidiProtocol::Midi2 ? "MIDI 2.0, " + std::to_string(seq.edo) + "-EDO"
                                                             : std::string("MIDI 1.0");
    detail += ", " + std::to_string(seq.noteCount) + (seq.noteCount == 1 ? " note" : " notes");
    if (!seq.usedBy.empty()) {
        detail += "  >";
        for (std::size_t i = 0; i < seq.usedBy.size(); ++i) detail += (i ? ", " : " ") + seq.usedBy[i];
    }
    label(nameX, y + 33.0f, detail, 11.0f, seq.usedBy.empty() ? kAmber : kDim);
    restore();
    // The editor opens from the Edit button, and from a click on the name or
    // the mini-map.
    auto openEditor = [this, id = seq.id](const Press&) {
        if (openSequenceEditor) openSequenceEditor(id);
    };
    region(nameX, y + 4.0f, editX - nameX - 10.0f, kSeqRowH - 8.0f, openEditor);
    button(editX, y + 10.0f, 44.0f, 24.0f, "Edit", false, true, kAccent);
    region(editX, y + 10.0f, 44.0f, 24.0f, openEditor);
    region(mapX, y + 10.0f, mapW, 24.0f, openEditor);

    // Mini-map: beats, note starts, the playhead.
    box(mapX, y + 10.0f, mapW, 24.0f, kRowA, 3.0f);
    const float  unitW        = mapW / static_cast<float>(seq.length);
    const double unitsPerBeat = seq.rate.unitsPerBeat();
    if (unitsPerBeat >= 1.0)
        for (double u = 0.0; u < seq.length; u += unitsPerBeat)
            box(mapX + static_cast<float>(u) * unitW, y + 10.0f, 1.0f, 24.0f, kTrack);
    for (const std::uint32_t start : seq.noteStarts)
        box(mapX + static_cast<float>(start) * unitW, y + 16.0f, std::max(2.0f, unitW - 1.0f), 12.0f, kAccent, 1.0f);
    if (running)
        box(mapX + static_cast<float>(seq.position) * unitW - 1.0f, y + 8.0f, 2.0f, 28.0f, kAmber);

    // At Start (starts with the transport), rate, length, launch, Repeat, the row menu.
    float x = mapX + mapW + 8.0f;
    button(x, y + 10.0f, 62.0f, 24.0f, "At Start", seq.atStart, true, kAccent);
    region(x, y + 10.0f, 62.0f, 24.0f,
           [this, id = seq.id, on = seq.atStart](const Press&) { controller.setSequenceAtStart(id, !on); });
    x += 72.0f;
    const std::string rate = seq.rate.denominator == 1
                                 ? std::to_string(seq.rate.numerator)
                                 : std::to_string(seq.rate.numerator) + "/" + std::to_string(seq.rate.denominator);
    button(x, y + 10.0f, 70.0f, 24.0f, "Rate " + rate, false, true, kAccent);
    region(x, y + 10.0f, 70.0f, 24.0f, [this, id = seq.id, x, y, current = seq.rate](const Press&) {
        Menu m;
        m.x     = x;
        m.y     = y + 36.0f;
        m.w     = 190.0f;
        m.title = "Units per beat";
        for (const auto [n, d] : {std::pair{1, 4}, {1, 3}, {1, 2}, {1, 1}, {2, 1}, {3, 1}, {4, 1}, {8, 1}, {16, 1}}) {
            const std::string text = d == 1 ? std::to_string(n) : std::to_string(n) + "/" + std::to_string(d);
            const auto        r    = SequenceRate{static_cast<std::uint16_t>(n), static_cast<std::uint16_t>(d)};
            m.items.push_back(MenuItem{text + (n == 4 && d == 1 ? "   (sixteenths)" : ""),
                                       [this, id, r] { controller.setSequenceRate(id, r); },
                                       current.numerator == r.numerator && current.denominator == r.denominator});
        }
        openMenu(std::move(m));
    });
    x += 78.0f;
    stepper(x, y + 11.0f, std::to_string(seq.length) + " units", [this, id = seq.id](int dir, bool big) {
        for (const SequenceRowModel& now : controller.view().sequences)
            if (now.id == id)
                controller.setSequenceLength(id, static_cast<std::uint32_t>(
                                                     std::max(1, static_cast<int>(now.length) + dir * (big ? 4 : 1))));
    });
    x += 148.0f;
    const char* launch = seq.launch == LaunchQuantize::Bar ? "Bar" : seq.launch == LaunchQuantize::Beat ? "Beat" : "Now";
    button(x, y + 10.0f, 56.0f, 24.0f, launch, false, true, kAccent);
    region(x, y + 10.0f, 56.0f, 24.0f, [this, id = seq.id, x, y, current = seq.launch](const Press&) {
        Menu m;
        m.x     = x;
        m.y     = y + 36.0f;
        m.w     = 200.0f;
        m.title = "Play and Stop take effect at";
        for (const auto [q, text] : {std::pair{LaunchQuantize::Bar, "The next bar"},
                                     {LaunchQuantize::Beat, "The next beat"},
                                     {LaunchQuantize::Immediate, "Immediately"}})
            m.items.push_back(MenuItem{text, [this, id, q = q] { controller.setSequenceLaunch(id, q); }, current == q});
        openMenu(std::move(m));
    });
    x += 62.0f;
    button(x, y + 10.0f, 64.0f, 24.0f, "Repeat", seq.repeat, true, kAccent);
    region(x, y + 10.0f, 64.0f, 24.0f,
           [this, id = seq.id, repeat = seq.repeat](const Press&) { controller.setSequenceRepeat(id, !repeat); });
    x += 70.0f;
    // MIDI triggers: lit when it has some, blinking amber while learning one.
    button(x, y + 10.0f, 50.0f, 24.0f, "MIDI", seq.learning ? blinkOn : !seq.triggers.empty(), true,
           seq.learning ? kAmber : kAccent);
    region(x, y + 10.0f, 50.0f, 24.0f, [this, seq, x, y](const Press&) { openTriggerMenu(seq, x, y + 36.0f); });
    x += 56.0f;
    box(x, y + 10.0f, 26.0f, 24.0f, kTrack, 4.0f);
    for (int i = -1; i <= 1; ++i) {
        beginPath();
        circle(x + 13.0f + static_cast<float>(i) * 6.0f, y + 22.0f, 2.0f);
        fill(kText);
        NanoVG::fill();
    }
    region(x, y + 10.0f, 26.0f, 24.0f, [this, seq, x, y](const Press&) { openSequenceMenu(seq, x, y + 36.0f); });
}

void RackWidget::openSequenceMenu(const SequenceRowModel& seq, float x, float y) {
    Menu m;
    m.x     = x;
    m.y     = y;
    m.w     = 230.0f;
    m.title = seq.name;
    m.items.push_back(MenuItem{"Edit notes...", [this, id = seq.id] {
                                   if (openSequenceEditor) openSequenceEditor(id);
                               }});
    m.items.push_back(MenuItem{"Remove...", [this, id = seq.id, name = seq.name, x, y] {
                                   Menu confirm;
                                   confirm.x     = x;
                                   confirm.y     = y;
                                   confirm.w     = 230.0f;
                                   confirm.title = "Remove " + name + "?";
                                   confirm.items.push_back(
                                       MenuItem{"Remove", [this, id] { controller.removeSequence(id); }, false, true});
                                   confirm.items.push_back(MenuItem{"Cancel", [] {}});
                                   openMenu(std::move(confirm));
                               },
                               false, true});
    openMenu(std::move(m));
}

// Learn a trigger (the next pad, key or pedal pressed), or remove one.
void RackWidget::openTriggerMenu(const SequenceRowModel& seq, float x, float y) {
    Menu m;
    m.x     = x;
    m.y     = y;
    m.w     = 340.0f;
    m.title = seq.learning ? "Press a pad, key or pedal..." : "MIDI control of " + seq.name;
    if (seq.learning) m.items.push_back(MenuItem{"Stop learning", [this] { controller.cancelLearn(); }});
    for (const auto [action, text] : {std::pair{SequenceTrigger::Action::Toggle, "Learn Play/Stop"},
                                      {SequenceTrigger::Action::Start, "Learn Start"},
                                      {SequenceTrigger::Action::Stop, "Learn Stop"}})
        m.items.push_back(MenuItem{text, [this, id = seq.id, a = action] { controller.learnTrigger(id, a); },
                                   seq.learning && seq.learningAction == action});
    for (std::size_t i = 0; i < seq.triggers.size(); ++i)
        m.items.push_back(MenuItem{"Remove " + seq.triggers[i],
                                   [this, id = seq.id, i] { controller.removeTrigger(id, i); }, false, true});
    openMenu(std::move(m));
}

// The transport's MIDI control: learn its triggers, remove them, and start on
// first note (off, any input, or one input).
void RackWidget::openTransportMenu(float x, float y) {
    controller.refreshSources();   // devices come and go
    const TransportModel& t = controller.view().transport;
    Menu                  m;
    m.x     = x;
    m.y     = y;
    m.w     = 340.0f;
    m.title = t.learning ? "Press a pad, key or pedal..." : "Transport: MIDI control";
    if (t.learning) m.items.push_back(MenuItem{"Stop learning", [this] { controller.cancelLearn(); }});
    for (const auto [action, text] : {std::pair{SequenceTrigger::Action::Toggle, "Learn Start/Stop"},
                                      {SequenceTrigger::Action::Start, "Learn Start"},
                                      {SequenceTrigger::Action::Stop, "Learn Stop"}})
        m.items.push_back(MenuItem{text, [this, a = action] { controller.learnTransportTrigger(a); },
                                   t.learning && t.learningAction == action});
    for (std::size_t i = 0; i < t.triggers.size(); ++i)
        m.items.push_back(MenuItem{"Remove " + t.triggers[i], [this, i] { controller.removeTransportTrigger(i); },
                                   false, true});
    m.items.push_back(MenuItem{"Start on first note: off", [this] { controller.setStartOnNote(false); },
                               t.startOnNote.empty()});
    for (const SourceChoice& choice : controller.view().sources) {
        if (choice.ref.kind == MidiSourceRef::Kind::Sequence) continue;
        const std::string label = choice.ref.kind == MidiSourceRef::Kind::All ? "any input" : choice.label;
        m.items.push_back(MenuItem{"Start on first note: " + label,
                                   [this, ref = choice.ref] { controller.setStartOnNote(true, ref); },
                                   !t.startOnNote.empty() && t.startOnNote == choice.label});
    }
    openMenu(std::move(m));
}

// --- the rack file: New, Open, Save, Save As, and quitting --------------------------------

void RackWidget::openFileMenu(float x, float y) {
    const bool savingOff = controller.savingOff();
    Menu       m;
    m.x     = x;
    m.y     = y;
    m.w     = 220.0f;
    m.title = controller.rackPath().empty() ? "Untitled rack" : controller.rackPath();
    m.items.push_back(MenuItem{"New rack          Ctrl-N", [this] { fileAction(FileAction::New); }});
    m.items.push_back(MenuItem{"Open...           Ctrl-O", [this] { fileAction(FileAction::Open); }});
    m.items.push_back(MenuItem{"Save              Ctrl-S",
                               savingOff ? std::function<void()>{} : [this] { fileAction(FileAction::Save); }});
    m.items.push_back(MenuItem{"Save As...",
                               savingOff ? std::function<void()>{} : [this] { fileAction(FileAction::SaveAs); }});
    openMenu(std::move(m));
}

// New, Open and Quit would lose an untitled rack's changes: ask first.
void RackWidget::fileAction(FileAction action) {
    closeMenu();
    if (action == FileAction::Save && !controller.rackPath().empty()) {
        controller.save();
        return;
    }
    if (action == FileAction::Save || action == FileAction::SaveAs) {
        saveAsThen({});
        return;
    }
    if (!controller.unsavedUntitled()) {
        proceed(action);
        return;
    }
    Menu m;
    m.x     = width() / 2 - 150.0f;
    m.y     = kToolbarH + 20.0f;
    m.w     = 300.0f;
    m.title = "Keep the changes to " + controller.view().rackName + "?";
    m.items.push_back(MenuItem{"Save As...", [this, action] { saveAsThen([this, action] { proceed(action); }); }});
    m.items.push_back(MenuItem{action == FileAction::Quit ? "Quit without saving" : "Discard the changes",
                               [this, action] { proceed(action); }, false, true});
    m.items.push_back(MenuItem{"Cancel", [] {}});
    openMenu(std::move(m));
}

void RackWidget::proceed(FileAction action) {
    switch (action) {
        case FileAction::New: controller.newRack(); break;
        case FileAction::Open:
            if (chooseFile)
                chooseFile(false, "Open a rack (choose its .rack folder)", startFolder(), "",
                           [this](const std::string& path) { controller.openRack(path); });
            break;
        case FileAction::Quit: controller.quit(); break;
        case FileAction::Save:
        case FileAction::SaveAs: break;   // handled by fileAction()
    }
}

// Save As, then `then` once it's saved. An existing rack is replaced only
// after asking.
void RackWidget::saveAsThen(std::function<void()> then) {
    if (!chooseFile) return;
    const std::string name = (controller.view().rackName.empty() ? std::string("Untitled") : controller.view().rackName) + ".rack";
    chooseFile(true, "Save the rack as", startFolder(), name, [this, then](const std::string& path) {
        const RackController::SaveAsResult result = controller.saveAs(path);
        if (result == RackController::SaveAsResult::Saved) {
            if (then) then();
            return;
        }
        if (result != RackController::SaveAsResult::NeedsReplace) return;
        Menu m;
        m.x     = width() / 2 - 170.0f;
        m.y     = kToolbarH + 20.0f;
        m.w     = 340.0f;
        m.title = "Replace the existing rack " + std::filesystem::path(saveAsPath(path)).filename().string() + "?";
        m.items.push_back(MenuItem{"Replace it", [this, path, then] {
                                       if (controller.saveAs(path, true) == RackController::SaveAsResult::Saved && then)
                                           then();
                                   },
                                   false, true});
        m.items.push_back(MenuItem{"Cancel", [] {}});
        openMenu(std::move(m));
    });
}

// Where a chooser starts: beside the current rack, else ~/Racks, else home.
std::string RackWidget::startFolder() const {
    namespace fs = std::filesystem;
    if (!controller.rackPath().empty()) return fs::path(controller.rackPath()).parent_path().string();
    const char* const home = std::getenv("HOME");
    if (home == nullptr) return "/";
    std::error_code ec;
    const fs::path  racks = fs::path(home) / "Racks";
    return fs::is_directory(racks, ec) ? racks.string() : std::string(home);
}

// Another rack replaced this one: nothing open refers to it any more.
void RackWidget::rackReplaced() {
    closeMenu();
    dismissOverlays();
    drag    = nullptr;
    dragEnd = nullptr;
    scrollY = dockScroll = 0;
}

// --- menus ---------------------------------------------------------------------------

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

void RackWidget::wheelUnclaimed(float, float, float notches, std::uint32_t) {
    if (!menuOpen() && filterId.empty()) scrollY = std::clamp(scrollY - notches * kRowH / 2.0f, 0.0f, maxScroll());
}

bool RackWidget::onKeyboard(const KeyboardEvent& ev) {
    if (!ev.press) return false;
    if (ev.key == dgl::kKeyEscape) {
        controller.cancelLearn();
        closeMenu();
        dismissOverlays();
        repaint();
        return true;
    }
    if (ev.key == dgl::kKeySpace && (ev.mod & dgl::kModifierControl) == 0 && !menuOpen()) {
        controller.toggleTransport();   // Start/Stop
        repaint();
        return true;
    }
    if ((ev.mod & dgl::kModifierControl) == 0) return false;
    // With Control held, X11 may report the control character instead of the letter.
    auto is = [&ev](char letter) {
        return ev.key == static_cast<std::uint32_t>(letter) || ev.key == static_cast<std::uint32_t>(letter - 32) ||
               ev.key == static_cast<std::uint32_t>(letter - 'a' + 1);
    };
    if (is('q')) fileAction(FileAction::Quit);
    else if (is('s')) fileAction(FileAction::Save);   // Ctrl-Shift-S too, as in plugin editors
    else if (is('n')) fileAction(FileAction::New);
    else if (is('o')) fileAction(FileAction::Open);
    else return false;
    repaint();
    return true;
}

} // namespace stagehand
