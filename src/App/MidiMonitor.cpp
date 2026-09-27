#include "App/MidiMonitor.h"

#include <cstdarg>
#include <cstdio>

#include "Core/Ump.h"

namespace stagehand {

namespace {

std::string format(const char* fmt, ...) {
    char buffer[192];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof buffer, fmt, args);
    va_end(args);
    return buffer;
}

std::string describeMidi1(const MidiMessage& m) {
    using ump::Status;
    const std::uint32_t w0 = m.words[0];
    const unsigned d1 = (w0 >> 8) & 0x7F;
    const unsigned d2 = w0 & 0x7F;
    const std::string prefix =
        format("MIDI1 g%u ch%-2u ", ump::group(m), ump::channel(m) + 1u);
    switch (ump::status(m)) {
        case Status::NoteOff:
            return prefix + format("NoteOff   note %u vel %u", d1, d2);
        case Status::NoteOn:
            return prefix + format("NoteOn    note %u vel %u%s", d1, d2,
                                   d2 == 0 ? "  (vel 0 = note-off)" : "");
        case Status::PolyPressure:
            return prefix + format("PolyPres  note %u value %u", d1, d2);
        case Status::ControlChange:
            return prefix + format("CC %-3u    value %u", d1, d2);
        case Status::ProgramChange:
            return prefix + format("Program   %u", d1);
        case Status::ChannelPressure:
            return prefix + format("ChanPres  %u", d1);
        case Status::PitchBend:
            return prefix + format("Bend      %d", static_cast<int>((d2 << 7) | d1) - 8192);
        default:
            return prefix + "?";
    }
}

std::string describeMidi2(const MidiMessage& m) {
    using ump::Status;
    const std::uint32_t w0 = m.words[0];
    const std::uint32_t w1 = m.words[1];
    const unsigned note = (w0 >> 8) & 0x7F;
    const std::string prefix =
        format("MIDI2 g%u ch%-2u ", ump::group(m), ump::channel(m) + 1u);

    switch (ump::status(m)) {
        case Status::NoteOff:
        case Status::NoteOn: {
            std::string text = prefix +
                format("%-9s note %u vel 0x%04X",
                       ump::status(m) == Status::NoteOn ? "NoteOn" : "NoteOff", note, w1 >> 16);
            const auto attribute = static_cast<ump::NoteAttribute>(w0 & 0xFF);
            const unsigned data  = w1 & 0xFFFF;
            if (attribute == ump::NoteAttribute::Pitch7_9)
                text += format(" pitch %.3f", (data >> 9) + (data & 0x1FF) / 512.0);
            else if (attribute != ump::NoteAttribute::None)
                text += format(" attr %u=0x%04X", static_cast<unsigned>(attribute), data);
            return text;
        }
        case Status::PolyPressure:
            return prefix + format("PolyPres  note %u value 0x%08X", note, w1);
        case Status::ControlChange:
            return prefix + format("CC %-3u    value 0x%08X", note, w1);
        case Status::ProgramChange:
            if (w0 & 1)
                return prefix + format("Program   %u bank %u:%u", (w1 >> 24) & 0x7F,
                                       (w1 >> 8) & 0x7F, w1 & 0x7F);
            return prefix + format("Program   %u", (w1 >> 24) & 0x7F);
        case Status::ChannelPressure:
            return prefix + format("ChanPres  0x%08X", w1);
        case Status::PitchBend:
            return prefix + format("Bend      0x%08X", w1);
        case Status::RegisteredController:
            return prefix + format("RPN %u:%u   value 0x%08X", note, w0 & 0x7F, w1);
        case Status::AssignableController:
            return prefix + format("NRPN %u:%u  value 0x%08X", note, w0 & 0x7F, w1);
        case Status::RelativeRegisteredController:
            return prefix + format("RelRPN %u:%u %+d", note, w0 & 0x7F, static_cast<std::int32_t>(w1));
        case Status::RelativeAssignableController:
            return prefix + format("RelNRPN %u:%u %+d", note, w0 & 0x7F, static_cast<std::int32_t>(w1));
        case Status::RegisteredPerNoteController:
            return prefix + format("PN-RegCC  note %u #%u 0x%08X", note, w0 & 0xFF, w1);
        case Status::AssignablePerNoteController:
            return prefix + format("PN-AsgCC  note %u #%u 0x%08X", note, w0 & 0xFF, w1);
        case Status::PerNotePitchBend:
            return prefix + format("PN-Bend   note %u 0x%08X", note, w1);
        case Status::PerNoteManagement:
            return prefix + format("PN-Mgmt   note %u flags %u", note, w0 & 0x03);
        default:
            return prefix + "?";
    }
}

std::string describeSystem(const MidiMessage& m) {
    const std::uint32_t w0     = m.words[0];
    const unsigned      status = (w0 >> 16) & 0xFF;
    switch (status) {
        case 0xF1: return "System MTC quarter frame";
        case 0xF2: return format("System SongPos %u", ((w0 & 0x7F) << 7) | ((w0 >> 8) & 0x7F));
        case 0xF3: return format("System SongSelect %u", (w0 >> 8) & 0x7F);
        case 0xF6: return "System TuneRequest";
        case 0xF8: return "System Clock";
        case 0xFA: return "System Start";
        case 0xFB: return "System Continue";
        case 0xFC: return "System Stop";
        case 0xFE: return "System ActiveSensing";
        case 0xFF: return "System Reset";
        default:   return format("System 0x%02X", status);
    }
}

} // namespace

