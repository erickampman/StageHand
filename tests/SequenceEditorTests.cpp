// SequenceEditor tests: drawing and stretching notes, moving (clamped) and
// copying, same-pitch trimming, resizing, velocity, selection, the arrow keys,
// undo/redo, EDO pitch rows and black keys, and adopting outside changes. The
// editor runs against a real Session and RackController, with fakes below.

#include <memory>
#include <string>
#include <vector>

#include "App/RackController.h"
#include "App/SequenceEditor.h"
#include "Check.h"
#include "Core/Ump.h"
#include "FakePlugin.h"
#include "SessionFakes.h"

using namespace stagehand;
using namespace stagehand::test;

namespace {

struct Rig {
    FakeFactory                     factory;
    FakeMidiInput                   midi;
    NullHost                        host;
    std::unique_ptr<Engine>         engine = std::make_unique<Engine>();
    std::unique_ptr<Session>        session;
    std::unique_ptr<RackController> controller;

    // `row`: a row "a" (a fake synth) that the sequence plays into.
    explicit Rig(MidiProtocol protocol = MidiProtocol::Midi1, int edo = 12, bool row = false) {
        session = std::make_unique<Session>(factory, host, midi, *engine);
        Rack     rack;
        Sequence seq;
        seq.id       = "s";
        seq.protocol = protocol;
        seq.edo      = edo;
        seq.length   = 16;
        if (row) {
            factory.installed = {installedPlugin("synth", "/clap/Synth.clap")};
            rack.slots        = {slot("a", "synth", "/clap/Synth.clap")};
            seq.targets       = {"a"};
        }
        rack.sequences = {seq};
        Session::LoadReport report;
        session->load(rack, 48000.0, report);
        controller = std::make_unique<RackController>(*session, *engine, midi, factory, RackController::Config{});
    }
    ~Rig() {
        controller.reset();
        session->unload();
        runBlock(*engine);
        engine->collect();
    }
    const std::vector<SequenceNote>& stored() const { return session->sequence("s")->sequence.notes; }
    // The note numbers row "a" was sent note-ons for in one block (note-offs
    // in `ended`, and how many All Notes Off in `allOff`).
    std::vector<int> ended;
    int              allOff = 0;
    std::vector<int> sounded() {
        ended.clear();
        allOff = 0;
        FakePlugin* plugin = factory.created["synth"];
        plugin->calls.clear();
        runBlock(*engine);
        std::vector<int> notes;
        for (const FakePlugin::Call& call : plugin->calls)
            for (const Event& e : call.events)
                if (ump::isNoteOn(e.midi))
                    notes.push_back(ump::noteNumber(e.midi));
                else if (ump::isNoteOff(e.midi))
                    ended.push_back(ump::noteNumber(e.midi));
                else if ((e.midi.words[0] & 0x00F0FF00u) == 0x00B07B00u)   // CC 123, any channel
                    ++allOff;
        return notes;
    }
};

bool has(const std::vector<SequenceNote>& notes, std::uint32_t start, std::uint32_t length, int pitch) {
    for (const SequenceNote& n : notes)
        if (n.start == start && n.length == length && n.pitch == pitch) return true;
    return false;
}

void testDrawAndUndo() {
    Rig            rig;
    SequenceEditor ed(*rig.controller, "s");
    CHECK(ed.addNote(0, 60));
    CHECK(rig.stored().size() == 1 && has(rig.stored(), 0, 1, 60));
    CHECK(rig.stored()[0].velocity == 100 && ed.selectedCount() == 1);
    CHECK(!ed.addNote(16, 60) && !ed.addNote(0, 128));   // off the grid

    // A drawn note's length follows the drag, and becomes the next note's length.
    ed.beginGesture();
    CHECK(ed.addNote(2, 62));
    ed.stretchNew(5);
    ed.stretchNew(40);   // clamped to the sequence's end
    ed.stretchNew(4);
    ed.endGesture();
    CHECK(has(rig.stored(), 2, 3, 62) && ed.newNoteLength() == 3);
    CHECK(ed.selectedCount() == 1 && ed.items()[1].selected);   // only the new one
    ed.addNote(8, 64);
    CHECK(has(rig.stored(), 8, 3, 64));

    // Three steps to undo, and back.
    CHECK(ed.undo() && rig.stored().size() == 2);
    CHECK(ed.undo() && ed.undo() && rig.stored().empty() && !ed.canUndo());
    CHECK(ed.redo() && ed.redo() && rig.stored().size() == 2 && has(rig.stored(), 2, 3, 62));
    ed.addNote(12, 60);   // a new edit clears the redo steps
    CHECK(!ed.canRedo());
}

void testMoveCopyAndTrim() {
    Rig            rig;
    SequenceEditor ed(*rig.controller, "s");
    ed.setNewNoteLength(4);
    ed.addNote(0, 60);

    // Moves are relative to the gesture's start and clamped to the grid.
    ed.beginGesture();
    ed.moveSelection(-3, 0, false);
    CHECK(has(rig.stored(), 0, 4, 60));
    ed.moveSelection(20, 2, false);
    CHECK(has(rig.stored(), 12, 4, 62));
    ed.moveSelection(5, 1, false);   // not cumulative: from the start
    CHECK(has(rig.stored(), 5, 4, 61) && rig.stored().size() == 1);
    ed.endGesture();

    // Alt-drag copies: the original stays, the copy is selected.
    ed.beginGesture();
    ed.moveSelection(0, 2, true);
    ed.endGesture();
    CHECK(has(rig.stored(), 5, 4, 61) && has(rig.stored(), 5, 4, 63));
    CHECK(ed.selectedCount() == 1);

    // Dropped onto a longer note on the same pitch: the earlier is trimmed.
    ed.clearSelection();
    ed.setNewNoteLength(8);
    ed.addNote(0, 70);                        // 0-7
    ed.setNewNoteLength(2);
    ed.addNote(12, 70);                       // 12-13, selected
    ed.beginGesture();
    ed.moveSelection(-9, 0, false);           // to 3
    ed.endGesture();
    CHECK(has(rig.stored(), 0, 3, 70) && has(rig.stored(), 3, 2, 70));

    // Two notes starting together: the one moved there is kept.
    ed.select(ed.noteAt(3, 70), false);
    ed.beginGesture();
    ed.moveSelection(-3, 0, false);           // onto the note at 0
    ed.endGesture();
    CHECK(has(rig.stored(), 0, 2, 70) && !has(rig.stored(), 0, 3, 70));
}

void testResizeVelocitySelection() {
    Rig            rig;
    SequenceEditor ed(*rig.controller, "s");
    ed.setNewNoteLength(2);
    ed.addNote(0, 60);
    ed.addNote(4, 60);

    // Stretching over a neighbour on the same pitch stops at it (the earlier is trimmed).
    ed.beginGesture();
    ed.resize(ed.noteAt(0, 60), 8);
    ed.endGesture();
    CHECK(has(rig.stored(), 0, 4, 60) && has(rig.stored(), 4, 2, 60));
    ed.beginGesture();
    ed.resize(ed.noteAt(4, 60), 50);   // clamped to the sequence's end
    ed.endGesture();
    CHECK(has(rig.stored(), 4, 12, 60));

    // Velocity, clamped to 1-127 for MIDI 1.0.
    ed.selectAll();
    ed.beginGesture();
    ed.changeVelocity(0.5);
    ed.endGesture();
    CHECK(rig.stored()[0].velocity == 127 && rig.stored()[1].velocity == 127);
    ed.beginGesture();
    ed.changeVelocity(-2.0);
    ed.endGesture();
    CHECK(rig.stored()[0].velocity == 1);

    // Rectangle selection, toggle, nudge, delete.
    ed.clearSelection();
    ed.setNewNoteLength(2);   // the resize above made 12 the length for new notes
    ed.addNote(8, 72);        // selected alone
    ed.selectRect(0, 3, 50, 65, false);
    CHECK(ed.selectedCount() == 1 && ed.items()[0].selected);
    ed.toggle(ed.noteAt(8, 72));
    CHECK(ed.selectedCount() == 2);
    ed.nudge(1, -1);
    CHECK(has(rig.stored(), 1, 4, 59) && has(rig.stored(), 9, 2, 71));
    ed.deleteSelection();
    CHECK(rig.stored().size() == 1 && has(rig.stored(), 4, 12, 60));
    ed.deleteNote(0);
    CHECK(rig.stored().empty());
}

void testEdoAndSync() {
    Rig            rig(MidiProtocol::Midi2, 31);
    SequenceEditor ed(*rig.controller, "s");
    // Reference C4 (60): steps -155 (MIDI 0) to 175 (just under 128).
    CHECK(ed.lowestPitch() == -155 && ed.highestPitch() == 175 && ed.stepsPerOctave() == 31);
    CHECK(!ed.isBlackKey(0) && ed.isBlackKey(2) && ed.isBlackKey(3) && !ed.isBlackKey(5));
    CHECK(ed.isBlackKey(7) && ed.isBlackKey(9) && !ed.isBlackKey(10) && ed.isBlackKey(31 + 15));
    CHECK(ed.addNote(0, -1) && rig.stored()[0].velocity > 127);   // 16-bit velocity by default

    // A change made elsewhere is adopted; the selection is cleared.
    rig.controller->setSequenceNotes("s", {{3, 1, 7, 1000}});
    ed.sync();
    CHECK(ed.items().size() == 1 && ed.items()[0].note.pitch == 7 && ed.selectedCount() == 0);

    // The protocol can change only while the sequence is empty.
    rig.controller->setSequenceProtocol("s", MidiProtocol::Midi1);
    CHECK(rig.session->sequence("s")->sequence.protocol == MidiProtocol::Midi2);
    ed.selectAll();
    ed.deleteSelection();
    rig.controller->setSequenceProtocol("s", MidiProtocol::Midi1);
    CHECK(rig.session->sequence("s")->sequence.protocol == MidiProtocol::Midi1 && ed.highestPitch() == 127);
}

// Adding or clicking a note sounds it; a move sounds the moved notes when its pitch
// changes, not when only its time does.
void testAudition() {
    Rig            rig(MidiProtocol::Midi1, 12, true);
    SequenceEditor ed(*rig.controller, "s");
    rig.sounded();   // the rack's graph is running
    CHECK(ed.addNote(0, 60) && rig.sounded() == std::vector<int>{60});

    ed.beginGesture();
    ed.moveSelection(2, 0, false);
    CHECK(rig.sounded().empty());
    ed.moveSelection(3, 2, false);
    ed.moveSelection(4, 2, false);   // the same pitch again: once is enough
    CHECK(rig.sounded() == std::vector<int>{62} && rig.ended == std::vector<int>{60});   // the old pitch ends
    ed.moveSelection(4, 0, false);   // back where it started is a change too
    CHECK(rig.sounded() == std::vector<int>{60});
    ed.endGesture();
    ed.stopSounding();               // the mouse released: it ends, then every key, then All Notes Off
    CHECK(rig.sounded().empty() && rig.ended.size() == 1 + 128 && rig.ended[0] == 60 && rig.allOff == 1);

    ed.addNote(8, 64);
    rig.sounded();
    ed.selectAll();
    ed.nudge(0, 1);   // the arrow keys: both notes
    CHECK(rig.sounded() == (std::vector<int>{61, 65}));
    ed.nudge(1, 0);
    CHECK(rig.sounded().empty());

    ed.soundNote(ed.noteAt(9, 65));   // a click on a note
    CHECK(rig.sounded() == std::vector<int>{65});
    ed.soundNote(99);                 // no such note: nothing
    CHECK(rig.sounded().empty());

    // Held (Ctrl-click): past its own length and the cap, until released.
    rig.sounded();                    // the plain click above has ended
    ed.soundNote(ed.noteAt(9, 65), true);
    CHECK(rig.sounded() == std::vector<int>{65});
    bool endedEarly = false;
    for (int block = 0; block < 48000 * 2 / 64; ++block) {   // 2 s
        rig.sounded();
        endedEarly = endedEarly || !rig.ended.empty();
    }
    CHECK(!endedEarly);
    ed.stopSounding();
    CHECK(rig.sounded().empty() && rig.ended.size() == 1 + 128 && rig.ended[0] == 65);
}

} // namespace

int main() {
    testDrawAndUndo();
    testMoveCopyAndTrim();
    testResizeVelocitySelection();
    testEdoAndSync();
    testAudition();
    return test::finish("SequenceEditor");
}
