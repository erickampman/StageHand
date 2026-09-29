#pragma once

// A HostedPlugin that records what the engine hands it and outputs a constant.
// Shared by the engine and session tests.

#include <cstdint>
#include <string>
#include <vector>

#include "Plugin/HostedPlugin.h"

namespace stagehand::test {

class FakePlugin final : public HostedPlugin {
public:
    struct Call {
        std::uint32_t      frames   = 0;
        std::int64_t       position = 0;
        TimeInfo           time;
        std::vector<Event> events;
    };

    explicit FakePlugin(std::uint32_t channels = 2,
                        NoteDialectMask dialects = NoteDialect::Midi | NoteDialect::Midi2,
                        float output = 1.0f, std::string id = "test.fake")
        : level(output) {
        desc.id   = std::move(id);
        desc.name = "Fake";
        layout.audioOutputs.push_back(AudioPortInfo{"out", channels, true});
        layout.noteInputs.push_back(NotePortInfo{"in", dialects, NoteDialect::Midi});
    }

    const PluginDescriptor&    descriptor() const override { return desc; }
    PortLayout                 portLayout() const override { return layout; }
    std::vector<ParameterInfo> parameters() const override { return {}; }
    bool activate(double, std::uint32_t) override {
        active = true;
        return true;
    }
    void   deactivate() override { active = false; }
    double paramValue(std::uint32_t) const override { return 0.0; }
    void   flushParams(const EventBlock&, EventSink*) override {}
    bool   saveState(std::vector<std::byte>& out) const override {
        out.insert(out.end(), state.begin(), state.end());
        return true;
    }
    bool loadState(const std::byte* data, std::size_t size) override {
        if (active) loadedWhileActive = true;   // the contract says: before activate()
        state.assign(data, data + size);
        return true;
    }
    void idle() override {}

    void process(const ProcessContext& ctx) noexcept override {
        processing = true;
        Call call;
        call.frames   = ctx.frameCount;
        call.position = ctx.time.samplePosition;
        call.time     = ctx.time;
        call.events.assign(ctx.eventsIn.events, ctx.eventsIn.events + ctx.eventsIn.count);
        calls.push_back(call);   // allocates: fine in a test
        for (std::uint32_t c = 0; c < ctx.audioOut.channelCount; ++c)
            for (std::uint32_t f = 0; f < ctx.frameCount; ++f) ctx.audioOut.channels[c][f] = level;
    }
    void stopProcessing() noexcept override {
        if (processing) ++stops;
        processing = false;
    }

    PluginDescriptor       desc;
    std::vector<Call>      calls;
    std::vector<std::byte> state;
    int                    stops             = 0;
    bool                   processing        = false;
    bool                   active            = false;
    bool                   loadedWhileActive = false;

private:
    PortLayout layout;
    float      level;
};

} // namespace stagehand::test
