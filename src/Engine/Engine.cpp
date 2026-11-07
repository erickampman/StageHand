#include "Engine/Engine.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <utility>
#include <vector>

#include "uw/base/Clock.h"
#include "uw/base/Denormals.h"
#include "uw/ump/Ump.h"
#include "Core/UwNames.h"

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

void Engine::armTouch(bool on) noexcept {
    touchArmed.store(on, std::memory_order_release);
    if (!on)
        for (Touched dropped; touches.pop(dropped);) {}
}

bool Engine::takeTouched(Touched& touched) noexcept {
    return touchArmed.load(std::memory_order_acquire) && touches.pop(touched);
}

void Engine::parameterTouched(const HostedPlugin* plugin, std::uint32_t param) noexcept {
    if (touchArmed.load(std::memory_order_relaxed)) touches.push(Touched{plugin, param});   // full: dropped
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

bool Engine::sendLooperCommand(MidiSourceHandle looper, LooperCommand command) noexcept {
    return looperCommands.push(QueuedLooperCommand{looper, command});
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

void Engine::setCountIn(double bars) noexcept {
    countInBars.store(bars <= 0.0 ? 0.0 : bars < 1.0 ? 0.5 : std::min(std::round(bars), 2.0), std::memory_order_relaxed);
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

Engine::Panics Engine::panics() const noexcept {
    Panics p;
    p.count = panicCount.load(std::memory_order_acquire);
    p.cut   = panicCut.load(std::memory_order_relaxed);
    return p;
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
    t.countIn          = publishedCountIn.load(std::memory_order_relaxed);
    t.countInBeat      = publishedCountInBeat.load(std::memory_order_relaxed);
    t.countInPulses    = publishedCountInPulses.load(std::memory_order_relaxed);
    t.at               = publishedAt.load(std::memory_order_relaxed);
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
    const Nanoseconds          started = nowNanoseconds();   // the DSP load: this block's time over its period
    const ScopedFlushDenormals flush;   // for the whole block, plugins included; restored after
    const bool switching = takeSwitch(ctx);
    if (!holdSwitch) adoptPendingGraph();
    advanceTransport(ctx);
    countIn.inBlock  = countIn.active;
    countIn.endFrame = UINT32_MAX;
    adoptSequences(ctx, switching);
    takeMidi(ctx);
    advanceCountIn(ctx);
    recordLoops();
    generateSequences(ctx);
    routeMidi(ctx);
    runSlots(ctx);
    renderClick(ctx);
    publishLinks();

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
    int countingLeft = 0;
    if (countIn.active) {
        countIn.position += static_cast<double>(ctx.frameCount) * clock.beatsPerFrame;
        const int counted = static_cast<int>(std::floor(std::max(countIn.position, 0.0) / countIn.pulseBeats + 1e-9));
        countingLeft      = std::max(1, countIn.pulses - counted);
    }
    publishedCountIn.store(countingLeft, std::memory_order_relaxed);
    publishedCountInBeat.store(countIn.active ? std::max(countIn.position, 0.0) : 0.0, std::memory_order_relaxed);
    publishedCountInPulses.store(countIn.active ? countIn.pulses : 0, std::memory_order_relaxed);
    if (ctx.time.sampleRate > 0.0)   // the next block's start: where the published position is
        publishedAt.store(ctx.blockStartTime + std::llround(ctx.frameCount * 1e9 / ctx.time.sampleRate),
                          std::memory_order_relaxed);
    publishedRunning.store(clock.running, std::memory_order_relaxed);
    const TimeInfo now = timeAt(ctx, 0);   // the next block's start (the clock has moved)
    publishedBeat.store(now.beatPosition, std::memory_order_relaxed);
    publishedBarStart.store(now.barStartBeats, std::memory_order_relaxed);
    publishedBar.store(now.barNumber, std::memory_order_relaxed);

    if (ctx.time.sampleRate > 0.0 && ctx.frameCount > 0) {
        const double period = ctx.frameCount / ctx.time.sampleRate;   // seconds
        const auto   share  = static_cast<float>(static_cast<double>(nowNanoseconds() - started) * 1e-9 / period);
        loadAverage += static_cast<float>(std::min(1.0, period / kLoadAverageSeconds)) * (share - loadAverage);
        publishedLoadAverage.store(loadAverage, std::memory_order_relaxed);
        loadPeak.add(share);
    }
}

Engine::DspLoad Engine::takeDspLoad() noexcept {
    DspLoad load;
    load.average = publishedLoadAverage.load(std::memory_order_relaxed);
    load.peak    = loadPeak.take();
    return load;
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
    if (currentSequences != nullptr) {
        currentSequences->command(kInvalidMidiSource, SequenceCommand::StopNow, sequenceBlock.from(frame));
        currentSequences->stopLoopers(sequenceBlock.from(frame));   // and back to their start
    }
    clock.stopPending = true;
    clock.stopFrame   = frame;
}

void Engine::transportAction(const AudioCallbackContext& ctx, TransportCommand command, std::uint32_t frame) noexcept {
    if (countIn.active) {   // Start again: nothing; Stop or Toggle: cancelled
        if (command != TransportCommand::Start) cancelCountIn(frame);
        return;
    }
    const bool running = clock.running && !clock.stopPending;
    if (command == TransportCommand::Start || (command == TransportCommand::Toggle && !running))
        requestStart(ctx, frame, true);
    else
        stopTransport(frame);
}

void Engine::requestStart(const AudioCallbackContext& ctx, std::uint32_t frame, bool counted) noexcept {
    if (clock.running && !clock.stopPending) return;
    if (countIn.active) return;   // already counting: it starts at bar 1
    const double bars = countInBars.load(std::memory_order_relaxed);
    if (!counted || bars <= 0.0 || clock.beatsPerFrame <= 0.0) {
        startTransport(ctx, frame);
        return;
    }
    // The click's pulses (6/8: two a bar); half a bar is half of them, rounded up.
    const MetronomePulse pulse =
        metronomePulse(clock.numerator, clock.denominator, clickEveryNote.load(std::memory_order_relaxed));
    countIn.active        = true;
    countIn.inBlock       = true;
    countIn.pulseBeats    = pulse.beats;
    countIn.perBar        = std::max(pulse.perBar, 1);
    countIn.pulses        = bars < 1.0 ? (countIn.perBar + 1) / 2 : countIn.perBar * static_cast<int>(bars);
    countIn.position      = -static_cast<double>(frame) * clock.beatsPerFrame;
    countIn.endFrame      = UINT32_MAX;
    countIn.deferredCount = 0;
}

// Bar 1, if the count-in's last pulse ends in this block: the transport
// starts at that frame as a Start does, and the sequences started meanwhile
// start with it.
void Engine::advanceCountIn(const AudioCallbackContext& ctx) noexcept {
    if (!countIn.active || clock.beatsPerFrame <= 0.0) return;
    const double end   = static_cast<double>(countIn.pulses) * countIn.pulseBeats;
    const double frame = std::ceil((end - countIn.position) / clock.beatsPerFrame - 1e-6);
    if (frame >= static_cast<double>(ctx.frameCount)) return;
    const auto at    = static_cast<std::uint32_t>(std::max(frame, 0.0));
    countIn.active   = false;
    countIn.endFrame = at;
    startTransport(ctx, at);
    for (std::size_t i = 0; i < countIn.deferredCount; ++i)
        sequenceCommand(ctx, countIn.deferred[i].source, countIn.deferred[i].command, at);
    countIn.deferredCount = 0;
}

// Stopped before bar 1: nothing starts; armed loopers go back to empty.
void Engine::cancelCountIn(std::uint32_t frame) noexcept {
    countIn.active        = false;
    countIn.endFrame      = frame;
    countIn.deferredCount = 0;
    if (currentSequences != nullptr) currentSequences->stopLoopers(sequenceBlock.from(frame));
}

void Engine::sequenceCommand(const AudioCallbackContext& ctx, MidiSourceHandle source, SequenceCommand command,
                             std::uint32_t frame) noexcept {
    if (currentSequences == nullptr) return;
    if ((!clock.running || clock.stopPending) && source != kInvalidMidiSource &&
        currentSequences->wouldStart(source, command)) {
        requestStart(ctx, frame, true);   // on beat 1, with no quantize wait
        if (countIn.active) {             // counting in: it starts on bar 1, after it
            if (countIn.deferredCount < std::size(countIn.deferred))
                countIn.deferred[countIn.deferredCount++] = CountIn::Deferred{source, command};
            return;
        }
    }
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
            if (currentSequences != nullptr && !switching) next->keepControlLinks(*currentSequences);   // a switch re-arms pick-up
            sequenceEventCount += next->adoptFrom(currentSequences, block, sequenceEvents, kMaxSequenceEvents);
            if (currentSequences != nullptr) retiredSequences.push(currentSequences);   // room checked above
            currentSequences = next;
        }
    }
    if (switching && countIn.active) cancelCountIn(0);   // the old song's count-in
    if (switching) applySwitch(ctx);   // before this block's commands: they act on the new song
    TransportCommand transport;
    while (transportCommands.pop(transport)) transportAction(ctx, transport, 0);
    QueuedCommand queued;
    while (sequenceCommands.pop(queued)) sequenceCommand(ctx, queued.source, queued.command, 0);
    for (QueuedLooperCommand looper; looperCommands.pop(looper);) looperAction(ctx, looper.looper, looper.command, 0);
}

void Engine::looperAction(const AudioCallbackContext& ctx, MidiSourceHandle looper, LooperCommand command,
                          std::uint32_t frame) noexcept {
    if (currentSequences == nullptr) return;
    const bool running = clock.running && !clock.stopPending;
    if (currentSequences->looperCommand(looper, command, sequenceBlock.from(frame), running) &&
        !currentSequences->hasTransportStart())
        requestStart(ctx, frame, true);   // armed while stopped: the recording starts on its first beat (after a count-in)
}

// The loopers record what they hear of this block's input, each message at
// its own beat, before they play (docs/looper.md).
void Engine::recordLoops() noexcept {
    if (currentSequences == nullptr || currentSequences->looperList().empty()) return;
    for (std::size_t i = 0; i < stagedCount; ++i)
        if (!staged[i].consumed)   // a control link's own
            currentSequences->recordLoops(staged[i].item.source, staged[i].item.message.message,
                                      sequenceBlock.from(staged[i].offset));
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
        for (const SlotConfig& slot : currentGraph->configs) {
            if (slot.plugin && !next->contains(slot.plugin.get())) slot.plugin->stopProcessing();
            for (const auto& effect : slot.midiEffects)   // a MIDI effect that left (or moved off) its row
                if (!next->contains(effect.get())) effect->stopProcessing();
        }
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
    panicFrame  = UINT32_MAX;
    const double        rate   = ctx.time.sampleRate;
    const std::uint32_t frames = ctx.frameCount;
    if (rate <= 0.0 || frames == 0) return;   // a Panic asked for waits for a real block
    if (panicRequested.exchange(false, std::memory_order_relaxed)) panicFrame = 0;   // the button: the block's start

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
        entry.linked = entry.consumed = false;
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
            // Then the looper switch, for the current looper (docs/looper.md, "Controls").
            bool pressed = false, released = false;
            if (currentSequences->matchLooperSwitch(entry.item.source, m, pressed, released)) {
                const MidiSourceHandle looper = currentLooper.load(std::memory_order_relaxed);
                if (looper != kInvalidMidiSource && (pressed || released))
                    looperAction(ctx, looper, pressed ? LooperCommand::Press : LooperCommand::Release, entry.offset);
                continue;
            }
            // Then Panic's control (docs/panic.md): one Panic a block, at the first press.
            if (currentSequences->matchPanic(entry.item.source, m, pressed)) {
                if (pressed && panicFrame == UINT32_MAX) panicFrame = entry.offset;
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
            // Then the control links (docs/controls.md): applied in routeMidi(), in order.
            // Passed on as well if any of them passes it on.
            bool passOn = false;
            for (const CompiledControlLink& link : currentSequences->controlLinks()) {
                double travel = 0.0;
                if (!link.matches(entry.item.source, m, travel)) continue;
                entry.linked = true;
                passOn       = passOn || link.passOn;
            }
            entry.consumed = entry.linked && !passOn;
            // Start on first note: the note starts the transport, and still plays.
            if (!clock.running && !countIn.active && currentSequences->startsOnNote(entry.item.source, m))
                requestStart(ctx, entry.offset, false);   // the note is bar 1: no count-in
        }
        ++stagedCount;
    }
}

