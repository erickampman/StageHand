// Engine tests with a fake plugin: sample-accurate MIDI placement (one period
// behind), block splitting, routing through filters and dialects, output mixing,
// and the commit / adopt / retire lifecycle. No JACK, ALSA or real plugins.

#include <cstdint>
#include <memory>
#include <vector>

#include "Check.h"
#include "Core/Ump.h"
#include "Engine/Engine.h"

using namespace stagehand;

namespace {

class FakePlugin final : public HostedPlugin {
public:
    struct Call {
        std::uint32_t      frames   = 0;
        std::int64_t       position = 0;
        std::vector<Event> events;
    };

    explicit FakePlugin(std::uint32_t channels = 2,
                        NoteDialectMask dialects = NoteDialect::Midi | NoteDialect::Midi2,
                        float output = 1.0f)
        : level(output) {
        layout.audioOutputs.push_back(AudioPortInfo{"out", channels, true});
        layout.noteInputs.push_back(NotePortInfo{"in", dialects, NoteDialect::Midi});
    }

    const PluginDescriptor&    descriptor() const override { return desc; }
    PortLayout                 portLayout() const override { return layout; }
    std::vector<ParameterInfo> parameters() const override { return {}; }
    bool                       activate(double, std::uint32_t) override { return true; }
    void                       deactivate() override {}
    double                     paramValue(std::uint32_t) const override { return 0.0; }
    void                       flushParams(const EventBlock&, EventSink*) override {}
    bool                       saveState(std::vector<std::byte>&) const override { return false; }
    bool                       loadState(const std::byte*, std::size_t) override { return false; }
    void                       idle() override {}

    void process(const ProcessContext& ctx) noexcept override {
        processing = true;
        Call call;
        call.frames   = ctx.frameCount;
        call.position = ctx.time.samplePosition;
        call.events.assign(ctx.eventsIn.events, ctx.eventsIn.events + ctx.eventsIn.count);
        calls.push_back(call);   // allocates: fine in a test
        for (std::uint32_t c = 0; c < ctx.audioOut.channelCount; ++c)
            for (std::uint32_t f = 0; f < ctx.frameCount; ++f) ctx.audioOut.channels[c][f] = level;
    }
    void stopProcessing() noexcept override {
        if (processing) ++stops;
        processing = false;
    }

    std::vector<Call> calls;
    int               stops      = 0;
    bool              processing = false;

private:
    PluginDescriptor desc;
    PortLayout       layout;
    float            level;
};

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
    engine->onMidi(1, midi2);                     // A yes; B can't take MIDI 2.0

    Block block(480, 48000.0, T);
    engine->process(block.ctx);
    CHECK(a->calls.size() == 1 && a->calls[0].events.size() == 2);
    CHECK(b->calls.size() == 1 && b->calls[0].events.size() == 1);
    if (b->calls.size() == 1 && b->calls[0].events.size() == 1)
        CHECK(ump::noteNumber(b->calls[0].events[0].midi) == 70);
    CHECK(engine->stats().unsupportedDialect == 1);
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
    a.gain       = 0.5f;
    SlotConfig b = slotFor(quiet);
    SlotConfig c = slotFor(mono);   // mono feeds outputs 1 and 2
    engine->commit(graphOf({a, b, c}));

    Block block(64, 48000.0, T);   // buffers start at 9.0: the engine must clear them
    engine->process(block.ctx);
    CHECK(block.samples[0][0] == 1.25f && block.samples[0][63] == 1.25f);   // 0.5 + 0.25 + 0.5
    CHECK(block.samples[1][0] == 1.25f);
    CHECK(engine->takeOutputPeak() == 1.25f);
    CHECK(engine->takeOutputPeak() == 0.0f);   // reset by the first call
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
    testTransposeKeepsPitchAttribute();
    testMixing();
    testGraphLifecycle();
    return test::finish("Engine");
}
