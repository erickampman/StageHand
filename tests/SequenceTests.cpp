// Sequence playback tests: exact sample positions from the beat clock, launch
// and stop quantizing, Repeat, the note-off offset, same-pitch overlap, EDO
// pitches and nearest-free note numbers, edits while playing, and routing (a
// sequence plays only where it's chosen). No JACK, ALSA or real plugins.
//
// At 120 BPM and 48 kHz one beat is 24000 frames; at rate 4 a unit is 6000.

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <memory>
#include <vector>

#include "Check.h"
#include "uw/ump/Ump.h"
#include "Core/UwNames.h"
#include "Engine/Engine.h"
#include "FakePlugin.h"

using namespace stagehand;
using stagehand::test::FakePlugin;

namespace {

constexpr MidiSourceHandle kSeq = kSequenceSourceBase + 1;

struct Heard {
    std::int64_t  frame;   // since the test started
    bool          on;
    std::uint8_t  note;
    MidiMessage   message;
};

// An engine with one plugin listening to one sequence, run in fixed blocks.
struct Rig {
    std::unique_ptr<Engine>          engine  = std::make_unique<Engine>();
    std::shared_ptr<FakePlugin>      plugin  = std::make_shared<FakePlugin>();
    std::shared_ptr<SequenceRuntime> runtime = std::make_shared<SequenceRuntime>();
    std::int64_t                     frames  = 0;
    std::vector<Heard>               heard;

    explicit Rig(bool listenToAll = false, bool startTransport = true) {
        SlotConfig slot;
        slot.plugin                = plugin;
        slot.midiFilter.allSources = listenToAll;          // "all inputs" never hears a sequence...
        slot.midiFilter.source     = kInvalidMidiSource;
        if (!listenToAll) slot.sequenceSources = {kSeq};   // ...it plays into the slots that target it
        std::vector<SlotConfig> slots{slot};
        engine->commit(std::make_unique<EngineGraph>(std::move(slots)));
        if (startTransport) engine->sendTransportCommand(Engine::TransportCommand::Start);   // beat 0 at frame 0
    }
    ~Rig() {
        engine->commit(nullptr);
        engine->commitSequences(nullptr);
        run(64);
        engine->collect();
    }

    void set(const Sequence& seq, std::vector<CompiledSequence::Trigger> triggers = {},
             TransportControl transport = {}, const Sequence* other = nullptr) {
        std::vector<CompiledSequence> list{CompiledSequence::compile(seq, kSeq, runtime)};
        list[0].triggers = std::move(triggers);
        if (other != nullptr) list.push_back(CompiledSequence::compile(*other, kSeq + 1, otherRuntime));
        engine->commitSequences(std::make_unique<SequenceSet>(std::move(list), std::move(transport)));
    }
    std::shared_ptr<SequenceRuntime> otherRuntime = std::make_shared<SequenceRuntime>();

    // Queue `m` from input `source` so it lands `offset` frames into the next
    // block of `block` frames (run() stamps every block's start at 1 s).
    void midi(MidiSourceHandle source, const MidiMessage& m, std::uint32_t offset, std::uint32_t block = 4800) {
        TimedMidiMessage t;
        t.message     = m;
        t.captureTime = 1'000'000'000 - (std::int64_t{block} * 1'000'000'000 + 47999) / 48000 +
                        (std::int64_t{offset} * 1'000'000'000 + 47999) / 48000;
        engine->onMidi(source, t);
    }

    // Run `total` frames in blocks of `block`.
    void run(std::int64_t total, std::uint32_t block = 4800) {
        for (std::int64_t done = 0; done < total; done += block) {
            std::vector<float> left(block), right(block);
            float*             channels[2] = {left.data(), right.data()};
            AudioCallbackContext ctx;
            ctx.frameCount          = block;
            ctx.blockStartTime      = 1'000'000'000;
            ctx.time.sampleRate     = 48000.0;
            ctx.time.samplePosition = frames;
            ctx.output              = AudioBuffer{channels, 2, block};
            plugin->calls.clear();
            engine->process(ctx);
            for (const FakePlugin::Call& call : plugin->calls)
                for (const Event& e : call.events)
                    if (ump::isNoteOn(e.midi) || ump::isNoteOff(e.midi))
                        heard.push_back(Heard{frames + e.sampleOffset, ump::isNoteOn(e.midi),
                                              ump::noteNumber(e.midi), e.midi});
            frames += block;
            engine->collect();
        }
    }

