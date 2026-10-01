#include "Ui/SequenceEditorWidget.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <tuple>
#include <utility>

namespace stagehand {

namespace dgl = DGL_NAMESPACE;
using namespace ui;

namespace {

constexpr float kStripW = 232.0f;   // settings
constexpr float kLabelW = 58.0f;    // pitch labels
constexpr float kRulerH = 26.0f;    // beat numbers
constexpr float kRowH   = 12.0f;    // one pitch
constexpr UiColor kBlackRow{12, 13, 16};   // the black-key bands

std::string noteName(int note) {   // MIDI 60 = C4
    static const char* const names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    note = std::clamp(note, 0, 127);
    return names[note % 12] + std::to_string(note / 12 - 1);
}

int positiveMod(int value, int modulus) {
    const int m = value % modulus;
    return m < 0 ? m + modulus : m;
}

std::string rateText(const SequenceRate& r) {
    return r.denominator == 1 ? std::to_string(r.numerator)
                              : std::to_string(r.numerator) + "/" + std::to_string(r.denominator);
}

bool isKey(std::uint32_t key, char letter) {   // with Control, X11 may send the control character
    return key == static_cast<std::uint32_t>(letter) || key == static_cast<std::uint32_t>(letter - 32) ||
           key == static_cast<std::uint32_t>(letter - 'a' + 1);
}

} // namespace

SequenceEditorWidget::SequenceEditorWidget(dgl::Window& window, RackController& rackController,
                                           SequenceEditor& sequenceEditor)
    : UiWidget(window), controller(rackController), editor(sequenceEditor) {}

// --- geometry ------------------------------------------------------------------------

float SequenceEditorWidget::canvasX() const {
    return kStripW + kLabelW;
}
float SequenceEditorWidget::canvasY() const {
    return kRulerH;
}
float SequenceEditorWidget::canvasW() {
    return std::max(40.0f, width() - canvasX() - 4.0f);
}
float SequenceEditorWidget::canvasH() {
    return std::max(40.0f, height() - canvasY() - 4.0f);
}
float SequenceEditorWidget::unitWidth() {
    if (zoom > 0.0f) return zoom;
    const Sequence* seq = editor.sequence();
    const float     len = seq != nullptr ? static_cast<float>(seq->length) : 16.0f;
    return std::clamp(canvasW() / len, 10.0f, 64.0f);
}
float SequenceEditorWidget::rowY(int pitch) const {
    return canvasY() + static_cast<float>(editor.highestPitch() - pitch) * kRowH - scrollY;
}
std::int64_t SequenceEditorWidget::unitAt(float x) {
    return static_cast<std::int64_t>(std::floor((x - canvasX() + scrollX) / unitWidth()));
}
int SequenceEditorWidget::pitchAt(float y) const {
    return editor.highestPitch() - static_cast<int>(std::floor((y - canvasY() + scrollY) / kRowH));
}

void SequenceEditorWidget::clampScroll() {
    const Sequence* seq = editor.sequence();
    if (seq == nullptr) return;
    const float rows   = static_cast<float>(editor.highestPitch() - editor.lowestPitch() + 1) * kRowH;
    const float across = static_cast<float>(seq->length) * unitWidth();
    scrollY            = std::clamp(scrollY, 0.0f, std::max(0.0f, rows - canvasH()));
    scrollX            = std::clamp(scrollX, 0.0f, std::max(0.0f, across - canvasW() + 20.0f));
}

// --- the frame -----------------------------------------------------------------------

void SequenceEditorWidget::draw() {
    box(0, 0, width(), height(), kBg);
    const Sequence* seq = editor.sequence();
    if (seq == nullptr) {
        label(width() / 2, height() / 2, "This sequence was removed.", 16.0f, kDim, ALIGN_CENTER | ALIGN_MIDDLE);
        return;
    }
    if (!centred) {   // start with the reference pitch (or middle C) in the middle
        const int middle = seq->protocol == MidiProtocol::Midi2 ? 0 : 60;
        scrollY          = static_cast<float>(editor.highestPitch() - middle) * kRowH - canvasH() / 2;
        centred          = true;
    }
    clampScroll();
    drawGrid(*seq);
    drawRuler(*seq);
    drawStrip(*seq);
}

void SequenceEditorWidget::drawGrid(const Sequence& seq) {
    const float x0 = canvasX(), y0 = canvasY(), w = canvasW(), h = canvasH(), uw = unitWidth();
    const int   hi = editor.highestPitch(), lo = editor.lowestPitch(), octave = editor.stepsPerOctave();
    const bool  midi2 = seq.protocol == MidiProtocol::Midi2;

    // Rows, with the black-key bands, octave lines, and pitch labels.
    save();
    scissor(kStripW, y0, kLabelW + w, h);
    for (int pitch = std::min(hi, pitchAt(y0)); pitch >= std::max(lo, pitchAt(y0 + h)); --pitch) {
        const float y         = rowY(pitch);
        const bool  black     = editor.isBlackKey(pitch);
        const bool  octaveRow = positiveMod(pitch, midi2 ? octave : 12) == 0;
        box(x0, y, w, kRowH, black ? kBlackRow : kRowA);
        box(kStripW, y, kLabelW, kRowH, black ? kBlackRow : kRowB);
        if (octaveRow) box(kStripW, y + kRowH - 1.0f, kLabelW + w, 1.0f, kTrack);
        std::string text = midi2 ? std::to_string(pitch) : noteName(pitch);
        if (midi2 && octaveRow) text = noteName(static_cast<int>(std::lround(editor.semitonesOf(pitch)))) + "  " + text;
        label(kStripW + kLabelW - 6.0f, y + kRowH / 2, text, 9.0f, octaveRow ? kText : kDim, ALIGN_RIGHT | ALIGN_MIDDLE);
    }
    restore();

    // Columns: units, beats and bars; the loop's end; notes; the playhead.
    save();
    scissor(x0, y0, w, h);
    const double upb      = seq.rate.unitsPerBeat();
    const auto&  t        = controller.view().transport;
    const double barUnits = upb * t.numerator * 4.0 / t.denominator;
    for (std::uint32_t u = 0; u <= seq.length; ++u) {
        const float x = x0 + static_cast<float>(u) * uw - scrollX;
        if (x < x0 - 1.0f || x > x0 + w) continue;
        const bool bar  = barUnits >= 1.0 && std::fabs(std::fmod(u, barUnits)) < 1e-9;
        const bool beat = upb >= 1.0 && std::fabs(std::fmod(u, upb)) < 1e-9;
        if (bar || beat || uw >= 8.0f) box(x, y0, 1.0f, h, bar ? kDim : beat ? kTrack : kLine);
    }
    const float end = x0 + static_cast<float>(seq.length) * uw - scrollX;
    if (end < x0 + w) box(end + 1.0f, y0, x0 + w - end, h, kBarBg);

    const double maxVelocity = midi2 ? 65535.0 : 127.0;
    for (const SequenceEditor::Item& item : editor.items()) {
        const SequenceNote& n = item.note;
        const float         x = x0 + static_cast<float>(n.start) * uw - scrollX;
        const float         y = rowY(n.pitch);
        const float         nw = static_cast<float>(n.length) * uw;
        if (x + nw < x0 || x > x0 + w || y + kRowH < y0 || y > y0 + h) continue;
        const int alpha = 110 + static_cast<int>(145.0 * n.velocity / maxVelocity);   // brighter is louder
        beginPath();
        roundedRect(x + 1.0f, y + 1.0f, std::max(2.0f, nw - 2.0f), kRowH - 2.0f, 2.0f);
        fill(kAccent, alpha);
        NanoVG::fill();
        if (item.selected) outline(x + 1.0f, y + 1.0f, std::max(2.0f, nw - 2.0f), kRowH - 2.0f, kText, 2.0f);
    }

    for (const SequenceRowModel& row : controller.view().sequences)
        if (row.id == seq.id && (row.state == SequenceState::Playing || row.state == SequenceState::Stopping))
            box(x0 + static_cast<float>(row.position) * uw - scrollX - 1.0f, y0, 2.0f, h, kAmber);

    if (banding) {
        const float bx = std::min(bandX0, bandX1), by = std::min(bandY0, bandY1);
        outline(bx, by, std::fabs(bandX1 - bandX0), std::fabs(bandY1 - bandY0), kAccent, 0.0f);
    }
    restore();
    region(x0, y0, w, h, [this](const Press& p) { pressCanvas(p); }, {}, true);
}

void SequenceEditorWidget::drawRuler(const Sequence& seq) {
    const float x0 = canvasX(), uw = unitWidth();
    box(kStripW, 0, width() - kStripW, kRulerH, kBarBg);
    region(kStripW, 0, width() - kStripW, kRulerH, nullptr);
    save();
    scissor(x0, 0, canvasW(), kRulerH);
    const double upb      = seq.rate.unitsPerBeat();
    const float  beatW    = static_cast<float>(upb) * uw;
    const int    skip     = beatW >= 22.0f ? 1 : static_cast<int>(std::ceil(22.0f / beatW));   // don't crowd
    for (int beat = 0; beat * upb < seq.length; beat += skip) {
        const float x = x0 + static_cast<float>(beat * upb) * uw - scrollX;
        label(x + 3.0f, kRulerH / 2, std::to_string(beat + 1), 11.0f, kDim);
    }
    restore();
    label(kStripW + 8.0f, kRulerH / 2, "beat", 10.0f, kDim);
}

void SequenceEditorWidget::settingRow(float y, const char* name) {
    label(kPad, y + 11.0f, name, 12.0f, kDim);
}

void SequenceEditorWidget::drawStrip(const Sequence& seq) {
    box(0, 0, kStripW, height(), kBarBg);
    box(kStripW - 1.0f, 0, 1.0f, height(), kTrack);
    region(0, 0, kStripW, height(), nullptr);
    const std::string id    = seq.id;
    const bool        midi2 = seq.protocol == MidiProtocol::Midi2;

    save();
    scissor(0, 0, kStripW - 8.0f, 50.0f);
    label(kPad, 18.0f, seq.name, 16.0f, kText);
    label(kPad, 38.0f,
          midi2 ? "MIDI 2.0, " + std::to_string(seq.edo) + "-EDO, " + std::to_string(seq.notes.size()) + " notes"
                : "MIDI 1.0, " + std::to_string(seq.notes.size()) + " notes",
          11.0f, kDim);
    restore();

    // Play/Stop, and what it's doing.
    SequenceState state = SequenceState::Stopped;
    for (const SequenceRowModel& row : controller.view().sequences)
        if (row.id == id) state = row.state;
    const bool running = state == SequenceState::Playing || state == SequenceState::Stopping;
    const bool pending = state == SequenceState::Starting || state == SequenceState::Stopping;
    const bool blink   = std::fmod(nowSeconds(), 0.5) < 0.25;
    box(kPad, 52.0f, 90.0f, 26.0f, pending ? (blink ? kAmber : kTrack) : running ? kGreen : kTrack, 4.0f);
    label(kPad + 45.0f, 65.0f, running ? "Stop" : "Play", 13.0f, running || (pending && blink) ? kBarBg : kText,
          ALIGN_CENTER | ALIGN_MIDDLE);
    region(kPad, 52.0f, 90.0f, 26.0f, [this, id](const Press&) { controller.togglePlay(id); });
    const char* status = state == SequenceState::Playing    ? "playing"
                         : state == SequenceState::Starting ? "starting..."
                         : state == SequenceState::Stopping ? "stopping..."
                                                            : "stopped";
    label(kPad + 100.0f, 65.0f, status, 12.0f, kDim);

    // Protocol: only while there are no notes (changing it would be a conversion).
    settingRow(90.0f, "Protocol");
    const bool empty = seq.notes.empty();
    for (const auto [p, text, x] : {std::tuple{MidiProtocol::Midi1, "1.0", 80.0f}, {MidiProtocol::Midi2, "2.0", 128.0f}}) {
        button(x, 90.0f, 44.0f, 22.0f, text, seq.protocol == p, empty || seq.protocol == p, kAccent);
        if (empty && seq.protocol != p)
            region(x, 90.0f, 44.0f, 22.0f, [this, id, p = p](const Press&) {
                controller.setSequenceProtocol(id, p);
                centred = false;   // a new pitch range: re-centre
            });
    }
    if (!empty) label(176.0f, 101.0f, "(has notes)", 10.0f, kDim);

    if (midi2) {
        settingRow(120.0f, "EDO");
        stepper(80.0f, 120.0f, std::to_string(seq.edo), [this, id, edo = seq.edo, ref = seq.referenceNote](int dir, bool big) {
            controller.setSequenceTuning(id, edo + dir * (big ? 12 : 1), ref);
        });
        settingRow(150.0f, "Step 0 at");
        stepper(80.0f, 150.0f, noteName(seq.referenceNote) + " (" + std::to_string(seq.referenceNote) + ")",
                [this, id, edo = seq.edo, ref = seq.referenceNote](int dir, bool big) {
                    controller.setSequenceTuning(id, edo, ref + dir * (big ? 12 : 1));
                });
    } else {
        label(kPad, 131.0f, "Rows are MIDI note numbers.", 11.0f, kDim);
    }

    settingRow(180.0f, "Rate");
    button(80.0f, 180.0f, 140.0f, 22.0f, rateText(seq.rate) + " per beat", false, true, kAccent);
    region(80.0f, 180.0f, 140.0f, 22.0f, [this, id, current = seq.rate](const Press&) {
        Menu m;
        m.x     = 80.0f;
        m.y     = 204.0f;
        m.w     = 170.0f;
        m.title = "Units per beat";
        for (const auto [n, d] : {std::pair{1, 4}, {1, 3}, {1, 2}, {1, 1}, {2, 1}, {3, 1}, {4, 1}, {8, 1}, {16, 1}}) {
            const auto r = SequenceRate{static_cast<std::uint16_t>(n), static_cast<std::uint16_t>(d)};
            m.items.push_back(MenuItem{rateText(r), [this, id, r] { controller.setSequenceRate(id, r); },
                                       current.numerator == r.numerator && current.denominator == r.denominator});
        }
        openMenu(std::move(m));
    });

    settingRow(210.0f, "Length");
    stepper(80.0f, 210.0f, std::to_string(seq.length) + (seq.length == 1 ? " unit" : " units"),
            [this, id, len = seq.length](int dir, bool big) {
        controller.setSequenceLength(id, static_cast<std::uint32_t>(std::max(1, static_cast<int>(len) + dir * (big ? 4 : 1))));
    });

    settingRow(240.0f, "Launch");
    const char* launch = seq.launch == LaunchQuantize::Bar    ? "Next bar"
                         : seq.launch == LaunchQuantize::Beat ? "Next beat"
                                                              : "Immediately";
    button(80.0f, 240.0f, 140.0f, 22.0f, launch, false, true, kAccent);
    region(80.0f, 240.0f, 140.0f, 22.0f, [this, id, current = seq.launch](const Press&) {
        Menu m;
        m.x     = 80.0f;
        m.y     = 264.0f;
        m.w     = 170.0f;
        m.title = "Play and Stop take effect at";
        for (const auto [q, text] : {std::pair{LaunchQuantize::Bar, "The next bar"},
                                     {LaunchQuantize::Beat, "The next beat"},
                                     {LaunchQuantize::Immediate, "Immediately"}})
            m.items.push_back(MenuItem{text, [this, id, q = q] { controller.setSequenceLaunch(id, q); }, current == q});
        openMenu(std::move(m));
    });

    settingRow(270.0f, "Repeat");
    button(80.0f, 270.0f, 70.0f, 22.0f, seq.repeat ? "On" : "Off", seq.repeat, true, kAccent);
    region(80.0f, 270.0f, 70.0f, 22.0f,
           [this, id, repeat = seq.repeat](const Press&) { controller.setSequenceRepeat(id, !repeat); });

    settingRow(300.0f, "Note-off");
    char offset[32];
    std::snprintf(offset, sizeof offset, "%+.0f ms", seq.noteOffOffsetMs);
    stepper(80.0f, 300.0f, offset, [this, id, ms = seq.noteOffOffsetMs](int dir, bool big) {
        const double step = big ? 100.0 : 10.0;   // onto the step's grid, so an odd value from a file lines up
        controller.setSequenceNoteOffOffset(id, std::round((ms + dir * step) / step) * step);
    });

    settingRow(330.0f, "Channel");
    stepper(80.0f, 330.0f, std::to_string(seq.channel + 1), [this, id, ch = seq.channel](int dir, bool) {
        controller.setSequenceChannel(id, ch + dir);
    });
    settingRow(360.0f, "Group");
    stepper(80.0f, 360.0f, std::to_string(seq.group + 1), [this, id, g = seq.group](int dir, bool) {
        controller.setSequenceGroup(id, g + dir);
    });

    settingRow(390.0f, "New notes");
    const std::uint32_t newLength = editor.newNoteLength();
    stepper(80.0f, 390.0f, std::to_string(newLength) + (newLength == 1 ? " unit" : " units"), [this](int dir, bool big) {
        editor.setNewNoteLength(static_cast<std::uint32_t>(
            std::max(1, static_cast<int>(editor.newNoteLength()) + dir * (big ? 4 : 1))));
    });

    button(kPad, 424.0f, 100.0f, 24.0f, "Undo", false, editor.canUndo(), kAccent);
    if (editor.canUndo()) region(kPad, 424.0f, 100.0f, 24.0f, [this](const Press&) { editor.undo(); });
    button(kPad + 108.0f, 424.0f, 100.0f, 24.0f, "Redo", false, editor.canRedo(), kAccent);
    if (editor.canRedo()) region(kPad + 108.0f, 424.0f, 100.0f, 24.0f, [this](const Press&) { editor.redo(); });

    static const char* const help[] = {
        "Click: add a note; drag right: longer", "Drag a note: move (Shift: one way)", "Drag its right edge: length",
        "Alt-drag: copy    Ctrl-drag: velocity", "Right- or double-click: delete", "Shift-click: select    Shift-drag: area",
        "Arrows: move    Delete: remove", "Ctrl-Z, Ctrl-Shift-Z: undo, redo", "Space: play or stop",
        "Wheel: scroll    Shift: sideways", "Ctrl-wheel: zoom"};
    float y = 468.0f;
    for (const char* line : help) {
        label(kPad, y, line, 10.5f, kDim);
        y += 15.0f;
    }
}

// --- input ---------------------------------------------------------------------------

// The gesture table in docs/sequences.md, section 5.
void SequenceEditorWidget::pressCanvas(const Press& p) {
    const Sequence* seq = editor.sequence();
    if (seq == nullptr) return;
    const std::int64_t unit  = unitAt(p.x);
    const int          pitch = pitchAt(p.y);
    if (pitch < editor.lowestPitch() || pitch > editor.highestPitch()) return;
    const bool inLoop = unit >= 0 && unit < static_cast<std::int64_t>(seq->length);
    const int  index  = inLoop ? editor.noteAt(static_cast<std::uint32_t>(unit), pitch) : -1;
    const bool shift  = (p.mod & dgl::kModifierShift) != 0;
    const bool ctrl   = (p.mod & dgl::kModifierControl) != 0;
    const bool alt    = (p.mod & dgl::kModifierAlt) != 0;

    if (p.button == dgl::kMouseButtonRight) {   // right-click deletes
        if (index >= 0) editor.deleteNote(index);
        return;
    }

    if (index >= 0) {
        const SequenceNote note   = editor.items()[static_cast<std::size_t>(index)].note;
        const bool         twice  = clicked && p.time - lastClickTime < 400 && lastClickUnit == note.start &&
                           lastClickPitch == note.pitch;
        clicked        = !twice;
        lastClickTime  = p.time;
        lastClickUnit  = note.start;
        lastClickPitch = note.pitch;
        if (twice) {   // double-click deletes
            editor.deleteNote(index);
            return;
        }
        if (shift && !ctrl) {   // Shift-click adds to (or takes from) the selection
            editor.toggle(index);
            return;
        }
        if (!editor.items()[static_cast<std::size_t>(index)].selected) editor.select(index, false);
        editor.soundNote(index);   // a click on a note plays it (whatever the drag does next)
        editor.beginGesture();
        dragEnd = [this] {
            editor.endGesture();
            editor.stopSounding();   // the note sounds only while it's held
        };

        if (ctrl) {   // velocity: up is louder, 200 px for the whole range
            drag = [this, startY = p.y](float, float y, std::uint32_t) { editor.changeVelocity((startY - y) / 200.0); };
            return;
        }
        const float rightEdge = canvasX() + static_cast<float>(note.start + note.length) * unitWidth() - scrollX;
        if (p.x >= rightEdge - 6.0f) {   // the right edge: length
            drag = [this, index, start = note.start](float x, float, std::uint32_t) {
                editor.resize(index, static_cast<std::uint32_t>(std::max<std::int64_t>(1, unitAt(x) - start + 1)));
            };
            return;
        }
        drag = [this, startX = p.x, startY = p.y, alt](float x, float y, std::uint32_t mod) {   // move (or copy)
            float dx = x - startX, dy = startY - y;
            if ((mod & dgl::kModifierShift) != 0) (std::fabs(dx) > std::fabs(dy) ? dy : dx) = 0.0f;   // one axis
            editor.moveSelection(static_cast<int>(std::lround(dx / unitWidth())),
                                 static_cast<int>(std::lround(dy / kRowH)), alt);
        };
        return;
    }

    clicked = false;
    if (shift) {   // a rubber band selects what it touches
        banding = true;
        bandX0 = bandX1 = p.x;
        bandY0 = bandY1 = p.y;
        drag            = [this](float x, float y, std::uint32_t) {
            bandX1 = x;
            bandY1 = y;
        };
        dragEnd = [this] {
            banding                = false;
            const std::int64_t u0  = std::max<std::int64_t>(0, unitAt(std::min(bandX0, bandX1)));
            const std::int64_t u1  = std::max<std::int64_t>(0, unitAt(std::max(bandX0, bandX1)));
            editor.selectRect(static_cast<std::uint32_t>(u0), static_cast<std::uint32_t>(u1),
                              pitchAt(std::max(bandY0, bandY1)), pitchAt(std::min(bandY0, bandY1)), false);
        };
        return;
    }
    if (!inLoop) {
        editor.clearSelection();
        return;
    }
    editor.beginGesture();   // a new note; dragging right lengthens it
    if (!editor.addNote(static_cast<std::uint32_t>(unit), pitch)) {
        editor.endGesture();
        return;
    }
    drag = [this](float x, float, std::uint32_t) {
        editor.stretchNew(static_cast<std::uint32_t>(std::max<std::int64_t>(0, unitAt(x))));
    };
    dragEnd = [this] {
            editor.endGesture();
            editor.stopSounding();   // the note sounds only while it's held
        };
}

void SequenceEditorWidget::wheelUnclaimed(float x, float, float notches, std::uint32_t mod) {
    if (x < canvasX()) return;
    if ((mod & dgl::kModifierControl) != 0) {   // zoom
        zoom = std::clamp(unitWidth() * (notches > 0 ? 1.25f : 0.8f), 4.0f, 200.0f);
    } else if ((mod & dgl::kModifierShift) != 0) {
        scrollX -= notches * 40.0f;
    } else {
        scrollY -= notches * kRowH * 3.0f;
    }
    clampScroll();
}

bool SequenceEditorWidget::onKeyboard(const KeyboardEvent& ev) {
    if (UiWidget::onKeyboard(ev)) return true;   // a text field has the keys
    if (!ev.press) return false;
    const Sequence* seq = editor.sequence();
    if (seq == nullptr) return false;
    const bool ctrl  = (ev.mod & dgl::kModifierControl) != 0;
    const bool shift = (ev.mod & dgl::kModifierShift) != 0;
    const int  beat  = std::max(1, static_cast<int>(std::lround(seq->rate.unitsPerBeat())));

    if (ctrl && isKey(ev.key, 'q')) {
        controller.quit();
    } else if (ctrl && shift && isKey(ev.key, 's')) {
        controller.save();
    } else if (ctrl && isKey(ev.key, 'z')) {
        shift ? editor.redo() : editor.undo();
    } else if (ctrl && isKey(ev.key, 'y')) {
        editor.redo();
    } else if (ctrl && isKey(ev.key, 'a')) {
        editor.selectAll();
    } else if (ev.key == dgl::kKeyDelete || ev.key == dgl::kKeyBackspace) {
        editor.deleteSelection();
    } else if (ev.key == dgl::kKeyEscape) {
        closeMenu();
        editor.clearSelection();
    } else if (ev.key == dgl::kKeySpace) {
        controller.togglePlay(seq->id);
    } else if (ev.key == dgl::kKeyLeft || ev.key == dgl::kKeyRight) {
        editor.nudge((ev.key == dgl::kKeyLeft ? -1 : 1) * (shift ? beat : 1), 0);
    } else if (ev.key == dgl::kKeyUp || ev.key == dgl::kKeyDown) {
        editor.nudge(0, (ev.key == dgl::kKeyDown ? -1 : 1) * (shift ? editor.stepsPerOctave() : 1));
    } else {
        return false;
    }
    repaint();
    return true;
}

} // namespace stagehand
