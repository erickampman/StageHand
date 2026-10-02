#include "Engine/Engine.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <utility>
#include <vector>

#include "Core/Ump.h"

namespace stagehand {

namespace {

// Does `filter` let `m` through? Note ranges apply to messages that carry a note
// number, velocity ranges to note-ons; other channel messages pass on source,
// group, and channel alone. (M5 adds the held-note table, so a note-off always
// follows its note-on even if a filter changed in between.)
bool accepts(const SlotConfig& slot, MidiSourceHandle source, const MidiMessage& m) noexcept {
    const MidiFilter& filter = slot.midiFilter;
    // A sequence plays only into the slots that list it (its targets); "all"
    // means every MIDI input, never a sequence. A live input is the slot's own.
    if (isSequenceSource(source)) {   // an audition comes from its sequence's rows too
        const MidiSourceHandle sequence = sequenceOfSource(source);
        if (std::find(slot.sequenceSources.begin(), slot.sequenceSources.end(), sequence) == slot.sequenceSources.end())
            return false;
    } else if (!filter.allSources && filter.source != source) {
        return false;
    }

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

// Do two note messages address the same note (group, channel, note number)?
bool sameNote(const MidiMessage& a, const MidiMessage& b) noexcept {
    return ump::group(a) == ump::group(b) && ump::channel(a) == ump::channel(b) &&
           ump::noteNumber(a) == ump::noteNumber(b);
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

// What MIDI learn captures: a note-on, or a controller at 64 or more (a press).
bool isPress(const MidiMessage& m) noexcept {
    if (ump::isNoteOn(m)) return true;
    if (!ump::isChannelVoice(m) || ump::status(m) != ump::Status::ControlChange) return false;
    return ump::messageType(m) == ump::MessageType::Midi2ChannelVoice ? m.words[1] >= 0x80000000u
                                                                        : (m.words[0] & 0x7F) >= 64;
}

// Sustain, sostenuto or soft pedal (CC 64, 66, 67), in either protocol.
bool isPedal(const MidiMessage& m) noexcept {
    if (!ump::isChannelVoice(m) || ump::status(m) != ump::Status::ControlChange) return false;
    const std::uint32_t controller = (m.words[0] >> 8) & 0x7F;
    return controller == 64 || controller == 66 || controller == 67;
}

// Do two pedal messages address the same pedal (group, channel, controller)?
bool samePedal(const MidiMessage& a, const MidiMessage& b) noexcept {
    return ump::group(a) == ump::group(b) && ump::channel(a) == ump::channel(b) &&
           ((a.words[0] >> 8) & 0x7F) == ((b.words[0] >> 8) & 0x7F);
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

std::uint64_t Engine::graphCommits() const noexcept {
    return issuedSequence.load(std::memory_order_relaxed);
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

void Engine::commitSwitch(std::unique_ptr<EngineGraph> graph, std::unique_ptr<SequenceSet> sequences,
                          const SwitchPlan& plan) {
    // Pending first, so setTempo() from here on waits for the switch too; then
    // the plan: the audio thread must never take the graph without it.
    switchesPending.fetch_add(1);
    setTempo(plan.tempoBpm);
    setTimeSignature(plan.numerator, plan.denominator);
    if (!switchPlans.push(QueuedSwitch{plan, issuedSequence.load(std::memory_order_relaxed) + 1})) {
        // Full: the audio thread hasn't run for 8 switches. The graph goes in
        // as an ordinary commit, with the tempo and signature set now.
        switchesPending.fetch_sub(1);
        setTempo(plan.tempoBpm);
        setTimeSignature(plan.numerator, plan.denominator);
    }
    commit(std::move(graph));
    commitSequences(std::move(sequences));
}

void Engine::commitSequences(std::unique_ptr<SequenceSet> sequences) {
    if (!sequences) sequences = std::make_unique<SequenceSet>(std::vector<CompiledSequence>{});
    delete pendingSequences.exchange(sequences.release(), std::memory_order_acq_rel);   // superseded, never ran
}

void Engine::armLearn(bool programChanges) noexcept {
    learnPrograms.store(programChanges, std::memory_order_relaxed);
    learnState.store(kLearnArmed, std::memory_order_release);
}

bool Engine::takeSetlistRequest(SetlistRequest& request) noexcept {
    return setlistRequests.pop(request);
}

void Engine::cancelLearn() noexcept {
    learnState.store(kLearnIdle, std::memory_order_release);
}

bool Engine::takeLearned(Learned& out) noexcept {
    if (learnState.load(std::memory_order_acquire) != kLearnCaptured) return false;
    out = learned;
    learnState.store(kLearnIdle, std::memory_order_release);
    return true;
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

bool Engine::auditionNote(MidiSourceHandle sequence, const MidiMessage& on, const MidiMessage& off,
                          double seconds) noexcept {
    return auditionRequests.push(AuditionRequest{auditionSourceOf(sequence), on, off, seconds, false});
}

bool Engine::endAuditions(MidiSourceHandle sequence, const MidiMessage* allNotesOff) noexcept {
    return auditionRequests.push(AuditionRequest{auditionSourceOf(sequence),
                                                 allNotesOff != nullptr ? *allNotesOff : MidiMessage{}, {}, 0.0, true});
}

void Engine::setClick(bool on, bool everyNote, float gain) noexcept {
    clickEveryNote.store(everyNote, std::memory_order_relaxed);
    clickGain.store(std::clamp(gain, 0.0f, 1.0f), std::memory_order_relaxed);
    clickOn.store(on, std::memory_order_relaxed);
}

bool Engine::sendTransportCommand(TransportCommand command) noexcept {
    return transportCommands.push(command);
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

// The rack's tempo and signature: now, or, while a switch is on its way, at
// the switch (it's the new rack's). The audio thread counts a switch done
// before it reads them, so neither a value nor the switch is ever missed.
void Engine::setTempo(double bpm) noexcept {
    const double clamped = std::clamp(bpm, kMinTempo, kMaxTempo);
    nextTempo.store(clamped);
    if (switchesPending.load() == 0) tempo.store(clamped);
}

void Engine::setTimeSignature(int numerator, int denominator) noexcept {
    const bool powerOfTwo = denominator > 0 && (denominator & (denominator - 1)) == 0;
    if (numerator < 1 || numerator > 32 || !powerOfTwo || denominator > 32) return;
    const std::uint32_t sig = (static_cast<std::uint32_t>(numerator) << 16) | static_cast<std::uint32_t>(denominator);
    nextSignature.store(sig);
    if (switchesPending.load() == 0) signature.store(sig);
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
    t.running          = publishedRunning.load(std::memory_order_relaxed);
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
    const bool switching = takeSwitch(ctx);
    if (!holdSwitch) adoptPendingGraph();
    advanceTransport(ctx);
    adoptSequences(ctx, switching);
    takeMidi(ctx);
    generateSequences(ctx);
    routeMidi(ctx.frameCount);
    runSlots(ctx);
    renderClick(ctx);

    clock.stopFrame = UINT32_MAX;
    if (clock.stopPending) {   // stopped in this block: the next one starts at beat 0, stopped
        clock.running       = false;
        clock.stopPending   = false;
        clock.beat          = 0.0;
        clock.barAnchorBeat = 0.0;
        clock.barAnchor     = 0;
    } else if (clock.running) {
        clock.beat += static_cast<double>(ctx.frameCount) * clock.beatsPerFrame;
    }
    publishedRunning.store(clock.running, std::memory_order_relaxed);
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
    t.playing             = clock.running;
    t.beatPosition        = clock.beat + static_cast<double>(frame) * clock.beatsPerFrame;
    const double barBeats = clock.numerator * 4.0 / clock.denominator;
    // A hair of tolerance, so a position exactly on a bar line isn't read as the
    // end of the previous bar after rounding.
    const double bars = std::floor((t.beatPosition - clock.barAnchorBeat) / barBeats + 1e-9);
    t.barStartBeats   = clock.barAnchorBeat + bars * barBeats;
    t.barNumber       = clock.barAnchor + static_cast<std::int32_t>(bars);
    return t;
}

// This block as the sequences see it, from the clock.
void Engine::makeSequenceBlock(const AudioCallbackContext& ctx) noexcept {
    const TimeInfo      now   = timeAt(ctx, 0);
    SequenceSet::Block& block = sequenceBlock;
    block.beat          = now.beatPosition;
    block.beatsPerFrame = clock.beatsPerFrame;
    block.tempoBpm      = clock.tempoBpm;
    block.frames        = ctx.frameCount;
    block.barStartBeats = now.barStartBeats;
    block.barBeats      = clock.numerator * 4.0 / clock.denominator;
}

// Beat 0 at `frame`: the clock is set back so that frame lands on it (plugins
// see a moment of pre-roll before it). At Start sequences start there.
void Engine::startTransport(const AudioCallbackContext& ctx, std::uint32_t frame) noexcept {
    if (clock.running && !clock.stopPending) return;
    clock.running       = true;
    clock.stopPending   = false;
    clock.beat          = -static_cast<double>(frame) * clock.beatsPerFrame;
    clock.barAnchorBeat = 0.0;
    clock.barAnchor     = 0;
    makeSequenceBlock(ctx);
    if (currentSequences != nullptr) currentSequences->transportStarted(sequenceBlock.from(frame));
}

// Every sequence ends at `frame`; the clock returns to beat 0 after this block.
void Engine::stopTransport(std::uint32_t frame) noexcept {
    if (!clock.running || clock.stopPending) return;
    if (currentSequences != nullptr)
        currentSequences->command(kInvalidMidiSource, SequenceCommand::StopNow, sequenceBlock.from(frame));
    clock.stopPending = true;
    clock.stopFrame   = frame;
}

void Engine::transportAction(const AudioCallbackContext& ctx, TransportCommand command, std::uint32_t frame) noexcept {
    const bool running = clock.running && !clock.stopPending;
    if (command == TransportCommand::Start || (command == TransportCommand::Toggle && !running))
        startTransport(ctx, frame);
    else
        stopTransport(frame);
}

void Engine::sequenceCommand(const AudioCallbackContext& ctx, MidiSourceHandle source, SequenceCommand command,
                             std::uint32_t frame) noexcept {
    if (currentSequences == nullptr) return;
    if ((!clock.running || clock.stopPending) && source != kInvalidMidiSource &&
        currentSequences->wouldStart(source, command))
        startTransport(ctx, frame);   // on beat 1, with no quantize wait
    currentSequences->command(source, command, sequenceBlock.from(frame));
}

// Adopt a new sequence snapshot, if any, and apply the transport's and the
// dock's commands (at the block's start).
void Engine::adoptSequences(const AudioCallbackContext& ctx, bool switching) noexcept {
    sequenceEventCount = 0;
    makeSequenceBlock(ctx);
    SequenceSet::Block& block = sequenceBlock;

    if (!holdSwitch && pendingSequences.load(std::memory_order_acquire) != nullptr &&
        (currentSequences == nullptr || retiredSequences.canPush())) {
        if (SequenceSet* next = pendingSequences.exchange(nullptr, std::memory_order_acq_rel)) {
            sequenceEventCount += next->adoptFrom(currentSequences, block, sequenceEvents, kMaxSequenceEvents);
            if (currentSequences != nullptr) retiredSequences.push(currentSequences);   // room checked above
            currentSequences = next;
        }
    }
    if (switching) applySwitch(ctx);   // before this block's commands: they act on the new song
    TransportCommand transport;
    while (transportCommands.pop(transport)) transportAction(ctx, transport, 0);
    QueuedCommand queued;
    while (sequenceCommands.pop(queued)) sequenceCommand(ctx, queued.source, queued.command, 0);
}

// This block's sequence events, sorted by (offset, order) for routeMidi() to
// merge with the input.
void Engine::generateSequences(const AudioCallbackContext& ctx) noexcept {
    if (currentSequences != nullptr) {
        const std::size_t room = kMaxSequenceEvents - sequenceEventCount;
        const std::size_t made = currentSequences->generate(sequenceBlock, sequenceEvents + sequenceEventCount, room);
        if (made == room) sequenceEventsFull.fetch_add(1, std::memory_order_relaxed);   // may have dropped some
        sequenceEventCount += made;
    }
    generateAuditions(ctx);

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

// Requests in order: an end ends its sequence's auditions (and with All Notes
// Off, sends a note-off for every key and the CC), and a new audition
// starts (ending any audition of the same note number first, so its note-off
// can't cut the new one short), all at the block's start. Then every
// audition whose time runs out in this block ends where it does.
void Engine::generateAuditions(const AudioCallbackContext& ctx) noexcept {
    auto push = [this](std::uint32_t offset, std::uint8_t order, MidiSourceHandle source, const MidiMessage& m) {
        if (sequenceEventCount == kMaxSequenceEvents) {
            sequenceEventsFull.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        sequenceEvents[sequenceEventCount++] = SequenceEvent{offset, order, source, m};
        return true;
    };
    AuditionRequest request;
    while (auditionRequests.pop(request)) {
        if (request.end) {
            for (AuditionVoice& voice : auditionVoices)
                if (voice.used && voice.source == request.source) {
                    push(0, 0, voice.source, voice.off);
                    voice.used = false;
                }
            if (request.on.wordCount > 0) {   // every key's note-off, in the CC's form, then the CC
                MidiMessage off = request.on;
                off.words[0]    = (off.words[0] & 0xFF0F0000u) | 0x00800000u;   // note-off, same type, group, channel
                if (off.wordCount > 1) off.words[1] = 0;
                for (std::uint32_t key = 0; key < 128; ++key) {
                    off.words[0] = (off.words[0] & 0xFFFF00FFu) | (key << 8);
                    push(0, 0, request.source, off);
                }
                push(0, 0, request.source, request.on);
            }
            continue;
        }
        AuditionVoice* free = nullptr;
        for (AuditionVoice& voice : auditionVoices) {
            if (voice.used && voice.source == request.source && sameNote(voice.off, request.off)) {
                push(0, 0, voice.source, voice.off);
                voice.used = false;
            }
            if (!voice.used && free == nullptr) free = &voice;
        }
        if (free == nullptr) {   // all sounding: the one nearest its end makes room
            free = &auditionVoices[0];
            for (AuditionVoice& voice : auditionVoices)
                if (voice.framesLeft < free->framesLeft) free = &voice;
            push(0, 0, free->source, free->off);
            free->used = false;
        }
        if (!push(0, 1, request.source, request.on)) continue;
        const double frames = std::max(1.0, request.seconds * ctx.time.sampleRate);
        *free = AuditionVoice{true, request.source, request.off, static_cast<std::uint64_t>(frames)};
    }
    for (AuditionVoice& voice : auditionVoices) {
        if (!voice.used) continue;
        if (voice.framesLeft < ctx.frameCount) {
            push(static_cast<std::uint32_t>(voice.framesLeft), 0, voice.source, voice.off);
            voice.used = false;
        } else {
            voice.framesLeft -= ctx.frameCount;
        }
    }
}

void Engine::adoptPendingGraph() noexcept {
    if (pendingGraph.load(std::memory_order_acquire) == nullptr) return;
    if (currentGraph != nullptr && !retiredGraphs.canPush()) return;   // main hasn't collected yet

    EngineGraph* const next = pendingGraph.exchange(nullptr, std::memory_order_acq_rel);
    if (next == nullptr) return;

    for (std::size_t i = 0; i < next->configs.size(); ++i) {   // new slots learn which pedals are down
        const SlotConfig& slot = next->configs[i];
        next->states[i].fresh  = slot.plugin && !slot.draining &&
                                (currentGraph == nullptr || !currentGraph->contains(slot.plugin.get()));
    }
    if (currentGraph != nullptr) {
        for (const SlotConfig& slot : currentGraph->configs)
            if (slot.plugin && !next->contains(slot.plugin.get())) slot.plugin->stopProcessing();
        for (const BusConfig& bus : currentGraph->busConfigs)   // and effects that left the rig
            for (const auto& effect : bus.effects)
                if (effect && !next->contains(effect.get())) effect->stopProcessing();
        retiredGraphs.push(currentGraph);   // room checked above; we're the only producer
    }
    currentGraph = next;
    adoptedSequence.store(next->sequence, std::memory_order_release);
}

bool Engine::takeSwitch(const AudioCallbackContext& ctx) noexcept {
    holdSwitch = false;
    for (QueuedSwitch next; switchPlans.pop(next);) {   // only the latest counts
        armedSwitch = next;
        switchArmed = true;
        ++switchesTaken;
    }
    if (!switchArmed) return false;
    // Not until its graph has been committed (it follows the plan at once).
    if (issuedSequence.load(std::memory_order_acquire) < armedSwitch.graph) {
        holdSwitch = true;
        return false;
    }
    switchBar = -1.0;
    const bool forced = switchForced.exchange(false, std::memory_order_acq_rel);
    if (armedSwitch.plan.atNextBar && clock.running && !clock.stopPending && !forced && clock.beatsPerFrame > 0.0) {
        const TimeInfo now      = timeAt(ctx, 0);
        const double   barBeats = clock.numerator * 4.0 / clock.denominator;
        double         bar      = now.barStartBeats;
        if (bar < now.beatPosition - 1e-9) bar += barBeats;   // the next line (or this one, if right on it)
        // (A hair of tolerance: a line right on the next block's start is that block's.)
        if ((bar - now.beatPosition) / clock.beatsPerFrame + 1e-6 >= static_cast<double>(ctx.frameCount)) {
            holdSwitch = true;   // a later block's
            return false;
        }
        switchBar = bar;
    }
    switchArmed = false;
    // The new rack's tempo and signature (as edited since, if they were), read
    // by advanceTransport() below. At a bar line, its bars count from that line.
    switchesPending.fetch_sub(std::exchange(switchesTaken, 0u));
    const std::uint32_t sig = nextSignature.load();
    tempo.store(nextTempo.load());
    signature.store(sig);
    if (switchBar >= 0.0) {
        const double oldBarBeats = clock.numerator * 4.0 / clock.denominator;   // the line's number, in the old bars
        clock.barAnchor += static_cast<std::int32_t>(std::floor((switchBar - clock.barAnchorBeat) / oldBarBeats + 1e-9));
        clock.barAnchorBeat = switchBar;
        clock.numerator     = static_cast<int>(sig >> 16);
        clock.denominator   = static_cast<int>(sig & 0xFFFF);
    }
    return true;
}

// The switch's transport: stopped, or running on with the new rack's At Start
// sequences from the next bar (the switch's own, when it's at one).
void Engine::applySwitch(const AudioCallbackContext& ctx) noexcept {
    if (!clock.running || clock.stopPending) return;
    if (armedSwitch.plan.stopTransport) {
        stopTransport(0);
        return;
    }
    if (currentSequences == nullptr) return;
    double bar = switchBar;
    if (bar < 0.0) {
        const TimeInfo now      = timeAt(ctx, 0);
        bar                     = now.barStartBeats;
        if (bar < now.beatPosition - 1e-9) bar += clock.numerator * 4.0 / clock.denominator;
    }
    SequenceSet::Block at = sequenceBlock;
    at.beat               = bar;
    currentSequences->transportStarted(at);
}

// Take the MIDI captured before this block started, placed one period behind
// (domain-model.md, sample-accurate alignment). MIDI learn and sequence
// triggers see it first; what they don't claim is staged for routeMidi().
void Engine::takeMidi(const AudioCallbackContext& ctx) noexcept {
    stagedCount = 0;
    const double        rate   = ctx.time.sampleRate;
    const std::uint32_t frames = ctx.frameCount;
    if (rate <= 0.0 || frames == 0) return;

    const Nanoseconds blockStart  = ctx.blockStartTime;
    const Nanoseconds windowStart = blockStart - std::llround(frames * 1e9 / rate);

    while (stagedCount < kMaxStagedMidi) {
        const QueuedMidi* next = midiQueue.peek();
        if (next == nullptr || next->message.captureTime >= blockStart) break;   // later: the next block's
        StagedMidi& entry = staged[stagedCount];
        midiQueue.pop(entry.item);
        countSource(entry.item.source);

        const Nanoseconds sinceWindow = entry.item.message.captureTime - windowStart;
        entry.offset                  = 0;
        if (sinceWindow < 0) {
            lateMidi.fetch_add(1, std::memory_order_relaxed);
        } else {
            const auto frame = static_cast<std::uint64_t>(static_cast<double>(sinceWindow) * rate / 1e9);
            entry.offset     = static_cast<std::uint32_t>(std::min<std::uint64_t>(frame, frames - 1));
        }

        const MidiMessage& m        = entry.item.message.message;
        const bool         learning = learnPrograms.load(std::memory_order_relaxed)
                                          ? ump::isChannelVoice(m) && ump::status(m) == ump::Status::ProgramChange
                                          : isPress(m);
        if (learning && learnState.load(std::memory_order_acquire) == kLearnArmed) {
            learned.source  = entry.item.source;
            learned.message = m;
            int armed       = kLearnArmed;   // unless the main thread cancelled meanwhile
            learnState.compare_exchange_strong(armed, kLearnCaptured, std::memory_order_acq_rel);
            continue;
        }
        if (currentSequences != nullptr) {
            // The setlist's triggers first: they win over the rack's (edge case 6).
            SequenceSet::SetlistHit setlistHit;
            bool                    setlistFired = false;
            if (currentSequences->matchSetlist(entry.item.source, m, setlistHit, setlistFired)) {
                if (setlistFired) setlistRequests.push(setlistHit);   // full: dropped (16 presses in one tick)
                continue;
            }
            SequenceSet::TriggerHit hits[16];
            bool                    matched = false;
            const std::size_t       fired =
                currentSequences->matchTriggers(entry.item.source, m, hits, std::size(hits), matched);
            for (std::size_t i = 0; i < fired; ++i) {
                const auto action = hits[i].action;
                if (hits[i].sequence == kInvalidMidiSource) {
                    transportAction(ctx,
                                    action == SequenceTrigger::Action::Start  ? TransportCommand::Start
                                    : action == SequenceTrigger::Action::Stop ? TransportCommand::Stop
                                                                              : TransportCommand::Toggle,
                                    entry.offset);
                } else {
                    sequenceCommand(ctx, hits[i].sequence,
                                    action == SequenceTrigger::Action::Start  ? SequenceCommand::Start
                                    : action == SequenceTrigger::Action::Stop ? SequenceCommand::Stop
                                                                              : SequenceCommand::Toggle,
                                    entry.offset);
                }
            }
            if (matched) continue;
            // Start on first note: the note starts the transport, and still plays.
            if (!clock.running && currentSequences->startsOnNote(entry.item.source, m))
                startTransport(ctx, entry.offset);
        }
        ++stagedCount;
    }
}

// Route the staged input and the sequences' events, merged in offset order
// (the input's offsets never decrease: the queue is in capture order).
void Engine::routeMidi(std::uint32_t frames) noexcept {
    if (currentGraph == nullptr) return;
    for (EngineGraph::SlotState& state : currentGraph->states) state.eventCount = 0;
    applyMutes();
    sendHeldPedals();

    std::size_t nextSequenceEvent = 0;
    auto routeSequencesUpTo = [&](std::uint32_t offset) {
        for (; nextSequenceEvent < sequenceEventCount && sequenceEvents[nextSequenceEvent].offset <= offset;
             ++nextSequenceEvent) {
            const SequenceEvent& e = sequenceEvents[nextSequenceEvent];
            QueuedMidi           item;
            item.source          = e.source;
            item.message.message = e.message;
            route(item, e.offset);
        }
    };
    for (std::size_t i = 0; i < stagedCount; ++i) {
        routeSequencesUpTo(staged[i].offset);
        route(staged[i].item, staged[i].offset);
    }
    routeSequencesUpTo(frames);
}

// A slot newly muted gets note-offs (at the block's start) for every note it
// holds, and leaves those notes' held entries; one unmuted is resumed.
void Engine::applyMutes() noexcept {
    for (std::size_t i = 0; i < currentGraph->configs.size(); ++i) {
        const SlotConfig& config  = currentGraph->configs[i];
        SlotRuntime&      runtime = *config.runtime;
        if (!runtime.muted.load(std::memory_order_relaxed)) {
            runtime.muteApplied = false;
            if (!config.draining) {   // resumed; a draining slot keeps counting toward its end
                runtime.suspended   = false;
                runtime.quietFrames = runtime.mutedFrames = 0;
            }
            continue;
        }
        if (runtime.muteApplied) continue;
        runtime.muteApplied = true;
        if (currentGraph->states[i].dialects == 0) continue;
        for (HeldNote& held : heldNotes) {
            if (!held.used) continue;
            for (std::uint8_t k = 0; k < held.count; ++k) {
                if (held.plugins[k] != config.plugin.get()) continue;
                MidiMessage off;   // MIDI 2.0: deliver() translates it for the slot's port
                off.words[0]  = 0x40000000u | (std::uint32_t{held.group} << 24) | ((0x80u | held.channel) << 16) |
                                (std::uint32_t{held.note} << 8);
                off.wordCount = 2;
                deliver(i, off, nullptr, 0, 0, held.deliveredNotes[k], nullptr);
                for (std::uint8_t j = k; j + 1 < held.count; ++j) {   // it no longer holds that note
                    held.plugins[j]        = held.plugins[j + 1];
                    held.deliveredNotes[j] = held.deliveredNotes[j + 1];
                }
                --held.count;
                --k;
            }
        }
    }
}

void Engine::trackPedal(MidiSourceHandle source, const MidiMessage& m) noexcept {
    HeldPedal* entry = nullptr;
    HeldPedal* free  = nullptr;
    for (HeldPedal& p : heldPedals) {
        if (p.used && p.source == source && samePedal(p.message, m)) entry = &p;
        if (!p.used && free == nullptr) free = &p;
    }
    if (!isPress(m)) {   // up
        if (entry != nullptr) entry->used = false;
        return;
    }
    if (entry == nullptr) entry = free;
    if (entry == nullptr) return;   // full: that pedal isn't passed on at a switch
    entry->used    = true;
    entry->source  = source;
    entry->message = m;
}

// At the block a graph is adopted: each pedal held down, to each new slot that
// hears its input, before anything else this block.
void Engine::sendHeldPedals() noexcept {
    for (std::size_t i = 0; i < currentGraph->configs.size(); ++i) {
        EngineGraph::SlotState& state = currentGraph->states[i];
        if (!state.fresh) continue;
        state.fresh = false;
        if (state.dialects == 0) continue;
        for (const HeldPedal& pedal : heldPedals) {
            if (!pedal.used || !accepts(currentGraph->configs[i], pedal.source, pedal.message)) continue;
            MidiMessage         upconverted[ump::kMaxTranslatedMessages];
            std::size_t         upconvertedCount = 0;
            ump::UpconvertState plain;   // a controller needs no stream state
            if (ump::messageType(pedal.message) == ump::MessageType::Midi1ChannelVoice)
                upconvertedCount = ump::upconvert(pedal.message, plain, upconverted, ump::kMaxTranslatedMessages);
            deliver(i, pedal.message, upconverted, upconvertedCount, 0, -1, nullptr);
        }
    }
}

void Engine::route(const QueuedMidi& item, std::uint32_t offset) noexcept {
    const MidiMessage& original = item.message.message;
    if (!isSequenceSource(item.source) && isPedal(original)) trackPedal(item.source, original);

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
        if (currentGraph->states[i].dialects == 0 || !accepts(config, item.source, original))
            continue;
        if (noteOn != nullptr && (config.runtime->muteApplied || config.draining)) continue;   // no new notes
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
            for (EngineGraph::BusState& bus : currentGraph->busStates)   // the sends below sum into these
                for (float* channel : bus.inputChannels) std::fill_n(channel, chunk, 0.0f);

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

                SlotRuntime& runtime = *config.runtime;
                if (config.draining && runtime.suspended)   // muted as well, and gone quiet: done too
                    runtime.drained.store(true, std::memory_order_relaxed);
                if (runtime.suspended && state.cursor == first) continue;   // nothing for it: not run, silent

                ProcessContext pc;
                pc.frameCount           = chunk;
                pc.time                 = timeAt(ctx, chunkStart);
                pc.audioOut             = AudioBuffer{state.channels.data(), state.audioChannels, chunk};
                pc.eventsIn             = EventBlock{state.events.data() + first, state.cursor - first};
                config.plugin->process(pc);
                if (runtime.suspended) {   // run only for its events (a controller, a note-off), and unheard
                    config.plugin->stopProcessing();
                    continue;
                }

                const double rate     = ctx.time.sampleRate;
                float        constant = 1.0f;
                const float* gains    = advanceGain(runtime.gain, runtime.targetGain.load(std::memory_order_relaxed),
                                                    chunk, rate, currentGraph->gainRamp.data(), constant);
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

                // Sends, after the fader: the slot's gain times each send level.
                for (std::size_t b = 0; b < currentGraph->busStates.size(); ++b) {
                    float*       sendBuffer = currentGraph->sendRamp.data();
                    float        sendLevel  = 0.0f;
                    const float* sends      = advanceGain(runtime.send[b], runtime.targetSend[b].load(std::memory_order_relaxed),
                                                          chunk, rate, sendBuffer, sendLevel);
                    if (sends == nullptr && sendLevel == 0.0f) continue;   // off
                    const float* combined   = nullptr;
                    float        level      = sendLevel * constant;
                    if (sends != nullptr || gains != nullptr) {
                        for (std::uint32_t f = 0; f < chunk; ++f)
                            sendBuffer[f] = (sends != nullptr ? sends[f] : sendLevel) * (gains != nullptr ? gains[f] : constant);
                        combined = sendBuffer;
                    } else if (level == 0.0f) {
                        continue;   // a muted slot sends nothing
                    }
                    EngineGraph::BusState& bus = currentGraph->busStates[b];
                    for (std::uint32_t c = 0; c < 2 && state.audioChannels > 0; ++c)   // mono feeds both sides
                        mixInto(bus.inputChannels[c], state.channels[std::min(c, state.audioChannels - 1)], chunk,
                                combined, level);
                }

                auto ownPeak = [&] {   // its own output, before the gain
                    float peak = 0.0f;
                    for (std::uint32_t c = 0; c < state.audioChannels; ++c)
                        for (std::uint32_t f = 0; f < chunk; ++f) peak = std::max(peak, std::fabs(state.channels[c][f]));
                    return peak;
                };
                if (runtime.muteApplied && runtime.gain.settledAt(0.0f)) {
                    runtime.quietFrames = ownPeak() < kQuietLevel ? runtime.quietFrames + chunk : 0;
                    runtime.mutedFrames += chunk;
                    if (runtime.quietFrames >= kQuietSeconds * rate || runtime.mutedFrames >= kMaxMutedSeconds * rate) {
                        runtime.suspended = true;
                        config.plugin->stopProcessing();
                    }
                } else if (config.draining && !runtime.muteApplied) {   // ringing out at its own gain
                    runtime.quietFrames = ownPeak() < kQuietLevel ? runtime.quietFrames + chunk : 0;
                    runtime.drainFrames += chunk;
                    if (runtime.quietFrames >= kQuietSeconds * rate || runtime.drainFrames >= kMaxDrainSeconds * rate) {
                        runtime.suspended = true;
                        config.plugin->stopProcessing();
                        runtime.drained.store(true, std::memory_order_relaxed);
                    }
                }
            }
            runBuses(ctx, chunkStart, chunk);
        }
    }

    // The master level, over the whole block (in chunks of the ramp buffer).
    for (std::uint32_t chunkStart = 0; chunkStart < ctx.frameCount; chunkStart += kMaxPluginBlockFrames) {
        const std::uint32_t chunk  = std::min(ctx.frameCount - chunkStart, kMaxPluginBlockFrames);
        float               master = 1.0f;
        const float*        gains  = advanceGain(masterRamp, masterTarget.load(std::memory_order_relaxed), chunk,
                                                 ctx.time.sampleRate, masterGains, master);
        if (gains == nullptr && master == 1.0f) continue;
        for (std::uint32_t c = 0; c < outputs; ++c) {
            float* out = ctx.output.channels[c] + chunkStart;
            for (std::uint32_t f = 0; f < chunk; ++f) out[f] *= gains != nullptr ? gains[f] : master;
        }
    }

    for (std::uint32_t c = 0; c < outputs; ++c) {
        float peak = 0.0f;
        for (std::uint32_t f = 0; f < ctx.frameCount; ++f) peak = std::max(peak, std::fabs(ctx.output.channels[c][f]));
        outputLevels.add(c, peak);
    }
}

// The click, on the aux outputs: a blip at each pulse in this block (before a
// stop in it), higher on the downbeat. A blip's tail runs on into the next
// block, and past a stop.
void Engine::renderClick(const AudioCallbackContext& ctx) noexcept {
    const AudioBuffer&  aux    = ctx.aux;
    const std::uint32_t frames = ctx.frameCount;
    for (std::uint32_t c = 0; c < aux.channelCount; ++c) std::fill_n(aux.channels[c], frames, 0.0f);
    const double rate = ctx.time.sampleRate;
    if (aux.channelCount == 0 || rate <= 0.0 || frames == 0) return;

    ClickVoice& v    = clickVoice;
    auto        play = [&](std::uint32_t from, std::uint32_t to) {
        for (std::uint32_t f = from; f < to && v.remaining > 0; ++f, --v.remaining) {
            const float sample = v.level * static_cast<float>(std::sin(v.phase));
            for (std::uint32_t c = 0; c < aux.channelCount; ++c) aux.channels[c][f] = sample;
            v.phase += v.step;
            v.level *= v.decay;
        }
    };

    auto startBlip = [&](bool downbeat, float gain) {
        v.remaining = static_cast<std::uint32_t>(std::lround(kClickSeconds * rate));
        v.phase     = 0.0;
        v.step      = 2.0 * 3.14159265358979323846 * kClickHz[downbeat ? 1 : 0] / rate;
        v.level     = gain;
        v.decay     = static_cast<float>(std::exp(-1.0 / (0.006 * rate)));   // 6 ms
    };
    if (clickTest.exchange(false, std::memory_order_relaxed)) startBlip(true, clickGain.load(std::memory_order_relaxed));

    std::uint32_t done = 0;
    if (clickOn.load(std::memory_order_relaxed) && clock.running && clock.beatsPerFrame > 0.0) {
        const MetronomePulse pulse =
            metronomePulse(clock.numerator, clock.denominator, clickEveryNote.load(std::memory_order_relaxed));
        const float    gain     = clickGain.load(std::memory_order_relaxed);
        const double   barStart = timeAt(ctx, 0).barStartBeats;
        const double   end      = clock.beat + frames * clock.beatsPerFrame;
        for (double k = std::ceil((clock.beat - barStart) / pulse.beats - 1e-9);; k += 1.0) {
            const double beat = barStart + k * pulse.beats;
            if (beat >= end - 1e-9) break;
            const double at = beat <= clock.beat ? 0.0 : std::floor((beat - clock.beat) / clock.beatsPerFrame + 1e-6);
            const auto   frame = static_cast<std::uint32_t>(std::min(at, static_cast<double>(frames - 1)));
            if (frame >= clock.stopFrame) break;
            play(done, frame);
            done               = frame;
            startBlip(k - pulse.perBar * std::floor(k / pulse.perBar) < 0.5, gain);
        }
    }
    play(done, frames);
}

// Advance the slot's gain by `frames`. While a ramp runs, fills the graph's
// scratch buffer with one gain per sample and returns it; otherwise returns
// null with the steady gain in `constantGain`. A new target restarts the ramp
// from wherever the gain is, so it always takes kGainRampSeconds to arrive,
// and the last step lands exactly on the target.
// Each bus: its effects in series on what was sent to it, then its return
// into its output pair at the return level (ramped), metered after the level.
void Engine::runBuses(const AudioCallbackContext& ctx, std::uint32_t chunkStart, std::uint32_t chunk) noexcept {
    const std::uint32_t outputs = ctx.output.channelCount;
    for (std::size_t b = 0; b < currentGraph->busStates.size(); ++b) {
        const BusConfig&       config = currentGraph->busConfigs[b];
        EngineGraph::BusState& state  = currentGraph->busStates[b];
        for (std::size_t e = 0; e < BusConfig::kEffects; ++e) {
            if (!config.effects[e]) continue;
            EngineGraph::BusState::Stage& stage = state.stages[e];
            ProcessContext                pc;
            pc.frameCount = chunk;
            pc.time       = timeAt(ctx, chunkStart);
            pc.audioIn    = ConstAudioBuffer{stage.in.data(), static_cast<std::uint32_t>(stage.in.size()), chunk};
            pc.audioOut   = AudioBuffer{stage.out.data(), static_cast<std::uint32_t>(stage.out.size()), chunk};
            config.effects[e]->process(pc);
        }

        SlotRuntime& runtime = *config.runtime;
        float        level   = 1.0f;
        const float* gains   = advanceGain(runtime.gain, runtime.targetGain.load(std::memory_order_relaxed), chunk,
                                           ctx.time.sampleRate, currentGraph->gainRamp.data(), level);
        if (state.silent) continue;
        for (std::uint32_t c = 0; c < 2; ++c) {
            const std::uint32_t out = config.firstOutput + c;
            runtime.meter.add(c, mixInto(out < outputs ? ctx.output.channels[out] + chunkStart : nullptr,
                                         state.returnChannels[c], chunk, gains, level));
        }
    }
}

const float* Engine::advanceGain(GainRamp& ramp, float target, std::uint32_t frames, double sampleRate, float* buffer,
                                 float& constantGain) noexcept {
    if (target != ramp.target) {
        const double length = std::round(kGainRampSeconds * sampleRate);
        ramp.target         = target;
        ramp.remaining      = length >= 1.0 ? static_cast<std::uint32_t>(length) : 1;
        ramp.step           = (target - ramp.current) / static_cast<float>(ramp.remaining);
    }
    if (ramp.remaining == 0) {
        constantGain = ramp.current;
        return nullptr;
    }
    for (std::uint32_t f = 0; f < frames; ++f) {
        if (ramp.remaining > 0) {
            if (--ramp.remaining == 0)
                ramp.current = ramp.target;
            else
                ramp.current += ramp.step;
        }
        buffer[f] = ramp.current;
    }
    return buffer;
}

} // namespace stagehand
