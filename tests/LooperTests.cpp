// Looper tests (docs/looper.md, milestone 1) with a fake plugin that hears only
// the loop: recording in time (beat or bar, early notes, ended by a press or a
// fixed length, quantize or off, cut at the end), playback, the footswitch's
// tap and hold (overdub), undo and clear, the cap, and the transport.

#include <cmath>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "Check.h"
#include "Core/Ump.h"
#include "Engine/Engine.h"
#include "FakePlugin.h"

using namespace stagehand;
using stagehand::test::FakePlugin;

namespace {

constexpr MidiSourceHandle kKeys = 3;                        // the input it records
constexpr MidiSourceHandle kLoop = kSequenceSourceBase + 7;  // the loop, as a source
constexpr Nanoseconds      T     = 1'000'000'000;
constexpr std::uint32_t    kBlock        = 480;
constexpr double           kFramesPerBeat = 24000.0;   // 120 BPM at 48 kHz

struct Heard {
    double      beat = 0.0;
    bool        on   = false;
    std::uint8_t note = 0;
};

// An engine with one row that hears only the loop, the transport started at
// frame 0 of the first block (beat 0), 120 BPM, 4/4.
struct Rig {
    std::unique_ptr<Engine>        engine = std::make_unique<Engine>();
    std::shared_ptr<FakePlugin>    plugin = std::make_shared<FakePlugin>(2, NoteDialect::Midi);
    std::shared_ptr<LooperRuntime> rt;
    std::uint64_t                  frame  = 0;   // the next block's first frame

    explicit Rig(CompiledLooper looper, bool startTransport = true, std::size_t capacity = LooperRuntime::kMaxNotes)
        : rt(std::make_shared<LooperRuntime>(capacity)) {
        SlotConfig slot;
        slot.plugin                = plugin;
        slot.midiFilter.allSources = false;
        slot.midiFilter.source     = kInvalidMidiSource;   // no live input: only what the loop plays
        slot.sequenceSources       = {kLoop};
        engine->commit(std::make_unique<EngineGraph>(std::vector<SlotConfig>{slot}));
        looper.source  = kLoop;
        looper.runtime = rt;
        looper.input   = kKeys;
        engine->commitSequences(std::make_unique<SequenceSet>(std::vector<CompiledSequence>{}, TransportControl{},
                                                              std::vector<CompiledLooper>{looper}));
        if (startTransport) engine->sendTransportCommand(Engine::TransportCommand::Start);
    }
    ~Rig() {
        engine->commit(nullptr);
        engine->commitSequences(nullptr);
        block();
        engine->collect();
    }

    // New settings for the same looper (a new snapshot; what it recorded is kept).
    void set(CompiledLooper looper) {
        looper.source  = kLoop;
        looper.runtime = rt;
        looper.input   = kKeys;
        engine->commitSequences(std::make_unique<SequenceSet>(std::vector<CompiledSequence>{}, TransportControl{},
                                                              std::vector<CompiledLooper>{looper}));
    }