    const Heard* find(bool on, std::uint8_t note, std::int64_t from = 0) const {
        for (const Heard& h : heard)
            if (h.on == on && h.note == note && h.frame >= from) return &h;
        return nullptr;
    }
};

Sequence midi1(std::vector<SequenceNote> notes, std::uint32_t length = 4) {
    Sequence s;
    s.id       = "s";
    s.protocol = MidiProtocol::Midi1;
    s.launch   = LaunchQuantize::Immediate;
    s.length   = length;
    s.notes    = std::move(notes);
    return s;
}

MidiMessage midi1Message(std::uint8_t status, std::uint8_t channel, std::uint8_t data1, std::uint8_t data2) {
    MidiMessage m;
    m.words[0]  = 0x20000000u | (std::uint32_t(status | channel) << 16) | (std::uint32_t{data1} << 8) | data2;
    m.wordCount = 1;
    return m;
}

CompiledSequence::Trigger trigger(SequenceTrigger::Control control, std::uint8_t channel, std::uint8_t number,
                                  SequenceTrigger::Action action = SequenceTrigger::Action::Toggle,
                                  MidiSourceHandle source = 1) {
    CompiledSequence::Trigger t;
    t.source  = source;
    t.action  = action;
    t.control = control;
    t.channel = channel;
    t.number  = number;
    return t;
}

bool at(const Heard* h, std::int64_t frame) {
    return h != nullptr && h->frame == frame;
}

void testTimingAndLoop() {
    Rig rig;
    rig.set(midi1({{0, 1, 60, 100}, {2, 2, 64, 90}}));   // a 4-unit (1-beat) loop
    rig.engine->sendSequenceCommand(kSeq, SequenceCommand::Start);
    rig.run(24000 * 2);   // two passes
    CHECK(at(rig.find(true, 60), 0));
    CHECK(at(rig.find(false, 60), 6000));
    CHECK(at(rig.find(true, 64), 12000));
    CHECK(at(rig.find(false, 64), 24000));
    CHECK(at(rig.find(true, 60, 1), 24000));   // the second pass
    CHECK(at(rig.find(true, 64, 24001), 36000));
    CHECK(rig.runtime->state.load() == SequenceState::Playing);
    const Heard* v = rig.find(true, 64);
    CHECK(v != nullptr && (v->message.words[0] & 0x7F) == 90);   // velocity as edited
}

void testLaunchQuantize() {
    Rig      rig;
    Sequence s = midi1({{0, 1, 60, 100}});
    s.launch   = LaunchQuantize::Bar;
    rig.set(s);
    rig.run(12000);   // half a beat in
    rig.engine->sendSequenceCommand(kSeq, SequenceCommand::Start);
    rig.run(4800);
    CHECK(rig.runtime->state.load() == SequenceState::Starting);
    rig.run(24000 * 4);
    CHECK(at(rig.find(true, 60), 24000 * 4));   // the next bar: beat 4

    Rig beat;
    s.launch = LaunchQuantize::Beat;
    beat.set(s);
    beat.run(12000);
    beat.engine->sendSequenceCommand(kSeq, SequenceCommand::Start);
    beat.run(24000 * 2);
    CHECK(at(beat.find(true, 60), 24000));   // the next beat
}

void testStops() {
    // A quantized stop (next bar) lets the loop run to it; the note sounding
    // then ends there.
    Rig      rig;
    Sequence s = midi1({{0, 4, 60, 100}});   // one note filling each beat-long loop
    rig.set(s);
    rig.engine->sendSequenceCommand(kSeq, SequenceCommand::Start);
    rig.run(24000 + 4800);   // into the second pass
    s.launch = LaunchQuantize::Bar;
    rig.set(s);
    rig.engine->sendSequenceCommand(kSeq, SequenceCommand::Stop);
    rig.run(24000 * 4);
    CHECK(rig.runtime->state.load() == SequenceState::Stopped);
    CHECK(rig.find(true, 60, 24000 * 4) == nullptr);        // nothing from the bar on
    CHECK(at(rig.find(false, 60, 24000 * 3 + 1), 24000 * 4));

    // StopNow ends a sounding note at the next block's start.
    Rig now;
    now.set(midi1({{0, 4, 62, 100}}));
    now.engine->sendSequenceCommand(kSeq, SequenceCommand::Start);
    now.run(9600);
    now.engine->sendSequenceCommand(kInvalidMidiSource, SequenceCommand::StopNow);   // all sequences
    now.run(4800);
    CHECK(at(now.find(false, 62), 9600));
    CHECK(now.runtime->state.load() == SequenceState::Stopped);
}

void testRepeatOff() {
    Rig      rig;
    Sequence s = midi1({{0, 1, 60, 100}, {3, 1, 67, 100}});
    s.repeat   = false;
    rig.set(s);
    rig.engine->sendSequenceCommand(kSeq, SequenceCommand::Start);
    rig.run(24000 * 3);
    CHECK(at(rig.find(false, 67), 24000));             // its end is the sequence's end
    CHECK(rig.find(true, 60, 1) == nullptr);           // once only
    CHECK(rig.runtime->state.load() == SequenceState::Stopped);
}

void testNoteOffOffset() {
    Rig      legato;
    Sequence s        = midi1({{0, 1, 60, 100}});
    s.noteOffOffsetMs = 10.0;   // 480 frames
    legato.set(s);
    legato.engine->sendSequenceCommand(kSeq, SequenceCommand::Start);
    legato.run(24000);
    CHECK(at(legato.find(false, 60), 6000 + 480));

    Rig staccato;
    s.noteOffOffsetMs = -500.0;   // longer than the note: it still lasts 1 ms (48 frames)
    staccato.set(s);
    staccato.engine->sendSequenceCommand(kSeq, SequenceCommand::Start);
    staccato.run(24000);
    CHECK(at(staccato.find(false, 60), 48));

    // A delayed note-off meets the next note on the same pitch: it's sent just
    // before that note-on, at the same frame.
    Rig same;
    s                 = midi1({{0, 1, 60, 100}, {1, 1, 60, 100}});
    s.noteOffOffsetMs = 50.0;
    same.set(s);
    same.engine->sendSequenceCommand(kSeq, SequenceCommand::Start);
    same.run(24000);
    std::vector<const Heard*> at6000;
    for (const Heard& h : same.heard)
        if (h.frame == 6000) at6000.push_back(&h);
    CHECK(at6000.size() == 2 && !at6000[0]->on && at6000[1]->on);   // off, then on
    CHECK(at(same.find(false, 60, 6001), 12000 + 2400));            // the second one's own offset
}

// Swing (docs/sequences.md, "Swing and slop"): every odd unit of the loop is
// late by (2 * swing - 1) of a unit, and notes end on the swung grid. The
// loop's length and the tempo don't change.
void testSwing() {
    Sequence s = midi1({{0, 1, 60, 100}, {1, 1, 61, 100}, {2, 1, 62, 100}, {3, 1, 63, 100}});
    s.swing    = 0.75;   // half a unit: 3000 frames
    for (const std::uint32_t block : {4800u, 1024u, 64u}) {   // a late start in a later block than its unit
        Rig rig;
        rig.set(s);
        rig.engine->sendSequenceCommand(kSeq, SequenceCommand::Start);
        rig.run(24000 * 2, block);
        CHECK(at(rig.find(true, 60), 0));
        CHECK(at(rig.find(true, 61), 9000));
        CHECK(at(rig.find(true, 62), 12000));
        CHECK(at(rig.find(true, 63), 21000));
        CHECK(at(rig.find(false, 60), 9000));         // it ends where the late unit starts: longer
        CHECK(at(rig.find(false, 61), 12000));        // the late unit's note is shorter
        CHECK(at(rig.find(false, 63), 24000));        // the loop's end isn't moved
        CHECK(at(rig.find(true, 60, 1), 24000));      // the next pass is on time
        CHECK(at(rig.find(true, 61, 9001), 33000));
        std::size_t heard = 0;                        // 8 on, 7 off (the last ends at 48000): none twice
        for (const Heard& h : rig.heard) heard += h.frame < 48000 ? 1 : 0;
        CHECK(heard == 15);
    }

    Rig sixty;   // 60%: a fifth of a unit, 1200 frames
    s.swing = 0.6;
    sixty.set(s);
    sixty.engine->sendSequenceCommand(kSeq, SequenceCommand::Start);
    sixty.run(24000);
    CHECK(at(sixty.find(true, 61), 7200) && at(sixty.find(true, 63), 19200));

    // Units are counted within the loop: in a 3-unit loop, the second pass's
    // first unit (the fourth since the start) is on time.
    Rig      odd;
    Sequence three = midi1({{0, 1, 60, 100}, {1, 1, 61, 100}, {2, 1, 62, 100}}, 3);
    three.swing    = 0.75;
    odd.set(three);
    odd.engine->sendSequenceCommand(kSeq, SequenceCommand::Start);
    odd.run(24000 * 2);
    CHECK(at(odd.find(true, 60, 1), 18000) && at(odd.find(true, 61, 9001), 27000));

    // Repeat off: it still stops at its length.
    Rig once;
    s.swing  = 0.75;
    s.repeat = false;
    once.set(s);
    once.engine->sendSequenceCommand(kSeq, SequenceCommand::Start);
    once.run(24000 * 2);
    CHECK(at(once.find(false, 63), 24000) && once.find(true, 60, 1) == nullptr);
    CHECK(once.runtime->state.load() == SequenceState::Stopped);
}

// The first `units` note-ons of a run, in order, and how far each is from its
// unit's grid line (`unitFrames` apart, from frame 0). A run can hear the next
// unit's note too, slopped early, or in a block past the run's end.
std::vector<std::int64_t> deviations(const Rig& rig, std::int64_t unitFrames, std::size_t units, std::size_t& count,
                                     bool& inOrder) {
    std::vector<std::int64_t> out;
    inOrder = true;
    for (const Heard& h : rig.heard) {
        if (!h.on || out.size() == units) continue;
        if (h.note != 40 + out.size() % 16) inOrder = false;   // each unit once, in turn
        out.push_back(h.frame - static_cast<std::int64_t>(out.size()) * unitFrames);
    }
    count = out.size();
    return out;
}

// Slop: each note-on early or late at random by up to the setting, never more
// than a quarter of a unit; not the first after a start; new every pass; note
// ends stay on the grid.
void testSlop() {
    std::vector<SequenceNote> notes;
    for (std::uint32_t u = 0; u < 16; ++u) notes.push_back({u, 1, static_cast<std::int32_t>(40 + u), 100});
    Sequence           s       = midi1(notes, 16);   // 4 beats; a unit is 6000 frames
    s.slopMs                   = 10.0;               // 480 frames
    constexpr int      kPasses = 25;
    std::vector<std::int64_t> first;
    for (const std::uint32_t block : {4800u, 256u}) {   // an early start in an earlier block than its unit
        Rig rig;
        rig.set(s);
        rig.engine->sendSequenceCommand(kSeq, SequenceCommand::Start);
        rig.run(96000 * kPasses, block);
        std::size_t count   = 0;
        bool        inOrder = false;
        const auto  dev     = deviations(rig, 6000, 16 * kPasses, count, inOrder);
        CHECK(count == 16 * kPasses && inOrder);
        if (dev.empty()) continue;
        CHECK(dev[0] == 0);   // the start lands on its launch
        double sum = 0;
        std::size_t moved = 0, worst = 0;
        bool        newPass = false;
        for (std::size_t k = 0; k < dev.size(); ++k) {
            sum += static_cast<double>(dev[k]);
            if (dev[k] != 0) ++moved;
            worst = std::max(worst, static_cast<std::size_t>(std::abs(dev[k])));
            if (k + 16 < dev.size() && dev[k] != dev[k + 16]) newPass = true;
        }
        CHECK(worst <= 480 && worst > 400);
        CHECK(moved > dev.size() * 9 / 10 && newPass);
        CHECK(std::fabs(sum / static_cast<double>(dev.size())) < 60.0);   // early as often as late
        std::int64_t k = 0;   // note-offs aren't slopped: each at its unit's end
        bool         ends = true;
        for (const Heard& h : rig.heard)
            if (!h.on && h.frame < 96000 * kPasses && h.frame != ++k * 6000) ends = false;
        CHECK(ends && k == 16 * kPasses - 1);
        if (first.empty()) first = dev;
        else CHECK(first != dev);   // fresh randomness, not a fixed pattern
    }

    // Held to a quarter of a unit: at rate 16 a unit is 1500 frames, so 50 ms
    // (2400 frames) moves a note at most 375.
    Rig fast;
    s.rate   = SequenceRate{16, 1};
    s.slopMs = 50.0;
    fast.set(s);
    fast.engine->sendSequenceCommand(kSeq, SequenceCommand::Start);
    fast.run(24000 * 20, 512);
    std::size_t count = 0;
    bool        inOrder = false;
    std::int64_t worst = 0;
    for (const std::int64_t d : deviations(fast, 1500, 16 * 20, count, inOrder)) worst = std::max(worst, std::abs(d));
    CHECK(count == 16 * 20 && inOrder && worst <= 375 && worst > 300);

    // With full swing too, units keep their order, and each plays once.
    Rig both;
    s.swing = 0.75;
    both.set(s);
    both.engine->sendSequenceCommand(kSeq, SequenceCommand::Start);
    both.run(24000 * 20, 512);
    deviations(both, 1500, 16 * 20, count, inOrder);
    CHECK(count == 16 * 20 && inOrder);

    // Repeat off: slop never pushes a note past the end.
    Rig once;
    s.repeat = false;
    once.set(s);
    once.engine->sendSequenceCommand(kSeq, SequenceCommand::Start);
    once.run(24000 * 2, 512);
    deviations(once, 1500, 32, count, inOrder);
    CHECK(count == 16 && inOrder);
    bool before = true;
    for (const Heard& h : once.heard)
        if (h.frame > 24000) before = false;
    CHECK(before && once.runtime->state.load() == SequenceState::Stopped);
}

void testEdoPitches() {
    Rig      rig;
    Sequence s;
    s.id            = "edo";
    s.protocol      = MidiProtocol::Midi2;
    s.edo           = 31;
    s.referenceNote = 60;
    s.launch        = LaunchQuantize::Immediate;
    s.length        = 4;
    s.notes         = {{0, 2, 0, 40000}, {0, 2, 1, 40000}, {0, 2, -1, 40000}};
    rig.set(s);
    rig.engine->sendSequenceCommand(kSeq, SequenceCommand::Start);
    rig.run(4800);

    // Steps -1, 0, 1 are 59.61, 60 and 60.39: all nearest 60, so they're spread
    // to the nearest free numbers, each carrying its exact pitch.
    std::vector<const Heard*> ons;
    for (const Heard& h : rig.heard)
        if (h.on) ons.push_back(&h);
    CHECK(ons.size() == 3);
    if (ons.size() != 3) return;
    auto pitch79 = [](const Heard* h) { return h->message.words[1] & 0xFFFF; };
    auto expect  = [](double semitones) { return static_cast<std::uint32_t>(std::lround(semitones * 512.0)); };
    // Sorted by pitch within a unit: step -1 first.
    CHECK(ons[0]->note == 60 && pitch79(ons[0]) == expect(60.0 - 12.0 / 31));
    CHECK(ons[1]->note == 59 && pitch79(ons[1]) == expect(60.0));
    CHECK(ons[2]->note == 61 && pitch79(ons[2]) == expect(60.0 + 12.0 / 31));
    CHECK((ons[0]->message.words[0] & 0xFF) == 0x03);            // attribute: pitch 7.9
    CHECK((ons[0]->message.words[1] >> 16) == 40000);            // 16-bit velocity
    rig.run(24000);
    CHECK(rig.find(false, 59) && rig.find(false, 60) && rig.find(false, 61));   // each released by its number
}

void testEditsWhilePlaying() {
    // Deleting a sounding note ends it at the next block; shortening moves its end.
    Rig      rig;
    Sequence s = midi1({{0, 4, 60, 100}, {0, 4, 64, 100}}, 8);
    rig.set(s);
    rig.engine->sendSequenceCommand(kSeq, SequenceCommand::Start);
    rig.run(9600);
    s.notes = {{0, 2, 64, 100}};   // 60 deleted; 64 shortened to 2 units (12000)
    rig.set(s);
    rig.run(4800);
    CHECK(at(rig.find(false, 60), 9600));
    rig.run(4800);
    CHECK(at(rig.find(false, 64), 12000));

    // A rate change keeps the loop's phase: halfway through an 8-unit loop at
    // rate 4 (beat 1 of 2) becomes halfway through at rate 2.
    Rig      phase;
    Sequence p = midi1({{0, 1, 60, 100}, {4, 1, 72, 100}}, 8);
    phase.set(p);
    phase.engine->sendSequenceCommand(kSeq, SequenceCommand::Start);
    phase.run(24000);             // exactly halfway: unit 4 is next
    p.rate = SequenceRate{2, 1};  // 2 per beat: the loop now lasts 4 beats
    phase.set(p);
    phase.run(24000 * 4);
    CHECK(at(phase.find(true, 72), 24000));            // unit 4 still comes now
    CHECK(at(phase.find(true, 60, 1), 24000 + 48000)); // then 4 units at 12000 frames each
}

void testRouting() {
    // "All inputs" doesn't hear sequences.
    Rig all(true);
    all.set(midi1({{0, 1, 60, 100}}));
    all.engine->sendSequenceCommand(kSeq, SequenceCommand::Start);
    all.run(24000);
    CHECK(all.heard.empty());

    // A sequence removed while its note sounds releases it.
    Rig rig;
    rig.set(midi1({{0, 4, 60, 100}}));
    rig.engine->sendSequenceCommand(kSeq, SequenceCommand::Start);
    rig.run(9600);
    rig.engine->commitSequences(nullptr);
    rig.run(4800);
    CHECK(at(rig.find(false, 60), 9600));
    CHECK(rig.runtime->state.load() == SequenceState::Stopped);

    // Tempo changes need nothing special: at 60 BPM a unit is 12000 frames.
    Rig slow;
    slow.engine->setTempo(60.0);
    slow.set(midi1({{0, 1, 60, 100}, {1, 1, 62, 100}}));
    slow.engine->sendSequenceCommand(kSeq, SequenceCommand::Start);
    slow.run(24000);
    CHECK(at(slow.find(true, 62), 12000));
}

// Auditions (the editor sounding a note): into the sequence's rows only,
// playing or not, for their own time, and held apart from what it plays.
void testAudition() {
    const AuditionMessages c4 = auditionMessages(midi1({}), SequenceNote{0, 1, 60, 90});

    // Stopped, transport too: on at the next block, off 0.25 s (12000 frames) later.
    Rig rig(false, false);
    rig.run(4800);
    CHECK(rig.engine->auditionNote(kSeq, c4.on, c4.off, 0.25));
    rig.run(24000);
    CHECK(at(rig.find(true, 60), 4800) && at(rig.find(false, 60), 4800 + 12000));
    CHECK(rig.heard.size() == 2 && (rig.find(true, 60)->message.words[0] & 0x7F) == 90);

    // "All inputs" doesn't hear it; nor does a row the sequence doesn't play into.
    Rig all(true, false);
    all.engine->auditionNote(kSeq, c4.on, c4.off, 0.25);
    all.run(24000);
    CHECK(all.heard.empty());

    // Again before it ends: it restarts (one off first, then one at the new end).
    Rig again(false, false);
    again.engine->auditionNote(kSeq, c4.on, c4.off, 0.25);
    again.run(4800);
    again.engine->auditionNote(kSeq, c4.on, c4.off, 0.25);
    again.run(24000);
    CHECK(at(again.find(false, 60), 4800) && at(again.find(true, 60, 1), 4800));
    CHECK(at(again.find(false, 60, 4801), 4800 + 12000) && again.heard.size() == 4);

    // Ended early (a dragged note leaving its pitch), then a new one at once.
    const AuditionMessages d4 = auditionMessages(midi1({}), SequenceNote{0, 1, 62, 90});
    Rig moved(false, false);
    moved.engine->auditionNote(kSeq, c4.on, c4.off, 0.25);
    moved.run(4800);
    moved.engine->endAuditions(kSeq);
    moved.engine->auditionNote(kSeq, d4.on, d4.off, 0.25);
    moved.run(24000);
    CHECK(at(moved.find(false, 60), 4800) && at(moved.find(true, 62), 4800));
    CHECK(at(moved.find(false, 62), 4800 + 12000) && moved.heard.size() == 4);
    CHECK(moved.heard[1].frame == 4800 && !moved.heard[1].on);   // the old note's off comes first

    // An end can carry All Notes Off: the audition's note-off, a note-off for
    // every key, then the CC, all at the block's start, into the same rows.
    Rig reset(false, false);
    reset.engine->auditionNote(kSeq, c4.on, c4.off, 0.25);
    reset.run(4800);
    const MidiMessage allOff = midi1Message(0xB0, 0, 123, 0);
    reset.engine->endAuditions(kSeq, &allOff);
    reset.plugin->calls.clear();
    reset.run(4800);   // run() keeps only notes: look at the last block's events
    int  offsBeforeCc = 0, keys = 0;
    bool sawCc = false, atStart = true;
    for (const FakePlugin::Call& call : reset.plugin->calls)
        for (const Event& e : call.events) {
            atStart = atStart && e.sampleOffset == 0;
            if (ump::isNoteOff(e.midi) && !sawCc) {
                ++offsBeforeCc;
                keys += ump::noteNumber(e.midi);
            }
            if (e.midi.words[0] == allOff.words[0]) sawCc = true;
        }
    CHECK(sawCc && atStart && offsBeforeCc == 1 + 128 && keys == 60 + 127 * 128 / 2);

    // Over the sequence playing the same note: each note-off ends its own note.
    Rig both;
    both.set(midi1({{0, 4, 60, 100}}));   // 0-24000
    both.engine->sendSequenceCommand(kSeq, SequenceCommand::Start);
    both.run(4800);
    both.engine->auditionNote(kSeq, c4.on, c4.off, 0.1);   // 4800-9600
    both.run(48000);
    CHECK(at(both.find(false, 60), 9600) && at(both.find(false, 60, 9601), 24000));

    // MIDI 2.0 in 24-EDO: the nearest note number, the exact pitch attached.
    Sequence quarter = midi1({});
    quarter.protocol = MidiProtocol::Midi2;
    quarter.edo      = 24;
    quarter.referenceNote = 60;
    const AuditionMessages q = auditionMessages(quarter, SequenceNote{0, 1, 1, 0x8000});   // 60.5
    CHECK(ump::noteNumber(q.on) == 61 && (q.on.words[1] & 0xFFFF) == 60 * 512 + 256);
    CHECK(ump::isNoteOff(q.off) && ump::noteNumber(q.off) == 61);
}

// A pad (note 36 on channel 10) toggles a sequence at the frame it arrives.
void testNoteTrigger() {
    Rig rig;
    rig.set(midi1({{0, 1, 60, 100}}), {trigger(SequenceTrigger::Control::Note, 9, 36)});
    rig.run(4800);
    rig.midi(1, midi1Message(0x90, 9, 36, 100), 1000);
    rig.run(4800);
    CHECK(at(rig.find(true, 60), 5800));                 // Immediate: at the pad's own frame
    rig.midi(1, midi1Message(0x80, 9, 36, 0), 10);       // its note-off does nothing
    rig.midi(1, midi1Message(0x90, 0, 36, 100), 20);     // another channel: not the trigger
    rig.midi(1, midi1Message(0x90, 9, 36, 100), 200);    // again: stop, ending the sounding note
    rig.run(4800 * 6);
    CHECK(rig.runtime->state.load() == SequenceState::Stopped);
    CHECK(at(rig.find(false, 60), 9800));
    CHECK(rig.find(true, 60, 5801) == nullptr);

    // Start does nothing to a playing sequence; Stop stops it.
    Rig two;
    two.set(midi1({{0, 1, 60, 100}}), {trigger(SequenceTrigger::Control::Note, 0, 40, SequenceTrigger::Action::Start),
                                       trigger(SequenceTrigger::Control::Note, 0, 41, SequenceTrigger::Action::Stop)});
    two.run(4800);
    two.midi(1, midi1Message(0x90, 0, 40, 100), 0);
    two.run(4800);
    two.midi(1, midi1Message(0x90, 0, 40, 100), 0);
    two.run(4800);
    CHECK(two.runtime->state.load() == SequenceState::Playing);
    two.midi(1, midi1Message(0x90, 0, 41, 100), 0);
    two.run(4800);
    CHECK(two.runtime->state.load() == SequenceState::Stopped);
    two.midi(2, midi1Message(0x90, 0, 40, 100), 0);      // another input: not the trigger
    two.run(4800);
    CHECK(two.runtime->state.load() == SequenceState::Stopped);
}

// A trigger that arrives just before a bar line starts the sequence on that
// bar, not the next one: triggers act before the block's notes are generated.
void testTriggerQuantize() {
    Rig      rig;
    Sequence s = midi1({{0, 1, 60, 100}});
    s.launch   = LaunchQuantize::Bar;
    rig.set(s, {trigger(SequenceTrigger::Control::Note, 0, 36)});
    rig.run(95000, 5000);                                // the block 95000-100000 holds bar 1 (96000)
    rig.midi(1, midi1Message(0x90, 0, 36, 100), 100, 5000);
    rig.run(10000, 5000);
    CHECK(at(rig.find(true, 60), 96000));
}

// A controller acts when it rises to 64 or more, once per press.
void testControllerTrigger() {
    Rig rig;
    rig.set(midi1({{0, 1, 60, 100}}), {trigger(SequenceTrigger::Control::Controller, 0, 64)});
    rig.run(4800);
    auto cc = [&](std::uint8_t value) {
        rig.midi(1, midi1Message(0xB0, 0, 64, value), 0);
        rig.run(4800);
        return rig.runtime->state.load();
    };
    CHECK(cc(127) == SequenceState::Playing);
    CHECK(cc(100) == SequenceState::Playing);   // still down
    CHECK(cc(0) == SequenceState::Playing);     // released
    CHECK(cc(127) == SequenceState::Stopped);   // pressed again

    // A held controller survives an edit without acting again.
    rig.set(midi1({{0, 2, 60, 100}}), {trigger(SequenceTrigger::Control::Controller, 0, 64)});
    rig.run(4800);
    CHECK(cc(120) == SequenceState::Stopped);

    // MIDI 2.0: the top half of the 32-bit value is "pressed".
    MidiMessage m2;
    m2.words[0]  = 0x40B04000u;
    m2.words[1]  = 0x80000000u;
    m2.wordCount = 2;
    rig.midi(1, midi1Message(0xB0, 0, 64, 0), 0);
    rig.midi(1, m2, 10);
    rig.run(4800);
    CHECK(rig.runtime->state.load() == SequenceState::Playing);
}

// Trigger messages go nowhere else; everything else is routed as usual. A
// trigger with no source listens to every input.
void testTriggerConsumesAndLearn() {
    Rig rig(true);   // the plugin hears all inputs
    rig.set(midi1({{0, 1, 60, 100}}),
            {trigger(SequenceTrigger::Control::Note, 9, 36, SequenceTrigger::Action::Toggle, kInvalidMidiSource)});
    rig.run(4800);
    rig.midi(3, midi1Message(0x90, 9, 36, 100), 0);
    rig.midi(3, midi1Message(0x90, 9, 37, 100), 10);
    rig.midi(3, midi1Message(0x80, 9, 36, 0), 20);
    rig.run(4800);
    CHECK(rig.find(true, 36) == nullptr && rig.find(false, 36) == nullptr);
    CHECK(rig.find(true, 37) != nullptr);
    CHECK(rig.runtime->state.load() == SequenceState::Playing);

    // Learn captures the next press only, and it goes nowhere else.
    Engine::Learned learned;
    rig.engine->armLearn();
    rig.midi(5, midi1Message(0xB0, 2, 7, 10), 0);   // a low controller value: not a press
    rig.midi(5, midi1Message(0x90, 2, 50, 90), 10);
    rig.midi(5, midi1Message(0x90, 2, 51, 90), 20);
    rig.run(4800);
    CHECK(rig.engine->takeLearned(learned));
    CHECK(learned.source == 5 && ump::isNoteOn(learned.message) && ump::noteNumber(learned.message) == 50 &&
          ump::channel(learned.message) == 2);
    CHECK(!rig.engine->takeLearned(learned));
    CHECK(rig.find(true, 50) == nullptr);
    CHECK(rig.find(true, 51) != nullptr);

    // Cancelled: nothing is captured.
    rig.engine->armLearn();
    rig.engine->cancelLearn();
    rig.midi(5, midi1Message(0x90, 2, 52, 90), 0);
    rig.run(4800);
    CHECK(!rig.engine->takeLearned(learned));
    CHECK(rig.find(true, 52) != nullptr);
}

// Stopped, a sequence's Play starts the transport, on beat 1 with no quantize
// wait, even with launch "next bar".
void testSequenceStartsTransport() {
    Rig      rig(false, false);
    Sequence s = midi1({{0, 4, 60, 100}});   // fills the loop
    s.launch   = LaunchQuantize::Bar;
    rig.set(s);
    rig.run(12000);   // stopped: nothing moves
    CHECK(!rig.engine->transport().running && rig.engine->transport().beatPosition == 0.0);
    rig.engine->sendSequenceCommand(kSeq, SequenceCommand::Start);
    rig.run(4800);
    CHECK(at(rig.find(true, 60), 14400));   // the next block's first frame: beat 0
    CHECK(rig.engine->transport().running);
    CHECK(std::fabs(rig.engine->transport().beatPosition - 0.2) < 1e-9);   // 4800 frames in

    // Stop ends every sequence at once and returns to beat 0.
    rig.run(2400);
    rig.engine->sendTransportCommand(Engine::TransportCommand::Stop);
    rig.run(4800);
    CHECK(at(rig.find(false, 60), 24000));   // the sounding note ends at the stop (run() uses whole blocks)
    CHECK(!rig.engine->transport().running && rig.engine->transport().beatPosition == 0.0);
    CHECK(rig.runtime->state.load() == SequenceState::Stopped);
}

// At Start sequences start with the transport, exactly on beat 1; others
// don't. A transport trigger acts at its own frame.
void testAtStart() {
    Rig      rig(false, false);
    Sequence armed = midi1({{0, 1, 60, 100}});
    armed.atStart  = true;
    Sequence other = midi1({{0, 1, 62, 100}});
    other.id       = "other";
    TransportControl transport;
    transport.triggers = {trigger(SequenceTrigger::Control::Note, 9, 36)};   // Start/Stop
    rig.set(armed, {}, transport, &other);
    rig.run(4800);
    rig.midi(1, midi1Message(0x90, 9, 36, 100), 1000);
    rig.run(4800);
    CHECK(rig.engine->transport().running);
    CHECK(at(rig.find(true, 60), 5800));
    CHECK(rig.otherRuntime->state.load() == SequenceState::Stopped);

    // A sequence Play while running waits for its launch boundary as before.
    rig.midi(1, midi1Message(0x90, 9, 36, 100), 0);   // toggle: stop
    rig.run(4800);
    CHECK(!rig.engine->transport().running && rig.runtime->state.load() == SequenceState::Stopped);
}

// Start on first note: the note starts the transport at its frame and still
// plays; later notes don't restart it. A sequence trigger while stopped starts
// the transport at its frame too.
void testStartOnNote() {
    Rig      rig(true, false);   // hears all inputs (not the sequence)
    Sequence armed = midi1({{0, 1, 60, 100}});
    armed.atStart  = true;
    TransportControl transport;
    transport.startOnNote = true;
    rig.set(armed, {}, transport);
    rig.run(4800);
    rig.midi(1, midi1Message(0x90, 0, 64, 100), 500);
    rig.run(4800);
    CHECK(rig.engine->transport().running && at(rig.find(true, 64), 5300));
    CHECK(rig.runtime->state.load() == SequenceState::Playing);
    const double beat = rig.engine->transport().beatPosition;
    CHECK(std::fabs(beat - (9600.0 - 5300.0) / 24000.0) < 1e-9);   // beat 0 at the note

    Rig      trig(false, false);
    Sequence s = midi1({{0, 1, 60, 100}});
    s.launch   = LaunchQuantize::Bar;
    trig.set(s, {trigger(SequenceTrigger::Control::Note, 9, 36)});
    trig.run(4800);
    trig.midi(1, midi1Message(0x90, 9, 36, 100), 700);
    trig.run(4800);
    CHECK(at(trig.find(true, 60), 5500));   // beat 1 at the pad, not the next bar
}

// The count-in (docs/perform.md): bar 1 lands after it, and everything that
// starts with the transport starts there. A trigger counts in from its own
// frame; a sequence started while stopped starts on bar 1; start on first
// note never counts in (the note is bar 1); a song switch cancels it. One bar
// of 4/4 at 120 BPM: 96000 frames.
void testCountIn() {
    {   // At Start, from a transport trigger at frame 5800: bar 1 at 101800.
        Rig      rig(false, false);
        Sequence armed = midi1({{0, 1, 60, 100}});
        armed.atStart  = true;
        TransportControl transport;
        transport.triggers = {trigger(SequenceTrigger::Control::Note, 9, 36)};
        rig.set(armed, {}, transport);
        rig.engine->setCountIn(1.0);
        rig.run(4800);
        rig.midi(1, midi1Message(0x90, 9, 36, 100), 1000);
        rig.run(4800);
        CHECK(!rig.engine->transport().running && rig.engine->transport().countIn > 0);
        rig.run(4800 * 19);   // to 100800: still counting
        CHECK(rig.runtime->state.load() == SequenceState::Stopped && rig.find(true, 60) == nullptr);
        rig.run(4800 * 2);
        CHECK(rig.engine->transport().running && at(rig.find(true, 60), 101800));
    }
    {   // A sequence started while stopped: the count-in, then it on bar 1.
        Rig      rig(false, false);
        Sequence s = midi1({{0, 1, 60, 100}});
        s.launch   = LaunchQuantize::Bar;
        rig.set(s);
        rig.engine->setCountIn(1.0);
        rig.run(14400);
        rig.engine->sendSequenceCommand(kSeq, SequenceCommand::Start);
        rig.run(4800 * 19);   // to 105600: still counting
        CHECK(!rig.engine->transport().running && rig.find(true, 60) == nullptr);
        rig.run(4800 * 2);
        CHECK(rig.engine->transport().running && at(rig.find(true, 60), 14400 + 96000));
    }
    {   // Start on first note: no count-in, whatever it's set to.
        Rig      rig(true, false);
        Sequence armed = midi1({{0, 1, 60, 100}});
        armed.atStart  = true;
        TransportControl transport;
        transport.startOnNote = true;
        rig.set(armed, {}, transport);
        rig.engine->setCountIn(2.0);
        rig.run(4800);
        rig.midi(1, midi1Message(0x90, 0, 64, 100), 500);
        rig.run(4800);
        CHECK(rig.engine->transport().running && rig.engine->transport().countIn == 0 && at(rig.find(true, 64), 5300));
    }
    {   // A song switch during the count-in cancels it: the new song is stopped.
        Rig      rig(false, false);
        Sequence armed = midi1({{0, 1, 60, 100}});
        armed.atStart  = true;
        rig.set(armed);
        rig.engine->setCountIn(1.0);
        rig.run(4800);
        rig.engine->sendTransportCommand(Engine::TransportCommand::Start);
        rig.run(4800 * 4);
        CHECK(rig.engine->transport().countIn > 0);
        auto next = std::make_shared<SequenceRuntime>();
        Sequence song = midi1({{0, 1, 62, 100}});
        song.atStart  = true;
        std::vector<CompiledSequence> list{CompiledSequence::compile(song, kSeq, next)};
        SlotConfig slot;
        slot.plugin          = rig.plugin;
        slot.sequenceSources = {kSeq};
        rig.engine->commitSwitch(std::make_unique<EngineGraph>(std::vector<SlotConfig>{slot}),
                                 std::make_unique<SequenceSet>(std::move(list)), Engine::SwitchPlan{});
        rig.run(4800 * 30);
        CHECK(!rig.engine->transport().running && rig.engine->transport().countIn == 0);
        CHECK(rig.find(true, 60) == nullptr && rig.find(true, 62) == nullptr);
    }
}

} // namespace

int main() {
    testTimingAndLoop();
    testLaunchQuantize();
    testStops();
    testRepeatOff();
    testNoteOffOffset();
    testSwing();
    testSlop();
    testEdoPitches();
    testEditsWhilePlaying();
    testRouting();
    testAudition();
    testNoteTrigger();
    testTriggerQuantize();
    testControllerTrigger();
    testTriggerConsumesAndLearn();
    testSequenceStartsTransport();
    testAtStart();
    testStartOnNote();
    testCountIn();
    return test::finish("Sequence");
}