// Route the staged input and the sequences' events, merged in offset order
// (the input's offsets never decrease: the queue is in capture order). A Panic
// goes in before the first of them at or after its frame.
void Engine::routeMidi(const AudioCallbackContext& ctx) noexcept {
    bool silenced = panicFrame == UINT32_MAX;
    auto silenceFrom = [&](std::uint32_t offset) {
        if (silenced || offset < panicFrame) return;
        silenced = true;
        silence(ctx, panicFrame);
    };
    if (currentGraph == nullptr) {
        silenceFrom(panicFrame);   // nothing to send, but the tables still clear
        return;
    }
    for (EngineGraph::SlotState& state : currentGraph->states) {
        state.eventCount = 0;
        for (EngineGraph::EffectState& effect : state.effects) effect.paramCount = 0;
    }
    for (EngineGraph::BusState& bus : currentGraph->busStates)
        for (EngineGraph::BusState::Stage& stage : bus.stages) stage.eventCount = 0;
    for (ParameterRequest request; parameterRequests.pop(request);) {   // the host's own controls, first
        Event event;
        event.type     = EventType::ParamValue;
        event.param.id = request.param;
        event.value    = request.value;
        appendParameter(request.plugin, event);
        parameterChanged(request.plugin, request.param, request.value);
        parameterTouched(request.plugin, request.param);   // the generic editor's
    }
    applyMutes();
    sendHeldPedals();

    std::size_t nextSequenceEvent = 0;
    auto routeSequencesUpTo = [&](std::uint32_t offset) {
        for (; nextSequenceEvent < sequenceEventCount && sequenceEvents[nextSequenceEvent].offset <= offset;
             ++nextSequenceEvent) {
            const SequenceEvent& e = sequenceEvents[nextSequenceEvent];
            silenceFrom(e.offset);
            QueuedMidi           item;
            item.source          = e.source;
            item.message.message = e.message;
            route(item, e.offset);
        }
    };
    for (std::size_t i = 0; i < stagedCount; ++i) {
        routeSequencesUpTo(staged[i].offset);
        silenceFrom(staged[i].offset);
        if (staged[i].linked) applyLinks(staged[i].item.source, staged[i].item.message.message, staged[i].offset);
        if (!staged[i].consumed) route(staged[i].item, staged[i].offset);
    }
    routeSequencesUpTo(ctx.frameCount);
    silenceFrom(panicFrame);   // after everything before it, if nothing came after
}

