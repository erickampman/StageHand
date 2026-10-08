#include "App/SequenceEditor.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "App/RackController.h"
#include "uw/ump/Ump.h"
#include "Core/UwNames.h"

namespace stagehand {

namespace {

constexpr std::size_t kMaxUndo = 200;

bool byStartThenPitch(const SequenceNote& a, const SequenceNote& b) {
    return a.start != b.start ? a.start < b.start : a.pitch < b.pitch;
}

} // namespace

SequenceEditor::SequenceEditor(RackController& rackController, std::string id)
    : controller(rackController), sequenceId(std::move(id)) {
    sync();
}

const Sequence* SequenceEditor::sequence() const {
    return controller.sequence(sequenceId);
}

void SequenceEditor::sync() {
    const Sequence* seq = sequence();
    if (seq == nullptr || gesture || seq->notes == notesOf(notes)) return;
    notes.clear();
    for (const SequenceNote& note : seq->notes) notes.push_back(Item{note, false});
    std::sort(notes.begin(), notes.end(), [](const Item& a, const Item& b) { return byStartThenPitch(a.note, b.note); });
}

std::vector<SequenceNote> SequenceEditor::notesOf(const std::vector<Item>& items) {
    std::vector<SequenceNote> out;
    out.reserve(items.size());
    for (const Item& item : items) out.push_back(item.note);
    return out;
}

int SequenceEditor::noteAt(std::uint32_t unit, int pitch) const {
    for (std::size_t i = 0; i < notes.size(); ++i) {
        const SequenceNote& n = notes[i].note;
        if (n.pitch == pitch && unit >= n.start && unit < n.start + n.length) return static_cast<int>(i);
    }
    return -1;
}

std::size_t SequenceEditor::selectedCount() const {
    return static_cast<std::size_t>(std::count_if(notes.begin(), notes.end(), [](const Item& i) { return i.selected; }));
}

// --- pitch rows --------------------------------------------------------------------

int SequenceEditor::stepsPerOctave() const {
    const Sequence* seq = sequence();
    return seq == nullptr || seq->protocol == MidiProtocol::Midi1 ? 12 : std::max(1, seq->edo);
}

double SequenceEditor::semitonesOf(int pitch) const {
    const Sequence* seq = sequence();
    if (seq == nullptr || seq->protocol == MidiProtocol::Midi1) return pitch;
    return seq->referenceNote + pitch * 12.0 / stepsPerOctave();
}

int SequenceEditor::lowestPitch() const {
    const Sequence* seq = sequence();
    if (seq == nullptr || seq->protocol == MidiProtocol::Midi1) return 0;
    return static_cast<int>(std::ceil(-seq->referenceNote * stepsPerOctave() / 12.0 - 1e-9));
}

int SequenceEditor::highestPitch() const {
    const Sequence* seq = sequence();
    if (seq == nullptr || seq->protocol == MidiProtocol::Midi1) return 127;
    return static_cast<int>(std::floor((127.999 - seq->referenceNote) * stepsPerOctave() / 12.0));
}

bool SequenceEditor::isBlackKey(int pitch) const {
    const int semitone = static_cast<int>(std::lround(semitonesOf(pitch))) % 12;
    return semitone == 1 || semitone == 3 || semitone == 6 || semitone == 8 || semitone == 10;
}

std::uint16_t SequenceEditor::maxVelocity() const {
    const Sequence* seq = sequence();
    return seq != nullptr && seq->protocol == MidiProtocol::Midi1 ? 127 : 65535;
}

std::uint16_t SequenceEditor::defaultVelocity() const {
    return maxVelocity() == 127 ? 100 : static_cast<std::uint16_t>(ump::scaleUp(100, 7, 16));
}

// --- applying a change ---------------------------------------------------------------

// The gesture's starting state (or the current one), plus a note being drawn,
// which is then the only one selected.
std::vector<SequenceEditor::Working> SequenceEditor::fromBase() const {
    std::vector<Working> out;
    for (const Item& item : (gesture ? base : notes))
        out.push_back(Working{item.note, item.selected && !added, false});
    if (added) out.push_back(Working{*added, true, true});
    return out;
}

// Resolve same-pitch overlaps, sort, keep the selection with its notes, and
// hand the notes to the controller.
void SequenceEditor::apply(std::vector<Working> next) {
    std::stable_sort(next.begin(), next.end(), [](const Working& a, const Working& b) {
        if (a.note.pitch != b.note.pitch) return a.note.pitch < b.note.pitch;
        if (a.note.start != b.note.start) return a.note.start < b.note.start;
        return a.edited && !b.edited;   // at one start, the edited note first: it's the one kept
    });
    std::vector<Working> kept;
    for (Working& w : next) {
        if (!kept.empty() && kept.back().note.pitch == w.note.pitch) {
            Working& earlier = kept.back();
            if (earlier.note.start == w.note.start) continue;   // same start: the first (edited) stays
            if (earlier.note.start + earlier.note.length > w.note.start)
                earlier.note.length = w.note.start - earlier.note.start;   // the earlier is trimmed
        }
        kept.push_back(w);
    }
    std::sort(kept.begin(), kept.end(),
              [](const Working& a, const Working& b) { return byStartThenPitch(a.note, b.note); });
    notes.clear();
    for (const Working& w : kept) notes.push_back(Item{w.note, w.selected});
    controller.setSequenceNotes(sequenceId, notesOf(notes));
}

void SequenceEditor::beginGesture() {
    if (gesture) return;
    gesture        = true;
    base           = notes;
    soundedPitches = 0;
    added.reset();
}

void SequenceEditor::endGesture() {
    if (!gesture) return;
    gesture = false;
    added.reset();
    if (notesOf(base) == notesOf(notes)) return;
    undoStack.push_back(notesOf(base));
    if (undoStack.size() > kMaxUndo) undoStack.erase(undoStack.begin());
    redoStack.clear();
}

// --- gestures ------------------------------------------------------------------------

bool SequenceEditor::addNote(std::uint32_t start, int pitch) {
    const Sequence* seq = sequence();
    if (seq == nullptr || start >= seq->length || pitch < lowestPitch() || pitch > highestPitch()) return false;
    const bool oneShot = !gesture;
    beginGesture();
    SequenceNote note;
    note.start    = start;
    note.length   = std::min(lastLength, seq->length - start);
    note.pitch    = pitch;
    note.velocity = defaultVelocity();
    added         = note;
    apply(fromBase());
    controller.auditionNotes(sequenceId, {note});
    if (oneShot) endGesture();
    return true;
}

void SequenceEditor::stretchNew(std::uint32_t lastUnit) {
    const Sequence* seq = sequence();
    if (!gesture || !added || seq == nullptr) return;
    const std::uint32_t end = std::clamp<std::uint32_t>(lastUnit + 1, added->start + 1, seq->length);
    added->length           = end - added->start;
    lastLength              = added->length;
    apply(fromBase());
}

void SequenceEditor::moveSelection(int units, int pitches, bool copy) {
    const Sequence* seq = sequence();
    if (!gesture || seq == nullptr) return;
    // Clamp the move so every selected note stays inside the grid.
    std::int64_t lowUnit = INT64_MIN, highUnit = INT64_MAX, lowPitch = INT64_MIN, highPitch = INT64_MAX;
    for (const Item& item : base) {
        if (!item.selected) continue;
        lowUnit   = std::max<std::int64_t>(lowUnit, -static_cast<std::int64_t>(item.note.start));
        highUnit  = std::min<std::int64_t>(highUnit, static_cast<std::int64_t>(seq->length) - item.note.start - item.note.length);
        lowPitch  = std::max<std::int64_t>(lowPitch, lowestPitch() - item.note.pitch);
        highPitch = std::min<std::int64_t>(highPitch, highestPitch() - item.note.pitch);
    }
    const std::int64_t du = std::clamp<std::int64_t>(units, lowUnit, std::max(lowUnit, highUnit));
    const auto         dp = static_cast<int>(std::clamp<std::int64_t>(pitches, lowPitch, std::max(lowPitch, highPitch)));

    std::vector<Working> next;
    for (const Item& item : base) {
        if (!item.selected) {
            next.push_back(Working{item.note, false, false});
            continue;
        }
        if (copy) next.push_back(Working{item.note, false, false});   // the original stays, unselected
        SequenceNote moved = item.note;
        moved.start        = static_cast<std::uint32_t>(static_cast<std::int64_t>(moved.start) + du);
        moved.pitch += dp;
        next.push_back(Working{moved, true, true});
    }
    apply(std::move(next));

    if (dp == soundedPitches) return;
    soundedPitches = dp;
    std::vector<SequenceNote> moved;
    for (const Item& item : notes)
        if (item.selected) moved.push_back(item.note);
    controller.auditionNotes(sequenceId, moved);
}

void SequenceEditor::resize(int index, std::uint32_t length) {
    const Sequence* seq = sequence();
    if (!gesture || seq == nullptr || index < 0 || static_cast<std::size_t>(index) >= base.size()) return;
    std::vector<Working> next = fromBase();
    Working&             note = next[static_cast<std::size_t>(index)];
    note.note.length          = std::clamp<std::uint32_t>(length, 1, seq->length - note.note.start);
    note.edited               = true;
    lastLength                = note.note.length;
    apply(std::move(next));
}

void SequenceEditor::changeVelocity(double fraction) {
    if (!gesture) return;
    const double         top  = maxVelocity();
    std::vector<Working> next = fromBase();
    for (Working& w : next)
        if (w.selected)
            w.note.velocity = static_cast<std::uint16_t>(std::clamp(std::lround(w.note.velocity + fraction * top), 1L,
                                                                    static_cast<long>(top)));
    apply(std::move(next));
}

void SequenceEditor::soundNote(int index, bool hold) {
    if (index >= 0 && static_cast<std::size_t>(index) < notes.size())
        controller.auditionNotes(sequenceId, {notes[static_cast<std::size_t>(index)].note}, hold);
}

void SequenceEditor::stopSounding() {
    controller.endAuditions(sequenceId, true);
}

// --- one-step edits --------------------------------------------------------------------

void SequenceEditor::select(int index, bool add) {
    for (std::size_t i = 0; i < notes.size(); ++i)
        if (static_cast<int>(i) == index)
            notes[i].selected = true;
        else if (!add)
            notes[i].selected = false;
}

void SequenceEditor::toggle(int index) {
    if (index >= 0 && static_cast<std::size_t>(index) < notes.size())
        notes[static_cast<std::size_t>(index)].selected = !notes[static_cast<std::size_t>(index)].selected;
}

void SequenceEditor::selectRect(std::uint32_t firstUnit, std::uint32_t lastUnit, int lowPitch, int highPitch, bool add) {
    for (Item& item : notes) {
        const bool inside = item.note.pitch >= lowPitch && item.note.pitch <= highPitch &&
                            item.note.start <= lastUnit && item.note.start + item.note.length > firstUnit;
        item.selected = inside || (add && item.selected);
    }
}

void SequenceEditor::selectAll() {
    for (Item& item : notes) item.selected = true;
}

void SequenceEditor::clearSelection() {
    for (Item& item : notes) item.selected = false;
}

void SequenceEditor::deleteSelection() {
    if (selectedCount() == 0) return;
    beginGesture();
    std::vector<Working> next;
    for (const Item& item : base)
        if (!item.selected) next.push_back(Working{item.note, false, false});
    apply(std::move(next));
    endGesture();
}

void SequenceEditor::deleteNote(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= notes.size()) return;
    beginGesture();
    std::vector<Working> next = fromBase();
    next.erase(next.begin() + index);
    apply(std::move(next));
    endGesture();
}

void SequenceEditor::nudge(int units, int pitches) {
    if (selectedCount() == 0) return;
    beginGesture();
    moveSelection(units, pitches, false);
    endGesture();
}

bool SequenceEditor::undo() {
    if (undoStack.empty() || gesture) return false;
    redoStack.push_back(notesOf(notes));
    std::vector<Working> next;
    for (const SequenceNote& note : undoStack.back()) next.push_back(Working{note, false, false});
    undoStack.pop_back();
    apply(std::move(next));
    return true;
}

bool SequenceEditor::redo() {
    if (redoStack.empty() || gesture) return false;
    undoStack.push_back(notesOf(notes));
    std::vector<Working> next;
    for (const SequenceNote& note : redoStack.back()) next.push_back(Working{note, false, false});
    redoStack.pop_back();
    apply(std::move(next));
    return true;
}

} // namespace stagehand
