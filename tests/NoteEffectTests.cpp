// A rack row with a real note effect in front of a real instrument: Retune
// (31-EDO, one step up) into StruckSynth, through the engine, into memory.
// Nothing reaches an audio device. Skips unless both are installed (~/.clap,
// $CLAP_PATH, and on macOS ~/Library/Audio/Plug-Ins/CLAP).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "Check.h"
#include "Core/UwNames.h"
#include "Engine/Engine.h"
#include "uw/clap/ClapPluginFactory.h"
#include "uw/plugin/HostServices.h"
#include "uw/ump/Ump.h"

using namespace stagehand;

namespace {

struct TestHost final : HostServices {
    void         requestRestart() override {}
    void         requestProcess() override {}
    void         requestParamFlush() override {}
    void         log(LogLevel, const char*) override {}
    HostInfo     hostInfo() const override { return {"StageHand tests", "Unlikelyware", "", "0.1.0"}; }
};

// Passes everything to the real plugin, keeping a copy of the events it's given.
class Recording final : public HostedPlugin {
public:
    explicit Recording(std::shared_ptr<HostedPlugin> inner) : plugin(std::move(inner)) {}

    const PluginDescriptor&    descriptor() const override { return plugin->descriptor(); }
    PortLayout                 portLayout() const override { return plugin->portLayout(); }
    std::vector<ParameterInfo> parameters() const override { return plugin->parameters(); }
    bool   activate(double rate, std::uint32_t frames) override { return plugin->activate(rate, frames); }
    void   deactivate() override { plugin->deactivate(); }
    double paramValue(std::uint32_t id) const override { return plugin->paramValue(id); }
    void   flushParams(const EventBlock& in, EventSink* out) override { plugin->flushParams(in, out); }
    bool   saveState(std::vector<std::byte>& out) const override { return plugin->saveState(out); }
    bool   loadState(const std::byte* data, std::size_t size) override { return plugin->loadState(data, size); }
    void   idle() override { plugin->idle(); }
    void   stopProcessing() noexcept override { plugin->stopProcessing(); }
    void   process(const ProcessContext& ctx) noexcept override {
        received.insert(received.end(), ctx.eventsIn.events, ctx.eventsIn.events + ctx.eventsIn.count);   // a test may allocate
        plugin->process(ctx);
    }

    std::shared_ptr<HostedPlugin> plugin;
    std::vector<Event>            received;
};

const PluginDescriptor* find(const std::vector<PluginDescriptor>& found, const char* name) {
    const auto it = std::find_if(found.begin(), found.end(), [&](const PluginDescriptor& d) { return d.name == name; });
    return it != found.end() ? &*it : nullptr;
}

void retuneIntoStruckSynth() {
    ClapPluginFactory factory;
    const auto        found  = factory.scan();
    const auto*       retune = find(found, "Retune");
    const auto*       synth  = find(found, "StruckSynth");
    if (retune == nullptr || synth == nullptr) {
        std::printf("note-effects: Retune and StruckSynth aren't both installed; skipped\n");
        return;
    }

    TestHost   host;
    const auto effect = std::shared_ptr<HostedPlugin>(factory.create(*retune, host));
    const auto inner  = std::shared_ptr<HostedPlugin>(factory.create(*synth, host));
    CHECK(effect != nullptr && inner != nullptr);
    if (!effect || !inner) return;
    CHECK(retune->kind == PluginKind::Other && synth->kind == PluginKind::Instrument);

    // One step of 31-EDO up: 12/31 semitone, 198/512 in Pitch 7.9 (Retune's state format).
    const std::string state = "Retune 1\ndivisions 31\noctaves 0\nsteps 1\n";
    CHECK(effect->loadState(reinterpret_cast<const std::byte*>(state.data()), state.size()));
    const auto recording = std::make_shared<Recording>(inner);
    constexpr double rate = 48000.0;
    CHECK(effect->activate(rate, kMaxPluginBlockFrames));
    CHECK(recording->activate(rate, kMaxPluginBlockFrames));

    auto       engine = std::make_unique<Engine>();
    SlotConfig slot;
    slot.plugin      = recording;
    slot.midiEffects = {effect};
    engine->commit(std::make_unique<EngineGraph>(std::vector<SlotConfig>{slot}));

    constexpr std::uint32_t kFrames = 512;
    std::vector<std::vector<float>> out(2, std::vector<float>(kFrames, 0.0f));
    float*                          pointers[2] = {out[0].data(), out[1].data()};
    AudioCallbackContext            ctx;
    ctx.frameCount      = kFrames;
    ctx.time.sampleRate = rate;
    ctx.output          = AudioBuffer{pointers, 2, kFrames};

    constexpr Nanoseconds kBlock = static_cast<Nanoseconds>(kFrames * 1e9 / rate);
    Nanoseconds           start  = 1'000'000'000;
    TimedMidiMessage      on;   // MIDI 2.0 note-on 60, captured during the block before the first
    on.captureTime       = start - kBlock / 2;
    on.message.words[0]  = 0x40903C00;
    on.message.words[1]  = 0xC0000000;
    on.message.wordCount = 2;
    engine->onMidi(1, on);

    float peak = 0.0f;
    for (int block = 0; block < 8; ++block, start += kBlock) {
        ctx.blockStartTime = start;
        ctx.time.samplePosition = block * static_cast<std::int64_t>(kFrames);
        engine->process(ctx);
        for (const auto& channel : out)
            for (float v : channel) peak = std::max(peak, std::fabs(v));
    }

    const auto noteOn = std::find_if(recording->received.begin(), recording->received.end(),
                                     [](const Event& e) { return e.type == EventType::Midi && ump::isNoteOn(e.midi); });
    CHECK(noteOn != recording->received.end());
    if (noteOn != recording->received.end()) {
        // StruckSynth takes MIDI 2.0, so the attribute arrives: 60 and 198/512 (nearest-below key 60).
        CHECK(ump::messageType(noteOn->midi) == ump::MessageType::Midi2ChannelVoice);
        CHECK(ump::notePitch79(noteOn->midi) == 60 * 512 + 198);
        CHECK(noteOn->sampleOffset == kFrames / 2);
        std::printf("note-effects: StruckSynth got note %u, Pitch 7.9 %u (%.3f semitones), output peak %.3f\n",
                    ump::noteNumber(noteOn->midi), ump::notePitch79(noteOn->midi),
                    ump::notePitch79(noteOn->midi) / 512.0, peak);
    }
    CHECK(peak > 0.0f && std::isfinite(peak));   // it sounded (into memory)
    CHECK(engine->stats().unsupportedDialect == 0 && engine->stats().slotEventsFull == 0);

    engine->commit(nullptr);   // shutdown: stops both
    engine->process(ctx);
    engine->collect();
    effect->deactivate();
    recording->deactivate();
}

} // namespace

int main() {
    retuneIntoStruckSynth();
    return test::finish("note-effects");
}