// A Panic at `offset` (docs/panic.md): a note-off for every note sounding, to
// the plugin that has it at the note it got; every row's pedals up and All
// Notes Off on all 16 channels; the held tables, the sequences' and loopers'
// sounding notes and the auditions forgotten. Within kPanicCutSeconds of the
// last one, it also cuts the sound: All Sound Off first, and every row and bus
// effect reset.
void Engine::silence(const AudioCallbackContext& ctx, std::uint32_t offset) noexcept {
    const Nanoseconds now =
        ctx.blockStartTime + (ctx.time.sampleRate > 0.0 ? std::llround(offset * 1e9 / ctx.time.sampleRate) : 0);
    const bool cut = panickedOnce && now - lastPanicTime < std::llround(kPanicCutSeconds * 1e9);
    panickedOnce   = true;
    lastPanicTime  = now;

    if (currentGraph != nullptr) {
        for (const HeldNote& held : heldNotes) {
            if (!held.used) continue;
            MidiMessage off;   // MIDI 2.0: deliver() translates it for the slot's port
            off.words[0]  = 0x40000000u | (std::uint32_t{held.group} << 24) | ((0x80u | held.channel) << 16) |
                            (std::uint32_t{held.note} << 8);
            off.wordCount = 2;
            for (std::uint8_t k = 0; k < held.count; ++k)
                for (std::size_t i = 0; i < currentGraph->configs.size(); ++i)
                    if (currentGraph->configs[i].plugin.get() == held.plugins[k] && currentGraph->states[i].dialects != 0)
                        deliver(i, off, nullptr, 0, offset, held.deliveredNotes[k], nullptr);
        }
        static constexpr std::uint8_t kPedalsUp[] = {64, 66, 67, 123};
        for (std::size_t i = 0; i < currentGraph->configs.size(); ++i) {
            if (!currentGraph->configs[i].plugin || currentGraph->states[i].dialects == 0) continue;
            for (std::uint32_t channel = 0; channel < 16; ++channel) {
                auto send = [&](std::uint8_t controller) {
                    MidiMessage cc;   // MIDI 1.0, group 1; upconverted for a MIDI-2.0-only port
                    cc.words[0]  = 0x20000000u | ((0xB0u | channel) << 16) | (std::uint32_t{controller} << 8);
                    cc.wordCount = 1;
                    MidiMessage         upconverted[ump::kMaxTranslatedMessages];
                    ump::UpconvertState plain;   // a controller needs no stream state
                    const std::size_t   n = ump::upconvert(cc, plain, upconverted, ump::kMaxTranslatedMessages);
                    deliver(i, cc, upconverted, n, offset, -1, nullptr);
                };
                if (cut) send(120);   // All Sound Off
                for (const std::uint8_t controller : kPedalsUp) send(controller);
            }
        }
        for (const SlotConfig& config : currentGraph->configs)   // after its note-offs, past its MIDI effects
            if (config.plugin && config.runtime->retunedCount != 0) config.runtime->resetAt = offset;
        if (cut) {
            for (const SlotConfig& config : currentGraph->configs) {
                if (config.plugin) config.plugin->reset();
                for (const auto& effect : config.midiEffects) effect->reset();
            }
            for (const BusConfig& bus : currentGraph->busConfigs)
                for (const auto& effect : bus.effects)
                    if (effect) effect->reset();
        }
    }
    for (HeldNote& held : heldNotes) held.used = false;
    for (HeldPedal& pedal : heldPedals) pedal.used = false;
    for (AuditionVoice& voice : auditionVoices) voice.used = false;
    if (currentSequences != nullptr) currentSequences->forgetSounding();

    panicCut.store(cut, std::memory_order_relaxed);
    panicCount.fetch_add(1, std::memory_order_release);
}

