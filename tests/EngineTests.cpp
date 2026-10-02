// Engine tests with a fake plugin: sample-accurate MIDI placement (one period
// behind), block splitting, routing through filters and dialects, output mixing,
// and the commit / adopt / retire lifecycle. No JACK, ALSA or real plugins.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include "Check.h"
#include "Core/Ump.h"
#include "Engine/Engine.h"
#include "FakePlugin.h"

using namespace stagehand;
using stagehand::test::FakeEffect;
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

// Mute: the plugin's held notes end at once, new note-ons don't reach it, and
// once it's quiet (or after kMaxMutedSeconds) it isn't run, except for a block
// that brings it events. Unmuting resumes it.
void testMute() {
    auto engine  = std::make_unique<Engine>();
    auto loud    = std::make_shared<FakePlugin>(2, NoteDialect::Midi, 1.0f);   // never quiet
    auto quiet   = std::make_shared<FakePlugin>(2, NoteDialect::Midi, 0.0f);
    SlotConfig a = slotFor(loud), b = slotFor(quiet);
    a.runtime    = std::make_shared<SlotRuntime>(1.0f);
    b.runtime    = std::make_shared<SlotRuntime>(1.0f);
    engine->commit(graphOf({a, b}));
    auto block = [&] {
        Block blk(64, 48000.0, T);
        engine->process(blk.ctx);
    };
    auto mute = [](SlotConfig& s, bool on) {
        s.runtime->targetGain.store(on ? 0.0f : 1.0f);
        s.runtime->muted.store(on);
    };

    engine->onMidi(1, note(T - 5 * ms, 60));
    block();
    CHECK(loud->calls.back().events.size() == 1);   // 60 is held

    // Muted: a note-off for 60 at the block's start, MIDI 1.0 as the port takes it.
    mute(a, true);
    mute(b, true);
    loud->calls.clear();
    block();
    CHECK(loud->calls.size() == 1 && loud->calls[0].events.size() == 1);
    if (!loud->calls.empty() && !loud->calls[0].events.empty()) {
        const MidiMessage& off = loud->calls[0].events[0].midi;
        CHECK(ump::isNoteOff(off) && ump::noteNumber(off) == 60 && loud->calls[0].events[0].sampleOffset == 0);
        CHECK(ump::messageType(off) == ump::MessageType::Midi1ChannelVoice);
    }
    CHECK(quiet->calls.back().events.size() == 1);   // it held 60 too

    // A note-on while muted reaches neither.
    loud->calls.clear();
    quiet->calls.clear();
    engine->onMidi(1, note(T - 5 * ms, 62));
    block();
    CHECK(loud->calls.size() == 1 && loud->calls[0].events.empty());
    CHECK(quiet->calls.size() == 1 && quiet->calls[0].events.empty());

    // The quiet one stops being run after 0.2 s (plus the 10 ms fade); the loud
    // one only after 5 s.
    for (int i = 0; i < 48000 * 3 / 10 / 64; ++i) block();   // 0.3 s
    quiet->calls.clear();
    block();
    CHECK(quiet->calls.empty() && quiet->stops == 1 && !quiet->processing);
    CHECK(loud->stops == 0);
    for (int i = 0; i < 48000 * 5 / 64; ++i) block();
    loud->calls.clear();
    block();
    CHECK(loud->calls.empty() && loud->stops == 1);

    // Suspended, it still runs for a block that brings it something (a sustain
    // pedal), then stops again.
    TimedMidiMessage pedal;
    pedal.captureTime       = T - 5 * ms;
    pedal.message.words[0]  = 0x20B04000u;   // CC 64, value 0
    pedal.message.wordCount = 1;
    engine->onMidi(1, pedal);
    block();
    CHECK(quiet->calls.size() == 1 && quiet->calls[0].events.size() == 1 && quiet->stops == 2);

    // Unmuted: run again, and notes reach it.
    mute(b, false);
    quiet->calls.clear();
    engine->onMidi(1, note(T - 5 * ms, 64));
    block();
    CHECK(quiet->calls.size() == 1 && quiet->calls[0].events.size() == 1 && quiet->processing);

    engine->commit(nullptr);
    block();
    engine->collect();
}

