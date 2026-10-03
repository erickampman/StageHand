#include "Engine/SequenceSet.h"

#include <algorithm>
#include <cmath>

#include "Core/Ump.h"

namespace stagehand {

namespace {

constexpr double kEps = 1e-9;

using Note = CompiledSequence::Note;

MidiMessage noteOnMessage(const CompiledSequence& s, std::uint8_t note, const Note& n) noexcept {
    MidiMessage m;
    const std::uint32_t head = (std::uint32_t{s.group} << 24) | ((0x90u | s.channel) << 16) | (std::uint32_t{note} << 8);
    if (s.protocol == MidiProtocol::Midi1) {
        m.words[0]  = 0x20000000u | head | std::clamp<std::uint32_t>(n.velocity, 1, 127);
        m.wordCount = 1;
    } else {
        m.words[0]  = 0x40000000u | head | 0x03u;   // attribute type 3: pitch 7.9
        m.words[1]  = (std::uint32_t{std::max<std::uint16_t>(n.velocity, 1)} << 16) | n.pitch79;
        m.wordCount = 2;
    }
    return m;
}

MidiMessage noteOffMessage(MidiProtocol protocol, std::uint8_t channel, std::uint8_t group,
                           std::uint8_t note) noexcept {
    MidiMessage m;
    const std::uint32_t head = (std::uint32_t{group} << 24) | ((0x80u | channel) << 16) | (std::uint32_t{note} << 8);
    if (protocol == MidiProtocol::Midi1) {
        m.words[0]  = 0x20000000u | head;
        m.wordCount = 1;
    } else {
        m.words[0]  = 0x40000000u | head;
        m.words[1]  = 0;
        m.wordCount = 2;
    }
    return m;
}

// The frame within the block where `beat` falls (clamped into the block). A hair
// of tolerance keeps a beat that lands exactly on a frame from rounding down.
std::uint32_t frameOf(const SequenceSet::Block& b, double beat) noexcept {
    if (b.beatsPerFrame <= 0.0 || beat <= b.beat || b.frames == 0) return 0;
    const double frame = std::floor((beat - b.beat) / b.beatsPerFrame + 1e-6);
    return static_cast<std::uint32_t>(std::min(frame, static_cast<double>(b.frames - 1)));
}

double blockEnd(const SequenceSet::Block& b) noexcept {
    return b.beat + b.frames * b.beatsPerFrame;
}

// Where a launch (or a quantized stop) requested now takes effect.
double boundary(LaunchQuantize quantize, const SequenceSet::Block& b) noexcept {
    switch (quantize) {
        case LaunchQuantize::Immediate: return b.beat;
        case LaunchQuantize::Beat:      return std::ceil(b.beat - kEps);
        case LaunchQuantize::Bar:
            return b.beat - b.barStartBeats < kEps ? b.barStartBeats : b.barStartBeats + b.barBeats;
    }
    return b.beat;
}

bool emit(SequenceEvent* out, std::size_t capacity, std::size_t& count, std::uint32_t offset, std::uint8_t order,
          MidiSourceHandle source, const MidiMessage& message) noexcept {
    if (count == capacity) return false;
    out[count++] = SequenceEvent{offset, order, source, message};
    return true;
}

// Release every sounding note of `s` at the block's start.
void releaseAll(const CompiledSequence& s, SequenceEvent* out, std::size_t capacity, std::size_t& count) noexcept {
    for (SequenceRuntime::Sounding& sounding : s.runtime->sounding) {
        if (!sounding.used) continue;
        emit(out, capacity, count, 0, 0, s.source, noteOffMessage(s.protocol, s.channel, s.group, sounding.note));
        sounding.used = false;
    }
}

// MIDI 1.0: the note number itself. MIDI 2.0: the free note number nearest the
// pitch (docs/domain-model.md, "nearest free").
std::uint8_t chooseNote(const CompiledSequence& s, const Note& n) noexcept {
    if (s.protocol == MidiProtocol::Midi1) return static_cast<std::uint8_t>(n.semitones);
    bool taken[128] = {};
    for (const SequenceRuntime::Sounding& sounding : s.runtime->sounding)
        if (sounding.used) taken[sounding.note] = true;
    int    best     = static_cast<int>(std::lround(n.semitones));
    double bestDist = 1e9;
    for (int note = 0; note < 128; ++note) {
        const double dist = std::fabs(note - n.semitones);
        if (!taken[note] && dist < bestDist) {
            best     = note;
            bestDist = dist;
        }
    }
    return static_cast<std::uint8_t>(std::clamp(best, 0, 127));
}

// A note's pitch resolved to what's sent (its length as written).
Note compileNote(const Sequence& sequence, const SequenceNote& note) noexcept {
    Note n;
    n.start  = note.start;
    n.length = note.length;
    n.pitch  = note.pitch;
    if (sequence.protocol == MidiProtocol::Midi1) {
        n.semitones = std::clamp(note.pitch, 0, 127);
        n.velocity  = std::clamp<std::uint16_t>(note.velocity, 1, 127);
    } else {
        const int edo = sequence.edo > 0 ? sequence.edo : 12;
        n.semitones   = std::clamp(sequence.referenceNote + note.pitch * 12.0 / edo, 0.0, 127.0 + 511.0 / 512.0);
        n.pitch79     = static_cast<std::uint16_t>(std::clamp(std::lround(n.semitones * 512.0), 0L, 65535L));
        n.velocity    = std::max<std::uint16_t>(note.velocity, 1);
    }
    return n;
}

} // namespace

AuditionMessages auditionMessages(const Sequence& sequence, const SequenceNote& note) noexcept {
    CompiledSequence s;   // only what the messages need
    s.protocol        = sequence.protocol;
    s.channel         = static_cast<std::uint8_t>(sequence.channel & 0x0F);
    s.group           = static_cast<std::uint8_t>(sequence.group & 0x0F);
    const Note n      = compileNote(sequence, note);
    const auto number = static_cast<std::uint8_t>(std::clamp(std::lround(n.semitones), 0L, 127L));
    return AuditionMessages{noteOnMessage(s, number, n), noteOffMessage(s.protocol, s.channel, s.group, number)};
}

CompiledSequence CompiledSequence::compile(const Sequence& sequence, MidiSourceHandle source,
                                           std::shared_ptr<SequenceRuntime> runtime) {
    CompiledSequence c;
    c.source          = source;
    c.runtime         = std::move(runtime);
    c.protocol        = sequence.protocol;
    c.channel         = static_cast<std::uint8_t>(sequence.channel & 0x0F);
    c.group           = static_cast<std::uint8_t>(sequence.group & 0x0F);
    c.unitsPerBeat    = sequence.rate.unitsPerBeat() > 0.0 ? sequence.rate.unitsPerBeat() : 4.0;
    c.length          = std::max<std::uint32_t>(sequence.length, 1);
    c.launch          = sequence.launch;
    c.repeat          = sequence.repeat;
    c.noteOffOffsetMs = sequence.noteOffOffsetMs;
    c.atStart         = sequence.atStart;

    for (const SequenceNote& note : sequence.notes) {
        if (note.length == 0 || note.start >= c.length) continue;
        Note n   = compileNote(sequence, note);
        n.length = std::min(note.length, c.length - note.start);
        c.notes.push_back(n);
    }
    std::sort(c.notes.begin(), c.notes.end(), [](const Note& a, const Note& b) {
        return a.start != b.start ? a.start < b.start : a.pitch < b.pitch;
    });
    return c;
}

std::size_t SequenceSet::adoptFrom(const SequenceSet* previous, const Block& b, SequenceEvent* out,
                                   std::size_t capacity) noexcept {
    std::size_t count = 0;
    auto        find  = [](const std::vector<CompiledSequence>& list, MidiSourceHandle source) -> const CompiledSequence* {
        for (const CompiledSequence& s : list)
            if (s.source == source) return &s;
        return nullptr;
    };

    if (previous != nullptr) {
        for (CompiledSequence::Trigger& t : transport.triggers)   // a held controller doesn't act again
            for (const CompiledSequence::Trigger& o : previous->transport.triggers)
                if (o.source == t.source && o.control == t.control && o.group == t.group && o.channel == t.channel &&
                    o.number == t.number)
                    t.high = o.high;
        for (CompiledSetlistTrigger& t : transport.setlist)   // nor a setlist pedal
            for (const CompiledSetlistTrigger& o : previous->transport.setlist)
                if (o.source == t.source && o.control == t.control && o.group == t.group && o.channel == t.channel &&
                    o.number == t.number)
                    t.high = o.high;
        for (const CompiledLooper& old : previous->loopers) {   // loopers gone: stopped, their notes ended
            const bool kept = std::any_of(loopers.begin(), loopers.end(),
                                          [&](const CompiledLooper& l) { return l.runtime == old.runtime; });
            if (!kept) count += releaseLooper(old, b, out + count, capacity - count);
        }
        for (const CompiledSequence& old : previous->all) {
            if (find(all, old.source) != nullptr) continue;
            releaseAll(old, out, capacity, count);   // removed: its notes end now
            old.runtime->current = SequenceState::Stopped;
            old.runtime->state.store(SequenceState::Stopped, std::memory_order_relaxed);
        }
    }

    const double beatsPerMs = b.tempoBpm / 60000.0;
    for (CompiledSequence& s : all) {
        SequenceRuntime& rt = *s.runtime;
        // A new channel, group or protocol: end what sounds on the old one.
        if (const CompiledSequence* old = previous ? find(previous->all, s.source) : nullptr;
            old != nullptr && (old->channel != s.channel || old->group != s.group || old->protocol != s.protocol))
            releaseAll(*old, out, capacity, count);

        // A new rate or length keeps the loop's phase (the same fraction through it).
        const bool running = rt.current == SequenceState::Playing || rt.current == SequenceState::Stopping;
        if (running && rt.unitsPerBeat > 0.0 && rt.length > 0 &&
            (rt.unitsPerBeat != s.unitsPerBeat || rt.length != s.length)) {
            const double units    = std::max(0.0, (b.beat - rt.anchorBeat) * rt.unitsPerBeat);
            const double fraction = std::fmod(units, static_cast<double>(rt.length)) / rt.length;
            rt.anchorBeat         = b.beat - fraction * s.length / s.unitsPerBeat;
            if (!s.repeat && rt.current == SequenceState::Playing) rt.stopBeat = rt.anchorBeat + s.length / s.unitsPerBeat;
        }

        // Sounding notes follow their note: deleted ends now, a new length moves the end.
        for (SequenceRuntime::Sounding& sounding : rt.sounding) {
            if (!sounding.used) continue;
            const auto match = std::find_if(s.notes.begin(), s.notes.end(), [&](const Note& n) {
                return n.start == sounding.start && n.pitch == sounding.pitch;
            });
            if (match == s.notes.end()) {
                sounding.offBeat = b.beat;
                continue;
            }
            const double end = sounding.onBeat + match->length / s.unitsPerBeat + s.noteOffOffsetMs * beatsPerMs;
            sounding.offBeat = std::max({end, sounding.onBeat + beatsPerMs, b.beat});
        }
        rt.unitsPerBeat = s.unitsPerBeat;
        rt.length       = s.length;

        // A controller held down through an edit doesn't act again.
        if (const CompiledSequence* old = previous ? find(previous->all, s.source) : nullptr)
            for (CompiledSequence::Trigger& t : s.triggers)
                for (const CompiledSequence::Trigger& o : old->triggers)
                    if (o.source == t.source && o.control == t.control && o.group == t.group &&
                        o.channel == t.channel && o.number == t.number)
                        t.high = o.high;
    }
    return count;
}

SequenceSet::Block SequenceSet::Block::from(std::uint32_t frame) const noexcept {
    Block b  = *this;
    frame    = std::min(frame, frames);
    b.beat  += frame * beatsPerFrame;
    b.frames = frames - frame;
    while (barBeats > 0.0 && b.beat - b.barStartBeats >= barBeats - kEps) b.barStartBeats += barBeats;
    return b;
}

void SequenceSet::command(MidiSourceHandle source, SequenceCommand command, const Block& b) noexcept {
    for (CompiledSequence& s : all)
        if (source == kInvalidMidiSource || s.source == source) apply(s, command, b);
}

namespace {

// Does trigger `t` match this message? Updates a controller's edge; `fires` is
// true for a note-on, or a controller rising to 64 or more.
bool matchOne(CompiledSequence::Trigger& t, MidiSourceHandle source, bool note, std::uint8_t group,
              std::uint8_t channel, std::uint8_t number, bool high, bool noteOn, bool& fires) noexcept {
    if ((t.source != kInvalidMidiSource && t.source != source) || t.group != group || t.channel != channel ||
        t.number != number || (t.control == SequenceTrigger::Control::Note) != note)
        return false;
    fires = note ? noteOn : high && !t.high;
    if (!note) t.high = high;
    return true;
}

} // namespace

void SequenceSet::keepControlLinks(const SequenceSet& previous) noexcept {
    for (CompiledControlLink& link : transport.links)
        for (const CompiledControlLink& old : previous.transport.links) {
            if (!old.sameSource(link)) continue;
            link.travel = old.travel;
            for (CompiledControlLink::Target& t : link.targets)
                for (const CompiledControlLink::Target& o : old.targets) {
                    if (!o.sameTarget(t)) continue;
                    t.current  = o.current;
                    t.picked   = o.picked;
                    t.hasLast  = o.hasLast;
                    t.last     = o.last;
                }
        }
}

std::size_t SequenceSet::matchTriggers(MidiSourceHandle source, const MidiMessage& m, TriggerHit* hits,
                                       std::size_t capacity, bool& matched) noexcept {
    matched = false;
    if (!ump::isChannelVoice(m) || isSequenceSource(source)) return 0;
    const ump::Status status = ump::status(m);
    const bool        note   = status == ump::Status::NoteOn || status == ump::Status::NoteOff;
    if (!note && status != ump::Status::ControlChange) return 0;
    const std::uint8_t group = ump::group(m), channel = ump::channel(m);
    const std::uint8_t number = static_cast<std::uint8_t>((m.words[0] >> 8) & 0x7F);   // note or controller
    const bool         high   = ump::messageType(m) == ump::MessageType::Midi2ChannelVoice
                                    ? m.words[1] >= 0x80000000u
                                    : (m.words[0] & 0x7F) >= 64;
    const bool         noteOn = ump::isNoteOn(m);

    std::size_t count = 0;
    auto        check = [&](CompiledSequence::Trigger& t, MidiSourceHandle sequence) {
        bool fires = false;
        if (!matchOne(t, source, note, group, channel, number, high, noteOn, fires)) return;
        matched = true;
        if (fires && count < capacity) hits[count++] = TriggerHit{sequence, t.action};
    };
    for (CompiledSequence::Trigger& t : transport.triggers) check(t, kInvalidMidiSource);
    for (CompiledSequence& s : all)
        for (CompiledSequence::Trigger& t : s.triggers) check(t, s.source);
    return count;
}

bool SequenceSet::matchSetlist(MidiSourceHandle source, const MidiMessage& m, SetlistHit& hit, bool& fired) noexcept {
    fired = false;
    if (transport.setlist.empty() || !ump::isChannelVoice(m) || isSequenceSource(source)) return false;
    const ump::Status  status = ump::status(m);
    const std::uint8_t group = ump::group(m), channel = ump::channel(m);
    if (status == ump::Status::ProgramChange) {
        for (const CompiledSetlistTrigger& t : transport.setlist) {
            if (t.action != SetlistTrigger::Action::Song || (t.source != kInvalidMidiSource && t.source != source) ||
                t.group != group || t.channel != channel)
                continue;
            hit.action  = SetlistTrigger::Action::Song;
            hit.program = static_cast<std::uint8_t>(ump::messageType(m) == ump::MessageType::Midi2ChannelVoice
                                                        ? (m.words[1] >> 24) & 0x7F
                                                        : (m.words[0] >> 8) & 0x7F);
            fired       = true;
            return true;
        }
        return false;
    }
    const bool note = status == ump::Status::NoteOn || status == ump::Status::NoteOff;
    if (!note && status != ump::Status::ControlChange) return false;
    const std::uint8_t number = static_cast<std::uint8_t>((m.words[0] >> 8) & 0x7F);
    const bool         high   = ump::messageType(m) == ump::MessageType::Midi2ChannelVoice
                                    ? m.words[1] >= 0x80000000u
                                    : (m.words[0] & 0x7F) >= 64;
    for (CompiledSetlistTrigger& t : transport.setlist) {
        if (t.action == SetlistTrigger::Action::Song) continue;
        CompiledSequence::Trigger as{t.source, SequenceTrigger::Action::Toggle, t.control, t.group, t.channel, t.number, t.high};
        bool fires = false;
        if (!matchOne(as, source, note, group, channel, number, high, ump::isNoteOn(m), fires)) continue;
        t.high = as.high;
        if (fires) {
            hit.action = t.action;
            fired      = true;
        }
        return true;
    }
    return false;
}

bool SequenceSet::wouldStart(MidiSourceHandle sequence, SequenceCommand command) const noexcept {
    for (const CompiledSequence& s : all) {
        if (s.source != sequence) continue;
        const SequenceState state = s.runtime->current;
        return command == SequenceCommand::Start ||
               (command == SequenceCommand::Toggle &&
                (state == SequenceState::Stopped || state == SequenceState::Stopping));
    }
    return false;
}

bool SequenceSet::startsOnNote(MidiSourceHandle source, const MidiMessage& m) const noexcept {
    return transport.startOnNote && !isSequenceSource(source) && ump::isNoteOn(m) &&
           (transport.startOnNoteSource == kInvalidMidiSource || transport.startOnNoteSource == source);
}

void SequenceSet::transportStarted(const Block& b) noexcept {
    loopersTransportStarted(b);
    for (CompiledSequence& s : all) {
        SequenceRuntime& rt = *s.runtime;
        if (!s.atStart || rt.current != SequenceState::Stopped) continue;
        rt.anchorBeat = b.beat;
        rt.current    = SequenceState::Starting;
        rt.stopBeat   = s.repeat ? std::numeric_limits<double>::infinity() : b.beat + s.length / s.unitsPerBeat;
        rt.state.store(rt.current, std::memory_order_relaxed);
    }
}

void SequenceSet::apply(CompiledSequence& s, SequenceCommand command, const Block& b) noexcept {
    SequenceRuntime& rt   = *s.runtime;
    const double     once = s.length / s.unitsPerBeat;   // one pass, in beats
    if (command == SequenceCommand::Toggle)
        command = rt.current == SequenceState::Stopped || rt.current == SequenceState::Stopping
                      ? SequenceCommand::Start
                      : SequenceCommand::Stop;
    switch (command) {
        case SequenceCommand::Start:
            if (rt.current == SequenceState::Stopping) {   // un-stop
                rt.current  = SequenceState::Playing;
                rt.stopBeat = s.repeat ? std::numeric_limits<double>::infinity() : rt.stopBeat;
            } else if (rt.current == SequenceState::Stopped) {
                rt.anchorBeat = boundary(s.launch, b);
                rt.current    = SequenceState::Starting;
                rt.stopBeat   = s.repeat ? std::numeric_limits<double>::infinity() : rt.anchorBeat + once;
            }
            break;
        case SequenceCommand::Stop:
            if (rt.current == SequenceState::Starting) {
                rt.current  = SequenceState::Stopped;
                rt.stopBeat = std::numeric_limits<double>::infinity();
            } else if (rt.current == SequenceState::Playing) {
                rt.current  = SequenceState::Stopping;
                rt.stopBeat = std::min(rt.stopBeat, boundary(s.launch, b));
            }
            break;
        case SequenceCommand::StopNow:
            if (rt.current != SequenceState::Stopped) {
                rt.current  = SequenceState::Stopping;
                rt.stopBeat = b.beat;
            }
            break;
        case SequenceCommand::Toggle: break;   // resolved above
    }
    rt.state.store(rt.current, std::memory_order_relaxed);
}

std::size_t SequenceSet::generate(const Block& b, SequenceEvent* out, std::size_t capacity) noexcept {
    std::size_t  count      = 0;
    const double end        = blockEnd(b);
    const double beatsPerMs = b.tempoBpm / 60000.0;

    for (CompiledSequence& s : all) {
        SequenceRuntime& rt = *s.runtime;

        if (rt.current == SequenceState::Starting && rt.anchorBeat < end) rt.current = SequenceState::Playing;
        if (rt.current == SequenceState::Playing || rt.current == SequenceState::Stopping) {
            // Each whole unit that starts in this block, before any stop.
            const double units0 = (b.beat - rt.anchorBeat) * s.unitsPerBeat;
            for (double unit = std::max(0.0, std::ceil(units0 - kEps));; unit += 1.0) {
                const double beat = rt.anchorBeat + unit / s.unitsPerBeat;
                if (beat >= end - kEps || beat >= rt.stopBeat - kEps) break;
                const auto inLoop = static_cast<std::uint32_t>(std::fmod(unit, static_cast<double>(s.length)));
                const std::uint32_t at = frameOf(b, beat);
                auto first = std::lower_bound(s.notes.begin(), s.notes.end(), inLoop,
                                              [](const Note& n, std::uint32_t start) { return n.start < start; });
                for (auto n = first; n != s.notes.end() && n->start == inLoop; ++n) {
                    // The same pitch still sounding (a delayed note-off): end it first.
                    for (SequenceRuntime::Sounding& sounding : rt.sounding)
                        if (sounding.used && sounding.pitch == n->pitch) {
                            emit(out, capacity, count, at, 0, s.source,
                                 noteOffMessage(s.protocol, s.channel, s.group, sounding.note));
                            sounding.used = false;
                        }
                    SequenceRuntime::Sounding* slot = nullptr;
                    for (SequenceRuntime::Sounding& sounding : rt.sounding)
                        if (!sounding.used) {
                            slot = &sounding;
                            break;
                        }
                    if (slot == nullptr) continue;   // 64 notes sounding at once: this one is skipped
                    const std::uint8_t note = chooseNote(s, *n);
                    if (!emit(out, capacity, count, at, 1, s.source, noteOnMessage(s, note, *n))) continue;
                    const double off = beat + n->length / s.unitsPerBeat + s.noteOffOffsetMs * beatsPerMs;
                    *slot = SequenceRuntime::Sounding{true, n->start, n->pitch, note, beat,
                                                      std::max(off, beat + beatsPerMs)};   // at least 1 ms
                }
            }
            if (rt.stopBeat < end - kEps) {   // stopped in this block
                for (SequenceRuntime::Sounding& sounding : rt.sounding)
                    if (sounding.used) sounding.offBeat = std::min(sounding.offBeat, std::max(rt.stopBeat, b.beat));
                rt.current  = SequenceState::Stopped;
                rt.stopBeat = std::numeric_limits<double>::infinity();
            }
        }

        // Note-offs due in this block.
        for (SequenceRuntime::Sounding& sounding : rt.sounding) {
            if (!sounding.used || sounding.offBeat >= end - kEps) continue;
            emit(out, capacity, count, frameOf(b, sounding.offBeat), 0, s.source,
                 noteOffMessage(s.protocol, s.channel, s.group, sounding.note));
            sounding.used = false;
        }

        rt.state.store(rt.current, std::memory_order_relaxed);
        const bool   playing = rt.current == SequenceState::Playing || rt.current == SequenceState::Stopping;
        const double units   = (end - rt.anchorBeat) * s.unitsPerBeat;
        rt.position.store(playing && units > 0.0 ? std::fmod(units, static_cast<double>(s.length)) : 0.0,
                          std::memory_order_relaxed);
    }
    return count + generateLoops(b, out + count, capacity - count);
}

} // namespace stagehand
