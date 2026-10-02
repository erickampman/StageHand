#include "Ui/RackWidget.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include "Core/Ump.h"
#include "Session/RackFile.h"
#include "Session/SetlistFile.h"

namespace stagehand {

namespace dgl = DGL_NAMESPACE;
using namespace ui;

namespace {

// --- look --------------------------------------------------------------------------
// Logical pixels, multiplied by the window's scale factor when drawn. Stage-first:
// dark, high contrast, big enough to hit.

constexpr float kToolbarH   = 44.0f;
constexpr float kSetlistH   = 40.0f;   // the setlist row, above the toolbar while a setlist is open
constexpr float kRowH       = 64.0f;
constexpr float kStatusH    = 30.0f;
constexpr float kInputW     = 330.0f;
constexpr float kTransportX = 280.0f;   // the toolbar's transport, left of the input column's edge
constexpr float kPluginW    = 270.0f;
constexpr float kMinOutputW = 330.0f;
constexpr float kRemoveSize = 26.0f;   // the red X at each row's right end
constexpr float kMidiW      = 34.0f;   // a MIDI control button (the DIN plug)
constexpr float kMinDb      = -60.0f;   // one scale for fader and meters, so they line up
constexpr float kMaxDb      = 6.0f;
constexpr float kKeyBarX    = kPad + 16.0f;   // 128 keys at 1 logical pixel each
constexpr float kDockHeadH  = 28.0f;   // the sources dock: header, inputs row, sequence rows
constexpr float kInputsH    = 34.0f;
constexpr float kSeqRowH    = 44.0f;
constexpr float kBusH       = 48.0f;   // a rig bus strip (docs/rig.md), between the rows and the dock
constexpr float kSendW      = 46.0f;   // a row's send box (A, B), left of its fader

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

// The same phases the drawing uses: the MIDI and Play/Stop blinks (0.5 s) and
// the text caret (1 s).
std::uint32_t RackWidget::timedLook(double now) const {
    const RackViewModel& v = controller.view();
    bool blinking          = v.transport.learning || v.setlist.learning || v.looperSwitchLearning;
    for (const LooperRowModel& looper : v.loopers) blinking = blinking || looper.waiting;
    for (const SequenceRowModel& seq : v.sequences)
        blinking = blinking || seq.learning || seq.state == SequenceState::Starting ||
                   seq.state == SequenceState::Stopping;
    std::uint32_t look = 0;
    if (blinking && std::fmod(now, 0.5) < 0.25) look |= 1u;
    if (editingText() && std::fmod(now, 1.0) < 0.6) look |= 2u;
    if (!notice.empty() && now < noticeUntil) look |= 4u;
    return look;
}

// --- geometry ------------------------------------------------------------------------

float RackWidget::outputX() const {
    return kInputW + kPluginW;
}
float RackWidget::outputW() {
    return std::max(kMinOutputW, width() - outputX());
}
// The setlist row (when open), then the toolbar, then the rows.
float RackWidget::toolbarY() const {
    return controller.view().setlist.open ? kSetlistH : 0.0f;
}
float RackWidget::topH() const {
    return toolbarY() + kToolbarH;
}
float RackWidget::rowsHeight() {   // the rows' area (and the buses', when they scroll)
    return busesScroll() ? dockTop() - topH() : dockTop() - rigHeight() - topH();
}
bool RackWidget::busesScroll() {
    const float rows = static_cast<float>(controller.view().rows.size()) * kRowH;
    return rigHeight() > 0.0f && rows + rigHeight() > dockTop() - topH() + 0.5f;
}
float RackWidget::rigHeight() const {
    return controller.view().rig.loaded ? kBusH * kRigBuses : 0.0f;
}
float RackWidget::rigTop() {
    if (busesScroll()) return topH() + static_cast<float>(controller.view().rows.size()) * kRowH - scrollY;
    return dockTop() - rigHeight();
}
// The dock grows with its sequences, up to a little over half the space; past
// that its sequence rows scroll.
float RackWidget::dockHeight() {
    // At least one row's height: an empty dock shows how to add one. Loopers
    // are rows under the sequences.
    const float rows = static_cast<float>(
        std::max<std::size_t>(controller.view().sequences.size() + controller.view().loopers.size(), 1));
    const float want = kDockHeadH + kInputsH + rows * kSeqRowH;
    const float most = std::max(kDockHeadH + kInputsH + kSeqRowH, (height() - topH() - kStatusH) * 0.55f);
    return std::min(want, most);
}
float RackWidget::wantedHeight() const {
    const RackViewModel& v    = controller.view();
    const auto           rows = static_cast<float>(std::clamp<std::size_t>(v.rows.size(), 3, 10));
    const auto sources = static_cast<float>(std::clamp<std::size_t>(v.sequences.size() + v.loopers.size(), 1, 4));
    return topH() + rows * kRowH + rigHeight() + kDockHeadH + kInputsH + sources * kSeqRowH + kStatusH;
}
float RackWidget::dockTop() {
    return height() - kStatusH - dockHeight();
}
float RackWidget::dockMaxScroll() {
    const float content =
        static_cast<float>(controller.view().sequences.size() + controller.view().loopers.size()) * kSeqRowH;
    return std::max(0.0f, content - (dockHeight() - kDockHeadH - kInputsH));
}
float RackWidget::rowY(std::size_t row) const {
    return topH() + static_cast<float>(row) * kRowH - scrollY;
}
float RackWidget::faderX0() const {   // after M, and the send boxes when there's a rig
    return outputX() + kPad + 30.0f + 14.0f + (controller.view().rig.loaded ? kSendW + 8.0f : 0.0f);
}
float RackWidget::faderX1() {   // the gain and output text, then the red X, to its right
    return outputX() + outputW() - kPad - 76.0f - 22.0f - kRemoveSize - 8.0f;
}
float RackWidget::maxScroll() {
    const float content = static_cast<float>(controller.view().rows.size()) * kRowH + (busesScroll() ? rigHeight() : 0.0f);
    return std::max(0.0f, content - rowsHeight());
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
    if (!editingText()) renamingId.clear();   // the rename ended (Enter, Escape, or a click)
    box(0, 0, width(), height(), kBg);

    const RackViewModel& view = controller.view();
    scrollY = std::clamp(scrollY, 0.0f, maxScroll());

    scissor(0, topH(), width(), rowsHeight());
    for (std::size_t i = 0; i < view.rows.size(); ++i) {
        const float y = rowY(i);
        if (y + kRowH < topH() || y > topH() + rowsHeight()) continue;
        drawRow(view.rows[i], i);
    }
    if (view.rows.empty())
        label(width() / 2, topH() + rowsHeight() / 2, "This rack has no plugins yet. Add one with + Add plugin.",
              16.0f, kDim, ALIGN_CENTER | ALIGN_MIDDLE);
    resetScissor();

    // Drawn after the rows, so their regions cover rows scrolled under them.
    if (view.rig.loaded) drawRig();
    drawDock();
    beginShift(toolbarY());   // the toolbar draws itself at the top
    drawToolbar();
    endShift(toolbarY());
    if (view.setlist.open) drawSetlistRow();
    drawStatus();
}

// --- the setlist row (docs/setlists.md, "The setlist row") ----------------------------------

// ◀ 3/12 Song name ▾ ▶, then where Next goes; the setlist's name at the right.
void RackWidget::drawSetlistRow() {
    const SetlistModel& set = controller.view().setlist;
    box(0, 0, width(), kSetlistH, kPanel);
    box(0, kSetlistH - 1.0f, width(), 1.0f, kTrack);
    region(0, 0, width(), kSetlistH, nullptr);

    const float y = 6.0f, h = 28.0f, arrowW = 44.0f;
    // Skip shapes (|◀ and ▶|), so they can't be taken for the transport's Start.
    auto arrow = [&](float x, bool left, bool enabled) {
        box(x, y, arrowW, h, enabled ? kTrack : kBarBg, 4.0f);
        const float   cx = x + arrowW / 2 + (left ? 2.0f : -2.0f), cy = y + h / 2;
        const UiColor c  = enabled ? kText : kTrack;
        beginPath();
        moveTo(cx + (left ? 5.0f : -5.0f), cy - 7.0f);
        lineTo(cx + (left ? -5.0f : 5.0f), cy);
        lineTo(cx + (left ? 5.0f : -5.0f), cy + 7.0f);
        closePath();
        fill(c);
        NanoVG::fill();
        box(left ? cx - 8.0f : cx + 6.0f, cy - 7.0f, 2.5f, 14.0f, c);
    };

    // Previous.
    float x = kPad;
    arrow(x, true, set.previous >= 0);
    if (set.previous >= 0)
        region(x, y, arrowW, h, [this](const Press&) { switchSong([this] { controller.previousSong(); }); });
    x += arrowW + 10.0f;

    // Where we are, and the song: a click opens the song list.
    const int         count = static_cast<int>(set.songs.size());
    const std::string where = set.current >= 0 ? std::to_string(set.current + 1) + "/" + std::to_string(count)
                                               : "-/" + std::to_string(count);
    label(x, kSetlistH / 2, where, 13.0f, kDim);
    const float nameX = x + std::max(44.0f, labelWidth(where, 13.0f) + 10.0f);
    songNameX         = nameX;
    std::string song;
    UiColor     songColor = kText;
    if (set.songs.empty()) {
        song      = "No songs yet";
        songColor = kDim;
    } else if (set.current >= 0) {
        song = set.songs[static_cast<std::size_t>(set.current)].name;
    } else {
        song      = "Not a song in this set: " + controller.view().rackName;
        songColor = kAmber;
    }
    const float nameW = std::min(labelWidth(song, 17.0f), std::max(80.0f, width() * 0.3f));
    save();
    scissor(nameX, 0, nameW, kSetlistH);
    label(nameX, kSetlistH / 2, song, 17.0f, songColor);
    restore();
    label(nameX + nameW + 6.0f, kSetlistH / 2, "\xE2\x96\xBE", 14.0f, kDim);   // U+25BE small down triangle
    region(nameX - 4.0f, y, nameW + 24.0f, h, [this, nameX](const Press&) { openSongMenu(nameX, kSetlistH); });
    x = nameX + nameW + 28.0f;

    // Next, and where it goes.
    arrow(x, false, set.next >= 0);
    if (set.next >= 0) region(x, y, arrowW, h, [this](const Press&) { switchSong([this] { controller.nextSong(); }); });
    x += arrowW + 12.0f;
    // A switch waiting for its rack (being prepared: the status bar counts),
    // else where Next goes, with a dot lit once its rack is prepared.
    std::string next = count > 0 ? "end of the set" : "";
    if (set.waiting >= 0) {
        next = "going to: " + set.songs[static_cast<std::size_t>(set.waiting)].name;
    } else if (set.next >= 0) {
        const SongModel& to = set.songs[static_cast<std::size_t>(set.next)];
        beginPath();
        circle(x + 4.0f, kSetlistH / 2, 4.0f);
        fill(to.prepared ? kGreen : kTrack);
        NanoVG::fill();
        x += 14.0f;
        next = "next: " + to.name;
    }
    const std::string name = set.name;
    // The setlist's MIDI control (section 4) at the right, before its name:
    // lit when it has triggers, blinking amber while learning.
    const float midiX     = width() - kPad - kMidiW;
    const bool  blinkOn   = std::fmod(ui::nowSeconds(), 0.5) < 0.25;
    midiButton(midiX, y, kMidiW, h, set.learning ? blinkOn : !set.triggers.empty(), set.learning ? kAmber : kAccent);
    region(midiX, y, kMidiW, h, [this, midiX](const Press&) { openSetlistMidiMenu(midiX, kSetlistH); });
    const float       nameRight = midiX - 10.0f;
    const float       setW      = labelWidth(name, 13.0f);
    save();
    scissor(x, 0, std::max(0.0f, nameRight - setW - 16.0f - x), kSetlistH);
    const bool nextProblems = set.next >= 0 && !set.songs[static_cast<std::size_t>(set.next)].problems.empty();
    label(x, kSetlistH / 2, next, 13.0f, set.waiting >= 0 || nextProblems ? kAmber : kDim);
    restore();
    label(nameRight, kSetlistH / 2, name, 13.0f, kDim, ALIGN_RIGHT | ALIGN_MIDDLE);
}

// The setlist's triggers: learn Next, Previous or Program Change, or remove one.
void RackWidget::openSetlistMidiMenu(float x, float y) {
    const SetlistModel& set = controller.view().setlist;
    Menu                m;
    m.w     = 380.0f;
    m.x     = std::max(0.0f, x + kMidiW - m.w);   // under the button, kept on screen
    m.y     = y;
    m.title = set.learning ? (set.learningAction == SetlistTrigger::Action::Song ? "Send a Program Change..."
                                                                                  : "Press a pad, key or pedal...")
                           : "Setlist: MIDI control (wins over a rack's)";
    if (set.learning) m.items.push_back(MenuItem{"Stop learning", [this] { controller.cancelLearn(); }});
    for (const auto [action, text] : {std::pair{SetlistTrigger::Action::Next, "Learn Move to Next Song"},
                                      {SetlistTrigger::Action::Previous, "Learn Move to Previous Song"},
                                      {SetlistTrigger::Action::Song, "Learn Go to Song by Program Change (n: song n)"}})
        m.items.push_back(MenuItem{text, [this, a = action] { controller.learnSetlistTrigger(a); },
                                   set.learning && set.learningAction == action});
    for (std::size_t i = 0; i < set.triggers.size(); ++i)
        m.items.push_back(MenuItem{"Remove " + set.triggers[i], [this, i] { controller.removeSetlistTrigger(i); },
                                   false, true});
    openMenu(std::move(m));
}

// Every song: its number and name, the playing one ticked, a missing rack
// greyed out (it can't be played until it's back).
void RackWidget::openSongMenu(float x, float y) {
    const SetlistModel& set = controller.view().setlist;
    Menu                m;
    m.x     = x;
    m.y     = y;
    m.w     = 360.0f;
    m.title = set.name + (set.checking ? "   (checking...)" : "");
    for (std::size_t i = 0; i < set.songs.size(); ++i) {
        const SongModel&  s    = set.songs[i];
        const std::string text = std::to_string(i + 1) + "   " + s.name + (s.missing ? "   (rack missing)" : "") +
                                 (s.unsaved ? "   (unsaved)" : "");
        MenuItem item{text,
                      s.missing ? std::function<void()>{[this, i, x, y] { openMissingSongMenu(i, x, y); }}
                                : std::function<void()>{[this, i] { switchSong([this, i] { controller.goToSong(i); }); }},
                      static_cast<int>(i) == set.current};
        item.warn = !s.problems.empty();
        m.items.push_back(std::move(item));
        // What Check setlist found, under the song, in amber.
        for (const std::string& problem : s.problems) {
            MenuItem line{"      " + problem, {}};
            line.warn = true;
            m.w       = std::max(m.w, std::min(760.0f, 7.5f * static_cast<float>(line.label.size()) + 40.0f));   // about 7.5 px a character
            m.items.push_back(std::move(line));
        }
    }
    if (set.songs.empty()) m.items.push_back(MenuItem{"No songs yet", {}});

    // The current song's own edits (adding songs is File > New / Add a rack).
    const int current = set.current;
    if (current >= 0) {
        const auto        i    = static_cast<std::size_t>(current);
        const std::string name = set.songs[i].name;
        m.items.push_back(MenuItem{"This song (" + name + "):", {}});
        m.items.push_back(MenuItem{"   Add this rack again", [this] { controller.addThisRackAgain(); }});
        m.items.push_back(MenuItem{"   Move up", current > 0 ? std::function<void()>{[this, i] { controller.moveSong(i, -1); }}
                                                             : std::function<void()>{}});
        m.items.push_back(MenuItem{"   Move down", i + 1 < set.songs.size()
                                                       ? std::function<void()>{[this, i] { controller.moveSong(i, 1); }}
                                                       : std::function<void()>{}});
        m.items.push_back(MenuItem{"   Rename in setlist...", [this, i, name] {
                                       editText(songNameX - 6.0f, 6.0f, 320.0f, 28.0f, name, RackController::kMaxNameBytes,
                                                [this, i](const std::string& text) { controller.renameSong(i, text); });
                                   }});
        m.items.push_back(MenuItem{"   Remove from setlist...", [this, i, x, y] { confirmRemoveSong(i, x, y); }, false,
                                   true});
    }
    openMenu(std::move(m));
}

void RackWidget::openMissingSongMenu(std::size_t index, float x, float y) {
    const SetlistModel& set = controller.view().setlist;
    if (index >= set.songs.size()) return;
    Menu m;
    m.x     = x;
    m.y     = y;
    m.w     = 360.0f;
    m.title = set.songs[index].name + ": its rack is missing";
    m.items.push_back(MenuItem{"Find its rack...", [this, index] {
                                   if (chooseFile)
                                       chooseFile(Choose::Rack, "Find the rack for this song (choose its .rack folder)",
                                                  setRackFolder(), "", [this, index](const std::string& path) {
                                                      controller.relinkSong(index, path);
                                                  });
                               }});
    m.items.push_back(MenuItem{"Remove from setlist...", [this, index, x, y] { confirmRemoveSong(index, x, y); }, false,
                               true});
    m.items.push_back(MenuItem{"Cancel", [] {}});
    openMenu(std::move(m));
}

void RackWidget::confirmRemoveSong(std::size_t index, float x, float y) {
    const SetlistModel& set = controller.view().setlist;
    if (index >= set.songs.size()) return;
    Menu m;
    m.x     = x;
    m.y     = y;
    m.w     = 360.0f;
    m.title = "Remove " + set.songs[index].name + " from the set?";
    m.items.push_back(MenuItem{"Remove it (the rack stays on disk)", [this, index] { controller.removeSong(index); },
                               false, true});
    m.items.push_back(MenuItem{"Cancel", [] {}});
    openMenu(std::move(m));
}

// New setlist: where to save it, then (with this rack) the running rack is
// its first song. An untitled rack is saved first; an existing setlist is
// replaced only after asking.
void RackWidget::newSetlist(bool withThisRack) {
    auto create = [this, withThisRack] {
        if (!chooseFile) return;
        chooseFile(Choose::Save, withThisRack ? "New setlist with this rack" : "New setlist", setlistFolder(),
                   "Setlist.setlist", [this, withThisRack](const std::string& path) {
                       if (controller.newSetlist(path, withThisRack) != RackController::NewSetlistResult::NeedsReplace)
                           return;
                       Menu m;
                       m.x     = width() / 2 - 170.0f;
                       m.y     = topH() + 20.0f;
                       m.w     = 340.0f;
                       m.title = "Replace the existing setlist " +
                                 std::filesystem::path(setlistPathOf(path)).filename().string() + "?";
                       m.items.push_back(MenuItem{"Replace it", [this, path, withThisRack] {
                                                      controller.newSetlist(path, withThisRack, true);
                                                  },
                                                  false, true});
                       m.items.push_back(MenuItem{"Cancel", [] {}});
                       openMenu(std::move(m));
                   });
    };
    if (withThisRack && controller.rackPath().empty())   // a song must be a rack on disk
        saveAsThen(create, "First, save this rack: it becomes song 1 (the setlist is named next)");
    else
        create();
}

std::string RackWidget::setlistFolder() const {
    if (controller.hasSetlist()) return std::filesystem::path(controller.setlistPath()).parent_path().string();
    return startFolder();
}

std::string RackWidget::setRackFolder() const {
    std::error_code   ec;
    const std::string folder = setlistFolder();
    const auto        racks  = std::filesystem::path(folder) / "Racks";
    return std::filesystem::is_directory(racks, ec) ? racks.string() : folder;
}

void RackWidget::switchSong(std::function<void()> go) {
    closeMenu();
    if (!controller.unsavedUntitled()) {
        go();
        return;
    }
    keepChangesThen(std::move(go), false);
}

void RackWidget::drawToolbar() {
    const RackViewModel& view = controller.view();
    box(0, 0, width(), kToolbarH, kBarBg);
    region(0, 0, width(), kToolbarH, nullptr);

    const std::string name = view.rackName.empty() ? "Untitled rack" : view.rackName;
    save();
    scissor(kPad, 0, kTransportX - kPad * 2, kToolbarH);
    label(kPad, kToolbarH / 2 - (view.modified ? 6.0f : 0.0f), name, 18.0f, kText);
    // Ctrl-S saves all of it: this rack, the others kept unsaved, the rig.
    const std::size_t songs = view.otherUnsaved + (view.modified ? 1 : 0);
    const std::string unsaved = view.otherUnsaved != 0 ? std::to_string(songs) + (songs == 1 ? " song unsaved" : " songs unsaved")
                                : view.modified     ? "unsaved changes"
                                : view.rig.modified ? "unsaved rig changes"
                                                    : "";
    if (!unsaved.empty()) label(kPad, kToolbarH / 2 + 12.0f, unsaved, 11.0f, kAmber);
    restore();
    drawTransport(kTransportX);

    const float fileX = width() - kPad - 70.0f;
    button(fileX, 8.0f, 70.0f, 28.0f, "File", false, true, kAccent);
    region(fileX, 8.0f, 70.0f, 28.0f, [this, fileX](const Press&) { openFileMenu(fileX - 190.0f, topH()); });

    const float addX = fileX - 10.0f - 118.0f;
    button(addX, 8.0f, 118.0f, 28.0f, "+ Add plugin", false, true, kAccent);
    region(addX, 8.0f, 118.0f, 28.0f, [this, addX](const Press&) { openAddMenu(addX, topH()); });
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
    midiButton(midiX, 8.0f, kMidiW, 28.0f, t.learning ? blinkOn : mapped,
               t.learning || !t.startOnNoteConnected ? kAmber : kAccent);
    region(midiX, 8.0f, kMidiW, 28.0f, [this, midiX](const Press&) { openTransportMenu(midiX, topH()); });

    const float sigX = midiX + kMidiW + 6.0f;
    std::snprintf(text, sizeof text, "%d/%d", t.numerator, t.denominator);
    button(sigX, 8.0f, 52.0f, 28.0f, text, false, true, kAccent);
    region(sigX, 8.0f, 52.0f, 28.0f, [this, sigX](const Press&) {
        Menu m;
        m.x     = sigX;
        m.y     = topH();
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

    // "⋮" after the lights: what they and the click count, and the click. Its
    // dots are blue while the click plays, amber if it's on but its output
    // isn't there.
    const float dotsX = sigX + 60.0f, moreX = sigX + 62.0f + static_cast<float>(beats) * spacing;
    for (int i = -1; i <= 1; ++i) {
        beginPath();
        circle(moreX + 6.0f, kToolbarH / 2 + static_cast<float>(i) * 6.0f, 2.0f);
        fill(!t.click ? kText : t.clickConnected ? kAccent : kAmber);
        NanoVG::fill();
    }
    region(dotsX, 8.0f, moreX + 14.0f - dotsX, 28.0f, [this, moreX](const Press&) { openMetronomeMenu(moreX, topH()); });
}

// The lights' count, and the click: on, level, and its output device.
void RackWidget::openMetronomeMenu(float x, float y) {
    controller.refreshPlaybackDevices();   // devices come and go
    const TransportModel& t = controller.view().transport;
    Menu                  m;
    m.x     = x;
    m.y     = y;
    m.w     = 420.0f;
    m.title = "Beat lights and click";
    m.items.push_back(MenuItem{"Count the beats (6/8: 2, 7/8: 7)", [this] { controller.setMetronomeCount(false); },
                               !t.everyNote});
    m.items.push_back(MenuItem{"Count every note (6/8: 6)", [this] { controller.setMetronomeCount(true); }, t.everyNote});
    // One choice: no click, or a click at a level (choosing a level turns it on).
    m.items.push_back(MenuItem{"No click", [this] { controller.setClick(false); }, !t.click});
    for (const float db : {-24.0f, -18.0f, -12.0f, -6.0f, 0.0f})
        m.items.push_back(MenuItem{"Click at " + std::to_string(static_cast<int>(db)) + " dB",
                                   [this, db] { controller.setClickLevel(db); },
                                   t.click && std::fabs(t.clickLevelDb - db) < 0.5f});
    // Why it's silent, where it's plain to see.
    if (t.click && t.clickOutput.empty()) m.items.push_back(MenuItem{"   Silent: choose an output below", {}});
    else if (t.click && !t.clickConnected) m.items.push_back(MenuItem{"   Silent: its output isn't connected", {}});
    else if (t.click && !t.running) m.items.push_back(MenuItem{"   Sounds while the transport runs (Space)", {}});
    m.items.push_back(MenuItem{"Test the click (one blip now)",
                               t.clickOutput.empty() ? std::function<void()>{} : [this] { controller.testClick(); }});
    bool listed = false;
    for (const std::string& device : t.playbackDevices) {
        listed = listed || device == t.clickOutput;
        const std::string note = device == t.mainDevice ? "  (also the mains)" : "";
        m.items.push_back(MenuItem{"Click output: " + device + note,
                                   [this, device] { controller.setClickOutput(device); }, device == t.clickOutput});
    }
    if (!t.clickOutput.empty() && !listed)
        m.items.push_back(MenuItem{"Click output: " + t.clickOutput + " (not connected)", {}, true});
    if (t.playbackDevices.empty()) m.items.push_back(MenuItem{"No playback devices found", {}});
    openMenu(std::move(m));
}

void RackWidget::drawRow(const RowModel& row, std::size_t index) {
    const float y = rowY(index);
    box(0, y, width(), kRowH, index % 2 ? kRowB : kRowA);
    box(0, y + kRowH - 1.0f, width(), 1.0f, kLine);
    box(kInputW, y + 8.0f, 1.0f, kRowH - 16.0f, kLine);
    box(outputX(), y + 8.0f, 1.0f, kRowH - 16.0f, kLine);

    // Dimmed when it can't sound: a placeholder, or muted (its M button stays bright).
    globalAlpha(row.state == RowState::Running && !row.muted ? 1.0f : 0.55f);
    drawRowInput(row, y);
    drawRowPlugin(row, y);
    if (row.state != RowState::Placeholder) drawRowOutput(row, y);
    globalAlpha(1.0f);

    // Remove: the red X at the right end, asking first.
    const float rx = outputX() + outputW() - kPad - kRemoveSize, ry = y + (kRowH - kRemoveSize) / 2;
    removeButton(rx, ry, kRemoveSize);
    region(rx, ry, kRemoveSize, kRemoveSize, [this, id = row.id, name = row.name, rx, ry](const Press&) {
        confirmRemoveRow(id, name, rx + kRemoveSize, ry + kRemoveSize + 2.0f);
    });
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

// Plugin: slot name, plugin and vendor (or why it didn't load), and the editor
// button.
void RackWidget::drawRowPlugin(const RowModel& row, float y) {
    const float px      = kInputW + kPad;
    const float editorX = kInputW + kPluginW - kPad - 70.0f;
    save();
    scissor(px, y, editorX - px - 8.0f, kRowH);
    label(px, y + 23.0f, row.name, 16.0f, kText);
    if (row.state == RowState::Placeholder) {
        label(px, y + 44.0f, "not loaded: " + row.problem, 12.0f, kRed);
    } else {
        std::string detail = row.pluginName + (row.vendor.empty() ? "" : " - " + row.vendor);
        label(px, y + 44.0f, detail, 12.0f, kDim);
    }
    restore();


    button(editorX, y + 19.0f, 70.0f, 26.0f, "Editor", row.editorOpen, row.hasEditor, kAccent);
    if (row.hasEditor)
        region(editorX, y + 19.0f, 70.0f, 26.0f, [this, id = row.id](const Press&) {
            if (const RowModel* now = rowById(id)) now->editorOpen ? controller.closeEditor(id) : controller.openEditor(id);
        });
}

// Output: mute, gain fader over the meters, clip light, gain and output text.
void RackWidget::drawRowOutput(const RowModel& row, float y) {
    const float ox = outputX() + kPad;
    save();
    globalAlpha(1.0f);   // the way back from muted is never dimmed
    button(ox, y + 10.0f, 30.0f, 26.0f, "M", row.muted, true, kAmber);
    restore();
    region(ox, y + 10.0f, 30.0f, 26.0f, [this, id = row.id](const Press&) {
        if (const RowModel* now = rowById(id)) controller.setMuted(id, !now->muted);
    });

    if (controller.view().rig.loaded) {   // the sends, A over B
        sendBox(row, 0, ox + 38.0f, y + 6.0f);
        sendBox(row, 1, ox + 38.0f, y + 34.0f);
    }

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
    // The output pair: a menu when there's more than one pair to choose from.
    const std::string out = "out " + std::to_string(row.firstOutput + 1) + "-" + std::to_string(row.firstOutput + 2);
    if (controller.view().status.outputs > 2) {
        label(x1 + 30.0f, y + 44.0f, out + " \xE2\x96\xBE", 12.0f, kText);   // U+25BE small down triangle
        region(x1 + 26.0f, y + 34.0f, 70.0f, 20.0f,
               [this, row, x = x1 + 26.0f, y](const Press&) { openOutputMenu(row, x, y + 56.0f); });
    } else {
        label(x1 + 30.0f, y + 44.0f, out, 12.0f, kDim);
    }
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

    // Master meter at the right, with the rig's master fader before it; the
    // notice or save target between.
    const float mx1 = width() - kPad - 22.0f, mx0 = mx1 - 180.0f;
    meterBars(mx0, mx1, y + 9.0f, s.outputDb, s.outputDb);
    box(mx1 + 8.0f, y + 8.0f, 14.0f, 14.0f, s.outputClipped ? kRed : kTrack, 2.0f);
    region(mx1 + 4.0f, y + 4.0f, 22.0f, 22.0f, [this](const Press&) { controller.clearOutputClip(); });
    float textEnd = mx0 - 12.0f;
    if (const RigModel& rig = controller.view().rig; rig.loaded) {
        const float f1 = mx0 - 14.0f, f0 = f1 - 110.0f;
        dbFader(f0, f1, y + kStatusH / 2, rig.masterDb, false, 18.0f, "\x01master",
                [this]() -> std::optional<float> { return controller.view().rig.masterDb; },
                [this](float db) { controller.setMasterDb(db); });
        const std::string master = "master " + dbText(rig.masterDb);
        label(f0 - 8.0f, y + kStatusH / 2, master, 12.0f, rig.masterDb > 0.0f ? kAmber : kDim, ALIGN_RIGHT | ALIGN_MIDDLE);
        textEnd = f0 - 16.0f - labelWidth(master, 12.0f);
    }

    const float tx = kPad + labelWidth(left, 12.0f) + 28.0f;
    save();
    scissor(tx, y, std::max(0.0f, textEnd - tx), kStatusH);
    if (!notice.empty() && ui::nowSeconds() < noticeUntil)
        label(tx, y + kStatusH / 2, notice, 12.0f, kAccent);
    else if (!s.preparing.empty())
        label(tx, y + kStatusH / 2, s.preparing, 12.0f, kDim);
    else if (!s.saveTarget.empty())
        label(tx, y + kStatusH / 2, "Saves to " + s.saveTarget, 12.0f, kDim);
    else
        label(tx, y + kStatusH / 2, s.noSaveReason, 12.0f, kAmber);
    restore();
}

// --- the rig (docs/rig.md) ------------------------------------------------------------------

void RackWidget::dbFader(float x0, float x1, float cy, float db, bool dim, float thumbH, const std::string& key,
                         std::function<std::optional<float>()> current, std::function<void(float)> set) {
    box(x0, cy - 3.0f, x1 - x0, 6.0f, kTrack, 3.0f);
    box(x0 + (x1 - x0) * dbToPos(0.0f) - 0.5f, cy - 7.0f, 1.0f, 14.0f, kDim);   // 0 dB mark
    const float thumb = x0 + (x1 - x0) * dbToPos(db);
    box(thumb - 5.0f, cy - thumbH / 2, 10.0f, thumbH, dim ? kDim : kText, 2.0f);
    region(
        x0 - 8.0f, cy - thumbH / 2 - 2.0f, x1 - x0 + 16.0f, thumbH + 4.0f,
        [this, key, current, set, span = x1 - x0](const Press& p) {
            const std::optional<float> now = current();
            if (!now) return;
            if (key == lastFaderId && p.time - lastFaderTime < 400) {   // as a row's fader
                set(0.0f);
                lastFaderId.clear();
                return;
            }
            lastFaderId   = key;
            lastFaderTime = p.time;
            drag = [set, start = *now, span, startX = p.x](float x, float, std::uint32_t mod) {
                const bool  fine = (mod & dgl::kModifierShift) != 0;
                const float to   = start + (x - startX) * (kMaxDb - kMinDb) / span * (fine ? 0.1f : 1.0f);
                float       out  = std::clamp(std::round(to * 10.0f) / 10.0f, kMinDb, kMaxDb);
                if (std::fabs(out) < 0.35f && !fine) out = 0.0f;   // detent at unity
                set(out);
            };
        },
        [current, set](float notches) {
            const std::optional<float> now = current();
            if (!now) return;
            set(std::round(std::clamp(*now + notches * 0.5f, kMinDb, kMaxDb) * 10.0f) / 10.0f);
        });
}

// "A -12" lit, or "A off" dim. Below kMinDb a send is off; from off, a drag or
// notch up starts it at kMinDb.
void RackWidget::sendBox(const RowModel& row, std::size_t bus, float x, float y) {
    const std::optional<float>& db   = row.sendDb[bus];
    char                        text[24];
    if (db) std::snprintf(text, sizeof text, "%c %.0f", 'A' + static_cast<int>(bus), static_cast<double>(*db));
    else std::snprintf(text, sizeof text, "%c off", 'A' + static_cast<int>(bus));
    button(x, y, kSendW, 24.0f, text, db.has_value(), true, kAccent);
    auto current = [this, id = row.id, bus]() -> std::optional<std::optional<float>> {
        const RowModel* now = rowById(id);
        if (now == nullptr) return std::nullopt;
        return now->sendDb[bus];
    };
    auto setTo = [this, id = row.id, bus](float to) {
        controller.setSendDb(id, bus, to < kMinDb ? std::nullopt : std::optional<float>(std::min(to, kMaxDb)));
    };
    region(
        x, y, kSendW, 24.0f,
        [this, key = row.id + "\x01send" + std::to_string(bus), current, setTo](const Press& p) {
            const auto now = current();
            if (!now) return;
            if (key == lastFaderId && p.time - lastFaderTime < 400) {   // double-click: on (0 dB) or off
                setTo(*now ? kMinDb - 1.0f : 0.0f);
                lastFaderId.clear();
                return;
            }
            lastFaderId   = key;
            lastFaderTime = p.time;
            drag = [setTo, start = now->value_or(kMinDb - 1.0f), startY = p.y](float, float y, std::uint32_t mod) {
                const bool  fine = (mod & dgl::kModifierShift) != 0;
                const float to   = std::max(start, kMinDb) + (startY - y) * (fine ? 0.05f : 0.5f);   // up: louder
                setTo(start < kMinDb && y >= startY ? start : fine ? std::round(to * 10.0f) / 10.0f : std::round(to));
            };
        },
        [current, setTo](float notches) {   // a dB a notch
            const auto now = current();
            if (!now) return;
            setTo(*now ? std::round(**now + notches) : notches > 0 ? kMinDb : kMinDb - 1.0f);
        });
}

void RackWidget::drawRig() {
    const RigModel& rig = controller.view().rig;
    const float     top = topH(), bottom = top + rowsHeight() + (busesScroll() ? 0.0f : rigHeight());
    for (std::size_t b = 0; b < kRigBuses; ++b) {
        const float y = rigTop() + static_cast<float>(b) * kBusH;
        // Scrolled out of sight: not drawn. Partly in sight, the toolbar and
        // the dock, drawn after, cover the rest (their regions too).
        if (y + kBusH > top && y < bottom) drawBusStrip(rig.buses[b], b, y);
    }
}

// Bus A: its two effects in series (each a menu, with its editor), then the
// return as a row's output: mute, fader over the meter, level, output pair.
void RackWidget::drawBusStrip(const BusModel& bus, std::size_t index, float y) {
    box(0, y, width(), kBusH, kPanel);
    box(0, y + kBusH - 1.0f, width(), 1.0f, kLine);
    region(0, y, width(), kBusH, nullptr);
    label(kPad, y + 17.0f, "Bus " + bus.name, 15.0f, kText);
    label(kPad, y + 35.0f, index == 0 ? "rig: " + controller.view().rig.name + (controller.view().rig.modified ? " *" : "")
                                      : "effects in series",
          11.0f, kDim);

    const float slotsX = 130.0f, slotW = (outputX() - slotsX - kPad - 8.0f) / RigBus::kEffects;
    for (std::size_t e = 0; e < RigBus::kEffects; ++e) {
        const BusEffectModel& effect = bus.effects[e];
        const float           x      = slotsX + static_cast<float>(e) * slotW;
        const float           editW  = effect.hasEditor ? 52.0f : 0.0f;
        const float           chipW  = slotW - 8.0f - (editW > 0.0f ? editW + 4.0f : 0.0f);
        const std::string     text   = std::to_string(e + 1) + "  " + (effect.name.empty() ? "empty" : effect.name) +
                                 (effect.problem.empty() ? "" : " (not loaded)") + "  \xE2\x96\xBE";
        box(x, y + 11.0f, chipW, 26.0f, kTrack, 4.0f);
        save();
        scissor(x, y, chipW - 4.0f, kBusH);
        label(x + 8.0f, y + kBusH / 2, text, 13.0f,
              !effect.problem.empty() ? kAmber : effect.name.empty() ? kDim : kText);
        restore();
        region(x, y + 11.0f, chipW, 26.0f, [this, index, e, x, y](const Press&) { openEffectMenu(index, e, x, y + kBusH); });
        if (editW > 0.0f) {
            const float ex = x + chipW + 4.0f;
            button(ex, y + 11.0f, editW, 26.0f, "Edit", effect.editorOpen, true, kAccent);
            region(ex, y + 11.0f, editW, 26.0f, [this, index, e, open = effect.editorOpen](const Press&) {
                open ? controller.closeBusEditor(index, e) : controller.openBusEditor(index, e);
            });
        }
    }

    const float ox = outputX() + kPad;
    button(ox, y + 8.0f, 30.0f, 24.0f, "M", bus.muted, true, kAmber);
    region(ox, y + 8.0f, 30.0f, 24.0f, [this, index](const Press&) {
        controller.setBusMuted(index, !controller.view().rig.buses[index].muted);
    });
    const float x0 = faderX0(), x1 = faderX1();
    dbFader(x0, x1, y + 18.0f, bus.returnDb, bus.muted, 20.0f, "\x01bus" + std::to_string(index),
            [this, index]() -> std::optional<float> { return controller.view().rig.buses[index].returnDb; },
            [this, index](float db) { controller.setReturnDb(index, db); });
    meterBars(x0, x1, y + 32.0f, bus.meterDb, bus.peakHoldDb);
    label(x1 + 30.0f, y + 17.0f, bus.muted ? "muted" : dbText(bus.returnDb), 13.0f, bus.muted ? kAmber : kText);
    const std::string out = "out " + std::to_string(bus.firstOutput + 1) + "-" + std::to_string(bus.firstOutput + 2);
    if (controller.view().status.outputs > 2) {
        label(x1 + 30.0f, y + 36.0f, out + " \xE2\x96\xBE", 12.0f, kText);
        region(x1 + 26.0f, y + 27.0f, 70.0f, 20.0f,
               [this, index, x = x1 + 26.0f, y](const Press&) { openBusOutputMenu(index, x, y + kBusH); });
    } else {
        label(x1 + 30.0f, y + 36.0f, out, 12.0f, kDim);
    }
}

// The effects first (the scan's audio effects), then the rest greyed; Empty.
void RackWidget::openEffectMenu(std::size_t bus, std::size_t slot, float x, float y) {
    const BusEffectModel& effect = controller.view().rig.buses[bus].effects[slot];
    Menu                  m;
    m.w     = 360.0f;
    m.x     = x;
    m.y     = y;
    m.title = "Bus " + controller.view().rig.buses[bus].name + ", effect " + std::to_string(slot + 1);
    m.items.push_back(MenuItem{"Empty (passes audio on)", [this, bus, slot] { controller.setBusEffect(bus, slot, nullptr); },
                               effect.name.empty()});
    std::vector<PluginDescriptor> plugins = controller.availablePlugins();   // scans once
    std::stable_sort(plugins.begin(), plugins.end(),
                     [](const PluginDescriptor& a, const PluginDescriptor& b) { return a.name < b.name; });
    bool any = false;
    for (const PluginDescriptor& plugin : plugins) {
        if (plugin.kind != PluginKind::Effect) continue;
        any = true;
        m.items.push_back(MenuItem{"   " + plugin.name + (plugin.vendor.empty() ? "" : "  -  " + plugin.vendor),
                                   [this, bus, slot, plugin] { controller.setBusEffect(bus, slot, &plugin); },
                                   plugin.name == effect.name});
    }
    if (!any) m.items.push_back(MenuItem{"No audio effects found", {}});
    openMenu(std::move(m));
}

void RackWidget::openBusOutputMenu(std::size_t bus, float x, float y) {
    const BusModel& b = controller.view().rig.buses[bus];
    Menu            m;
    m.w     = 180.0f;
    m.x     = std::min(x, width() - m.w - 8.0f);
    m.y     = y;
    m.title = "Bus " + b.name + " returns into";
    const std::uint32_t outputs = controller.view().status.outputs;
    for (std::uint32_t o = 0; o + 1 < outputs; o += 2)
        m.items.push_back(MenuItem{"Output " + std::to_string(o + 1) + "-" + std::to_string(o + 2),
                                   [this, bus, o] { controller.setBusOutput(bus, o); }, b.firstOutput == o});
    openMenu(std::move(m));
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
        // Then where it plays, then its name (newSequence).
        m.items.push_back(MenuItem{"MIDI 2.0 (EDO pitches)", [this, addX, top] { newSequence(MidiProtocol::Midi2, addX, top + kDockHeadH); }});
        m.items.push_back(MenuItem{"MIDI 1.0 (note numbers)", [this, addX, top] { newSequence(MidiProtocol::Midi1, addX, top + kDockHeadH); }});
        openMenu(std::move(m));
    });
    // + Looper (its switch is in each looper's row).
    const float looperX = addX - 8.0f - 96.0f;
    button(looperX, top + 3.0f, 96.0f, 22.0f, "+ Looper", false, true, kAccent);
    region(looperX, top + 3.0f, 96.0f, 22.0f,
           [this, looperX, top](const Press&) { newLooper(looperX, top + kDockHeadH); });
    button(stopX, top + 3.0f, 80.0f, 22.0f, "Stop all", false, !view.sequences.empty(), kAccent);
    region(stopX, top + 3.0f, 80.0f, 22.0f, [this](const Press&) { controller.stopAllSequences(); });

    // Inputs: one chip per open input, lit by its messages.
    const float iy = top + kDockHeadH;
    label(kPad, iy + kInputsH / 2, "Inputs", 12.0f, kDim);
    float x = kPad + 56.0f;
    save();
    scissor(0, iy, width() - kPad, kInputsH);
    for (const InputChipModel& chip : view.inputs) {
        // A device that left: amber, until it's back.
        const std::string text = chip.label + (chip.connected ? "" : " (disconnected)");
        const float       w    = labelWidth(text, 12.0f) + 30.0f;
        box(x, iy + 6.0f, w, 22.0f, kTrack, 11.0f);
        beginPath();
        circle(x + 11.0f, iy + 17.0f, 4.0f);
        fill(!chip.connected ? kAmber : chip.active ? kGreen : kBarBg);
        NanoVG::fill();
        label(x + 21.0f, iy + 17.0f, text, 12.0f, chip.connected ? kText : kAmber);
        x += w + 8.0f;
    }
    if (view.inputs.empty()) label(x, iy + kInputsH / 2, "no MIDI inputs open", 12.0f, kDim);
    restore();

    // Sequence rows, scrolled if there are more than fit.
    const float rowsTop = iy + kInputsH, rowsH = dockHeight() - kDockHeadH - kInputsH;
    dockScroll          = std::clamp(dockScroll, 0.0f, dockMaxScroll());
    save();
    scissor(0, rowsTop, width(), rowsH);
    for (std::size_t i = 0; i < view.sequences.size() + view.loopers.size(); ++i) {
        const float y = rowsTop + static_cast<float>(i) * kSeqRowH - dockScroll;
        if (y + kSeqRowH < rowsTop || y > rowsTop + rowsH) continue;
        if (i < view.sequences.size()) drawSequenceRow(view.sequences[i], y);
        else drawLooperRow(view.loopers[i - view.sequences.size()], y);
    }
    restore();
    if (view.sequences.empty() && view.loopers.empty() && rowsH >= kSeqRowH / 2)
        label(kPad + 56.0f, rowsTop + kSeqRowH / 2, "No sequences or loopers yet. Add one with + Sequence or + Looper.",
              12.0f, kDim);
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
    if (seq.id == renamingId) moveTextField(nameX - 6.0f, y + 4.0f, editX - nameX, 24.0f);   // it follows the row
    save();
    scissor(nameX, y, editX - nameX - 10.0f, kSeqRowH);
    label(nameX, y + 16.0f, seq.name, 14.0f, kText);
    std::string detail = seq.protocol == MidiProtocol::Midi2 ? "MIDI 2.0, " + std::to_string(seq.edo) + "-EDO"
                                                             : std::string("MIDI 1.0");
    detail += ", " + std::to_string(seq.noteCount) + (seq.noteCount == 1 ? " note" : " notes");
    // Where it plays: a click on this line opens the Plays into menu.
    if (seq.usedBy.empty()) {
        detail += "  > no rows: click to choose";
    } else {
        detail += "  >";
        for (std::size_t i = 0; i < seq.usedBy.size(); ++i) detail += (i ? ", " : " ") + seq.usedBy[i];
    }
    label(nameX, y + 33.0f, detail, 11.0f, seq.usedBy.empty() ? kAmber : kDim);
    restore();
    // A click on the name renames it; the editor opens from the Edit button and
    // the mini-map; the second line chooses the rows.
    auto openEditor = [this, id = seq.id](const Press&) {
        if (openSequenceEditor) openSequenceEditor(id);
    };
    region(nameX, y + 4.0f, editX - nameX - 10.0f, 21.0f,
           [this, id = seq.id, name = seq.name](const Press&) { startRename(id, name); });
    region(nameX, y + 25.0f, editX - nameX - 10.0f, kSeqRowH - 29.0f,
           [this, id = seq.id, nameX, y](const Press&) { openTargetsMenu(id, nameX, y + kSeqRowH - 4.0f); });
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

    // At Start (starts with the transport), rate, length, launch, Repeat, MIDI;
    // the red X at the right end.
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
    midiButton(x, y + 10.0f, kMidiW, 24.0f, seq.learning ? blinkOn : !seq.triggers.empty(), seq.learning ? kAmber : kAccent);
    region(x, y + 10.0f, kMidiW, 24.0f, [this, seq, x, y](const Press&) { openTriggerMenu(seq, x, y + 36.0f); });
    x += kMidiW + 6.0f;
    const float rx = std::max(x + 8.0f, width() - kPad - kRemoveSize), ry = y + (kSeqRowH - kRemoveSize) / 2;
    removeButton(rx, ry, kRemoveSize);
    region(rx, ry, kRemoveSize, kRemoveSize, [this, id = seq.id, name = seq.name, rx, ry](const Press&) {
        confirmRemoveSequence(id, name, rx + kRemoveSize, ry + kRemoveSize + 2.0f);
    });
}

// --- loopers (docs/looper.md) ----------------------------------------------------------

namespace {

std::string beatsText(double beats) {
    if (beats <= 0.0) return "off";
    if (beats >= 1.0) return beats == 1.0 ? "1 beat" : std::to_string(static_cast<int>(beats)) + " beats";
    return "1/" + std::to_string(static_cast<int>(std::lround(4.0 / beats)));   // a quarter-note beat: 1/16 is 0.25
}

} // namespace

void RackWidget::drawLooperRow(const LooperRowModel& l, float y) {
    if (l.current) box(0, y, width(), kSeqRowH, kPanel);   // the one the looper switch drives
    box(0, y, width(), 1.0f, kLine);
    region(0, y, width(), kSeqRowH, [this, id = l.id](const Press&) { controller.selectLooper(id); });

    // The button: what the looper switch's tap does next. Record (a red dot),
    // stop (a square, while playing) or play (a triangle); waiting blinks.
    const bool    blinkOn   = std::fmod(ui::nowSeconds(), 0.5) < 0.25;
    const bool    recording = l.state == LooperState::Recording || l.state == LooperState::Overdubbing;
    const bool    playing   = l.state == LooperState::Playing;
    const UiColor face      = l.waiting ? (blinkOn ? kAmber : kTrack) : recording ? kRed : playing ? kGreen : kTrack;
    box(kPad, y + 8.0f, 32.0f, 28.0f, face, 4.0f);
    beginPath();
    if (l.state == LooperState::Empty || l.state == LooperState::Armed || recording) {
        circle(kPad + 16.0f, y + 22.0f, 6.0f);
        fill(recording ? kText : kRed);
    } else if (playing) {
        rect(kPad + 10.0f, y + 16.0f, 12.0f, 12.0f);
        fill(kBarBg);
    } else {
        moveTo(kPad + 11.0f, y + 15.0f);
        lineTo(kPad + 23.0f, y + 22.0f);
        lineTo(kPad + 11.0f, y + 29.0f);
        closePath();
        fill(kText);
    }
    NanoVG::fill();
    region(kPad, y + 8.0f, 32.0f, 28.0f, [this, id = l.id](const Press&) {
        controller.selectLooper(id);
        controller.tapLooper(id);
    });

    // Name; its state, length and notes; where it records from and plays into.
    const float nameX = kPad + 44.0f, textW = 230.0f;
    static const char* const kStates[] = {"empty", "armed", "recording", "playing", "overdubbing", "stopped"};
    std::string state = kStates[static_cast<int>(l.state)];
    if (l.waiting) state += l.state == LooperState::Armed ? ": waiting for the " + std::string(l.atBar ? "bar" : "beat")
                                                          : ": at the next " + std::string(l.atBar ? "bar" : "beat");
    const std::string renameKey = "\x01loop:" + l.id;   // not to be taken for a sequence with the same id
    if (renamingId == renameKey) moveTextField(nameX - 6.0f, y + 3.0f, textW, 24.0f);   // it follows the row
    save();
    scissor(nameX, y, textW, kSeqRowH);
    label(nameX, y + 15.0f, l.name + (l.current ? "   (looper switch)" : ""), 14.0f, kText);
    std::string detail = state;
    if (l.lengthBeats > 0.0) detail += ", " + beatsText(l.lengthBeats);
    if (l.passes > 1) detail += ", " + std::to_string(l.passes - 1) + (l.passes == 2 ? " overdub" : " overdubs");
    detail += "   " + l.inputLabel + " >";
    if (l.targetNames.empty()) detail += " no rows: click to choose";
    for (std::size_t i = 0; i < l.targetNames.size(); ++i) detail += (i ? ", " : " ") + l.targetNames[i];
    label(nameX, y + 32.0f, detail, 11.0f, !l.inputConnected || l.targetNames.empty() ? kAmber : kDim);
    restore();
    // A click on the name renames it (and makes it current); the second line chooses the rows.
    region(nameX, y + 4.0f, textW, 21.0f, [this, id = l.id, name = l.name](const Press&) {
        controller.selectLooper(id);
        startLooperRename(id, name);
    });
    region(nameX, y + 25.0f, textW, kSeqRowH - 29.0f,
           [this, id = l.id, nameX, y](const Press&) { openLooperTargetsMenu(id, nameX, y + kSeqRowH - 4.0f); });

    // Where it is in the loop, and how full it is (shown once past 75%).
    const float mapX = nameX + textW + 10.0f, mapW = 130.0f;
    box(mapX, y + 10.0f, mapW, 24.0f, kRowA, 3.0f);
    if (l.lengthBeats > 0.0 && (playing || l.state == LooperState::Overdubbing))
        box(mapX, y + 10.0f, mapW * static_cast<float>(l.position / l.lengthBeats), 24.0f, recording ? kRed : kGreen, 3.0f);
    else if (recording)
        box(mapX, y + 10.0f, mapW, 24.0f, kRed, 3.0f);
    const float full = l.capacity ? static_cast<float>(l.notes) / static_cast<float>(l.capacity) : 0.0f;
    if (full > 0.75f || l.dropped != 0)
        label(mapX + mapW / 2, y + 22.0f,
              l.dropped != 0 ? "full: " + std::to_string(l.dropped) + " dropped"
                             : std::to_string(l.notes) + "/" + std::to_string(l.capacity) + " notes",
              11.0f, l.dropped != 0 ? kRed : kAmber, ALIGN_CENTER | ALIGN_MIDDLE);

    // Undo, Clear; then its settings: input, quantize, start, length.
    float x = mapX + mapW + 8.0f;
    const bool undo = l.passes > 1 || l.state == LooperState::Overdubbing;
    button(x, y + 10.0f, 50.0f, 24.0f, "Undo", false, undo, kAccent);
    if (undo) region(x, y + 10.0f, 50.0f, 24.0f, [this, id = l.id](const Press&) { controller.undoLooper(id); });
    x += 56.0f;
    const bool clear = l.state != LooperState::Empty;
    button(x, y + 10.0f, 50.0f, 24.0f, "Clear", false, clear, kAccent);
    if (clear) region(x, y + 10.0f, 50.0f, 24.0f, [this, id = l.id](const Press&) { controller.clearLooper(id); });
    x += 56.0f;
    // Keep: convert to a sequence in this rack (saved with it); the looper plays on.
    const bool keep = l.lengthBeats > 0.0;
    button(x, y + 10.0f, 50.0f, 24.0f, "Keep", false, keep, kAccent);
    if (keep) region(x, y + 10.0f, 50.0f, 24.0f, [this, id = l.id](const Press&) { controller.keepLooperAsSequence(id); });
    x += 62.0f;
    button(x, y + 10.0f, 64.0f, 24.0f, "Input", false, true, l.inputConnected ? kAccent : kAmber);
    region(x, y + 10.0f, 64.0f, 24.0f, [this, id = l.id, x, y](const Press&) {
        controller.refreshSources();
        Menu m;
        m.x     = x;
        m.y     = y + 36.0f;
        m.w     = 260.0f;
        m.title = "Records from";
        m.items.push_back(MenuItem{"All inputs", [this, id] { controller.setLooperInput(id, MidiSourceRef{}); }});
        for (const SourceChoice& choice : controller.view().sources)
            if (choice.ref.kind == MidiSourceRef::Kind::Device || choice.ref.kind == MidiSourceRef::Kind::Virtual)
                m.items.push_back(MenuItem{choice.label, [this, id, ref = choice.ref] { controller.setLooperInput(id, ref); }});
        openMenu(std::move(m));
    });
    x += 70.0f;
    button(x, y + 10.0f, 74.0f, 24.0f, "Q " + beatsText(l.quantize), false, true, kAccent);
    region(x, y + 10.0f, 74.0f, 24.0f, [this, id = l.id, x, y, current = l.quantize](const Press&) {
        Menu m;
        m.x     = x;
        m.y     = y + 36.0f;
        m.w     = 220.0f;
        m.title = "Quantize what it records to";
        for (const auto [beats, text] : {std::pair{0.0, "Off (as played)"}, {0.125, "1/32"}, {0.25, "1/16"},
                                         {0.5, "1/8"}, {1.0, "1/4 (a beat)"}, {2.0, "1/2"}})
            m.items.push_back(MenuItem{text, [this, id, b = beats] { controller.setLooperQuantize(id, b); }, current == beats});
        openMenu(std::move(m));
    });
    x += 80.0f;
    button(x, y + 10.0f, 50.0f, 24.0f, l.atBar ? "Bar" : "Beat", false, true, kAccent);
    region(x, y + 10.0f, 50.0f, 24.0f, [this, id = l.id, x, y, atBar = l.atBar](const Press&) {
        Menu m;
        m.x     = x;
        m.y     = y + 36.0f;
        m.w     = 260.0f;
        m.title = "Starts, ends and stops at";
        m.items.push_back(MenuItem{"The next bar", [this, id] { controller.setLooperStart(id, true); }, atBar});
        m.items.push_back(MenuItem{"The next beat", [this, id] { controller.setLooperStart(id, false); }, !atBar});
        openMenu(std::move(m));
    });
    x += 56.0f;
    const std::string length = l.fixedLength == 0 ? "By press"
                               : std::to_string(l.fixedLength) + (l.fixedInBars ? (l.fixedLength == 1 ? " bar" : " bars")
                                                                                : (l.fixedLength == 1 ? " beat" : " beats"));
    button(x, y + 10.0f, 76.0f, 24.0f, length, false, true, kAccent);
    region(x, y + 10.0f, 76.0f, 24.0f, [this, id = l.id, x, y, l](const Press&) {
        Menu m;
        m.x     = x;
        m.y     = y + 36.0f;
        m.w     = 240.0f;
        m.title = "Length";
        m.items.push_back(MenuItem{"Ended by a press", [this, id] { controller.setLooperLength(id, 0, true); },
                                   l.fixedLength == 0});
        for (const std::uint32_t n : {1u, 2u, 4u, 8u})
            m.items.push_back(MenuItem{std::to_string(n) + (n == 1 ? " bar" : " bars"),
                                       [this, id, n] { controller.setLooperLength(id, n, true); },
                                       l.fixedInBars && l.fixedLength == n});
        for (const std::uint32_t n : {1u, 2u, 4u})
            m.items.push_back(MenuItem{std::to_string(n) + (n == 1 ? " beat" : " beats"),
                                       [this, id, n] { controller.setLooperLength(id, n, false); },
                                       !l.fixedInBars && l.fixedLength == n});
        openMenu(std::move(m));
    });
    x += 82.0f;
    // Clean: a fat finger's slip (the short note beside the right one) isn't played.
    button(x, y + 10.0f, 50.0f, 24.0f, "Clean", l.cleanup, true, kAccent);
    region(x, y + 10.0f, 50.0f, 24.0f,
           [this, id = l.id, on = l.cleanup](const Press&) { controller.setLooperCleanup(id, !on); });
    x += 56.0f;
    // The looper switch: one, in the rig, driving the current looper; the
    // same in every looper's row. Lit when learned, amber if its input isn't
    // connected, blinking amber while learning.
    const RackViewModel& view = controller.view();
    midiButton(x, y + 10.0f, kMidiW, 24.0f, view.looperSwitchLearning ? blinkOn : !view.looperSwitch.empty(),
               view.looperSwitchLearning || !view.looperSwitchConnected ? kAmber : kAccent);
    region(x, y + 10.0f, kMidiW, 24.0f, [this, x, y](const Press&) { openLooperSwitchMenu(x, y + 36.0f); });
    x += kMidiW + 6.0f;

    // Remove: the red X at the right, without asking (what it recorded isn't saved).
    const float rx = std::max(x + 8.0f, width() - kPad - kRemoveSize), ry = y + (kSeqRowH - kRemoveSize) / 2;
    removeButton(rx, ry, kRemoveSize);
    region(rx, ry, kRemoveSize, kRemoveSize, [this, id = l.id](const Press&) { controller.removeLooper(id); });
}

// + Looper: what it records (an input), then a row it plays into (more from its row's second line).
void RackWidget::newLooper(float x, float y) {
    controller.refreshSources();
    Menu m;
    m.x     = x;
    m.y     = y;
    m.w     = 260.0f;
    m.title = "New looper: records from";
    // Then its name, typed in its row (Enter keeps "Loop N"), as for a new sequence.
    auto named = [this](const std::string& id) {
        if (const LooperSetup* looper = controller.looper(id)) startLooperRename(id, looper->name);
    };
    auto then = [this, x, y, named](MidiSourceRef input) {
        Menu rows;
        rows.x     = x;
        rows.y     = y;
        rows.w     = 260.0f;
        rows.title = "and plays into";
        for (const RowModel& row : controller.view().rows)
            rows.items.push_back(
                MenuItem{row.name, [this, named, input, id = row.id] { named(controller.addLooper(input, {id})); }});
        rows.items.push_back(MenuItem{"No row yet", [this, named, input] { named(controller.addLooper(input, {})); }});
        openMenu(std::move(rows));
    };
    m.items.push_back(MenuItem{"All inputs", [then] { then(MidiSourceRef{}); }});
    for (const SourceChoice& choice : controller.view().sources)
        if (choice.ref.kind == MidiSourceRef::Kind::Device || choice.ref.kind == MidiSourceRef::Kind::Virtual)
            m.items.push_back(MenuItem{choice.label, [then, ref = choice.ref] { then(ref); }});
    openMenu(std::move(m));
}

void RackWidget::startLooperRename(const std::string& id, const std::string& name) {
    renamingId = "\x01loop:" + id;
    editText(kPad + 38.0f, 0.0f, 230.0f, 24.0f, name, RackController::kMaxNameBytes,   // placed when the row draws
             [this, id](const std::string& text) {
                 renamingId.clear();
                 controller.renameLooper(id, text);
             });
}

void RackWidget::openLooperTargetsMenu(const std::string& id, float x, float y) {
    const LooperRowModel* looper = nullptr;
    for (const LooperRowModel& l : controller.view().loopers)
        if (l.id == id) looper = &l;
    if (looper == nullptr) return;
    Menu m;
    m.x     = x;
    m.y     = y;
    m.w     = 260.0f;
    m.title = looper->name + " plays into";
    for (const RowModel& row : controller.view().rows) {
        const bool on = std::find(looper->targetIds.begin(), looper->targetIds.end(), row.id) != looper->targetIds.end();
        m.items.push_back(MenuItem{row.name, [this, id, row = row.id, on] { controller.setLooperTarget(id, row, !on); }, on});
    }
    if (controller.view().rows.empty()) m.items.push_back(MenuItem{"No rows yet", {}});
    openMenu(std::move(m));
}

// The looper switch: one momentary footswitch for the current looper. Tap:
// record, play, stop; hold while playing: overdub.
void RackWidget::openLooperSwitchMenu(float x, float y) {
    const RackViewModel& view = controller.view();
    Menu                 m;
    m.x     = x;
    m.y     = y;
    m.w     = 380.0f;
    m.title = view.looperSwitchLearning ? "Press the footswitch..." : "Looper switch: drives the current looper (all songs)";
    if (view.looperSwitchLearning) m.items.push_back(MenuItem{"Stop learning", [this] { controller.cancelLearn(); }});
    else m.items.push_back(MenuItem{"Learn (a momentary switch, for hold)", [this] { controller.learnLooperSwitch(); }});
    if (!view.looperSwitch.empty())
        m.items.push_back(MenuItem{"Remove " + view.looperSwitch + (view.looperSwitchConnected ? "" : " (not connected)"),
                                   [this] { controller.removeLooperSwitch(); }, false, true});
    m.items.push_back(MenuItem{"Tap: record, play, stop.  Hold while playing: overdub.", {}});
    openMenu(std::move(m));
}

void RackWidget::confirmRemoveSequence(const std::string& id, const std::string& name, float right, float y) {
    Menu m;
    m.w     = 230.0f;
    m.x     = std::max(8.0f, right - m.w);
    m.y     = y;
    m.title = "Remove " + name + "?";
    m.items.push_back(MenuItem{"Remove", [this, id] { controller.removeSequence(id); }, false, true});
    m.items.push_back(MenuItem{"Cancel", [] {}});
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
    m.w     = 260.0f;
    m.title = controller.rackPath().empty() ? "Untitled rack" : controller.rackPath();
    // In a setlist, a rack is a song in it: New and Open add to the set rather
    // than leave it. Only Close setlist leaves it.
    const bool inSet = controller.hasSetlist();
    m.items.push_back(MenuItem{inSet ? "New rack in this set...     Ctrl-N" : "New rack          Ctrl-N",
                               [this] { fileAction(FileAction::New); }});
    m.items.push_back(MenuItem{inSet ? "Add a rack to this set...  Ctrl-O" : "Open...           Ctrl-O",
                               [this] { fileAction(FileAction::Open); }});
    // The one step out of the set besides Close setlist: an untitled rack on its own.
    if (inSet)
        m.items.push_back(MenuItem{"New rack outside the set", [this] {
                                       if (controller.unsavedUntitled())
                                           keepChangesThen([this] { controller.newRack(); }, false);
                                       else
                                           controller.newRack();   // closes the setlist
                                   }});
    m.items.push_back(MenuItem{"Save              Ctrl-S",
                               savingOff ? std::function<void()>{} : [this] { fileAction(FileAction::Save); }});
    m.items.push_back(MenuItem{"Save As...",
                               savingOff ? std::function<void()>{} : [this] { fileAction(FileAction::SaveAs); }});
    // Setlists (docs/setlists.md, section 1).
    m.items.push_back(MenuItem{"New setlist with this rack...", [this] { newSetlist(true); }});
    m.items.push_back(MenuItem{"New setlist...", [this] { newSetlist(false); }});
    m.items.push_back(MenuItem{"Open setlist...", [this] {
                                   if (chooseFile)
                                       chooseFile(Choose::Setlist, "Open a setlist", setlistFolder(), "",
                                                  [this](const std::string& path) {
                                                      switchSong([this, path] { controller.openSetlist(path); });
                                                  });
                               }});
    if (inSet) {
        m.items.push_back(MenuItem{controller.checkingSetlist() ? "Check setlist (checking...)" : "Check setlist",
                                   [this] { controller.checkSetlist(); }});
        m.items.push_back(MenuItem{"Setlist settings...", [this, x, y] { openSwitchMenu(x, y); }});
        m.items.push_back(MenuItem{"Close setlist", [this] { controller.closeSetlist(); }});
    }
    openMenu(std::move(m));
}

// How a song switch behaves (docs/setlists.md, section 5): one choice ticked
// in each group; a click changes it and the menu stays open on the new state.
void RackWidget::openSwitchMenu(float x, float y) {
    const SwitchSettings s = controller.view().setlist.switching;
    Menu                 m;
    m.x     = x;
    m.y     = y;
    m.w     = 330.0f;
    m.title = "Switching songs (this setlist)";
    auto choice = [&](const char* text, bool checked, SwitchSettings changed) {
        m.items.push_back(MenuItem{std::string("   ") + text, [this, changed, x, y] {
                                       controller.setSwitchSettings(changed);
                                       openSwitchMenu(x, y);
                                   },
                                   checked});
    };
    using S = SwitchSettings;
    S next = s;
    m.items.push_back(MenuItem{"The transport", {}});
    next.transport = S::Transport::Keep;
    choice("keeps running (medleys)", s.transport == S::Transport::Keep, next);
    next.transport = S::Transport::Stop;
    choice("stops: the song starts from bar 1", s.transport == S::Transport::Stop, next);
    next = s;
    m.items.push_back(MenuItem{"A switch happens", {}});
    next.when = S::When::Now;
    choice("at once", s.when == S::When::Now, next);
    next.when = S::When::NextBar;
    choice("at the next bar (while it runs)", s.when == S::When::NextBar, next);
    next = s;
    m.items.push_back(MenuItem{"The old song", {}});
    next.oldRack = S::OldRack::RingOut;
    choice("rings out", s.oldRack == S::OldRack::RingOut, next);
    next.oldRack = S::OldRack::Cut;
    choice("is cut (its notes end, faded)", s.oldRack == S::OldRack::Cut, next);
    openMenu(std::move(m));
}

// New, Open and Quit would lose an untitled rack's changes: ask first. In a
// setlist, New and Open add a song instead (docs/setlists.md, section 1).
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
    if (controller.hasSetlist() && action == FileAction::New) {   // a new song, after the current one
        if (chooseFile)
            chooseFile(Choose::Save,
                       controller.view().setlist.current >= 0 ? "New rack in this set (a copy of this song's)"
                                                              : "New rack in this set (empty)",
                       setRackFolder(), "New song.rack",
                       [this](const std::string& path) {
                           switchSong([this, path] { controller.addNewRackToSetlist(path); });
                       });
        return;
    }
    if (controller.hasSetlist() && action == FileAction::Open) {   // added after the current song; no switch
        if (chooseFile)
            chooseFile(Choose::Rack, "Add a rack to this set (choose its .rack folder)", setRackFolder(), "",
                       [this](const std::string& path) { controller.addRackToSetlist(path); });
        return;
    }
    if (!controller.unsavedUntitled()) {
        proceed(action);
        return;
    }
    keepChangesThen([this, action] { proceed(action); }, action == FileAction::Quit);
}

void RackWidget::keepChangesThen(std::function<void()> then, bool quitting) {
    Menu m;
    m.x     = width() / 2 - 150.0f;
    m.y     = topH() + 20.0f;
    m.w     = 300.0f;
    m.title = "Keep the changes to " + controller.view().rackName + "?";
    m.items.push_back(MenuItem{"Save As...", [this, then] { saveAsThen(then); }});
    m.items.push_back(MenuItem{quitting ? "Quit without saving" : "Discard the changes", then, false, true});
    m.items.push_back(MenuItem{"Cancel", [] {}});
    openMenu(std::move(m));
}

void RackWidget::proceed(FileAction action) {
    switch (action) {
        case FileAction::New: controller.newRack(); break;
        case FileAction::Open:
            if (chooseFile)
                chooseFile(Choose::Rack, "Open a rack (choose its .rack folder)", startFolder(), "",
                           [this](const std::string& path) { controller.openRack(path); });
            break;
        case FileAction::Quit: controller.quit(); break;
        case FileAction::Save:
        case FileAction::SaveAs: break;   // handled by fileAction()
    }
}

// Save As, then `then` once it's saved. An existing rack is replaced only
// after asking.
void RackWidget::saveAsThen(std::function<void()> then, const std::string& title) {
    if (!chooseFile) return;
    const std::string name = (controller.view().rackName.empty() ? std::string("Untitled") : controller.view().rackName) + ".rack";
    chooseFile(Choose::Save, title, startFolder(), name, [this, then](const std::string& path) {
        const RackController::SaveAsResult result = controller.saveAs(path);
        if (result == RackController::SaveAsResult::Saved) {
            if (then) then();
            return;
        }
        if (result != RackController::SaveAsResult::NeedsReplace) return;
        Menu m;
        m.x     = width() / 2 - 170.0f;
        m.y     = topH() + 20.0f;
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

// Type a sequence's name over its row's name (Enter keeps it, Escape doesn't).
void RackWidget::startRename(const std::string& id, const std::string& name) {
    renamingId = id;
    editText(kPad + 38.0f, 0.0f, 212.0f, 24.0f, name, RackController::kMaxNameBytes,   // placed when the row draws
             [this, id](const std::string& text) {
                 renamingId.clear();
                 controller.renameSequence(id, text);
             });
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
        // A virtual input's label already says its protocol.
        if (choice.protocol != SourceProtocol::Mixed && choice.ref.kind != MidiSourceRef::Kind::Virtual)
            text += std::string("   (") + protocolText(choice.protocol) + ")";
        m.items.push_back(MenuItem{text, [this, id = row.id, ref = choice.ref] { controller.setSource(id, ref); },
                                   sameSource(choice.ref, row.source), false});
    }
    openMenu(std::move(m));
}

void RackWidget::confirmRemoveRow(const std::string& id, const std::string& name, float right, float y) {
    Menu m;
    m.w     = 220.0f;
    m.x     = std::max(8.0f, right - m.w);
    m.y     = y;
    m.title = "Remove " + name + "?";
    m.items.push_back(MenuItem{"Remove", [this, id] {
                                   if (filterId == id) filterId.clear();
                                   controller.removeSlot(id);
                               },
                               false, true});
    m.items.push_back(MenuItem{"Cancel", [] {}});
    openMenu(std::move(m));
}

// Which backend output pair the row plays into.
void RackWidget::openOutputMenu(const RowModel& row, float x, float y) {
    Menu m;
    m.w     = 180.0f;
    m.x     = std::min(x, width() - m.w - 8.0f);
    m.y     = y;
    m.title = row.name + " plays into";
    const std::uint32_t outputs = controller.view().status.outputs;
    for (std::uint32_t o = 0; o + 1 < outputs; o += 2)
        m.items.push_back(MenuItem{"Output " + std::to_string(o + 1) + "-" + std::to_string(o + 2),
                                   [this, id = row.id, o] { controller.setFirstOutput(id, o); }, row.firstOutput == o});
    openMenu(std::move(m));
}

void RackWidget::openAddMenu(float x, float y) {
    openPluginMenu(x, y, "Add a plugin (added at the bottom)", [this](const PluginDescriptor& plugin) {
        controller.addPlugin(plugin, controller.view().rows.size());
        scrollY = 1e9f;   // show the new row (clamped when drawn)
    });
}

void RackWidget::openPluginMenu(float x, float y, const std::string& title,
                                std::function<void(const PluginDescriptor&)> chosen, const std::string& none) {
    Menu m;
    m.x     = x;
    m.y     = y;
    m.w     = 360.0f;
    m.title = title;
    if (!none.empty()) m.items.push_back(MenuItem{none, [chosen] { chosen(PluginDescriptor{}); }, false, false});

    // Instruments first, then other plugins (note effects), each by name. Audio
    // effects are listed after them, greyed: a row gives its plugin no audio,
    // so they wait for the rig's buses (docs/rig.md).
    std::vector<PluginDescriptor> plugins = controller.availablePlugins();   // scans once; may take a moment
    std::stable_sort(plugins.begin(), plugins.end(), [](const PluginDescriptor& a, const PluginDescriptor& b) {
        return a.name < b.name;
    });
    auto text = [](const PluginDescriptor& p) { return p.name + (p.vendor.empty() ? "" : "  -  " + p.vendor); };
    auto section = [&](PluginKind kind, const std::string& heading, bool usable) {
        bool headed = false;
        for (const PluginDescriptor& plugin : plugins) {
            if (plugin.kind != kind) continue;
            if (!headed) m.items.push_back(MenuItem{heading, {}, false, false});
            headed = true;
            m.items.push_back(MenuItem{"   " + text(plugin),
                                       usable ? std::function<void()>{[chosen, plugin] { chosen(plugin); }}
                                              : std::function<void()>{},
                                       false, false});
        }
    };
    section(PluginKind::Instrument, "Instruments", true);
    section(PluginKind::Other, "Other (note effects)", true);
    section(PluginKind::Effect, "Effects (for the rig's buses, below the rows)", false);
    if (plugins.empty()) m.items.push_back(MenuItem{"No CLAP plugins found", {}, false, false});
    openMenu(std::move(m));
}

void RackWidget::openTargetsMenu(const std::string& seqId, float x, float y) {
    const Sequence* seq = controller.sequence(seqId);
    if (seq == nullptr) return;
    Menu m;
    m.x     = x;
    m.y     = y;
    m.w     = 300.0f;
    m.title = seq->name + " plays into";
    for (const RowModel& row : controller.view().rows) {   // ticked: playing into it; a click toggles
        const bool on = std::find(seq->targets.begin(), seq->targets.end(), row.id) != seq->targets.end();
        m.items.push_back(MenuItem{row.name, [this, seqId, rowId = row.id, on] {
                                       controller.setSequenceTarget(seqId, rowId, !on);
                                   },
                                   on, false});
    }
    m.items.push_back(MenuItem{"New row...", [this, seqId, x, y] {
                                   openPluginMenu(x, y, "New row for this sequence", [this, seqId](const PluginDescriptor& plugin) {
                                       if (!controller.addRowForSequence(seqId, plugin).empty()) scrollY = 1e9f;
                                   });
                               },
                               false, false});
    openMenu(std::move(m));
}

void RackWidget::newSequence(MidiProtocol protocol, float x, float y) {
    // Create it playing into `row` ("" = nowhere yet), or into a new row for
    // `plugin`; then show it and name it straight away (Enter keeps "Sequence N").
    auto create = [this, protocol](const std::string& row, const PluginDescriptor* plugin) {
        const std::string id = controller.addSequence(protocol, row);
        if (plugin != nullptr && !plugin->id.empty() && !controller.addRowForSequence(id, *plugin).empty())
            scrollY = 1e9f;   // show the new row too
        dockScroll = 1e9f;    // show the new sequence (clamped when drawn)
        if (const Sequence* seq = controller.sequence(id)) startRename(id, seq->name);
    };
    auto choosePlugin = [this, x, y, create](const std::string& title) {
        openPluginMenu(x, y, title, [create](const PluginDescriptor& plugin) { create({}, &plugin); },
                       "No instrument for now");
    };
    if (controller.view().rows.empty()) {   // nothing to play into: choose an instrument first
        choosePlugin("New sequence: choose an instrument for it");
        return;
    }
    Menu m;
    m.x     = x;
    m.y     = y;
    m.w     = 300.0f;
    m.title = "New sequence plays into";
    for (const RowModel& row : controller.view().rows)
        m.items.push_back(MenuItem{row.name, [create, rowId = row.id] { create(rowId, nullptr); }, false, false});
    m.items.push_back(MenuItem{"New row...", [choosePlugin] { choosePlugin("New sequence: choose an instrument for it"); },
                               false, false});
    m.items.push_back(MenuItem{"No row for now", [create] { create({}, nullptr); }, false, false});
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
    y             = std::clamp(y, topH() + 4.0f, std::max(topH() + 4.0f, height() - kStatusH - h - 4.0f));
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
    if (UiWidget::onKeyboard(ev)) return true;   // a text field has the keys
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