// Draining (docs/setlists.md, section 3): a slot of the rack switched away
// from hears no new note-ons, but a held key's note-off and the pedal still
// reach it; its gain stays; it's finished once quiet (0.2 s) or after 8 s. A
// slot new in a graph is sent the pedals held down (edge case 2).
void testDraining() {
    auto engine = std::make_unique<Engine>();
    auto loud   = std::make_shared<FakePlugin>(2, NoteDialect::Midi, 1.0f);    // never quiet
    auto quiet  = std::make_shared<FakePlugin>(2, NoteDialect::Midi, 0.0f);
    auto next   = std::make_shared<FakePlugin>(2, NoteDialect::Midi2, 0.5f);   // the next song: MIDI 2.0
    SlotConfig a = slotFor(loud), b = slotFor(quiet), c = slotFor(next);
    a.runtime    = std::make_shared<SlotRuntime>(1.0f);
    b.runtime    = std::make_shared<SlotRuntime>(1.0f);
    c.runtime    = std::make_shared<SlotRuntime>(1.0f);
    engine->commit(graphOf({a, b}));
    std::vector<float> last(2);
    auto block = [&] {
        Block blk(64, 48000.0, T);
        engine->process(blk.ctx);
        last = {blk.samples[0].back(), blk.samples[1].back()};
    };
    auto cc = [](std::uint8_t controller, std::uint8_t value) {
        TimedMidiMessage m;
        m.captureTime       = T - 5 * ms;
        m.message.words[0]  = 0x20B00000u | (std::uint32_t{controller} << 8) | value;
        m.message.wordCount = 1;
        return m;
    };

    engine->onMidi(1, note(T - 5 * ms, 60));   // held across the switch
    engine->onMidi(1, cc(64, 127));            // sustain down
    engine->onMidi(1, cc(1, 90));              // the mod wheel: not a pedal
    block();

    // The switch: one graph with the next song's slot and the old ones draining.
    a.draining = b.draining = true;
    engine->commit(graphOf({c, a, b}));
    loud->calls.clear();
    engine->onMidi(1, note(T - 5 * ms, 62));   // a new note: the next song's only
    block();
    CHECK(next->calls.size() == 1 && next->calls[0].events.size() == 2);
    if (next->calls.size() == 1 && next->calls[0].events.size() == 2) {
        const MidiMessage& pedal = next->calls[0].events[0].midi;   // first, at the block's start
        CHECK(ump::messageType(pedal) == ump::MessageType::Midi2ChannelVoice &&
              ump::status(pedal) == ump::Status::ControlChange && ((pedal.words[0] >> 8) & 0x7F) == 64);
        CHECK(next->calls[0].events[0].sampleOffset == 0 && ump::isNoteOn(next->calls[0].events[1].midi));
    }
    CHECK(loud->calls.size() == 1 && loud->calls[0].events.empty());
    CHECK(last[0] == 1.5f);   // its gain kept: the old slot rings at full level

    // The held key's note-off and the pedal-up reach the old patch; the
    // pedal-up reaches the new one too, which only learns of the pedal once.
    loud->calls.clear();
    next->calls.clear();
    TimedMidiMessage off = note(T - 5 * ms, 60);
    off.message.words[0] = 0x20803C00u;
    engine->onMidi(1, off);
    engine->onMidi(1, cc(64, 0));
    block();
    CHECK(loud->calls.size() == 1 && loud->calls[0].events.size() == 2);
    if (loud->calls.size() == 1 && loud->calls[0].events.size() == 2) {
        CHECK(ump::isNoteOff(loud->calls[0].events[0].midi) && ump::noteNumber(loud->calls[0].events[0].midi) == 60);
        CHECK(ump::status(loud->calls[0].events[1].midi) == ump::Status::ControlChange);
    }
    CHECK(next->calls.size() == 1 && next->calls[0].events.size() == 1);   // the pedal-up only

    // The quiet one finishes after 0.2 s; the loud one only at the 8 s cap.
    for (int i = 0; i < 48000 * 3 / 10 / 64; ++i) block();   // 0.3 s
    CHECK(b.runtime->drained.load() && quiet->stops == 1 && !quiet->processing);
    CHECK(!a.runtime->drained.load() && loud->stops == 0);
    for (int i = 0; i < 48000 * 8 / 64; ++i) block();
    CHECK(a.runtime->drained.load() && loud->stops == 1);
    loud->calls.clear();
    block();
    CHECK(loud->calls.empty());   // not run any more

    // A draining slot muted (a further switch) finishes through the mute path.
    auto other = std::make_shared<FakePlugin>(2, NoteDialect::Midi, 0.0f);
    SlotConfig d = slotFor(other);
    d.runtime    = std::make_shared<SlotRuntime>(1.0f);
    d.draining   = true;
    engine->commit(graphOf({c, d}));
    block();
    d.runtime->targetGain.store(0.0f);
    d.runtime->muted.store(true);
    for (int i = 0; i < 48000 * 3 / 10 / 64; ++i) block();
    CHECK(d.runtime->drained.load() && other->stops == 1);

    engine->commit(nullptr);
    block();
    engine->collect();
}