// Control links (docs/controls.md): each link `message` is the controller of
// moves its targets, unless it's picking up and hasn't reached them yet. A
// target moved this way is where every other link on it now is: they pick it
// up again.
void Engine::applyLinks(MidiSourceHandle source, const MidiMessage& message, std::uint32_t offset) noexcept {
    if (currentSequences == nullptr) return;
    std::vector<CompiledControlLink>& links = currentSequences->controlLinks();
    for (CompiledControlLink& link : links) {
        double travel = 0.0;
        if (!link.matches(source, message, travel)) continue;
        link.travel = travel;
        for (CompiledControlLink::Target& t : link.targets) {
            const double mapped = t.map(travel);
            const double now = t.kind == CompiledControlLink::Target::Kind::Parameter ? t.current : linkLevel(t);
            if (link.pickUp && !t.picked && !t.pickUpAt(mapped, now)) continue;
            setLinkTarget(t, mapped, offset);
            t.current = mapped;
            for (CompiledControlLink& other : links)
                for (CompiledControlLink::Target& o : other.targets)
                    if (&o != &t && o.sameTarget(t)) o.moved(mapped);
        }
    }
}

void Engine::publishLinks() noexcept {
    std::uint32_t count = 0;
    if (currentSequences != nullptr && currentGraph != nullptr)   // linkLevel() looks in the graph
        for (CompiledControlLink& link : currentSequences->controlLinks())
            for (const CompiledControlLink::Target& t : link.targets) {
                if (count == kShownLinkTargets) break;
                ShownLinkTarget& shown = shownLinks[count++];
                shown.origin.store(t.origin, std::memory_order_relaxed);
                shown.value.store(t.kind == CompiledControlLink::Target::Kind::Parameter ? t.current : linkLevel(t),
                                  std::memory_order_relaxed);
                shown.travel.store(link.travel, std::memory_order_relaxed);
                shown.waiting.store(link.pickUp && !t.picked && link.travel >= 0.0, std::memory_order_relaxed);
            }
    shownLinkCount.store(count, std::memory_order_release);
}

std::vector<Engine::LinkState> Engine::linkStates() const {
    std::vector<LinkState> out(shownLinkCount.load(std::memory_order_acquire));
    for (std::size_t i = 0; i < out.size(); ++i) {
        const ShownLinkTarget& shown = shownLinks[i];
        out[i].origin  = shown.origin.load(std::memory_order_relaxed);
        out[i].value   = shown.value.load(std::memory_order_relaxed);
        out[i].travel  = shown.travel.load(std::memory_order_relaxed);
        out[i].waiting = shown.waiting.load(std::memory_order_relaxed);
    }
    return out;
}