    void block() {
        std::vector<float>   left(kBlock), right(kBlock);
        float*               channels[2] = {left.data(), right.data()};
        AudioCallbackContext ctx;
        ctx.frameCount      = kBlock;
        ctx.blockStartTime  = T;
        ctx.time.sampleRate = 48000.0;
        ctx.output          = AudioBuffer{channels, 2, kBlock};
        engine->process(ctx);
        frame += kBlock;
    }
    // Run blocks until the next one holds `beat`.
    void runTo(double beat) {
        while (static_cast<double>(frame + kBlock) <= beat * kFramesPerBeat + 1e-6) block();
    }
    // A key, at `beat` (in the next block once runTo() has got there).
    void key(bool on, std::uint8_t note, double beat) {
        runTo(beat);
        const auto offset = static_cast<std::int64_t>(std::llround(beat * kFramesPerBeat)) - static_cast<std::int64_t>(frame);
        TimedMidiMessage m;
        m.captureTime       = T - std::llround(static_cast<double>(kBlock - offset) * 1e9 / 48000.0);
        m.message.words[0]  = (on ? 0x20900064u : 0x20800000u) | (std::uint32_t{note} << 8);
        m.message.wordCount = 1;
        engine->onMidi(kKeys, m);
    }
    void play(std::uint8_t note, double on, double off) {
        key(true, note, on);
        key(false, note, off);
    }
    // The footswitch (or a button), at the start of the block holding `beat`.
    void command(LooperCommand c, double beat) {
        runTo(beat);
        engine->sendLooperCommand(kLoop, c);
    }
    // What the row heard from the loop, by beat (block i is call i).
    std::vector<Heard> heard() const {
        std::vector<Heard> out;
        for (std::size_t i = 0; i < plugin->calls.size(); ++i)
            for (const Event& e : plugin->calls[i].events)
                out.push_back(Heard{(static_cast<double>(i) * kBlock + e.sampleOffset) / kFramesPerBeat,
                                    ump::isNoteOn(e.midi), ump::noteNumber(e.midi)});
        return out;
    }
    bool heardAt(bool on, std::uint8_t note, double beat) const {
        for (const Heard& h : heard())
            if (h.on == on && h.note == note && std::fabs(h.beat - beat) < 1e-3) return true;
        return false;
    }
    std::size_t heardOns(double from, double to) const {
        std::size_t n = 0;
        for (const Heard& h : heard())
            if (h.on && h.beat >= from - 1e-6 && h.beat < to - 1e-6) ++n;
        return n;
    }
    LooperState state() const { return rt->state.load(); }
};

// Ended by a press, at a bar: armed mid-bar, it records from the next bar; a
// note a little early counts as on the start, one earlier is lost; quantized
// to a sixteenth; the end rounds up to the bar; it plays from there, in time.
void testRecordByPress() {
    CompiledLooper settings;
    settings.atBar    = true;
    settings.quantize = 0.25;
    Rig rig(settings);

    rig.command(LooperCommand::Press, 1.5);
    rig.block();
    CHECK(rig.state() == LooperState::Armed && rig.rt->waiting.load());
    rig.play(62, 3.5, 3.6);    // too early: before the window
    rig.play(60, 3.9, 4.5);    // a little early: on the start, 0.6 long
    rig.play(64, 5.02, 5.52);  // quantized to 5.0 (loop beat 1), 0.5 long
    rig.runTo(4.5);
    CHECK(rig.state() == LooperState::Recording);
    rig.command(LooperCommand::Press, 9.1);   // ends at bar 3 (beat 12): 8 beats
    rig.runTo(22.0);
    CHECK(rig.state() == LooperState::Playing && rig.rt->length.load() == 8.0 && rig.rt->noteCount.load() == 2);
    CHECK(rig.heardOns(0.0, 12.0) == 0);   // nothing from the loop while it recorded
    CHECK(rig.heardAt(true, 60, 12.0) && rig.heardAt(false, 60, 12.6));
    CHECK(rig.heardAt(true, 64, 13.0) && rig.heardAt(false, 64, 13.5));
    CHECK(rig.heardAt(true, 60, 20.0) && rig.heardAt(true, 64, 21.0));   // and again, a loop later
    CHECK(rig.heardOns(0.0, 22.0) == 4);   // 62 never
}

// A fixed length (a bar), at a beat, quantize off: it goes to play by itself,
// with the played timing.
void testFixedLength() {
    CompiledLooper settings;
    settings.atBar       = false;
    settings.quantize    = 0.0;
    settings.fixedLength = 1;
    settings.fixedInBars = true;
    Rig rig(settings);

    rig.command(LooperCommand::Press, 1.3);   // from beat 2, to beat 6
    rig.play(60, 2.13, 2.4);
    rig.runTo(7.0);
    CHECK(rig.state() == LooperState::Playing && rig.rt->length.load() == 4.0);
    CHECK(rig.heardAt(true, 60, 6.13) && rig.heardAt(false, 60, 6.4));
}

// The footswitch while playing: a hold overdubs (from the press, the pass
// kept, heard from the next loop on); undo takes it back; a tap stops at the
// next beat, dropping what the tap might have recorded; a press when stopped
// plays from the start at the next beat.
void testOverdubAndTap() {
    CompiledLooper settings;
    settings.atBar       = false;
    settings.quantize    = 0.0;
    settings.fixedLength = 4;
    settings.fixedInBars = false;
    Rig rig(settings);
    rig.command(LooperCommand::Press, 1.5);   // records beats 2-6, then plays: cycles from 6, 10, 14...
    rig.play(60, 2.0, 2.5);

    rig.command(LooperCommand::Press, 10.0);   // a hold
    rig.play(67, 10.5, 10.7);
    rig.command(LooperCommand::Release, 11.2);   // 1.2 beats: 0.6 s
    rig.runTo(18.0);
    CHECK(rig.state() == LooperState::Playing && rig.rt->passes.load() == 2);
    CHECK(!rig.heardAt(true, 67, 10.5));   // not in the pass that recorded it
    CHECK(rig.heardAt(true, 67, 14.5) && rig.heardAt(false, 67, 14.7) && rig.heardAt(true, 60, 14.0));

    rig.command(LooperCommand::Undo, 18.0);
    rig.runTo(23.0);
    CHECK(rig.rt->passes.load() == 1 && !rig.heardAt(true, 67, 18.5) && rig.heardAt(true, 60, 22.0));

    rig.command(LooperCommand::Press, 23.1);   // a tap
    rig.play(72, 23.15, 23.2);
    rig.command(LooperCommand::Release, 23.2);   // 0.1 beats: 0.05 s
    rig.runTo(23.6);
    CHECK(rig.state() == LooperState::Playing && rig.rt->waiting.load());   // the stop waits for beat 24
    rig.runTo(30.0);
    CHECK(rig.state() == LooperState::Stopped && rig.heardOns(24.0, 30.0) == 0 && rig.rt->noteCount.load() == 1);

    rig.command(LooperCommand::Press, 31.5);   // play again, from beat 32
    rig.runTo(33.0);
    CHECK(rig.state() == LooperState::Playing && rig.heardAt(true, 60, 32.0));
}

// An overdub held across the loop's end: the key held there is cut at the end.
void testOverdubAcrossTheEnd() {
    CompiledLooper settings;
    settings.atBar       = false;
    settings.quantize    = 0.0;
    settings.fixedLength = 4;
    settings.fixedInBars = false;
    Rig rig(settings);
    rig.command(LooperCommand::Press, 1.5);   // beats 2-6; cycles from 6
    rig.play(60, 2.0, 2.5);
    rig.command(LooperCommand::Press, 8.0);
    rig.play(65, 9.5, 10.8);   // held over beat 10 (the loop's end)
    rig.command(LooperCommand::Release, 11.0);
    rig.runTo(15.0);
    CHECK(rig.heardAt(true, 65, 13.5) && rig.heardAt(false, 65, 14.0));   // cut at the end, beat 4 of the loop
}

// Armed: a press cancels. Clear empties a loop. The transport stopping stops
// a playing looper at once, its notes ended.
void testCancelClearAndTransport() {
    CompiledLooper settings;
    settings.atBar    = true;
    settings.quantize = 0.0;
    Rig rig(settings);
    rig.command(LooperCommand::Press, 1.0);
    rig.command(LooperCommand::Press, 2.0);
    rig.block();
    CHECK(rig.state() == LooperState::Empty);

    rig.command(LooperCommand::Press, 2.5);   // from beat 4
    rig.play(60, 4.0, 7.0);
    rig.command(LooperCommand::Press, 5.0);   // to beat 8
    rig.runTo(9.0);
    CHECK(rig.state() == LooperState::Playing && rig.heardAt(true, 60, 8.0));
    rig.engine->sendTransportCommand(Engine::TransportCommand::Stop);
    rig.block();
    rig.block();
    CHECK(rig.state() == LooperState::Stopped && rig.rt->position.load() == 0.0);
    bool ended = false;
    for (const Heard& h : rig.heard()) ended = ended || (!h.on && h.note == 60 && h.beat > 8.0);
    CHECK(ended);

    rig.engine->sendLooperCommand(kLoop, LooperCommand::Clear);
    rig.block();
    CHECK(rig.state() == LooperState::Empty && rig.rt->noteCount.load() == 0);
}

// Armed with the transport stopped: the press starts it, and the recording
// begins on its first beat.
void testArmStartsTheTransport() {
    CompiledLooper settings;
    settings.atBar       = true;
    settings.quantize    = 0.0;
    settings.fixedLength = 1;
    Rig rig(settings, false);
    rig.engine->sendLooperCommand(kLoop, LooperCommand::Press);
    rig.block();
    CHECK(rig.engine->transport().running && rig.state() == LooperState::Recording);
    rig.play(60, 0.5, 1.0);
    rig.runTo(5.0);
    CHECK(rig.rt->length.load() == 4.0 && rig.heardAt(true, 60, 4.5));
}

// The cap: notes past it are dropped and counted.
void testCap() {
    CompiledLooper settings;
    settings.atBar    = false;
    settings.quantize = 0.0;
    Rig rig(settings, true, 2);
    rig.command(LooperCommand::Press, 0.5);
    rig.play(60, 1.0, 1.2);
    rig.play(62, 1.5, 1.7);
    rig.play(64, 2.0, 2.2);
    rig.command(LooperCommand::Press, 2.5);
    rig.runTo(4.0);
    CHECK(rig.rt->noteCount.load() == 2 && rig.rt->dropped.load() == 1);
}

// Convert to sequence (milestone 3): a loop on its quantize grid (64ths when
// off, a unit every two beats for a half-note grid), its notes' starts and
// lengths, protocol, channel and velocities; and the engine's copy of a loop,
// made on the audio thread, with nothing while it's still recording.
void testLoopToSequence() {
    auto on = [](std::uint8_t note, std::uint8_t channel, std::uint8_t velocity) {
        MidiMessage m;
        m.words[0]  = 0x20900000u | (std::uint32_t{channel} << 16) | (std::uint32_t{note} << 8) | velocity;
        m.wordCount = 1;
        return m;
    };
    LooperRuntime::Snapshot loop;
    loop.length = 8.0;
    LooperRuntime::Note a, b;
    a.on = 0.0, a.off = 0.6, a.onMessage = on(60, 1, 100);
    b.on = 1.0, b.off = 1.5, b.onMessage = on(64, 1, 90);
    loop.notes = {b, a};
    const Sequence seq = loopToSequence(loop, 0.25, false);
    CHECK(seq.protocol == MidiProtocol::Midi1 && seq.channel == 1 && seq.rate == (SequenceRate{4, 1}) &&
          seq.length == 32 && seq.launch == LaunchQuantize::Beat && seq.repeat);
    CHECK(seq.notes.size() == 2);
    if (seq.notes.size() == 2) {
        CHECK(seq.notes[0].start == 0 && seq.notes[0].length == 2 && seq.notes[0].pitch == 60 && seq.notes[0].velocity == 100);
        CHECK(seq.notes[1].start == 4 && seq.notes[1].length == 2 && seq.notes[1].pitch == 64 && seq.notes[1].velocity == 90);
    }
    const Sequence fine = loopToSequence(loop, 0.0, true);   // quantize off: 64ths
    CHECK(fine.rate == (SequenceRate{64, 1}) && fine.length == 512 && fine.launch == LaunchQuantize::Bar &&
          fine.notes.size() == 2 && fine.notes[0].length == 38);   // 0.6 beats
    const Sequence coarse = loopToSequence(loop, 2.0, true);   // half notes: a unit every two beats
    CHECK(coarse.rate == (SequenceRate{1, 2}) && coarse.length == 4);

    // Through the engine: a copy of what was recorded; while recording, none.
    CompiledLooper settings;
    settings.atBar       = false;
    settings.quantize    = 0.25;
    settings.fixedLength = 4;
    settings.fixedInBars = false;
    Rig rig(settings);
    rig.command(LooperCommand::Press, 0.5);   // beats 1-5
    rig.play(62, 1.5, 2.0);
    rig.command(LooperCommand::Snapshot, 2.5);
    rig.block();
    LooperRuntime::Snapshot copy;
    CHECK(rig.rt->takeSnapshot(copy) && copy.length == 0.0 && copy.notes.empty());
    CHECK(!rig.rt->takeSnapshot(copy));   // taken once
    rig.command(LooperCommand::Snapshot, 6.0);
    rig.block();
    CHECK(rig.rt->takeSnapshot(copy) && copy.length == 4.0 && copy.notes.size() == 1 &&
          std::fabs(copy.notes[0].on - 0.5) < 1e-9);
}

// Quantize applies as the loop plays, not as it records: changing it moves
// the notes from the next pass, and off brings back the played timing. A note
// played just before the start is on the start whatever the setting, off too.
void testRequantize() {
    CompiledLooper settings;
    settings.atBar       = false;
    settings.quantize    = 0.0;
    settings.fixedLength = 4;
    settings.fixedInBars = false;
    Rig rig(settings);
    rig.command(LooperCommand::Press, 0.5);   // beats 1-5; cycles from 5, 9, 13, 17...
    rig.play(55, 0.9, 1.2);    // just early: on the start, 0.3 long
    rig.play(60, 1.3, 1.6);    // 0.3 into the loop, 0.3 long
    rig.play(62, 3.85, 4.1);   // 2.85 in
    rig.runTo(9.0);
    CHECK(rig.heardAt(true, 60, 5.3) && rig.heardAt(false, 60, 5.6) && rig.heardAt(true, 62, 7.85));
    CHECK(rig.heardAt(true, 55, 5.0) && rig.heardAt(false, 55, 5.3));

    settings.quantize = 0.5;   // eighths: 0.3 -> 0.5, 2.85 -> 3.0; lengths kept
    rig.set(settings);
    rig.runTo(13.0);
    CHECK(rig.heardAt(true, 60, 9.5) && rig.heardAt(false, 60, 9.8) && rig.heardAt(true, 62, 12.0));
    settings.quantize = 1.0;   // beats: 0.3 -> 0, 2.85 -> 3
    rig.set(settings);
    rig.runTo(17.0);
    CHECK(rig.heardAt(true, 60, 13.0) && rig.heardAt(true, 62, 16.0));
    settings.quantize = 0.0;   // off: as played again
    rig.set(settings);
    rig.runTo(21.0);
    CHECK(rig.heardAt(true, 60, 17.3) && rig.heardAt(true, 62, 19.85));
    CHECK(rig.heardAt(true, 55, 9.0) && rig.heardAt(true, 55, 13.0) && rig.heardAt(true, 55, 17.0));   // always the start
}

// Every note exactly once a pass, whatever quantize does mid-pass: a note
// moved later after it played doesn't play again; one moved just behind the
// playhead (a grid step at most) plays at once rather than not at all.
void testExactlyOnce() {
    CompiledLooper settings;
    settings.atBar       = false;
    settings.quantize    = 0.0;
    settings.fixedLength = 4;
    settings.fixedInBars = false;
    Rig rig(settings);
    rig.command(LooperCommand::Press, 0.5);   // beats 1-5; cycles from 5, 9...
    rig.play(60, 1.3, 1.5);    // 0.3 in
    rig.play(62, 3.4, 3.6);    // 2.4 in
    rig.runTo(5.4);            // 60 played at 5.3
    settings.quantize = 0.5;   // 60 -> 0.5 (5.5, ahead): not again; 62 -> 2.5
    rig.set(settings);
    rig.runTo(7.3);
    settings.quantize = 1.0;   // 62 -> 2.0 (7.0, just behind): now
    rig.set(settings);
    rig.runTo(11.5);
    CHECK(rig.heardAt(true, 60, 5.3) && !rig.heardAt(true, 60, 5.5));
    CHECK(rig.heardAt(true, 62, 7.3) && !rig.heardAt(true, 62, 7.5));
    CHECK(rig.heardOns(5.0, 9.0) == 2);
    CHECK(rig.heardAt(true, 60, 9.0) && rig.heardAt(true, 62, 11.0));   // then on the new grid
}

// Fat fingers: two keys a semitone or two apart, starting together, one at
// most half as long as the other: the short one isn't played, nor kept as a
// sequence. A chord of equal notes and a lone short note are kept. Cleanup
// off plays everything.
void testFatFingers() {
    CompiledLooper settings;
    settings.atBar       = false;
    settings.quantize    = 0.25;
    settings.fixedLength = 4;
    settings.fixedInBars = false;
    Rig rig(settings);
    rig.command(LooperCommand::Press, 0.5);   // beats 1-5; cycles from 5, 9...
    rig.key(true, 60, 1.0);
    rig.key(true, 61, 1.03);    // the slip: 0.12 long
    rig.key(false, 61, 1.15);
    rig.key(false, 60, 2.0);
    rig.key(true, 64, 3.0);     // a chord, a tone apart, as long as each other
    rig.key(true, 66, 3.02);
    rig.key(false, 64, 3.5);
    rig.key(false, 66, 3.5);
    rig.play(70, 4.0, 4.1);     // short, but alone
    rig.runTo(9.0);
    CHECK(rig.heardAt(true, 60, 5.0) && rig.heardAt(true, 64, 7.0) && rig.heardAt(true, 66, 7.0) &&
          rig.heardAt(true, 70, 8.0));
    CHECK(rig.heardOns(5.0, 9.0) == 4);   // not 61

    rig.command(LooperCommand::Snapshot, 9.0);
    rig.block();
    LooperRuntime::Snapshot copy;
    CHECK(rig.rt->takeSnapshot(copy) && copy.notes.size() == 5);
    CHECK(loopToSequence(copy, 0.25, false).notes.size() == 4 &&
          loopToSequence(copy, 0.25, false, false).notes.size() == 5);

    settings.cleanup = false;
    rig.set(settings);
    rig.runTo(13.5);
    CHECK(rig.heardAt(true, 61, 13.0) && rig.heardOns(13.0, 13.5) == 2);
}

// Armed while stopped with a count-in (docs/perform.md): the count-in, then
// the recording from bar 1; Stop during the count-in leaves it empty.
void testArmWithCountIn() {
    CompiledLooper settings;
    settings.atBar       = true;
    settings.quantize    = 0.0;
    settings.fixedLength = 1;
    Rig rig(settings, false);
    rig.engine->setCountIn(1.0);   // a bar: beats 0 to 4
    rig.engine->sendLooperCommand(kLoop, LooperCommand::Press);
    rig.block();
    CHECK(!rig.engine->transport().running && rig.engine->transport().countIn > 0 &&
          rig.state() == LooperState::Armed);
    rig.play(62, 2.0, 2.5);   // played during the count-in: not recorded
    rig.runTo(4.0);
    rig.block();
    CHECK(rig.engine->transport().running && rig.state() == LooperState::Recording);
    rig.play(60, 4.5, 5.0);
    rig.runTo(9.0);
    CHECK(rig.rt->length.load() == 4.0 && rig.heardAt(true, 60, 8.5) && rig.rt->noteCount.load() == 1);

    Rig cancelled(settings, false);
    cancelled.engine->setCountIn(2.0);
    cancelled.engine->sendLooperCommand(kLoop, LooperCommand::Press);
    cancelled.block();
    CHECK(cancelled.state() == LooperState::Armed);
    cancelled.engine->sendTransportCommand(Engine::TransportCommand::Stop);
    cancelled.runTo(12.0);
    CHECK(!cancelled.engine->transport().running && cancelled.state() == LooperState::Empty);
}

} // namespace

int main() {
    testRecordByPress();
    testFixedLength();
    testOverdubAndTap();
    testOverdubAcrossTheEnd();
    testCancelClearAndTransport();
    testArmStartsTheTransport();
    testArmWithCountIn();
    testCap();
    testLoopToSequence();
    testRequantize();
    testExactlyOnce();
    testFatFingers();
    return test::finish("Looper");
}
