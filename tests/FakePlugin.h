#pragma once

// A HostedPlugin that records what the engine hands it and outputs a constant.
// Shared by the engine and session tests.

#include <algorithm>
#include <chrono>
#include <map>
#include <cstdint>
#include <string>
#include <vector>

#include "uw/plugin/HostedPlugin.h"
#include "uw/ump/Ump.h"
#include "Core/UwNames.h"

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
    std::vector<ParameterInfo> parameters() const override { return params; }
    bool activate(double, std::uint32_t) override {
        active = true;
        return true;
    }
    void   deactivate() override { active = false; }
    double paramValue(std::uint32_t id) const override {
        const auto it = values.find(id);
        return it != values.end() ? it->second : 0.0;
    }
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
    bool hasEditor() const override { return editor; }

    void process(const ProcessContext& ctx) noexcept override {
        processing = true;
        Call call;
        call.frames   = ctx.frameCount;
        call.position = ctx.time.samplePosition;
        call.time     = ctx.time;
        call.events.assign(ctx.eventsIn.events, ctx.eventsIn.events + ctx.eventsIn.count);
        calls.push_back(call);   // allocates: fine in a test
        if (ctx.eventsOut != nullptr)   // its own parameter changes (as if from its GUI)
            for (const Event& e : emit) ctx.eventsOut->push(e);
        emit.clear();
        if (busy.count() > 0)    // a heavy plugin: this long a call, at least
            for (const auto end = std::chrono::steady_clock::now() + busy; std::chrono::steady_clock::now() < end;) {}
        for (std::uint32_t c = 0; c < ctx.audioOut.channelCount; ++c)
            for (std::uint32_t f = 0; f < ctx.frameCount; ++f) ctx.audioOut.channels[c][f] = level;
    }
    void stopProcessing() noexcept override {
        if (processing) ++stops;
        processing = false;
    }
    void reset() noexcept override { ++resets; }

    PluginDescriptor       desc;
    std::vector<Call>      calls;
    std::vector<std::byte> state;
    int                    stops             = 0;
    int                    resets            = 0;
    std::chrono::nanoseconds busy{0};                // each process() call takes this long
    std::vector<Event>     emit;                     // pushed to eventsOut at the next process()
    std::vector<ParameterInfo>      params;          // parameters()
    std::map<std::uint32_t, double> values;          // paramValue(); 0 if not set
    bool                   processing        = false;
    bool                   active            = false;
    bool                   loadedWhileActive = false;
    bool                   editor            = false;   // hasEditor()

private:
    PortLayout layout;
    float      level;
};

// An effect: its main input times `factor` on its main output (an input
// channel past the last feeds from the last). Counts its process() calls.
class FakeEffect final : public HostedPlugin {
public:
    explicit FakeEffect(float multiplyBy = 1.0f, std::uint32_t inChannels = 2, std::uint32_t outChannels = 2)
        : factor(multiplyBy) {
        desc.id   = "test.effect";
        desc.name = "Fake effect";
        desc.kind = PluginKind::Effect;
        if (inChannels > 0) layout.audioInputs.push_back(AudioPortInfo{"in", inChannels, true});
        layout.audioOutputs.push_back(AudioPortInfo{"out", outChannels, true});
    }

    const PluginDescriptor&    descriptor() const override { return desc; }
    PortLayout                 portLayout() const override { return layout; }
    std::vector<ParameterInfo> parameters() const override { return {}; }
    bool                       activate(double, std::uint32_t) override { return true; }
    void                       deactivate() override {}
    double                     paramValue(std::uint32_t) const override { return 0.0; }
    void                       flushParams(const EventBlock&, EventSink*) override {}
    bool                       saveState(std::vector<std::byte>&) const override { return true; }
    bool                       loadState(const std::byte*, std::size_t) override { return true; }
    void                       idle() override {}

