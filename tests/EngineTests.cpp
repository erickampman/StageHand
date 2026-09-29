// Engine tests with a fake plugin: sample-accurate MIDI placement (one period
// behind), block splitting, routing through filters and dialects, output mixing,
// and the commit / adopt / retire lifecycle. No JACK, ALSA or real plugins.

#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include "Check.h"
#include "Core/Ump.h"
#include "Engine/Engine.h"
#include "FakePlugin.h"

using namespace stagehand;
using stagehand::test::FakePlugin;

namespace {

// Output buffers plus a context pointing at them.
struct Block {
    Block(std::uint32_t frames, double rate, Nanoseconds start, std::uint32_t outputs = 2)
        : samples(outputs, std::vector<float>(frames, 9.0f)), pointers(outputs) {
        for (std::uint32_t c = 0; c < outputs; ++c) pointers[c] = samples[c].data();
        ctx.frameCount      = frames;
        ctx.blockStartTime  = start;
        ctx.time.sampleRate = rate;
        ctx.output          = AudioBuffer{pointers.data(), outputs, frames};
    }
    std::vector<std::vector<float>> samples;
    std::vector<float*>             pointers;
    AudioCallbackContext            ctx;
};

TimedMidiMessage note(Nanoseconds time, std::uint8_t noteNumber, std::uint8_t channel = 0) {
    TimedMidiMessage m;
    m.captureTime       = time;
    m.message.words[0]  = 0x20900064u | (std::uint32_t{channel} << 16) | (std::uint32_t{noteNumber} << 8);
    m.message.wordCount = 1;
    return m;
}

std::unique_ptr<EngineGraph> graphOf(std::vector<SlotConfig> slots) {
    return std::make_unique<EngineGraph>(std::move(slots));
}

SlotConfig slotFor(const std::shared_ptr<FakePlugin>& plugin) {
    SlotConfig slot;
    slot.plugin = plugin;
    return slot;
}

constexpr Nanoseconds T  = 1'000'000'000;   // an arbitrary block start
constexpr Nanoseconds ms = 1'000'000;

void testOnePeriodBehind() {
    auto engine = std::make_unique<Engine>();
    auto plugin = std::make_shared<FakePlugin>();
    engine->commit(graphOf({slotFor(plugin)}));

    // 48 kHz, 480 frames = 10 ms: MIDI captured in [T-10ms, T) plays in this block.
    engine->onMidi(1, note(T - 20 * ms, 59));    // older than the window: late, offset 0
    engine->onMidi(1, note(T - 10 * ms, 60));    // window start: offset 0
    engine->onMidi(1, note(T - 5 * ms, 61));     // mid-window: offset 240
    engine->onMidi(1, note(T - 1, 62));          // just before T: offset 479
    engine->onMidi(1, note(T + 1 * ms, 63));     // at/after T: next block

    Block first(480, 48000.0, T);
    engine->process(first.ctx);
    CHECK(plugin->calls.size() == 1);
    const std::vector<Event>& events = plugin->calls[0].events;
    CHECK(events.size() == 4);
    if (events.size() == 4) {
        CHECK(ump::noteNumber(events[0].midi) == 59 && events[0].sampleOffset == 0);
        CHECK(ump::noteNumber(events[1].midi) == 60 && events[1].sampleOffset == 0);
        CHECK(ump::noteNumber(events[2].midi) == 61 && events[2].sampleOffset == 240);
        CHECK(ump::noteNumber(events[3].midi) == 62 && events[3].sampleOffset == 479);
    }
    CHECK(engine->stats().lateMidi == 1);

    Block second(480, 48000.0, T + 10 * ms);   // window [T, T+10ms): +1 ms = offset 48
    engine->process(second.ctx);
    CHECK(plugin->calls.size() == 2);
    CHECK(plugin->calls[1].events.size() == 1);
    if (plugin->calls[1].events.size() == 1) {
        CHECK(ump::noteNumber(plugin->calls[1].events[0].midi) == 63);
        CHECK(plugin->calls[1].events[0].sampleOffset == 48);
    }
}

void testBlockSplitting() {
    auto engine = std::make_unique<Engine>();
    auto plugin = std::make_shared<FakePlugin>();
    engine->commit(graphOf({slotFor(plugin)}));

    // 1 MHz makes 1 frame = 1000 ns, so offsets are exact.
    const std::uint32_t frames      = kMaxPluginBlockFrames + 100;
    const Nanoseconds   windowStart = T - Nanoseconds{frames} * 1000;
    engine->onMidi(1, note(windowStart + 8200 * 1000 + 500, 64));   // offset 8200

    Block block(frames, 1'000'000.0, T);
    block.ctx.time.samplePosition = 5000;
    engine->process(block.ctx);
    CHECK(plugin->calls.size() == 2);
    if (plugin->calls.size() == 2) {
        CHECK(plugin->calls[0].frames == kMaxPluginBlockFrames && plugin->calls[0].events.empty());
        CHECK(plugin->calls[1].frames == 100);
        CHECK(plugin->calls[1].position == 5000 + kMaxPluginBlockFrames);
        CHECK(plugin->calls[1].events.size() == 1);
        if (!plugin->calls[1].events.empty())
            CHECK(plugin->calls[1].events[0].sampleOffset == 8200 - kMaxPluginBlockFrames);
    }
}

void testRoutingAndDialects() {
    auto engine = std::make_unique<Engine>();
    auto a = std::make_shared<FakePlugin>(2, NoteDialect::Midi | NoteDialect::Midi2);
    auto b = std::make_shared<FakePlugin>(2, NoteDialect::Midi);   // MIDI 1.0 only
    SlotConfig slotA = slotFor(a);
    slotA.midiFilter.channelMask = 0x0001;    // channel 1 only
    SlotConfig slotB = slotFor(b);
    slotB.midiFilter.noteLow = 60;            // split: 60 and up
    engine->commit(graphOf({slotA, slotB}));

    engine->onMidi(1, note(T - 9 * ms, 50, 0));   // ch1 note 50: A yes, B no (below split)
    engine->onMidi(1, note(T - 8 * ms, 70, 1));   // ch2 note 70: A no (channel), B yes
    TimedMidiMessage midi2;                        // MIDI 2.0 ch1 note 72
    midi2.captureTime       = T - 7 * ms;
    midi2.message.words[0]  = 0x40904800;
    midi2.message.words[1]  = 0x80000000;
    midi2.message.wordCount = 2;
    engine->onMidi(1, midi2);                     // A as-is; B downconverted (MIDI 1.0 only)

    Block block(480, 48000.0, T);
    engine->process(block.ctx);
    CHECK(a->calls.size() == 1 && a->calls[0].events.size() == 2);
    CHECK(b->calls.size() == 1 && b->calls[0].events.size() == 2);
    if (b->calls.size() == 1 && b->calls[0].events.size() == 2) {
        CHECK(ump::noteNumber(b->calls[0].events[0].midi) == 70);
        CHECK(b->calls[0].events[1].midi.words[0] == 0x20904840);   // note 72, vel 0x8000 -> 64
    }
    CHECK(engine->stats().unsupportedDialect == 0);
}

// Each slot gets each message in a form its note port takes.
void testTranslation() {
    auto engine = std::make_unique<Engine>();
    auto midi1  = std::make_shared<FakePlugin>(2, NoteDialect::Midi);
    auto midi2  = std::make_shared<FakePlugin>(2, NoteDialect::Midi2);
    engine->commit(graphOf({slotFor(midi1), slotFor(midi2)}));

    engine->onMidi(1, note(T - 9 * ms, 60));      // MIDI 1.0 note-on, vel 100
    TimedMidiMessage rpn;                          // MIDI 2.0 RPN 0:0 = 2 semitones
    rpn.captureTime       = T - 8 * ms;
    rpn.message.words[0]  = 0x40200000;
    rpn.message.words[1]  = 0x04000000;
    rpn.message.wordCount = 2;
    engine->onMidi(1, rpn);
    TimedMidiMessage cc101 = note(T - 7 * ms, 0);  // MIDI 1.0 CC 101 = 0
    cc101.message.words[0] = 0x20B06500;
    engine->onMidi(1, cc101);

    Block block(480, 48000.0, T);
    engine->process(block.ctx);

    // MIDI 1.0 plugin: note as-is, RPN expanded to CC 101/100/6/38, CC 101 as-is.
    CHECK(midi1->calls.size() == 1 && midi1->calls[0].events.size() == 6);
    if (midi1->calls.size() == 1 && midi1->calls[0].events.size() == 6) {
        const std::vector<Event>& e = midi1->calls[0].events;
        CHECK(e[0].midi.words[0] == 0x20903C64);
        CHECK(e[1].midi.words[0] == 0x20B06500 && e[4].midi.words[0] == 0x20B02600);
        CHECK(e[1].sampleOffset == e[4].sampleOffset);   // an expansion shares its offset
        CHECK(e[5].midi.words[0] == 0x20B06500);
    }
    // MIDI 2.0 plugin: note upconverted, RPN as-is, CC 101 absorbed (nothing).
    CHECK(midi2->calls.size() == 1 && midi2->calls[0].events.size() == 2);
    if (midi2->calls.size() == 1 && midi2->calls[0].events.size() == 2) {
        CHECK(midi2->calls[0].events[0].midi.words[0] == 0x40903C00);
        CHECK(midi2->calls[0].events[0].midi.words[1] == 0xC9240000);
        CHECK(midi2->calls[0].events[1].midi.words[0] == 0x40200000);
    }
    CHECK(engine->stats().unsupportedDialect == 0);
}

// A note-off goes wherever its note-on went, at the note each plugin got, even
// after the graph (filters, transposes) changed in between.
void testHeldNotes() {
    auto engine = std::make_unique<Engine>();
    auto low    = std::make_shared<FakePlugin>();
    auto high   = std::make_shared<FakePlugin>();
    SlotConfig lowSlot  = slotFor(low);
    lowSlot.midiFilter.noteHigh = 59;
    SlotConfig highSlot = slotFor(high);
    highSlot.midiFilter.noteLow   = 60;
    highSlot.midiFilter.transpose = 2;
    engine->commit(graphOf({lowSlot, highSlot}));

    engine->onMidi(1, note(T - 5 * ms, 60));      // plays `high`, as note 62
    Block first(480, 48000.0, T);
    engine->process(first.ctx);
    CHECK(high->calls.size() == 1 && high->calls[0].events.size() == 1);
    CHECK(low->calls.size() == 1 && low->calls[0].events.empty());

    // Move the split and change the transpose while the key is down.
    SlotConfig lowAll  = slotFor(low);             // now takes every note
    SlotConfig highNow = slotFor(high);
    highNow.midiFilter.channelMask = 0;            // now hears nothing
    highNow.midiFilter.transpose   = 5;
    engine->commit(graphOf({lowAll, highNow}));

    TimedMidiMessage off = note(T + 5 * ms, 60);   // MIDI 1.0 note-off (vel 0)
    off.message.words[0] = 0x20903C00;
    engine->onMidi(1, off);
    Block second(480, 48000.0, T + 10 * ms);
    engine->process(second.ctx);

    CHECK(high->calls.size() == 2 && high->calls[1].events.size() == 1);   // the note-off still arrives
    if (high->calls.size() == 2 && high->calls[1].events.size() == 1) {
        CHECK(ump::isNoteOff(high->calls[1].events[0].midi));
        CHECK(ump::noteNumber(high->calls[1].events[0].midi) == 62);      // the note it got, not 65
    }
    CHECK(low->calls.size() == 2 && low->calls[1].events.empty());         // and nowhere else
}

void testTransposeKeepsPitchAttribute() {
    auto engine = std::make_unique<Engine>();
    auto plugin = std::make_shared<FakePlugin>();
    SlotConfig slot = slotFor(plugin);
    slot.midiFilter.transpose = 2;
    engine->commit(graphOf({slot}));

    TimedMidiMessage m;   // MIDI 2.0 note 61 with pitch 7.9 = 61.5
    m.captureTime       = T - 5 * ms;
    m.message.words[0]  = 0x40903D03;
    m.message.words[1]  = 0x80007B00;
    m.message.wordCount = 2;
    engine->onMidi(1, m);

    Block block(480, 48000.0, T);
    engine->process(block.ctx);
    CHECK(plugin->calls.size() == 1 && plugin->calls[0].events.size() == 1);
    if (!plugin->calls.empty() && plugin->calls[0].events.size() == 1) {
        const MidiMessage& out = plugin->calls[0].events[0].midi;
        CHECK(ump::noteNumber(out) == 63);
        CHECK((out.words[1] & 0xFFFF) == ((63u << 9) | 256u));   // 63.5
    }
}

void testMixing() {
    auto engine = std::make_unique<Engine>();
    auto loud   = std::make_shared<FakePlugin>(2, NoteDialect::Midi, 1.0f);
    auto quiet  = std::make_shared<FakePlugin>(2, NoteDialect::Midi, 0.25f);
    auto mono   = std::make_shared<FakePlugin>(1, NoteDialect::Midi, 0.5f);
    SlotConfig a = slotFor(loud);
    a.runtime    = std::make_shared<SlotRuntime>(0.5f);
    SlotConfig b = slotFor(quiet);
    SlotConfig c = slotFor(mono);   // mono feeds outputs 1 and 2
    engine->commit(graphOf({a, b, c}));

    Block block(64, 48000.0, T);   // buffers start at 9.0: the engine must clear them
    engine->process(block.ctx);
    CHECK(block.samples[0][0] == 1.25f && block.samples[0][63] == 1.25f);   // 0.5 + 0.25 + 0.5
    CHECK(block.samples[1][0] == 1.25f);
    const LevelMeter::Reading master = engine->outputMeter().take();
    CHECK(master.peak[0] == 1.25f && master.peak[1] == 1.25f && master.clipped);
    CHECK(engine->outputMeter().take().peak[0] == 0.0f);   // reset by the first take()
}

// Feed `frames` more frames through the engine in blocks of 64; append output 1.
void run(Engine& engine, std::uint32_t frames, std::vector<float>& out) {
    for (std::uint32_t done = 0; done < frames; done += 64) {
        Block block(64, 48000.0, T);
        engine.process(block.ctx);
        CHECK(block.samples[0] == block.samples[1]);
        out.insert(out.end(), block.samples[0].begin(), block.samples[0].end());
    }
}

void testGainRamp() {
    auto engine  = std::make_unique<Engine>();
    auto plugin  = std::make_shared<FakePlugin>(2, NoteDialect::Midi, 1.0f);
    auto runtime = std::make_shared<SlotRuntime>(1.0f);
    SlotConfig slot = slotFor(plugin);
    slot.runtime    = runtime;
    engine->commit(graphOf({slot}));

    std::vector<float> out;
    run(*engine, 64, out);
    CHECK(out.front() == 1.0f && out.back() == 1.0f);   // steady: no ramp

    // 1 -> 0: 10 ms at 48 kHz = 480 samples, bounded steps, landing exactly on 0.
    constexpr float step = 1.0f / 480.0f;
    out.clear();
    runtime->targetGain.store(0.0f);
    run(*engine, 512, out);
    CHECK(out[0] < 1.0f && out[0] > 1.0f - 1.5f * step);
    bool bounded = true;
    for (std::size_t f = 1; f < out.size(); ++f)
        bounded = bounded && out[f] <= out[f - 1] && out[f - 1] - out[f] <= 1.5f * step;
    CHECK(bounded);
    CHECK(out[478] > 0.0f && out[479] == 0.0f && out.back() == 0.0f);

    // Retarget mid-ramp: 0 -> 1, then after half of it (at ~0.5) back to 0.25.
    // The new ramp starts where the gain is and still takes 480 samples.
    out.clear();
    runtime->targetGain.store(1.0f);
    run(*engine, 256, out);   // 256 of 480 samples: ~0.53
    const float reached = out.back();
    CHECK(reached > 0.5f && reached < 0.55f);
    runtime->targetGain.store(0.25f);
    out.clear();
    run(*engine, 512, out);
    CHECK(out[0] < reached && reached - out[0] < 1.5f * step);
    CHECK(out[478] != 0.25f && out[479] == 0.25f && out.back() == 0.25f);

    // The ramp survives a commit: a new graph with the same runtime picks up
    // mid-ramp instead of jumping.
    runtime->targetGain.store(1.0f);
    out.clear();
    run(*engine, 128, out);
    const float before = out.back();
    engine->commit(graphOf({slot}));
    out.clear();
    run(*engine, 64, out);
    CHECK(out[0] > before && out[0] - before < 1.5f * step);
    engine->collect();
}

void testSlotMeters() {
    auto engine = std::make_unique<Engine>();
    auto stereo = std::make_shared<FakePlugin>(2, NoteDialect::Midi, 1.0f);
    auto mono   = std::make_shared<FakePlugin>(1, NoteDialect::Midi, 0.5f);
    auto muted  = std::make_shared<FakePlugin>(2, NoteDialect::Midi, 1.0f);
    SlotConfig a = slotFor(stereo);
    SlotConfig b = slotFor(mono);
    SlotConfig c = slotFor(muted);
    a.runtime    = std::make_shared<SlotRuntime>(0.5f);
    b.runtime    = std::make_shared<SlotRuntime>(3.0f);   // 1.5 out: clips
    c.runtime    = std::make_shared<SlotRuntime>(0.0f);
    a.midiFilter.noteHigh = 59;                           // only b and c hear note 60
    engine->commit(graphOf({a, b, c}));

    engine->onMidi(1, note(T - 5 * ms, 60));
    Block block(64, 48000.0, T);
    engine->process(block.ctx);
    CHECK(block.samples[0][0] == 2.0f && block.samples[1][63] == 2.0f);   // 0.5 + 1.5 + 0

    LevelMeter::Reading ra = a.runtime->meter.take();
    LevelMeter::Reading rb = b.runtime->meter.take();
    LevelMeter::Reading rc = c.runtime->meter.take();
    CHECK(ra.peak[0] == 0.5f && ra.peak[1] == 0.5f && !ra.clipped);
    CHECK(rb.peak[0] == 1.5f && rb.peak[1] == 1.5f && rb.clipped);   // mono shows on both
    CHECK(rc.peak[0] == 0.0f && rc.peak[1] == 0.0f);                 // muted: silent
    CHECK(a.runtime->midiEvents.load() == 0);
    CHECK(b.runtime->midiEvents.load() == 1 && c.runtime->midiEvents.load() == 1);

    rb = b.runtime->meter.take();   // peaks reset; the clip stays latched...
    CHECK(rb.peak[0] == 0.0f && rb.clipped);
    b.runtime->meter.clearClip();   // ...until cleared
    CHECK(!b.runtime->meter.take().clipped);
}

// The free-running beat clock: what plugins see, block by block and chunk by
// chunk, through tempo and time-signature changes.
void testTransport() {
    auto engine = std::make_unique<Engine>();
    auto plugin = std::make_shared<FakePlugin>();
    engine->commit(graphOf({slotFor(plugin)}));

    // 120 BPM at 48 kHz: 2 beats a second, so 480 frames = 0.02 beats.
    for (int i = 0; i < 3; ++i) {
        Block block(480, 48000.0, T);
        engine->process(block.ctx);
    }
    CHECK(plugin->calls.size() == 3);
    const TimeInfo& third = plugin->calls[2].time;
    CHECK(third.playing && third.tempoBpm == 120.0 && third.timeSigNumerator == 4);
    CHECK(std::fabs(third.beatPosition - 0.04) < 1e-9);
    CHECK(std::fabs(engine->transport().beatPosition - 0.06) < 1e-9);

    // A split block: each chunk starts where it starts. 48000 frames = 2 beats,
    // in chunks of 8192 frames (0.341333 beats each).
    plugin->calls.clear();
    Block big(48000, 48000.0, T);
    engine->process(big.ctx);
    CHECK(plugin->calls.size() == 6);
    if (plugin->calls.size() == 6) {
        CHECK(std::fabs(plugin->calls[0].time.beatPosition - 0.06) < 1e-9);
        CHECK(std::fabs(plugin->calls[1].time.beatPosition - (0.06 + 8192.0 / 24000.0)) < 1e-9);
        CHECK(plugin->calls[1].time.samplePosition == 8192);
    }
    CHECK(std::fabs(engine->transport().beatPosition - 2.06) < 1e-9);

    // 60 BPM from the next block: 48000 frames = 1 beat. Bars of 4 beats.
    engine->setTempo(60.0);
    plugin->calls.clear();
    for (int i = 0; i < 3; ++i) {   // to beat 5.06: bar 1, which started at 4
        Block block(48000, 48000.0, T);
        engine->process(block.ctx);
    }
    Engine::Transport now = engine->transport();
    CHECK(now.tempoBpm == 60.0 && std::fabs(now.beatPosition - 5.06) < 1e-9);
    CHECK(now.barNumber == 1 && now.barStartBeats == 4.0);

    // 3/4 from bar 1's start (beat 4): bar 2 begins at beat 7, not 8.
    engine->setTimeSignature(3, 4);
    for (int i = 0; i < 2; ++i) {   // to beat 7.06
        Block block(48000, 48000.0, T);
        engine->process(block.ctx);
    }
    now = engine->transport();
    CHECK(now.numerator == 3 && now.barNumber == 2 && std::fabs(now.barStartBeats - 7.0) < 1e-9);

    // 6/8: bars of 3 quarter-note beats. Invalid signatures and tempos are
    // refused or clamped.
    engine->setTimeSignature(6, 8);
    engine->setTimeSignature(5, 6);    // not a power of two: ignored
    engine->setTimeSignature(0, 4);    // ignored
    engine->setTempo(1000.0);          // clamped
    now = engine->transport();
    CHECK(now.numerator == 6 && now.denominator == 8 && now.tempoBpm == Engine::kMaxTempo);
    engine->collect();
}

void testGraphLifecycle() {
    auto engine = std::make_unique<Engine>();
    auto first  = std::make_shared<FakePlugin>();
    auto second = std::make_shared<FakePlugin>();

    engine->commit(graphOf({slotFor(first)}));
    CHECK(!engine->committedGraphAdopted());
    Block block(64, 48000.0, T);
    engine->process(block.ctx);
    CHECK(engine->committedGraphAdopted());
    CHECK(first->processing);

    // Swap: `first` leaves, so the audio thread stops it before retiring the graph.
    engine->commit(graphOf({slotFor(second)}));
    CHECK(!engine->committedGraphAdopted());
    engine->process(block.ctx);
    CHECK(engine->committedGraphAdopted());
    CHECK(first->stops == 1 && !first->processing);
    CHECK(second->processing);
    CHECK(first.use_count() == 2);        // the retired graph still holds it...
    CHECK(engine->collect() == 1);
    CHECK(first.use_count() == 1);        // ...until the main thread collects it

    // A plugin present in both graphs keeps processing.
    engine->commit(graphOf({slotFor(second)}));
    engine->process(block.ctx);
    CHECK(second->stops == 0);

    // Shutdown: run nothing.
    engine->commit(nullptr);
    engine->process(block.ctx);
    CHECK(engine->committedGraphAdopted());
    CHECK(second->stops == 1);
    CHECK(engine->collect() == 2);

    // Two commits before the audio thread runs: the first never runs.
    auto skipped = std::make_shared<FakePlugin>();
    engine->commit(graphOf({slotFor(skipped)}));
    engine->commit(nullptr);
    CHECK(skipped.use_count() == 1);      // destroyed at the second commit
    engine->process(block.ctx);
    CHECK(skipped->calls.empty());
}

} // namespace

int main() {
    testOnePeriodBehind();
    testBlockSplitting();
    testRoutingAndDialects();
    testTranslation();
    testHeldNotes();
    testTransposeKeepsPitchAttribute();
    testMixing();
    testGainRamp();
    testSlotMeters();
    testTransport();
    testGraphLifecycle();
    return test::finish("Engine");
}