// The setlist's triggers (docs/setlists.md, section 4): matched before the
// rack's (a transport trigger on the same pedal doesn't act), passed to the
// main thread, and going nowhere else; Program Change carries its number.
void testSetlistTriggers() {
    auto engine = std::make_unique<Engine>();
    auto plugin = std::make_shared<FakePlugin>(2, NoteDialect::Midi);
    engine->commit(graphOf({slotFor(plugin)}));
    TransportControl control;
    control.triggers.push_back(CompiledSequence::Trigger{1, SequenceTrigger::Action::Start,
                                                         SequenceTrigger::Control::Controller, 0, 0, 80});
    control.setlist.push_back(CompiledSetlistTrigger{1, SetlistTrigger::Action::Next,
                                                     SequenceTrigger::Control::Controller, 0, 0, 80});
    control.setlist.push_back(CompiledSetlistTrigger{kInvalidMidiSource, SetlistTrigger::Action::Song,
                                                     SequenceTrigger::Control::Note, 0, 15, 0});
    engine->commitSequences(std::make_unique<SequenceSet>(std::vector<CompiledSequence>{}, control));
    auto send = [&](std::uint32_t word) {
        TimedMidiMessage m;
        m.captureTime       = T - 5 * ms;
        m.message.words[0]  = word;
        m.message.wordCount = 1;
        engine->onMidi(1, m);
        Block blk(64, 48000.0, T);
        engine->process(blk.ctx);
    };

    send(0x20B0507Fu);   // CC 80 pressed
    Engine::SetlistRequest request;
    CHECK(engine->takeSetlistRequest(request) && request.action == SetlistTrigger::Action::Next);
    CHECK(!engine->takeSetlistRequest(request));
    CHECK(!engine->transport().running);   // the rack's Start on the same pedal didn't act
    send(0x20B05000u);   // released: still the setlist's, nothing fires
    CHECK(!engine->takeSetlistRequest(request));
    send(0x20CF0400u);   // Program Change 4 (the fifth) on channel 16
    CHECK(engine->takeSetlistRequest(request) && request.action == SetlistTrigger::Action::Song && request.program == 4);
    send(0x20C00400u);   // on channel 1: not a trigger, so a slot hears it
    CHECK(!engine->takeSetlistRequest(request));
    std::size_t heard = 0;
    for (const FakePlugin::Call& call : plugin->calls) heard += call.events.size();
    CHECK(heard == 1);

    // Learning a Program Change ignores presses.
    engine->armLearn(true);
    send(0x20B0507Fu);
    Engine::Learned learned;
    CHECK(!engine->takeLearned(learned));
    send(0x20C10700u);
    CHECK(engine->takeLearned(learned) && ump::status(learned.message) == ump::Status::ProgramChange);

    engine->commit(nullptr);
    Block blk(64, 48000.0, T);
    engine->process(blk.ctx);
    engine->collect();
}