SlotRuntime* Engine::findRuntime(const SlotRuntime* key) const noexcept {
    for (const SlotConfig& config : currentGraph->configs)
        if (config.runtime.get() == key) return config.runtime.get();
    for (const BusConfig& bus : currentGraph->busConfigs)
        if (bus.runtime.get() == key) return bus.runtime.get();
    return nullptr;
}

double Engine::linkLevel(const CompiledControlLink::Target& t) noexcept {
    using Kind = CompiledControlLink::Target::Kind;
    if (t.kind == Kind::Master) return linkMasterDb;
    const SlotRuntime* runtime = findRuntime(t.runtime);
    if (runtime == nullptr) return t.current;
    return t.kind == Kind::Send ? runtime->linkSendDb[std::min<std::size_t>(t.param, kBusCount - 1)] : runtime->linkGainDb;
}

void Engine::setLinkTarget(const CompiledControlLink::Target& t, double value, std::uint32_t offset) noexcept {
    using Kind = CompiledControlLink::Target::Kind;
    if (t.kind == Kind::Master) {
        linkMasterDb = value;
        return;
    }
    if (t.kind != Kind::Parameter) {   // a level: ramped where it's applied, as a fader is
        if (SlotRuntime* runtime = findRuntime(t.runtime)) {
            if (t.kind == Kind::Send) runtime->linkSendDb[std::min<std::size_t>(t.param, kBusCount - 1)] = value;
            else runtime->linkGainDb = value;
        }
        return;
    }
    Event event;
    event.sampleOffset = offset;
    event.type         = EventType::ParamValue;
    event.param.id     = t.param;
    event.value        = value;
    appendParameter(t.plugin, event);
}

void Engine::appendParameter(const HostedPlugin* plugin, const Event& event) noexcept {
    if (currentGraph == nullptr || plugin == nullptr) return;
    auto add = [&](std::vector<Event>& events, std::uint32_t& count) {
        if (count == events.size()) slotEventsFull.fetch_add(1, std::memory_order_relaxed);
        else events[count++] = event;
    };
    for (std::size_t i = 0; i < currentGraph->configs.size(); ++i) {   // a row's, or one of its MIDI effects
        const SlotConfig&       config = currentGraph->configs[i];
        EngineGraph::SlotState& state  = currentGraph->states[i];
        if (config.plugin.get() == plugin) add(state.events, state.eventCount);
        for (std::size_t e = 0; e < config.midiEffects.size(); ++e)
            if (config.midiEffects[e].get() == plugin) add(state.effects[e].params, state.effects[e].paramCount);
    }
    for (std::size_t b = 0; b < currentGraph->busConfigs.size(); ++b)   // or a bus effect's
        for (std::size_t e = 0; e < BusConfig::kEffects; ++e) {
            if (currentGraph->busConfigs[b].effects[e].get() != plugin) continue;
            EngineGraph::BusState::Stage& stage = currentGraph->busStates[b].stages[e];
            add(stage.events, stage.eventCount);
        }
}

