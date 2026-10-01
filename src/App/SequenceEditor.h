#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "Core/Sequence.h"

namespace stagehand {

class RackController;

// The editing logic behind one sequence's editor window (docs/sequences.md,
// section 5): the notes being edited, the selection, gestures, and undo. No
// DGL: the window turns mouse and keys into these calls, and tests call them
// directly. Main thread.
//
// Notes are the editor's; every other setting is read from the Session each
// time, so the dock and the editor never overwrite each other. Every change is
// pushed to the controller at once (and so heard at once, if it plays).
//
// A gesture (a drag) is computed from the state at its start on every move,
// so nothing accumulates, and becomes one undo step when it ends. Calls made
// outside a gesture are one step each.
//
// Same-pitch overlaps are never left: after any change, the earlier of two
// overlapping notes on one pitch is trimmed to end where the later starts; two
// notes starting together keep the one being edited.
class SequenceEditor {
public:
    SequenceEditor(RackController& controller, std::string sequenceId);

    const std::string& id() const noexcept { return sequenceId; }
    // The sequence as the Session has it (settings current); null once removed.
    const Sequence* sequence() const;
    // Adopt notes changed elsewhere (a rack load) when not mid-gesture; the
    // selection is cleared then. Call each tick.
    void sync();

    struct Item {
        SequenceNote note;
        bool         selected = false;
    };
    const std::vector<Item>& items() const noexcept { return notes; }
    int                      noteAt(std::uint32_t unit, int pitch) const;   // index, or -1
    std::size_t              selectedCount() const;

    // The pitch rows: MIDI 1.0 note numbers, or the EDO steps that sound
    // within MIDI's range.
    int    lowestPitch() const;
    int    highestPitch() const;
    double semitonesOf(int pitch) const;
    bool   isBlackKey(int pitch) const;   // rounds to C#, D#, F#, G# or A#
    int    stepsPerOctave() const;        // 12, or the EDO

    // --- gestures
    void beginGesture();
    void endGesture();
    bool inGesture() const noexcept { return gesture; }

    // Add a note (the last length used, the default velocity) and select only
    // it, and sound it. False if (start, pitch) is off the grid.
    bool addNote(std::uint32_t start, int pitch);
    void stretchNew(std::uint32_t lastUnit);                       // a new note's end follows the drag
    // Relative to the gesture's start. The moved notes sound each time the
    // move's pitch changes (a move in time alone is silent).
    void moveSelection(int units, int pitches, bool copy);
    void resize(int index, std::uint32_t length);                  // `index` at the gesture's start
    void changeVelocity(double fractionOfRange);                   // relative to the gesture's start

    // Sound one note (a click on it), as an added or moved note sounds; `hold`:
    // until stopSounding() (a Ctrl-click, held while velocity is dragged).
    void soundNote(int index, bool hold = false);
    // End what the editor is sounding (the mouse released), then send a
    // note-off for every key and All Notes Off on the sequence's channel into
    // its rows, so nothing is left stuck.
    void stopSounding();

    // --- one-step edits
    void select(int index, bool add);
    void toggle(int index);
    void selectRect(std::uint32_t firstUnit, std::uint32_t lastUnit, int lowPitch, int highPitch, bool add);
    void selectAll();
    void clearSelection();
    void deleteSelection();
    void deleteNote(int index);
    void nudge(int units, int pitches);   // the arrow keys

    bool undo();
    bool redo();
    bool canUndo() const noexcept { return !undoStack.empty(); }
    bool canRedo() const noexcept { return !redoStack.empty(); }

    std::uint32_t newNoteLength() const noexcept { return lastLength; }
    void          setNewNoteLength(std::uint32_t length) { lastLength = std::max<std::uint32_t>(1, length); }
    std::uint16_t defaultVelocity() const;

private:
    struct Working {
        SequenceNote note;
        bool         selected = false;
        bool         edited   = false;   // wins a same-start collision
    };
    static std::vector<SequenceNote> notesOf(const std::vector<Item>& items);
    void apply(std::vector<Working> next);   // resolve overlaps, sort, push
    std::vector<Working> fromBase() const;   // the gesture's start, as working items
    std::uint16_t        maxVelocity() const;

    RackController& controller;
    std::string     sequenceId;

    std::vector<Item>                      notes;       // sorted by (start, pitch)
    std::vector<Item>                      base;        // at the gesture's start
    bool                                   gesture   = false;
    std::optional<SequenceNote>            added;       // a note this gesture is drawing
    std::uint32_t                          lastLength = 1;
    int                                    soundedPitches = 0;   // the move's pitch offset last sounded
    std::vector<std::vector<SequenceNote>> undoStack, redoStack;
};

} // namespace stagehand