// The rig's buses (docs/rig.md): post-fader sends, effects in series (an empty
// slot passes audio on), the return level and output pair, the master, and
// bus plugins kept across commits. A slot outputs a constant, so after the
// 10 ms ramps every sample is the same.
void testBuses() {
    // Run 2048 frames (past every ramp) in 64-frame blocks with `outputs`
    // outputs; the last block's samples.
    auto settle = [](Engine& engine, std::uint32_t outputs = 2) {
        std::vector<std::vector<float>> last;
        for (int i = 0; i < 32; ++i) {
            Block block(64, 48000.0, T, outputs);
            engine.process(block.ctx);
            last = block.samples;
        }
        return last;
    };
    auto bus = [](std::shared_ptr<HostedPlugin> first, std::shared_ptr<HostedPlugin> second, float level = 1.0f,
                  std::uint32_t firstOutput = 0) {
        BusConfig b;
        b.effects[0]  = std::move(first);
        b.effects[1]  = std::move(second);
        b.runtime     = std::make_shared<SlotRuntime>(level);
        b.firstOutput = firstOutput;
        return b;
    };

    auto engine  = std::make_unique<Engine>();
    auto synth   = std::make_shared<FakePlugin>(2, NoteDialect::Midi, 1.0f);   // outputs 1.0
    auto doubler = std::make_shared<FakeEffect>(2.0f);
    auto tripler = std::make_shared<FakeEffect>(3.0f);
    SlotConfig slot = slotFor(synth);
    slot.runtime    = std::make_shared<SlotRuntime>(1.0f);
    slot.runtime->targetSend[0].store(0.5f);
    engine->commit(std::make_unique<EngineGraph>(std::vector<SlotConfig>{slot},
                                                 std::vector<BusConfig>{bus(doubler, nullptr), bus(tripler, nullptr)}));

    // Dry 1.0, plus 0.5 sent to A and doubled: 2.0. B gets nothing (its send is off).
    auto out = settle(*engine);
    CHECK(std::fabs(out[0][63] - 2.0f) < 1e-5f && std::fabs(out[1][63] - 2.0f) < 1e-5f);
    CHECK(doubler->lastInputChannels == 2 && tripler->processed > 0);

    // After the fader: at gain 0.5, the dry half and the send half: 0.5 + 0.5 * 0.5 * 2.
    slot.runtime->targetGain.store(0.5f);
    out = settle(*engine);
    CHECK(std::fabs(out[0][63] - 1.0f) < 1e-5f);

    // Muted: nothing dry and nothing sent.
    slot.runtime->targetGain.store(0.0f);
    out = settle(*engine);
    CHECK(out[0][63] == 0.0f && out[1][63] == 0.0f);
    slot.runtime->targetGain.store(1.0f);

    // Both sends: 1 + 0.5 * 2 + 0.25 * 3.
    slot.runtime->targetSend[1].store(0.25f);
    out = settle(*engine);
    CHECK(std::fabs(out[0][63] - 2.75f) < 1e-5f);

    // The master scales it all.
    engine->setMasterGain(0.5f);
    out = settle(*engine);
    CHECK(std::fabs(out[0][63] - 1.375f) < 1e-5f);
    engine->setMasterGain(1.0f);

    // In series (2 then 3), an empty first slot passing audio on, a return at
    // half level, and a return on outputs 3-4. Same plugins: kept across the
    // commit, never stopped.
    slot.runtime->targetSend[1].store(0.0f);
    engine->commit(std::make_unique<EngineGraph>(
        std::vector<SlotConfig>{slot}, std::vector<BusConfig>{bus(doubler, tripler, 0.5f), bus(nullptr, nullptr)}));
    out = settle(*engine);
    CHECK(std::fabs(out[0][63] - (1.0f + 0.5f * 6.0f * 0.5f)) < 1e-5f);   // 2.5
    engine->collect();
    CHECK(doubler->stops == 0 && tripler->stops == 0);

    slot.runtime->targetSend[0].store(0.0f);
    slot.runtime->targetSend[1].store(1.0f);   // B is empty: what's sent comes back as is
    out = settle(*engine);
    CHECK(std::fabs(out[0][63] - 2.0f) < 1e-5f);

    engine->commit(std::make_unique<EngineGraph>(std::vector<SlotConfig>{slot},
                                                 std::vector<BusConfig>{bus(doubler, nullptr, 1.0f, 2), bus(nullptr, nullptr)}));
    slot.runtime->targetSend[0].store(1.0f);
    slot.runtime->targetSend[1].store(0.0f);
    out = settle(*engine, 4);
    CHECK(std::fabs(out[0][63] - 1.0f) < 1e-5f && std::fabs(out[2][63] - 2.0f) < 1e-5f &&
          std::fabs(out[3][63] - 2.0f) < 1e-5f);
    engine->collect();
    CHECK(tripler->stops == 1 && doubler->stops == 0);   // tripler left the rig

    // A mono effect input takes the left; a mono output feeds both sides.
    auto mono = std::make_shared<FakeEffect>(1.0f, 1, 1);
    engine->commit(std::make_unique<EngineGraph>(std::vector<SlotConfig>{slot}, std::vector<BusConfig>{bus(mono, nullptr)}));
    out = settle(*engine);
    CHECK(mono->lastInputChannels == 1 && std::fabs(out[0][63] - 2.0f) < 1e-5f && std::fabs(out[1][63] - 2.0f) < 1e-5f);

    engine->commit(nullptr);
    settle(*engine);
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

// The beat clock: stopped until Start, then what plugins see, block by block
// and chunk by chunk, through tempo and time-signature changes; Stop returns
// to beat 0.
void testTransport() {
    auto engine = std::make_unique<Engine>();
    auto plugin = std::make_shared<FakePlugin>();
    engine->commit(graphOf({slotFor(plugin)}));

    // Stopped: beat 0, not playing, however long it runs.
    for (int i = 0; i < 2; ++i) {
        Block block(480, 48000.0, T);
        engine->process(block.ctx);
    }
    CHECK(!plugin->calls[1].time.playing && plugin->calls[1].time.beatPosition == 0.0);
    CHECK(!engine->transport().running && engine->transport().beatPosition == 0.0);
    plugin->calls.clear();

    // 120 BPM at 48 kHz: 2 beats a second, so 480 frames = 0.02 beats.
    CHECK(engine->sendTransportCommand(Engine::TransportCommand::Start));
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

    // Stop: after that block, back to beat 0 and not playing; Toggle starts again.
    engine->sendTransportCommand(Engine::TransportCommand::Stop);
    plugin->calls.clear();
    for (int i = 0; i < 2; ++i) {
        Block block(480, 48000.0, T);
        engine->process(block.ctx);
    }
    CHECK(plugin->calls.size() == 2 && plugin->calls[0].time.playing && !plugin->calls[1].time.playing);
    CHECK(!engine->transport().running && engine->transport().beatPosition == 0.0 &&
          engine->transport().barNumber == 0);
    engine->sendTransportCommand(Engine::TransportCommand::Toggle);
    Block again(480, 48000.0, T);
    engine->process(again.ctx);
    CHECK(engine->transport().running && engine->transport().beatPosition > 0.0);
    engine->collect();
}

// The click: on the aux outputs only, at each pulse from the transport's start,
// higher on the downbeat; the pulse follows the count (6/8: dotted quarters, or
// every eighth). Off, or stopped: silence.
void testClick() {
    auto engine = std::make_unique<Engine>();
    engine->commit(graphOf({}));
    std::vector<float> aux[2] = {std::vector<float>(96000), std::vector<float>(96000)};
    auto run = [&](std::uint32_t frames) {   // one block, aux included
        Block  block(frames, 48000.0, T);
        float* channels[2] = {aux[0].data(), aux[1].data()};
        block.ctx.aux      = AudioBuffer{channels, 2, frames};
        engine->process(block.ctx);
    };
    auto onsets = [&](std::uint32_t frames) {   // frames where a blip begins
        std::vector<std::uint32_t> at;
        for (std::uint32_t f = 0; f < frames; ++f)
            if (aux[0][f] != 0.0f && (f == 0 || aux[0][f - 1] == 0.0f)) at.push_back(f);
        return at;
    };
    auto crossings = [&](std::uint32_t from) {   // over 10 ms: about 2 per cycle
        int n = 0;
        for (std::uint32_t f = from + 1; f < from + 480; ++f) n += (aux[0][f - 1] < 0.0f) != (aux[0][f] < 0.0f);
        return n;
    };

    engine->setClick(true, false, 0.5f);
    run(48000);   // stopped: silent
    CHECK(onsets(48000).empty());

    // 4/4 at 120 BPM: a beat is 24000 frames. (The blip's first sample is
    // sin(0) = 0, so it shows from the next frame.)
    engine->sendTransportCommand(Engine::TransportCommand::Start);
    run(96000);
    CHECK((onsets(96000) == std::vector<std::uint32_t>{1, 24001, 48001, 72001}));
    CHECK(aux[1] == aux[0]);                                      // both sides alike
    CHECK(crossings(0) > crossings(24000) + 8);                   // the downbeat is higher
    CHECK(std::fabs(*std::max_element(aux[0].begin(), aux[0].end())) <= 0.5f);

    // 6/8, counted: a dotted quarter (36000 frames); every note: an eighth (12000).
    engine->sendTransportCommand(Engine::TransportCommand::Stop);
    run(480);
    engine->setTimeSignature(6, 8);
    engine->sendTransportCommand(Engine::TransportCommand::Start);
    run(96000);
    CHECK((onsets(96000) == std::vector<std::uint32_t>{1, 36001, 72001}));
    engine->sendTransportCommand(Engine::TransportCommand::Stop);
    run(480);
    engine->setClick(true, true, 0.5f);
    engine->sendTransportCommand(Engine::TransportCommand::Start);
    run(48000);
    CHECK((onsets(48000) == std::vector<std::uint32_t>{1, 12001, 24001, 36001}));

    // Off: silence, running or not.
    engine->setClick(false, false, 0.5f);
    run(4800);   // a blip's tail may end in this block
    run(48000);
    CHECK(onsets(48000).empty());

    // The test blip: at once, with the click off and the transport stopped.
    engine->sendTransportCommand(Engine::TransportCommand::Stop);
    run(4800);
    engine->testClick();
    run(4800);
    CHECK(onsets(4800) == std::vector<std::uint32_t>{1});
    run(4800);
    CHECK(onsets(4800).empty());   // just the one
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
    testClick();
    testOnePeriodBehind();
    testBlockSplitting();
    testRoutingAndDialects();
    testTranslation();
    testHeldNotes();
    testTransposeKeepsPitchAttribute();
    testMixing();
    testGainRamp();
    testSlotMeters();
    testMute();
    testDraining();
    testSetlistTriggers();
    testBuses();
    testTransport();
    testGraphLifecycle();
    return test::finish("Engine");
}