void Engine::parameterChanged(const HostedPlugin* plugin, std::uint32_t param, double value) noexcept {
    if (currentSequences == nullptr) return;
    for (CompiledControlLink& link : currentSequences->controlLinks())
        for (CompiledControlLink::Target& t : link.targets)
            if (t.kind == CompiledControlLink::Target::Kind::Parameter && t.plugin == plugin && t.param == param &&
                std::fabs(value - t.current) > 1e-9)   // not the plugin echoing what a link sent
                t.moved(value);
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
    const SlotConfig& config = currentGraph->configs[slot];
    event.portIndex    = config.midiEffects.empty() ? config.notePort : 0;   // a MIDI effect's own: port 0
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
            state.cursor    = 0;
            state.processNs = 0;
            for (EngineGraph::EffectState& effect : state.effects) effect.paramCursor = 0;
            if (state.eventCount != 0)
                currentGraph->configs[i].runtime->midiEvents.fetch_add(state.eventCount, std::memory_order_relaxed);
        }
        for (EngineGraph::BusState& bus : currentGraph->busStates)
            for (EngineGraph::BusState::Stage& stage : bus.stages) stage.cursor = 0;

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
                const bool resets = runtime.resetAt != SlotRuntime::kNoReset && runtime.resetAt < chunkStart + chunk;
                if (runtime.suspended && state.cursor == first && !resets) continue;   // nothing for it: not run, silent
                auto stopRow = [&] {
                    for (const auto& effect : config.midiEffects) effect->stopProcessing();
                    config.plugin->stopProcessing();
                };

                ProcessContext pc;
                pc.frameCount           = chunk;
                pc.time                 = timeAt(ctx, chunkStart);
                pc.audioOut             = AudioBuffer{state.channels.data(), state.audioChannels, chunk};
                const Nanoseconds start = nowNanoseconds();
                pc.eventsIn             = runMidiEffects(i, pc.time, chunkStart, chunk,
                                                         EventBlock{state.events.data() + first, state.cursor - first});
                if (resets)
                    pc.eventsIn = addRetuneResets(i, pc.eventsIn, runtime.resetAt > chunkStart ? runtime.resetAt - chunkStart : 0);
                if ((state.pluginDialects & NoteDialect::Midi2) != 0) trackRetuned(runtime, pc.eventsIn);
                pc.eventsOut            = &feedback;
                feedback.plugin         = config.plugin.get();
                config.plugin->process(pc);
                state.processNs += nowNanoseconds() - start;
                if (runtime.suspended) {   // run only for its events (a controller, a note-off), and unheard
                    stopRow();
                    continue;
                }

                const double rate     = ctx.time.sampleRate;
                float        constant = 1.0f;
                const float  target   = runtime.targetGain.load(std::memory_order_relaxed) * linkLevelGain(runtime.linkGainDb);
                const float* gains    = advanceGain(runtime.gain, target, chunk, rate, currentGraph->gainRamp.data(), constant);
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
                    const float  sendTarget = runtime.targetSend[b].load(std::memory_order_relaxed) *
                                             linkLevelGain(runtime.linkSendDb[b]);
                    const float* sends      = advanceGain(runtime.send[b], sendTarget, chunk, rate, sendBuffer, sendLevel);
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
                        stopRow();
                    }
                } else if (config.draining && !runtime.muteApplied) {   // ringing out at its own gain
                    runtime.quietFrames = ownPeak() < kQuietLevel ? runtime.quietFrames + chunk : 0;
                    runtime.drainFrames += chunk;
                    if (runtime.quietFrames >= kQuietSeconds * rate || runtime.drainFrames >= kMaxDrainSeconds * rate) {
                        runtime.suspended = true;
                        stopRow();
                        runtime.drained.store(true, std::memory_order_relaxed);
                    }
                }
            }
            runBuses(ctx, chunkStart, chunk);
        }

        for (const SlotConfig& config : currentGraph->configs)   // resets still to send: from the next block's start
            if (config.runtime->resetAt != SlotRuntime::kNoReset)
                config.runtime->resetAt = config.runtime->retunedCount != 0 ? 0 : SlotRuntime::kNoReset;

        // Each row's and bus's share of the period.
        const double toShare = ctx.frameCount == 0 ? 0.0 : ctx.time.sampleRate / (ctx.frameCount * 1e9);
        for (std::size_t i = 0; i < currentGraph->configs.size(); ++i)
            currentGraph->configs[i].runtime->load.add(static_cast<float>(currentGraph->states[i].processNs * toShare));
        for (std::size_t b = 0; b < currentGraph->busStates.size(); ++b) {
            EngineGraph::BusState& bus = currentGraph->busStates[b];
            if (const auto& runtime = currentGraph->busConfigs[b].runtime)
                runtime->load.add(static_cast<float>(bus.processNs * toShare));
            bus.processNs = 0;
        }
    }

    // The master level, over the whole block (in chunks of the ramp buffer).
    for (std::uint32_t chunkStart = 0; chunkStart < ctx.frameCount; chunkStart += kMaxPluginBlockFrames) {
        const std::uint32_t chunk  = std::min(ctx.frameCount - chunkStart, kMaxPluginBlockFrames);
        float               master = 1.0f;
        const float         target = masterTarget.load(std::memory_order_relaxed) * linkLevelGain(linkMasterDb);
        const float*        gains  = advanceGain(masterRamp, target, chunk, ctx.time.sampleRate, masterGains, master);
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

namespace {

// Pitch 7.25 is MIDI 2.0's registered per-note controller 3.
bool isPitch725(const MidiMessage& m) noexcept {
    return ump::messageType(m) == ump::MessageType::Midi2ChannelVoice &&
           ump::status(m) == ump::Status::RegisteredPerNoteController && (m.words[0] & 0xFFu) == 3;
}

// A Per-Note Management message with its S flag: the key's controllers to their defaults.
bool isPerNoteReset(const MidiMessage& m) noexcept {
    return ump::messageType(m) == ump::MessageType::Midi2ChannelVoice &&
           ump::status(m) == ump::Status::PerNoteManagement && (m.words[0] & 0x01u) != 0;
}

std::uint32_t retunedKey(const MidiMessage& m) noexcept {
    return (std::uint32_t{ump::group(m)} * 16 + ump::channel(m)) * 128 + ump::noteNumber(m);
}

} // namespace

void Engine::trackRetuned(SlotRuntime& runtime, EventBlock input) noexcept {
    for (std::uint32_t k = 0; k < input.count; ++k) {
        const Event& e = input.events[k];
        if (e.type != EventType::Midi) continue;
        const bool on = isPitch725(e.midi);
        if (!on && !isPerNoteReset(e.midi)) continue;
        const std::uint32_t key  = retunedKey(e.midi);
        std::uint64_t&      word = runtime.retuned[key / 64];
        const std::uint64_t bit  = std::uint64_t{1} << (key % 64);
        if (on == ((word & bit) != 0)) continue;   // no change
        word ^= bit;
        on ? ++runtime.retunedCount : --runtime.retunedCount;
    }
}

// The resets go after the chunk's events up to `at` (the Panic's note-offs),
// with D and S set: a voice still ringing out keeps its pitch, and the key's
// next note starts in tune. They're marked sent here; trackRetuned() sees
// them again and changes nothing.
EventBlock Engine::addRetuneResets(std::size_t slot, EventBlock input, std::uint32_t at) noexcept {
    const SlotConfig&       config  = currentGraph->configs[slot];
    EngineGraph::SlotState& state   = currentGraph->states[slot];
    SlotRuntime&            runtime = *config.runtime;
    std::vector<Event>&     out     = state.withResets;
    if (out.size() < std::size_t{input.count} + EngineGraph::kMaxRetuneResets) {   // not MIDI 2.0: nothing was retuned
        runtime.resetAt = SlotRuntime::kNoReset;
        return input;
    }
    std::uint32_t n = 0, k = 0;
    while (k < input.count && input.events[k].sampleOffset <= at) out[n++] = input.events[k++];
    std::uint32_t sent = 0;
    for (std::uint32_t w = 0; w < runtime.retuned.size() && sent < EngineGraph::kMaxRetuneResets; ++w) {
        while (runtime.retuned[w] != 0 && sent < EngineGraph::kMaxRetuneResets) {
            const std::uint64_t word = runtime.retuned[w];
            const unsigned      bit  = static_cast<unsigned>(__builtin_ctzll(word));
            runtime.retuned[w]       = word & (word - 1);
            --runtime.retunedCount;
            const std::uint32_t key   = w * 64 + bit;
            const std::uint32_t note  = key % 128;
            const std::uint32_t chan  = (key / 128) % 16;
            const std::uint32_t group = key / (128 * 16);
            Event& e       = out[n++];
            e              = Event{};
            e.sampleOffset = at;
            e.type         = EventType::Midi;
            e.portIndex    = config.notePort;
            e.midi.words[0]  = 0x40000000u | (group << 24) | ((0xF0u | chan) << 16) | (note << 8) | 0x03u;   // D, S
            e.midi.wordCount = 2;
            ++sent;
        }
    }
    while (k < input.count) out[n++] = input.events[k++];
    if (runtime.retunedCount == 0) runtime.resetAt = SlotRuntime::kNoReset;
    else runtime.resetAt = 0;   // the rest from the next chunk on
    return EventBlock{out.data(), n};
}

EventBlock Engine::runMidiEffects(std::size_t slot, const TimeInfo& time, std::uint32_t chunkStart,
                                  std::uint32_t chunk, EventBlock input) noexcept {
    const SlotConfig&       config = currentGraph->configs[slot];
    EngineGraph::SlotState& state  = currentGraph->states[slot];
    const std::size_t       count  = config.midiEffects.size();
    if (count == 0) return input;

    // Two time-sorted lists into state.merged: `a`'s events that `keepA` takes
    // and `b`'s that `keepB` takes, `a`'s first at the same offset.
    auto merge = [&](EventBlock a, auto keepA, EventBlock b, auto keepB) {
        Event* const        out = state.merged.data();
        const std::uint32_t cap = static_cast<std::uint32_t>(state.merged.size());
        std::uint32_t       n = 0, i = 0, j = 0;
        auto put = [&](const Event& e) {
            if (n < cap) out[n++] = e;
            else slotEventsFull.fetch_add(1, std::memory_order_relaxed);
        };
        while (i < a.count || j < b.count) {
            if (i < a.count && !keepA(a.events[i])) { ++i; continue; }
            if (j < b.count && !keepB(b.events[j])) { ++j; continue; }
            if (j == b.count || (i < a.count && a.events[i].sampleOffset <= b.events[j].sampleOffset)) put(a.events[i++]);
            else put(b.events[j++]);
        }
        return EventBlock{out, n};
    };
    auto midi    = [](const Event& e) { return e.type == EventType::Midi; };
    auto notMidi = [](const Event& e) { return e.type != EventType::Midi; };
    auto all     = [](const Event&) { return true; };

    const EventBlock slotEvents   = input;   // its parameter changes are the plugin's
    EventBlock       previous     = input;   // MIDI only is taken from it while it's the slot's
    bool             previousSlot = true;
    std::size_t      written      = 0;       // chain buffers used: the next one's input is the other
    for (std::size_t e = 0; e < count; ++e) {
        EngineGraph::EffectState& effect = state.effects[e];

        // This chunk's parameter changes for this effect, rebased to the chunk start.
        const std::uint32_t firstParam = effect.paramCursor;
        while (effect.paramCursor < effect.paramCount &&
               effect.params[effect.paramCursor].sampleOffset < chunkStart + chunk)
            effect.params[effect.paramCursor++].sampleOffset -= chunkStart;
        const EventBlock params{effect.params.data() + firstParam, effect.paramCursor - firstParam};

        ProcessContext pc;
        pc.frameCount = chunk;
        pc.time       = time;
        pc.audioOut   = AudioBuffer{effect.channels.data(), effect.audioChannels, chunk};
        if (config.bypassed(e)) {   // its own parameter changes only; the MIDI goes around it
            pc.eventsIn     = params;
            pc.eventsOut    = &feedback;
            feedback.plugin = config.midiEffects[e].get();
            config.midiEffects[e]->process(pc);
            continue;
        }

        std::size_t next = e + 1;   // where its output goes: the next effect not bypassed, or the plugin
        while (next < count && config.bypassed(next)) ++next;
        const bool          last = next == count;
        std::vector<Event>& out  = state.chain[written++ % 2];
        pc.eventsIn = previousSlot ? merge(previous, midi, params, all) : merge(previous, all, params, all);

        chainSink.plugin    = config.midiEffects[e].get();
        chainSink.events    = out.data();
        chainSink.capacity  = static_cast<std::uint32_t>(out.size());
        chainSink.count     = 0;
        chainSink.frames    = chunk;
        chainSink.dialects  = last ? state.pluginDialects : state.effects[next].dialects;
        chainSink.port      = last ? config.notePort : 0;
        chainSink.upconvert = &effect.upconvert;
        pc.eventsOut        = &chainSink;
        config.midiEffects[e]->process(pc);

        // CLAP asks a plugin for its output in time order; keep it so if one
        // doesn't (an insertion sort: nothing to do when it's in order).
        Event* const events = out.data();
        for (std::uint32_t k = 1; k < chainSink.count; ++k)
            for (std::uint32_t j = k; j > 0 && events[j].sampleOffset < events[j - 1].sampleOffset; --j)
                std::swap(events[j], events[j - 1]);
        previous     = EventBlock{events, chainSink.count};
        previousSlot = false;
    }
    chainSink.plugin = nullptr;
    if (previousSlot) return input;   // every effect bypassed: the row's own events, as they were
    return merge(previous, all, slotEvents, notMidi);   // the plugin: the chain's MIDI, the row's parameter changes
}

void Engine::ChainSink::push(const Event& event) noexcept {
    if (event.type == EventType::ParamValue) {
        engine->parameterChanged(plugin, event.param.id, event.value);
        engine->parameterTouched(plugin, event.param.id);
        return;
    }
    if (event.type == EventType::ParamGestureBegin) {
        engine->parameterTouched(plugin, event.param.id);
        return;
    }
    if (event.type != EventType::Midi || dialects == 0) return;
    const std::uint32_t    offset = std::min(event.sampleOffset, frames > 0 ? frames - 1 : 0u);
    const MidiMessage&     m      = event.midi;
    const ump::MessageType type   = ump::messageType(m);
    if (dialectAccepts(dialects, m)) {   // its own protocol: unchanged
        add(m, offset);
        return;
    }
    MidiMessage translated[ump::kMaxTranslatedMessages];
    std::size_t n = 0;   // nothing is normal for an upconversion: bank select, RPN selection
    if (type == ump::MessageType::Midi2ChannelVoice && acceptsMidi1(dialects)) {
        n = ump::downconvert(m, translated, ump::kMaxTranslatedMessages);
    } else if (type == ump::MessageType::Midi1ChannelVoice && (dialects & NoteDialect::Midi2) != 0) {
        n = ump::upconvert(m, *upconvert, translated, ump::kMaxTranslatedMessages);
    } else {
        engine->unsupportedDialect.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    for (std::size_t j = 0; j < n; ++j) add(translated[j], offset);
}

void Engine::ChainSink::add(const MidiMessage& message, std::uint32_t offset) noexcept {
    if (count == capacity) {
        engine->slotEventsFull.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    Event& event       = events[count++];
    event              = Event{};
    event.sampleOffset = offset;
    event.type         = EventType::Midi;
    event.midi         = message;
    event.portIndex    = port;
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
    // The count-in's pulses, before bar 1 (or until it's cancelled), whether
    // the click is on or not. A pulse is a downbeat where it would be in the
    // bars before bar 1: half a bar in 4/4 counts "3, 4".
    if (countIn.inBlock && clock.beatsPerFrame > 0.0) {
        const float gain = clickGain.load(std::memory_order_relaxed);
        for (int k = std::max(0, static_cast<int>(std::ceil(countIn.position / countIn.pulseBeats - 1e-9)));
             k < countIn.pulses; ++k) {
            const double at = std::floor((k * countIn.pulseBeats - countIn.position) / clock.beatsPerFrame + 1e-6);
            if (at >= static_cast<double>(frames) || at >= static_cast<double>(countIn.endFrame)) break;
            const auto frame = static_cast<std::uint32_t>(std::max(at, 0.0));
            play(done, frame);
            done                = frame;
            const int inBar     = ((k - countIn.pulses) % countIn.perBar + countIn.perBar) % countIn.perBar;
            startBlip(inBar == 0, gain);
        }
    }
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
        const Nanoseconds start = nowNanoseconds();
        for (std::size_t e = 0; e < BusConfig::kEffects; ++e) {
            if (!config.effects[e]) continue;
            EngineGraph::BusState::Stage& stage = state.stages[e];
            ProcessContext                pc;
            pc.frameCount = chunk;
            pc.time       = timeAt(ctx, chunkStart);
            pc.audioIn    = ConstAudioBuffer{stage.in.data(), static_cast<std::uint32_t>(stage.in.size()), chunk};
            pc.audioOut   = AudioBuffer{stage.out.data(), static_cast<std::uint32_t>(stage.out.size()), chunk};
            const std::uint32_t first = stage.cursor;   // this chunk's events, rebased to its start
            while (stage.cursor < stage.eventCount && stage.events[stage.cursor].sampleOffset < chunkStart + chunk)
                stage.events[stage.cursor++].sampleOffset -= chunkStart;
            pc.eventsIn     = EventBlock{stage.events.data() + first, stage.cursor - first};
            pc.eventsOut    = &feedback;
            feedback.plugin = config.effects[e].get();
            config.effects[e]->process(pc);
        }
        state.processNs += nowNanoseconds() - start;

        SlotRuntime& runtime = *config.runtime;
        float        level   = 1.0f;
        const float  target  = runtime.targetGain.load(std::memory_order_relaxed) * linkLevelGain(runtime.linkGainDb);
        const float* gains   = advanceGain(runtime.gain, target, chunk, ctx.time.sampleRate,
                                           currentGraph->gainRamp.data(), level);
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