std::string describeUmp(const MidiMessage& m) {
    const std::uint32_t w0 = m.words[0];
    switch (ump::messageType(m)) {
        case ump::MessageType::Midi1ChannelVoice: return describeMidi1(m);
        case ump::MessageType::Midi2ChannelVoice: return describeMidi2(m);
        case ump::MessageType::System:            return describeSystem(m);
        case ump::MessageType::Data64: {
            static const char* const kinds[] = {"complete", "start", "continue", "end"};
            const unsigned kind = (w0 >> 20) & 0x0F;
            return format("SysEx7 g%u %s, %u bytes", ump::group(m),
                          kind < 4 ? kinds[kind] : "?", (w0 >> 16) & 0x0F);
        }
        case ump::MessageType::Utility:
            return format("Utility status %u", (w0 >> 20) & 0x0F);
        case ump::MessageType::Stream:
            return format("Stream status 0x%03X", (w0 >> 16) & 0x3FF);
        default:
            return format("MT 0x%X", static_cast<unsigned>(w0 >> 28));
    }
}

void MidiMonitor::onMidi(MidiSourceHandle source, const TimedMidiMessage& msg) noexcept {
    if (!queue.push(Entry{source, msg})) dropped.fetch_add(1, std::memory_order_relaxed);
}

void MidiMonitor::drain(const std::map<MidiSourceHandle, std::string>& names) {
    Entry entry;
    while (queue.pop(entry)) {
        const Nanoseconds t = entry.message.captureTime;
        if (firstTime == 0) firstTime = lastTime = t;

        std::string words;
        for (std::uint8_t i = 0; i < entry.message.message.wordCount; ++i)
            words += format("%08X ", entry.message.message.words[i]);

        const auto name = names.find(entry.source);
        std::printf("%10.6f  +%9.3f ms  %-22s  %-36s %s\n", (t - firstTime) / 1e9,
                    (t - lastTime) / 1e6,
                    name != names.end() ? name->second.c_str() : "?", words.c_str(),
                    describeUmp(entry.message.message).c_str());
        lastTime = t;
    }
    if (const std::uint64_t d = dropped.load(std::memory_order_relaxed); d != reportedDrops) {
        std::printf("(monitor queue full: %llu messages dropped)\n",
                    static_cast<unsigned long long>(d));
        reportedDrops = d;
    }
    std::fflush(stdout);
}

} // namespace stagehand
