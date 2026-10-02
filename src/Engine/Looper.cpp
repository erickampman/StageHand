// The looper's audio-thread side (docs/looper.md): recording, playback,
// overdubs and the footswitch's commands, as SequenceSet members. Everything
// works in beats, so a loop keeps time through tempo changes.

#include "Engine/Looper.h"

#include <algorithm>
#include <cmath>

#include "Core/Ump.h"
#include "Engine/SequenceSet.h"

namespace stagehand {

namespace {

constexpr double kEps = 1e-9;
constexpr double kInf = std::numeric_limits<double>::infinity();

using Note     = LooperRuntime::Note;
using Sounding = LooperRuntime::Sounding;

std::uint32_t frameAt(const SequenceSet::Block& b, double beat) noexcept {
    if (b.beatsPerFrame <= 0.0 || beat <= b.beat || b.frames == 0) return 0;
    const double frame = std::floor((beat - b.beat) / b.beatsPerFrame + 1e-6);
    return static_cast<std::uint32_t>(std::min(frame, static_cast<double>(b.frames - 1)));
}

double endOf(const SequenceSet::Block& b) noexcept {
    return b.beat + b.frames * b.beatsPerFrame;
}

// The next beat or bar line at or after the block's position.
double nextLine(const CompiledLooper& l, const SequenceSet::Block& b) noexcept {
    if (!l.atBar) return std::ceil(b.beat - 1e-6);
    return b.beat - b.barStartBeats < 1e-6 ? b.barStartBeats : b.barStartBeats + b.barBeats;
}

// One unit of the loop's grid: a bar or a beat.
double unitOf(const CompiledLooper& l, const SequenceSet::Block& b) noexcept {
    return l.atBar && b.barBeats > 0.0 ? b.barBeats : 1.0;
}

// A fixed length, in beats: its bars are the time signature's, whatever the grid.
double fixedBeats(const CompiledLooper& l, const SequenceSet::Block& b) noexcept {
    return l.fixedLength * (l.fixedInBars && b.barBeats > 0.0 ? b.barBeats : 1.0);
}

void put(SequenceEvent* out, std::size_t capacity, std::size_t& count, std::uint32_t offset, std::uint8_t order,
         MidiSourceHandle source, const MidiMessage& message) noexcept {
    if (count < capacity) out[count++] = SequenceEvent{offset, order, source, message};
}

// A note-off for a recorded note-on, in its protocol (velocity 0).
MidiMessage offFor(const MidiMessage& on) noexcept {
    MidiMessage off = on;
    off.words[0]    = (on.words[0] & 0xFF0FFF00u) | 0x00800000u;   // status 8; MIDI 1.0 velocity (or 2.0 attribute type) 0
    if (ump::messageType(on) == ump::MessageType::Midi2ChannelVoice) off.words[1] = 0;
    return off;
}

double quantized(const CompiledLooper& l, double beats) noexcept {
    return l.quantize > 0.0 ? std::round(beats / l.quantize) * l.quantize : beats;
}

// Every note the loop is sounding ends at `at` (a beat in this block, or its start).
void releaseSounding(LooperRuntime& rt, double at, std::uint32_t fromIndex = 0) noexcept {
    for (Sounding& s : rt.sounding)
        if (s.used && s.index >= fromIndex) s.offBeat = std::min(s.offBeat, at);
}

// Keys still held while recording: cut at `loopBeat` (a beat into the loop).
void closeOpen(LooperRuntime& rt, double loopBeat) noexcept {
    for (LooperRuntime::Open& o : rt.open) {
        if (!o.used) continue;
        Note& n = rt.notes[o.index];
        n.off   = std::max(loopBeat, n.on);
        o.used  = false;
    }
}

// Drop the notes from `start` on (a pass undone or a tap's provisional overdub).
void dropFrom(LooperRuntime& rt, std::uint32_t start, double now) noexcept {
    for (LooperRuntime::Open& o : rt.open)
        if (o.used && o.index >= start) o.used = false;
    releaseSounding(rt, now, start);
    rt.notes.resize(std::min<std::size_t>(start, rt.notes.size()));
}

// The first recording done: its length known, held keys cut at its end, and
// the loop plays from there on.
void finishRecording(LooperRuntime& rt) noexcept {
    rt.loopLength = rt.recordEnd - rt.startBeat;
    closeOpen(rt, rt.loopLength);
    for (Note& n : rt.notes) {
        if (n.on >= rt.loopLength - kEps) n.on = std::fmod(n.on, rt.loopLength);   // quantized past the end
        n.off = std::min(n.off < 0.0 ? rt.loopLength : n.off, rt.loopLength);       // cut at the end
        if (n.off <= n.on) n.off = rt.loopLength;
        n.firstCycle = 1;   // cycle 0 is the recording itself
    }
    rt.anchorBeat = rt.startBeat;
    rt.passStarts.assign(1, 0);
    rt.current = LooperState::Playing;
    if (rt.stopAfterRecording) rt.stopBeat = rt.recordEnd;
    rt.stopAfterRecording = false;
}

// An overdub ends: its held keys cut where it ended, the pass kept (past the
// last pass allowed, it joins the one before).
void endOverdub(LooperRuntime& rt, double now) noexcept {
    if (rt.notes.size() > rt.overdubStart && rt.passStarts.size() < LooperRuntime::kMaxPasses)
        rt.passStarts.push_back(rt.overdubStart);
    if (rt.loopLength > 0.0) {
        const double into = std::fmod(std::max(0.0, now - rt.anchorBeat), rt.loopLength);
        for (LooperRuntime::Open& o : rt.open) {
            if (!o.used) continue;
            Note& n = rt.notes[o.index];
            // Cut at the release, or at the loop's end if it wrapped since.
            const std::int64_t cycle = static_cast<std::int64_t>(std::floor((now - rt.anchorBeat) / rt.loopLength));
            n.off  = cycle > o.cycle ? rt.loopLength : std::max(into, n.on);
            o.used = false;
        }
    }
    rt.provisional = false;
    rt.current     = LooperState::Playing;
}

void stopNow(LooperRuntime& rt, double now) noexcept {
    switch (rt.current) {
        case LooperState::Empty: return;
        case LooperState::Armed:
            rt.notes.clear();   // the early notes, if any
            for (LooperRuntime::Open& o : rt.open) o.used = false;
            rt.current = LooperState::Empty;
            return;
        case LooperState::Recording:
            // The transport stopped mid-take: keep what was played, up to here
            // (a beat at least).
            rt.recordEnd = std::max(rt.startBeat + 1.0, std::min(rt.recordEnd, now));
            finishRecording(rt);
            break;
        case LooperState::Overdubbing:
            if (rt.provisional) dropFrom(rt, rt.overdubStart, now);   // a tap's overdub isn't kept
            else endOverdub(rt, now);
            rt.provisional = false;
            break;
        default: break;
    }
    releaseSounding(rt, now);
    rt.current  = LooperState::Stopped;
    rt.stopBeat = kInf;
}

} // namespace

LooperRuntime::LooperRuntime(std::size_t notesCapacity) : capacity(std::max<std::size_t>(notesCapacity, 1)) {
    notes.reserve(capacity);
    passStarts.reserve(kMaxPasses);
    snapshot.notes.reserve(capacity);
}

bool LooperRuntime::takeSnapshot(Snapshot& out) {
    if (!snapshotReady.load(std::memory_order_acquire)) return false;
    out = snapshot;
    snapshotReady.store(false, std::memory_order_release);
    return true;
}

Sequence loopToSequence(const LooperRuntime::Snapshot& loop, double quantize, bool atBar) {
    Sequence seq;
    seq.repeat = true;
    seq.launch = atBar ? LaunchQuantize::Bar : LaunchQuantize::Beat;
    // The grid: its quantize (units per beat, as a ratio), else 64ths (as fine
    // as a sequence goes), coarser if the loop would pass 4096 units.
    std::uint16_t num = 64, den = 1;
    if (quantize >= 1.0) {
        num = 1;
        den = static_cast<std::uint16_t>(std::clamp(std::lround(quantize), 1L, 64L));
    } else if (quantize > 0.0) {
        num = static_cast<std::uint16_t>(std::clamp(std::lround(1.0 / quantize), 1L, 64L));
    }
    while (num > 1 && loop.length * num / den > 4096.0) num /= 2;
    seq.rate               = SequenceRate{num, den};
    const double unitsPerBeat = static_cast<double>(num) / den;
    seq.length = static_cast<std::uint32_t>(std::clamp(std::lround(loop.length * unitsPerBeat), 1L, 4096L));

    if (!loop.notes.empty()) {
        const MidiMessage& first = loop.notes.front().onMessage;
        seq.protocol = ump::messageType(first) == ump::MessageType::Midi2ChannelVoice ? MidiProtocol::Midi2 : MidiProtocol::Midi1;
        seq.channel  = ump::channel(first);
        seq.group    = ump::group(first);
    }
    seq.edo           = 12;
    seq.referenceNote = 60;
    for (const LooperRuntime::Note& n : loop.notes) {
        SequenceNote note;
        const auto start = std::lround(n.on * unitsPerBeat);
        if (start < 0 || start >= static_cast<long>(seq.length)) continue;
        note.start  = static_cast<std::uint32_t>(start);
        note.length = static_cast<std::uint32_t>(
            std::clamp(std::lround((n.off - n.on) * unitsPerBeat), 1L, static_cast<long>(seq.length - note.start)));
        const std::uint8_t number = ump::noteNumber(n.onMessage);
        if (seq.protocol == MidiProtocol::Midi1) {
            note.pitch    = number;
            note.velocity = ump::messageType(n.onMessage) == ump::MessageType::Midi2ChannelVoice
                                ? static_cast<std::uint16_t>(std::max(1u, (n.onMessage.words[1] >> 25) & 0x7Fu))
                                : static_cast<std::uint16_t>(std::max(1u, n.onMessage.words[0] & 0x7Fu));
        } else {
            note.pitch    = static_cast<std::int32_t>(number) - seq.referenceNote;
            note.velocity = ump::messageType(n.onMessage) == ump::MessageType::Midi2ChannelVoice
                                ? static_cast<std::uint16_t>(std::max(1u, n.onMessage.words[1] >> 16))
                                : static_cast<std::uint16_t>(std::max(1u, (n.onMessage.words[0] & 0x7Fu) << 9));
        }
        seq.notes.push_back(note);
    }
    std::sort(seq.notes.begin(), seq.notes.end(), [](const SequenceNote& a, const SequenceNote& b) {
        return a.start != b.start ? a.start < b.start : a.pitch < b.pitch;
    });
    return seq;
}

// --- commands ----------------------------------------------------------------------

bool SequenceSet::looperCommand(MidiSourceHandle source, LooperCommand command, const Block& b, bool running) noexcept {
    bool wantsTransport = false;
    for (CompiledLooper& l : loopers) {
        if (l.source != source && source != kInvalidMidiSource) continue;
        LooperRuntime& rt = *l.runtime;
        switch (command) {
            case LooperCommand::Press:
                switch (rt.current) {
                    case LooperState::Empty:   // arm: at the next beat or bar, or when the transport starts
                        rt.notes.clear();
                        rt.passStarts.clear();
                        rt.droppedCount = 0;
                        rt.startKnown   = running;
                        rt.startBeat    = running ? nextLine(l, b) : 0.0;
                        rt.recordEnd    = kInf;
                        if (running && l.fixedLength > 0)
                            rt.recordEnd = rt.startBeat + fixedBeats(l, b);
                        rt.current     = LooperState::Armed;
                        wantsTransport = !running;
                        break;
                    case LooperState::Armed: stopNow(rt, b.beat); break;   // cancelled
                    case LooperState::Recording: {   // the end, rounded up to the grid
                        const double unit   = unitOf(l, b);
                        const double played = std::max(b.beat - rt.startBeat, 0.0);
                        const double end    = rt.startBeat + std::max(unit, std::ceil(played / unit - 1e-6) * unit);
                        rt.recordEnd        = std::min(rt.recordEnd, end);
                        break;
                    }
                    case LooperState::Playing:
                        if (rt.stopBeat < kInf) {   // a stop on its way: taken back
                            rt.stopBeat = kInf;
                            break;
                        }
                        // An overdub, provisionally: a tap drops it at release.
                        rt.overdubStart = static_cast<std::uint32_t>(rt.notes.size());
                        rt.provisional  = true;
                        rt.pressBeat   = b.beat;
                        rt.current     = LooperState::Overdubbing;
                        break;
                    case LooperState::Overdubbing: endOverdub(rt, b.beat); break;   // (a missed release)
                    case LooperState::Stopped:   // play from the start, at the next beat or bar
                        rt.anchorBeat = nextLine(l, b);
                        for (Note& n : rt.notes) n.firstCycle = 0;
                        rt.current = LooperState::Playing;
                        break;
                }
                break;
            case LooperCommand::Release:
                if (rt.current != LooperState::Overdubbing || !rt.provisional) break;
                if ((b.beat - rt.pressBeat) * 60.0 / std::max(b.tempoBpm, 1.0) < LooperRuntime::kHoldSeconds) {
                    // A tap: no overdub after all; the loop stops at the next beat or bar.
                    dropFrom(rt, rt.overdubStart, b.beat);
                    rt.provisional = false;
                    rt.current     = LooperState::Playing;
                    rt.stopBeat    = nextLine(l, b);
                } else {
                    endOverdub(rt, b.beat);
                }
                break;
            case LooperCommand::Stop:
                if (rt.current == LooperState::Armed) {
                    stopNow(rt, b.beat);
                } else if (rt.current == LooperState::Recording) {
                    const double unit = unitOf(l, b);
                    rt.recordEnd = std::min(rt.recordEnd, rt.startBeat + std::max(unit, std::ceil(std::max(b.beat - rt.startBeat, 0.0) / unit - 1e-6) * unit));
                    rt.stopAfterRecording = true;
                } else if (rt.current == LooperState::Playing || rt.current == LooperState::Overdubbing) {
                    if (rt.current == LooperState::Overdubbing) endOverdub(rt, b.beat);   // the pass is kept
                    rt.stopBeat = std::min(rt.stopBeat, nextLine(l, b));
                }
                break;
            case LooperCommand::Clear:
                releaseSounding(rt, b.beat);
                for (LooperRuntime::Open& o : rt.open) o.used = false;
                rt.notes.clear();
                rt.passStarts.clear();
                rt.droppedCount = 0;
                rt.loopLength   = 0.0;
                rt.stopBeat     = kInf;
                rt.provisional  = false;
                rt.current      = LooperState::Empty;
                break;
            case LooperCommand::Snapshot: {
                LooperRuntime::Snapshot& snap = rt.snapshot;
                const bool recorded = rt.loopLength > 0.0 && rt.current != LooperState::Empty &&
                                      rt.current != LooperState::Armed && rt.current != LooperState::Recording;
                snap.length = recorded ? rt.loopLength : 0.0;
                snap.notes.clear();
                if (recorded)
                    for (const Note& n : rt.notes)
                        if (n.off >= 0.0 && snap.notes.size() < snap.notes.capacity()) snap.notes.push_back(n);
                rt.snapshotReady.store(true, std::memory_order_release);
                break;
            }
            case LooperCommand::Undo:
                if (rt.current == LooperState::Overdubbing) {   // this pass
                    dropFrom(rt, rt.overdubStart, b.beat);
                    rt.provisional = false;
                    rt.current     = LooperState::Playing;
                } else if ((rt.current == LooperState::Playing || rt.current == LooperState::Stopped) &&
                           rt.passStarts.size() > 1) {   // back to the first recording, no further
                    dropFrom(rt, rt.passStarts.back(), b.beat);
                    rt.passStarts.pop_back();
                }
                break;
        }
        rt.state.store(rt.current, std::memory_order_relaxed);
    }
    return wantsTransport;
}

bool SequenceSet::hasTransportStart() const noexcept {
    return !transport.triggers.empty() || transport.startOnNote;
}

// --- recording -----------------------------------------------------------------------

void SequenceSet::recordLoops(MidiSourceHandle source, const MidiMessage& m, const Block& at) noexcept {
    if (loopers.empty() || isSequenceSource(source)) return;
    const bool on = ump::isNoteOn(m), off = !on && ump::isNoteOff(m);
    if (!on && !off) return;
    const std::uint8_t group = ump::group(m), channel = ump::channel(m), number = ump::noteNumber(m);
    const double       beat  = at.beat;

    for (CompiledLooper& l : loopers) {
        if ((l.input != kInvalidMidiSource && l.input != source) || ((l.channelMask >> channel) & 1u) == 0) continue;
        LooperRuntime& rt = *l.runtime;

        // Where this lands in the loop, if it's being recorded at all.
        double       loopBeat = 0.0;
        std::int64_t cycle    = 0;
        const bool   first    = (rt.current == LooperState::Armed || rt.current == LooperState::Recording) &&
                             rt.startKnown && beat >= rt.startBeat - LooperRuntime::kEarlyBeats && beat < rt.recordEnd;
        const bool   overdub  = rt.current == LooperState::Overdubbing && rt.loopLength > 0.0;
        if (first) {
            loopBeat = std::max(0.0, beat - rt.startBeat);   // a note just early counts as on the start
        } else if (overdub) {
            const double since = beat - rt.anchorBeat;
            cycle              = static_cast<std::int64_t>(std::floor(since / rt.loopLength));
            loopBeat           = since - cycle * rt.loopLength;
        } else if (on) {
            continue;
        }

        if (on) {
            if (!first && !overdub) continue;
            LooperRuntime::Open* slot = nullptr;
            for (LooperRuntime::Open& o : rt.open)
                if (!o.used) {
                    slot = &o;
                    break;
                }
            if (rt.notes.size() >= rt.capacity || slot == nullptr) {
                ++rt.droppedCount;   // the cap (docs/looper.md, 18)
                continue;
            }
            Note n;
            n.on = quantized(l, loopBeat);
            if (overdub) {
                if (n.on >= rt.loopLength - kEps) n.on -= rt.loopLength;   // rounded onto the next cycle's start
                n.firstCycle = cycle + 1;   // not again in the pass that's recording it
            }
            n.onMessage  = m;
            n.offMessage = offFor(m);
            rt.notes.push_back(n);
            *slot = LooperRuntime::Open{true, group, channel, number, static_cast<std::uint32_t>(rt.notes.size() - 1),
                                        beat, cycle};
        } else {
            for (LooperRuntime::Open& o : rt.open) {
                if (!o.used || o.group != group || o.channel != channel || o.note != number) continue;
                Note& n = rt.notes[o.index];
                // As long as it was played, from its (quantized) start; cut at the loop's end.
                double end = n.on + (beat - o.onBeat);
                if (rt.loopLength > 0.0) end = std::min(end, rt.loopLength);
                if (overdub && cycle > o.cycle) end = rt.loopLength;   // held across the loop's end: cut there
                n.off        = std::max(end, n.on);
                n.offMessage = m;
                o.used       = false;
                break;
            }
        }
    }
}

// --- playback ------------------------------------------------------------------------

std::size_t SequenceSet::generateLoops(const Block& b, SequenceEvent* out, std::size_t capacity) noexcept {
    std::size_t  count      = 0;
    const double end        = endOf(b);
    const double beatsPerMs = b.tempoBpm / 60000.0;

    for (CompiledLooper& l : loopers) {
        LooperRuntime& rt = *l.runtime;
        if (rt.current == LooperState::Armed && rt.startKnown && rt.startBeat < end - kEps)
            rt.current = LooperState::Recording;
        if (rt.current == LooperState::Recording && rt.recordEnd < end - kEps) finishRecording(rt);

        const bool playing = rt.current == LooperState::Playing || rt.current == LooperState::Overdubbing;
        if (playing && rt.loopLength > 0.0) {
            const double L      = rt.loopLength;
            const double from   = std::max(b.beat, rt.anchorBeat);
            const double until  = std::min(end, rt.stopBeat);
            if (from < until - kEps) {
                const auto k0 = static_cast<std::int64_t>(std::floor((from - rt.anchorBeat) / L + kEps));
                const auto k1 = static_cast<std::int64_t>(std::floor((until - rt.anchorBeat) / L));
                for (std::int64_t k = k0; k <= k1; ++k) {
                    const double cycleStart = rt.anchorBeat + static_cast<double>(k) * L;
                    for (std::size_t i = 0; i < rt.notes.size(); ++i) {
                        const Note& n = rt.notes[i];
                        if (k < n.firstCycle || n.off < 0.0) continue;   // not yet, or a key still held
                        const double beat = cycleStart + n.on;
                        if (beat < from - kEps || beat >= until - kEps) continue;
                        Sounding* slot = nullptr;
                        for (Sounding& s : rt.sounding) {
                            if (s.used && s.index == i) {   // still sounding from the last cycle: end it first
                                put(out, capacity, count, frameAt(b, beat), 0, l.source, s.off);
                                s.used = false;
                            }
                            if (!s.used && slot == nullptr) slot = &s;
                        }
                        if (slot == nullptr) continue;   // 64 notes sounding at once: this one is skipped
                        put(out, capacity, count, frameAt(b, beat), 1, l.source, n.onMessage);
                        *slot = Sounding{true, static_cast<std::uint32_t>(i),
                                         std::max(cycleStart + n.off, beat + beatsPerMs), n.offMessage};
                    }
                }
            }
            // An overdub held across the loop's end: its keys are cut there.
            if (rt.current == LooperState::Overdubbing) {
                const auto cycleNow = static_cast<std::int64_t>(std::floor((end - rt.anchorBeat) / L));
                for (LooperRuntime::Open& o : rt.open)
                    if (o.used && cycleNow > o.cycle) {
                        rt.notes[o.index].off = L;
                        o.used                = false;
                    }
            }
            if (rt.stopBeat < end - kEps) stopNow(rt, std::max(rt.stopBeat, b.beat));   // a quantized stop
        }

        // Note-offs due in this block.
        for (Sounding& s : rt.sounding) {
            if (!s.used || s.offBeat >= end - kEps) continue;
            put(out, capacity, count, frameAt(b, s.offBeat), 0, l.source, s.off);
            s.used = false;
        }

        rt.state.store(rt.current, std::memory_order_relaxed);
        rt.length.store(rt.current == LooperState::Recording || rt.current == LooperState::Armed ? 0.0 : rt.loopLength,
                        std::memory_order_relaxed);
        const bool sounding = (rt.current == LooperState::Playing || rt.current == LooperState::Overdubbing) &&
                              rt.loopLength > 0.0 && end > rt.anchorBeat;
        rt.position.store(sounding ? std::fmod(end - rt.anchorBeat, rt.loopLength) : 0.0, std::memory_order_relaxed);
        rt.noteCount.store(static_cast<std::uint32_t>(rt.notes.size()), std::memory_order_relaxed);
        rt.dropped.store(rt.droppedCount, std::memory_order_relaxed);
        rt.passes.store(static_cast<std::uint32_t>(rt.passStarts.size()), std::memory_order_relaxed);
        rt.waiting.store((rt.current == LooperState::Armed) || rt.stopBeat < kInf ||
                             (rt.current == LooperState::Playing && rt.anchorBeat > end),
                         std::memory_order_relaxed);
    }
    return count;
}

bool SequenceSet::matchLooperSwitch(MidiSourceHandle source, const MidiMessage& m, bool& pressed,
                                    bool& released) noexcept {
    pressed = released = false;
    if (!transport.hasLooperSwitch || !ump::isChannelVoice(m) || isSequenceSource(source)) return false;
    CompiledSequence::Trigger& t = transport.looperSwitch;
    const ump::Status status = ump::status(m);
    const bool        note   = status == ump::Status::NoteOn || status == ump::Status::NoteOff;
    if (!note && status != ump::Status::ControlChange) return false;
    if ((t.source != kInvalidMidiSource && t.source != source) || t.group != ump::group(m) ||
        t.channel != ump::channel(m) || t.number != ((m.words[0] >> 8) & 0x7F) ||
        (t.control == SequenceTrigger::Control::Note) != note)
        return false;
    if (note) {
        pressed  = ump::isNoteOn(m);
        released = !pressed;
        return true;
    }
    const bool high = ump::messageType(m) == ump::MessageType::Midi2ChannelVoice ? m.words[1] >= 0x80000000u
                                                                                : (m.words[0] & 0x7F) >= 64;
    pressed  = high && !t.high;
    released = !high && t.high;
    t.high   = high;
    return true;
}

// The transport started: a looper armed while it was stopped records from its first beat.
void SequenceSet::loopersTransportStarted(const Block& b) noexcept {
    for (CompiledLooper& l : loopers) {
        LooperRuntime& rt = *l.runtime;
        if (rt.current != LooperState::Armed || rt.startKnown) continue;
        rt.startKnown = true;
        rt.startBeat  = b.beat;
        if (l.fixedLength > 0) rt.recordEnd = rt.startBeat + fixedBeats(l, b);
    }
}

// The transport stopped (or a snapshot dropped them): every looper stops now,
// back to its start, its notes ended.
void SequenceSet::stopLoopers(const Block& b) noexcept {
    for (CompiledLooper& l : loopers) stopNow(*l.runtime, b.beat);
}

std::size_t SequenceSet::releaseLooper(const CompiledLooper& l, const Block& b, SequenceEvent* out,
                                       std::size_t capacity) noexcept {
    std::size_t count = 0;
    stopNow(*l.runtime, b.beat);
    for (Sounding& s : l.runtime->sounding)
        if (s.used) {
            put(out, capacity, count, 0, 0, l.source, s.off);
            s.used = false;
        }
    l.runtime->state.store(l.runtime->current, std::memory_order_relaxed);
    return count;
}

} // namespace stagehand
