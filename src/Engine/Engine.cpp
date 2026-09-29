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
    // "All" means every MIDI input, not every sequence: a sequence plays only
    // where it's chosen.
    if (filter.allSources ? isSequenceSource(source) : filter.source != source) return false;

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
    delete pendingSequences.exchange(nullptr);
    collect();
    delete currentGraph;                     // the audio thread has stopped calling process()
    delete currentSequences;
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
    SequenceSet* sequences = nullptr;
    while (retiredSequences.pop(sequences)) delete sequences;   // not counted: graphs only
    return count;
}

void Engine::commitSequences(std::unique_ptr<SequenceSet> sequences) {
    if (!sequences) sequences = std::make_unique<SequenceSet>(std::vector<CompiledSequence>{});
    delete pendingSequences.exchange(sequences.release(), std::memory_order_acq_rel);   // superseded, never ran
}

std::uint32_t Engine::sourceActivity(MidiSourceHandle source) const noexcept {
    for (const SourceCount& c : sourceCounts)
        if (c.source.load(std::memory_order_acquire) == source) return c.count.load(std::memory_order_relaxed);
    return 0;
}

void Engine::countSource(MidiSourceHandle source) noexcept {
    for (SourceCount& c : sourceCounts) {
        const MidiSourceHandle held = c.source.load(std::memory_order_relaxed);
        if (held == source) {
            c.count.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (held == kInvalidMidiSource) {   // first time heard: claim this slot
            c.count.store(1, std::memory_order_relaxed);
            c.source.store(source, std::memory_order_release);
            return;
        }
    }
}

bool Engine::sendSequenceCommand(MidiSourceHandle source, SequenceCommand command) noexcept {
    return sequenceCommands.push(QueuedCommand{source, command});
}

Engine::Stats Engine::stats() const noexcept {
    Stats s;
    s.midiReceived       = midiReceived.load(std::memory_order_relaxed);
    s.midiQueueFull      = midiQueueFull.load(std::memory_order_relaxed);
    s.slotEventsFull     = slotEventsFull.load(std::memory_order_relaxed);
    s.unsupportedDialect = unsupportedDialect.load(std::memory_order_relaxed);
    s.lateMidi           = lateMidi.load(std::memory_order_relaxed);
    s.sequenceEventsFull = sequenceEventsFull.load(std::memory_order_relaxed);
    return s;
}

void Engine::setTempo(double bpm) noexcept {
    tempo.store(std::clamp(bpm, kMinTempo, kMaxTempo), std::memory_order_relaxed);
}

void Engine::setTimeSignature(int numerator, int denominator) noexcept {
    const bool powerOfTwo = denominator > 0 && (denominator & (denominator - 1)) == 0;
    if (numerator < 1 || numerator > 32 || !powerOfTwo || denominator > 32) return;
    signature.store((static_cast<std::uint32_t>(numerator) << 16) | static_cast<std::uint32_t>(denominator),
                    std::memory_order_relaxed);
}

Engine::Transport Engine::transport() const noexcept {
    Transport t;
    t.tempoBpm         = tempo.load(std::memory_order_relaxed);
    const auto sig     = signature.load(std::memory_order_relaxed);
    t.numerator        = static_cast<int>(sig >> 16);
    t.denominator      = static_cast<int>(sig & 0xFFFF);
    t.beatPosition     = publishedBeat.load(std::memory_order_relaxed);
    t.barStartBeats    = publishedBarStart.load(std::memory_order_relaxed);
    t.barNumber        = publishedBar.load(std::memory_order_relaxed);
    return t;
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
    advanceTransport(ctx);
    runSequences(ctx);
    gatherMidi(ctx);
    runSlots(ctx);

    clock.beat += static_cast<double>(ctx.frameCount) * clock.beatsPerFrame;
    const TimeInfo now = timeAt(ctx, 0);   // the next block's start (the clock has moved)
    publishedBeat.store(now.beatPosition, std::memory_order_relaxed);
    publishedBarStart.store(now.barStartBeats, std::memory_order_relaxed);
    publishedBar.store(now.barNumber, std::memory_order_relaxed);
}

// Read this block's tempo and signature. A new signature starts at the current
// bar's start (under the old one), so bars stay contiguous.
void Engine::advanceTransport(const AudioCallbackContext& ctx) noexcept {
    clock.tempoBpm      = tempo.load(std::memory_order_relaxed);
    clock.beatsPerFrame = ctx.time.sampleRate > 0.0 ? clock.tempoBpm / 60.0 / ctx.time.sampleRate : 0.0;

    const auto sig         = signature.load(std::memory_order_relaxed);
    const int  numerator   = static_cast<int>(sig >> 16);
    const int  denominator = static_cast<int>(sig & 0xFFFF);
    if (numerator == clock.numerator && denominator == clock.denominator) return;
    const TimeInfo now  = timeAt(ctx, 0);
    clock.barAnchorBeat = now.barStartBeats;
    clock.barAnchor     = now.barNumber;
    clock.numerator     = numerator;
    clock.denominator   = denominator;
}

// The transport as of `frame` frames into this block.
TimeInfo Engine::timeAt(const AudioCallbackContext& ctx, std::uint32_t frame) const noexcept {
    TimeInfo t            = ctx.time;
    t.samplePosition     += frame;
    t.tempoBpm            = clock.tempoBpm;
    t.timeSigNumerator    = clock.numerator;
    t.timeSigDenominator  = clock.denominator;
    t.playing             = true;   // free-running
    t.beatPosition        = clock.beat + static_cast<double>(frame) * clock.beatsPerFrame;
    const double barBeats = clock.numerator * 4.0 / clock.denominator;
    // A hair of tolerance, so a position exactly on a bar line isn't read as the
    // end of the previous bar after rounding.
    const double bars = std::floor((t.beatPosition - clock.barAnchorBeat) / barBeats + 1e-9);
    t.barStartBeats   = clock.barAnchorBeat + bars * barBeats;
    t.barNumber       = clock.barAnchor + static_cast<std::int32_t>(bars);
    return t;
}

// This block's sequence events: adopt a new snapshot, apply commands, generate,
// and sort by (offset, order) for gatherMidi() to merge with incoming MIDI.
void Engine::runSequences(const AudioCallbackContext& ctx) noexcept {
    sequenceEventCount = 0;
    const TimeInfo     now = timeAt(ctx, 0);
    SequenceSet::Block block;
    block.beat          = now.beatPosition;
    block.beatsPerFrame = clock.beatsPerFrame;
    block.tempoBpm      = clock.tempoBpm;
    block.frames        = ctx.frameCount;
    block.barStartBeats = now.barStartBeats;
    block.barBeats      = clock.numerator * 4.0 / clock.denominator;

    if (pendingSequences.load(std::memory_order_acquire) != nullptr &&
        (currentSequences == nullptr || retiredSequences.canPush())) {
        if (SequenceSet* next = pendingSequences.exchange(nullptr, std::memory_order_acq_rel)) {
            sequenceEventCount += next->adoptFrom(currentSequences, block, sequenceEvents, kMaxSequenceEvents);
            if (currentSequences != nullptr) retiredSequences.push(currentSequences);   // room checked above
            currentSequences = next;
        }
    }
    if (currentSequences == nullptr) return;

    QueuedCommand queued;
    while (sequenceCommands.pop(queued)) currentSequences->command(queued.source, queued.command, block);

    const std::size_t room = kMaxSequenceEvents - sequenceEventCount;
    const std::size_t made = currentSequences->generate(block, sequenceEvents + sequenceEventCount, room);
    if (made == room) sequenceEventsFull.fetch_add(1, std::memory_order_relaxed);   // may have dropped some
    sequenceEventCount += made;

    // Insertion sort, stable: a block has few events, and most arrive in order.
    for (std::size_t i = 1; i < sequenceEventCount; ++i) {
        const SequenceEvent e = sequenceEvents[i];
        std::size_t         j = i;
        for (; j > 0 && (sequenceEvents[j - 1].offset > e.offset ||
                         (sequenceEvents[j - 1].offset == e.offset && sequenceEvents[j - 1].order > e.order));
             --j)
            sequenceEvents[j] = sequenceEvents[j - 1];
        sequenceEvents[j] = e;
    }
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

    // Sequence events are merged in offset order with the captured MIDI (whose
    // offsets never decrease: the queue is in capture order).
    std::size_t nextSequenceEvent = 0;
    auto routeSequencesUpTo = [&](std::uint32_t offset) {
        for (; nextSequenceEvent < sequenceEventCount && sequenceEvents[nextSequenceEvent].offset <= offset;
             ++nextSequenceEvent) {
            if (currentGraph == nullptr) continue;
            const SequenceEvent& e = sequenceEvents[nextSequenceEvent];
            QueuedMidi           item;
            item.source            = e.source;
            item.message.message   = e.message;
            route(item, e.offset);
        }
    };

    // Map the previous cycle's window onto this block, one period behind
    // (domain-model.md, sample-accurate alignment).
    const Nanoseconds blockStart  = ctx.blockStartTime;
    const Nanoseconds windowStart = blockStart - std::llround(frames * 1e9 / rate);

    QueuedMidi item;
    while (const QueuedMidi* next = midiQueue.peek()) {
        if (next->message.captureTime >= blockStart) break;   // belongs to the next block
        midiQueue.pop(item);
        countSource(item.source);

        const Nanoseconds sinceWindow = item.message.captureTime - windowStart;
        std::uint32_t     offset      = 0;
        if (sinceWindow < 0) {
            lateMidi.fetch_add(1, std::memory_order_relaxed);
        } else {
            const auto frame = static_cast<std::uint64_t>(static_cast<double>(sinceWindow) * rate / 1e9);
            offset = static_cast<std::uint32_t>(std::min<std::uint64_t>(frame, frames - 1));
        }
        routeSequencesUpTo(offset);
        if (currentGraph != nullptr) route(item, offset);
    }
    routeSequencesUpTo(frames);
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
                pc.time                 = timeAt(ctx, chunkStart);
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