    void process(const ProcessContext& ctx) noexcept override {
        ++processed;
        processing = true;
        received.insert(received.end(), ctx.eventsIn.events, ctx.eventsIn.events + ctx.eventsIn.count);
        lastInputChannels = ctx.audioIn.channelCount;
        for (std::uint32_t c = 0; c < ctx.audioOut.channelCount; ++c)
            for (std::uint32_t f = 0; f < ctx.frameCount; ++f) {
                const float in = ctx.audioIn.channelCount == 0
                                     ? 0.0f
                                     : ctx.audioIn.channels[std::min(c, ctx.audioIn.channelCount - 1)][f];
                ctx.audioOut.channels[c][f] = in * factor;
            }
    }
    void stopProcessing() noexcept override {
        if (processing) ++stops;
        processing = false;
    }
    void reset() noexcept override { ++resets; }

    PluginDescriptor desc;
    float            factor;
    int              processed         = 0;
    std::vector<Event> received;   // every event it was given
    int              stops             = 0;
    int              resets            = 0;
    bool             processing        = false;
    std::uint32_t    lastInputChannels = 0;

private:
    PortLayout layout;
};

// A note effect (a CLAP "note-effect"): no audio ports, one note input and one
// note output. Each note on/off it's given leaves `semitones` higher (MIDI 1.0
// and 2.0 alike), and a MIDI 2.0 one `pitch79` 1/512 semitones higher on top,
// written as a Pitch 7.9 attribute; everything else leaves unchanged, at its
// offset, in its own protocol. `emitLate` adds a note-on at that offset (out of
// time order: after the rest). Records what it's given.
class FakeNoteEffect final : public HostedPlugin {
public:
    explicit FakeNoteEffect(int transpose = 0, NoteDialectMask dialects = NoteDialect::Midi | NoteDialect::Midi2)
        : semitones(transpose) {
        desc.id   = "test.note-effect";
        desc.name = "Fake note effect";
        desc.kind = PluginKind::Other;
        layout.noteInputs.push_back(NotePortInfo{"in", dialects, NoteDialect::Midi2});
        layout.noteOutputs.push_back(NotePortInfo{"out", dialects, NoteDialect::Midi2});
    }

    const PluginDescriptor&    descriptor() const override { return desc; }
    PortLayout                 portLayout() const override { return layout; }
    std::vector<ParameterInfo> parameters() const override { return {}; }
    bool                       activate(double, std::uint32_t) override { return true; }
    void                       deactivate() override {}
    double                     paramValue(std::uint32_t) const override { return 0.0; }
    void                       flushParams(const EventBlock&, EventSink*) override {}
    bool                       saveState(std::vector<std::byte>&) const override { return true; }
    bool                       loadState(const std::byte*, std::size_t) override { return true; }
    void                       idle() override {}

    void process(const ProcessContext& ctx) noexcept override {
        ++processed;
        processing = true;
        lastAudioChannels = ctx.audioOut.channelCount;
        for (std::uint32_t i = 0; i < ctx.eventsIn.count; ++i) {
            Event e = ctx.eventsIn.events[i];
            received.push_back(e);   // allocates: fine in a test
            if (e.type != EventType::Midi || ctx.eventsOut == nullptr) continue;
            if (ump::isNoteOn(e.midi) || ump::isNoteOff(e.midi)) {
                if (semitones != 0) ump::transposeNote(e.midi, semitones);
                if (pitch79 != 0) ump::transposeNote79(e.midi, pitch79);
            }
            e.portIndex = 0;
            ctx.eventsOut->push(e);
        }
        if (emitLate >= 0 && ctx.eventsOut != nullptr) {
            Event late;
            late.sampleOffset   = static_cast<std::uint32_t>(emitLate);
            late.midi.words[0]  = 0x20907F40;   // MIDI 1.0 note 127
            late.midi.wordCount = 1;
            ctx.eventsOut->push(late);
        }
    }
    void stopProcessing() noexcept override {
        if (processing) ++stops;
        processing = false;
    }
    void reset() noexcept override { ++resets; }

    PluginDescriptor   desc;
    int                semitones;
    int                pitch79           = 0;
    int                emitLate          = -1;
    int                processed         = 0;
    std::vector<Event> received;
    int                stops             = 0;
    int                resets            = 0;
    bool               processing        = false;
    std::uint32_t      lastAudioChannels = 99;

private:
    PortLayout layout;
};

} // namespace stagehand::test
