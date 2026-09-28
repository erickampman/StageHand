#include "Engine/Engine.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "Core/Ump.h"

namespace stagehand {

namespace {

// Does `filter` let `m` through? Note ranges apply to messages that carry a note
// number, velocity ranges to note-ons; other channel messages pass on source,
// group, and channel alone. (M5 adds the held-note table, so a note-off always
// follows its note-on even if a filter changed in between.)
bool accepts(const MidiFilter& filter, MidiSourceHandle source, const MidiMessage& m) noexcept {
    if (!filter.allSources && filter.source != source) return false;

    const ump::MessageType type = ump::messageType(m);
    if (type == ump::MessageType::Utility || type == ump::MessageType::Stream ||
        type == ump::MessageType::FlexData)
        return false;   // endpoint and utility traffic, not for plugins
    if (((filter.groupMask >> ump::group(m)) & 1u) == 0) return false;
    if (!ump::isChannelVoice(m)) return true;
    if (((filter.channelMask >> ump::channel(m)) & 1u) == 0) return false;

    using ump::Status;
    const Status s     = ump::status(m);
    const bool   midi2 = type == ump::MessageType::Midi2ChannelVoice;
    const bool   carriesNote =
        s == Status::NoteOn || s == Status::NoteOff || s == Status::PolyPressure ||
        (midi2 && (s == Status::RegisteredPerNoteController ||
                   s == Status::AssignablePerNoteController || s == Status::PerNotePitchBend ||
                   s == Status::PerNoteManagement));
    if (carriesNote) {
        const std::uint8_t note = ump::noteNumber(m);
        if (note < filter.noteLow || note > filter.noteHigh) return false;
    }
    if (ump::isNoteOn(m)) {
        const std::uint16_t velocity = ump::velocity16(m);
        if (velocity < filter.velocityLow || velocity > filter.velocityHigh) return false;
    }
    return true;
}

// Can a note port with `dialects` take `m` unchanged?
bool dialectAccepts(NoteDialectMask dialects, const MidiMessage& m) noexcept {
    switch (ump::messageType(m)) {
        case ump::MessageType::Midi1ChannelVoice:
            return (dialects & (NoteDialect::Midi | NoteDialect::MidiMpe)) != 0;
        case ump::MessageType::Midi2ChannelVoice:
            return (dialects & NoteDialect::Midi2) != 0;
        case ump::MessageType::System:
            return (dialects & (NoteDialect::Midi | NoteDialect::MidiMpe | NoteDialect::Midi2)) != 0;
        default:
            return false;   // SysEx needs reassembly first (architecture.md section 1)
    }
}

// destination += source * gain, where the gain is per sample (`gains`) or one
// constant (`gains` null). A null destination only measures. Returns the peak
// absolute value of source * gain.
float mixInto(float* destination, const float* source, std::uint32_t frames, const float* gains,
              float gain) noexcept {
    float peak = 0.0f;
    if (gains != nullptr) {
        for (std::uint32_t f = 0; f < frames; ++f) {
            const float v = source[f] * gains[f];
            peak          = std::max(peak, std::fabs(v));
            if (destination != nullptr) destination[f] += v;
        }
    } else if (gain != 0.0f) {   // a muted slot adds nothing
        for (std::uint32_t f = 0; f < frames; ++f) {
            const float v = source[f] * gain;
            peak          = std::max(peak, std::fabs(v));
            if (destination != nullptr) destination[f] += v;
        }
    }
    return peak;
}

void setNoteNumber(MidiMessage& m, std::uint8_t note) noexcept {
    m.words[0] = (m.words[0] & ~0x7F00u) | (std::uint32_t{note & 0x7Fu} << 8);
}

bool acceptsMidi1(NoteDialectMask dialects) noexcept {
    return (dialects & (NoteDialect::Midi | NoteDialect::MidiMpe)) != 0;
}

} // namespace

Engine::Engine() = default;

Engine::~Engine() {
    delete pendingGraph.exchange(nullptr);   // never adopted
    collect();
    delete currentGraph;                     // the audio thread has stopped calling process()
}

// --- main thread -----------------------------------------------------------------

void Engine::commit(std::unique_ptr<EngineGraph> graph) {
    if (!graph) graph = std::make_unique<EngineGraph>(std::vector<SlotConfig>{});
    graph->sequence = issuedSequence.load(std::memory_order_relaxed) + 1;
    issuedSequence.store(graph->sequence, std::memory_order_release);
    delete pendingGraph.exchange(graph.release(), std::memory_order_acq_rel);   // superseded, never ran
}

bool Engine::committedGraphAdopted() const noexcept {
    return adoptedSequence.load(std::memory_order_acquire) ==
           issuedSequence.load(std::memory_order_acquire);
}

std::size_t Engine::collect() {
    std::size_t  count = 0;
    EngineGraph* retired = nullptr;
    while (retiredGraphs.pop(retired)) {
        delete retired;   // may drop the last reference to a removed plugin
        ++count;
    }
    return count;
}

Engine::Stats Engine::stats() const noexcept {
    Stats s;
    s.midiReceived       = midiReceived.load(std::memory_order_relaxed);
    s.midiQueueFull      = midiQueueFull.load(std::memory_order_relaxed);
    s.slotEventsFull     = slotEventsFull.load(std::memory_order_relaxed);
    s.unsupportedDialect = unsupportedDialect.load(std::memory_order_relaxed);
    s.lateMidi           = lateMidi.load(std::memory_order_relaxed);
    return s;
}

// --- MIDI thread -----------------------------------------------------------------

void Engine::onMidi(MidiSourceHandle source, const TimedMidiMessage& msg) noexcept {
    if (midiQueue.push(QueuedMidi{source, msg}))
        midiReceived.fetch_add(1, std::memory_order_relaxed);
    else
        midiQueueFull.fetch_add(1, std::memory_order_relaxed);
}

// --- audio thread ----------------------------------------------------------------

void Engine::process(const AudioCallbackContext& ctx) noexcept {
    adoptPendingGraph();
    gatherMidi(ctx);
    runSlots(ctx);
}

void Engine::adoptPendingGraph() noexcept {
    if (pendingGraph.load(std::memory_order_acquire) == nullptr) return;
    if (currentGraph != nullptr && !retiredGraphs.canPush()) return;   // main hasn't collected yet

    EngineGraph* const next = pendingGraph.exchange(nullptr, std::memory_order_acq_rel);
    if (next == nullptr) return;

    if (currentGraph != nullptr) {
        for (const SlotConfig& slot : currentGraph->configs)
            if (slot.plugin && !next->contains(slot.plugin.get())) slot.plugin->stopProcessing();
        retiredGraphs.push(currentGraph);   // room checked above; we're the only producer
    }
    currentGraph = next;
    adoptedSequence.store(next->sequence, std::memory_order_release);
}

void Engine::gatherMidi(const AudioCallbackContext& ctx) noexcept {
    if (currentGraph != nullptr)
        for (EngineGraph::SlotState& state : currentGraph->states) state.eventCount = 0;

    const double        rate   = ctx.time.sampleRate;
    const std::uint32_t frames = ctx.frameCount;
    if (rate <= 0.0 || frames == 0) return;

    // Map the previous cycle's window onto this block, one period behind
    // (domain-model.md, sample-accurate alignment).
    const Nanoseconds blockStart  = ctx.blockStartTime;
    const Nanoseconds windowStart = blockStart - std::llround(frames * 1e9 / rate);

    QueuedMidi item;
    while (const QueuedMidi* next = midiQueue.peek()) {
        if (next->message.captureTime >= blockStart) break;   // belongs to the next block
        midiQueue.pop(item);

        const Nanoseconds sinceWindow = item.message.captureTime - windowStart;
        std::uint32_t     offset      = 0;
        if (sinceWindow < 0) {
            lateMidi.fetch_add(1, std::memory_order_relaxed);
        } else {
            const auto frame = static_cast<std::uint64_t>(static_cast<double>(sinceWindow) * rate / 1e9);
            offset = static_cast<std::uint32_t>(std::min<std::uint64_t>(frame, frames - 1));
        }
        if (currentGraph != nullptr) route(item, offset);
    }
}

void Engine::route(const QueuedMidi& item, std::uint32_t offset) noexcept {
    const MidiMessage& original = item.message.message;

    // Upconvert every MIDI 1.0 message once, whether or not a slot needs it: the
    // state (RPN selection, bank) belongs to the stream and must see it all.
    MidiMessage upconverted[ump::kMaxTranslatedMessages];
    std::size_t upconvertedCount = 0;
    if (ump::messageType(original) == ump::MessageType::Midi1ChannelVoice)
        if (ump::UpconvertState* state = upconvertStateFor(item.source))
            upconvertedCount = ump::upconvert(original, *state, upconverted, ump::kMaxTranslatedMessages);

    // A note-off follows its note-on, whatever the filters say now.
    if (ump::isNoteOff(original)) {
        if (HeldNote* held = findHeld(item.source, original)) {
            for (std::size_t i = 0; i < currentGraph->configs.size(); ++i)
                for (std::uint8_t k = 0; k < held->count; ++k)
                    if (currentGraph->configs[i].plugin.get() == held->plugins[k])
                        deliver(i, original, upconverted, upconvertedCount, offset,
                                held->deliveredNotes[k], nullptr);
            held->used = false;
            return;
        }
    }

    HeldNote* const noteOn = ump::isNoteOn(original) ? claimHeld(item.source, original) : nullptr;
    for (std::size_t i = 0; i < currentGraph->configs.size(); ++i) {
        const SlotConfig& config = currentGraph->configs[i];
        if (currentGraph->states[i].dialects == 0 || !accepts(config.midiFilter, item.source, original))
            continue;
        std::uint8_t deliveredNote = 0;
        if (deliver(i, original, upconverted, upconvertedCount, offset, -1, &deliveredNote) &&
            noteOn != nullptr && noteOn->count < kMaxLayers) {
            noteOn->plugins[noteOn->count]        = config.plugin.get();
            noteOn->deliveredNotes[noteOn->count] = deliveredNote;
            ++noteOn->count;
        }
    }
}

// Deliver one incoming message to one slot, in a form its note port takes.
// noteOverride >= 0 places it at that note (a held note's note-off); otherwise
// the slot's transpose applies. Returns true if anything was queued.
bool Engine::deliver(std::size_t slot, const MidiMessage& original, const MidiMessage* upconverted,
                     std::size_t upconvertedCount, std::uint32_t offset, int noteOverride,
                     std::uint8_t* deliveredNote) noexcept {
    const SlotConfig&     config   = currentGraph->configs[slot];
    const NoteDialectMask dialects = currentGraph->states[slot].dialects;
    auto place = [&](MidiMessage& m) {
        if (noteOverride >= 0) {
            setNoteNumber(m, static_cast<std::uint8_t>(noteOverride));
            return true;
        }
        return config.midiFilter.transpose == 0 || ump::transposeNote(m, config.midiFilter.transpose);
    };
    const ump::MessageType type = ump::messageType(original);

    if (dialectAccepts(dialects, original)) {   // its own protocol: unchanged
        MidiMessage m = original;
        return place(m) && append(slot, m, offset, deliveredNote);
    }
    if (type == ump::MessageType::Midi2ChannelVoice && acceptsMidi1(dialects)) {
        MidiMessage m = original;   // transpose first, so a pitch attribute stays consistent
        if (!place(m)) return false;
        MidiMessage down[ump::kMaxTranslatedMessages];
        const std::size_t n = ump::downconvert(m, down, ump::kMaxTranslatedMessages);
        bool any = false;
        for (std::size_t j = 0; j < n; ++j) any = append(slot, down[j], offset, deliveredNote) || any;
        return any;
    }
    if (type == ump::MessageType::Midi1ChannelVoice && (dialects & NoteDialect::Midi2) != 0) {
        bool any = false;   // nothing to send is normal: bank select, RPN selection
        for (std::size_t j = 0; j < upconvertedCount; ++j) {
            MidiMessage m = upconverted[j];
            if (place(m)) any = append(slot, m, offset, deliveredNote) || any;
        }
        return any;
    }
    unsupportedDialect.fetch_add(1, std::memory_order_relaxed);
    return false;
}

bool Engine::append(std::size_t slot, const MidiMessage& message, std::uint32_t offset,
                    std::uint8_t* deliveredNote) noexcept {
    EngineGraph::SlotState& state = currentGraph->states[slot];
    if (state.eventCount == state.events.size()) {
        slotEventsFull.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    Event& event       = state.events[state.eventCount++];
    event              = Event{};
    event.sampleOffset = offset;
    event.type         = EventType::Midi;
    event.midi         = message;
    event.portIndex    = currentGraph->configs[slot].notePort;
    if (deliveredNote != nullptr) *deliveredNote = ump::noteNumber(message);
    return true;
}

// Sources are few; handles aren't reused, so a table of 16 lasts a session. If
// it's full, MIDI-2.0-only plugins get no MIDI 1.0 from further sources.
ump::UpconvertState* Engine::upconvertStateFor(MidiSourceHandle source) noexcept {
    SourceState* unused = nullptr;
    for (SourceState& s : sourceStates) {
        if (s.source == source) return &s.upconvert;
        if (s.source == kInvalidMidiSource && unused == nullptr) unused = &s;
    }
    if (unused == nullptr) return nullptr;
    unused->source    = source;
    unused->upconvert = ump::UpconvertState{};
    return &unused->upconvert;
}

Engine::HeldNote* Engine::findHeld(MidiSourceHandle source, const MidiMessage& m) noexcept {
    const std::uint8_t g = ump::group(m), ch = ump::channel(m), note = ump::noteNumber(m);
    for (HeldNote& h : heldNotes)
        if (h.used && h.source == source && h.group == g && h.channel == ch && h.note == note) return &h;
    return nullptr;
}

// Start (or restart, for a repeated note-on) tracking a key. Null if the table
// is full; that key's note-off then falls back to routing through the filters.
Engine::HeldNote* Engine::claimHeld(MidiSourceHandle source, const MidiMessage& m) noexcept {
    HeldNote* entry = findHeld(source, m);
    if (entry == nullptr)
        for (HeldNote& h : heldNotes)
            if (!h.used) {
                entry = &h;
                break;
            }
    if (entry == nullptr) return nullptr;
    entry->used    = true;
    entry->source  = source;
    entry->group   = ump::group(m);
    entry->channel = ump::channel(m);
    entry->note    = ump::noteNumber(m);
    entry->count   = 0;
    return entry;
}

void Engine::runSlots(const AudioCallbackContext& ctx) noexcept {
    const std::uint32_t outputs = ctx.output.channelCount;
    for (std::uint32_t c = 0; c < outputs; ++c) std::fill_n(ctx.output.channels[c], ctx.frameCount, 0.0f);

    if (currentGraph != nullptr) {
        for (std::size_t i = 0; i < currentGraph->configs.size(); ++i) {
            EngineGraph::SlotState& state = currentGraph->states[i];
            state.cursor = 0;
            if (state.eventCount != 0)
                currentGraph->configs[i].runtime->midiEvents.fetch_add(state.eventCount, std::memory_order_relaxed);
        }

        for (std::uint32_t chunkStart = 0; chunkStart < ctx.frameCount; chunkStart += kMaxPluginBlockFrames) {
            const std::uint32_t chunk = std::min(ctx.frameCount - chunkStart, kMaxPluginBlockFrames);

            for (std::size_t i = 0; i < currentGraph->configs.size(); ++i) {
                const SlotConfig&       config = currentGraph->configs[i];
                EngineGraph::SlotState& state  = currentGraph->states[i];
                if (!config.plugin) continue;

                // This chunk's events, rebased to the chunk start.
                const std::uint32_t first = state.cursor;
                while (state.cursor < state.eventCount &&
                       state.events[state.cursor].sampleOffset < chunkStart + chunk) {
                    state.events[state.cursor].sampleOffset -= chunkStart;
                    ++state.cursor;
                }

                ProcessContext pc;
                pc.frameCount           = chunk;
                pc.time                 = ctx.time;
                pc.time.samplePosition += chunkStart;
                pc.audioOut             = AudioBuffer{state.channels.data(), state.audioChannels, chunk};
                pc.eventsIn             = EventBlock{state.events.data() + first, state.cursor - first};
                config.plugin->process(pc);

                SlotRuntime& runtime  = *config.runtime;
                float        constant = 1.0f;
                const float* gains    = advanceGain(runtime, chunk, ctx.time.sampleRate, constant);
                auto destination = [&](std::uint32_t out) {
                    return out < outputs ? ctx.output.channels[out] + chunkStart : nullptr;
                };
                for (std::uint32_t c = 0; c < state.audioChannels; ++c)
                    runtime.meter.add(c, mixInto(destination(config.firstOutput + c), state.channels[c], chunk,
                                                 gains, constant));
                if (state.audioChannels == 1) {   // mono feeds two outputs and shows on both meters
                    runtime.meter.add(1, mixInto(destination(config.firstOutput + 1), state.channels[0], chunk,
                                                 gains, constant));
                }
            }
        }
    }

    for (std::uint32_t c = 0; c < outputs; ++c) {
        float peak = 0.0f;
        for (std::uint32_t f = 0; f < ctx.frameCount; ++f) peak = std::max(peak, std::fabs(ctx.output.channels[c][f]));
        outputLevels.add(c, peak);
    }
}

// Advance the slot's gain by `frames`. While a ramp runs, fills the graph's
// scratch buffer with one gain per sample and returns it; otherwise returns
// null with the steady gain in `constantGain`. A new target restarts the ramp
// from wherever the gain is, so it always takes kGainRampSeconds to arrive,
// and the last step lands exactly on the target.
const float* Engine::advanceGain(SlotRuntime& runtime, std::uint32_t frames, double sampleRate,
                                 float& constantGain) noexcept {
    const float target = runtime.targetGain.load(std::memory_order_relaxed);
    if (target != runtime.rampTarget) {
        const double length   = std::round(kGainRampSeconds * sampleRate);
        runtime.rampTarget    = target;
        runtime.rampRemaining = length >= 1.0 ? static_cast<std::uint32_t>(length) : 1;
        runtime.rampStep      = (target - runtime.currentGain) / static_cast<float>(runtime.rampRemaining);
    }
    if (runtime.rampRemaining == 0) {
        constantGain = runtime.currentGain;
        return nullptr;
    }
    float* const gains = currentGraph->gainRamp.data();
    for (std::uint32_t f = 0; f < frames; ++f) {
        if (runtime.rampRemaining > 0) {
            if (--runtime.rampRemaining == 0)
                runtime.currentGain = runtime.rampTarget;
            else
                runtime.currentGain += runtime.rampStep;
        }
        gains[f] = runtime.currentGain;
    }
    return gains;
}

} // namespace stagehand
