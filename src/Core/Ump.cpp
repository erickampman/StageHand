#include "Core/Ump.h"

namespace stagehand::ump {

// MIDI 2.0 min-center-max upscaling: values at or below the source center shift
// up; values above it fill the new low bits by repeating the source's low bits,
// so the maximum maps to the maximum. Matches the ALSA kernel's output (M2).
std::uint32_t scaleUp(std::uint32_t value, unsigned srcBits, unsigned dstBits) noexcept {
    if (srcBits == 0 || dstBits <= srcBits) return scaleDown(value, srcBits, dstBits);
    const unsigned      scaleBits = dstBits - srcBits;
    std::uint32_t       shifted   = value << scaleBits;
    const std::uint32_t srcCenter = 1u << (srcBits - 1);
    if (value <= srcCenter) return shifted;

    const unsigned repeatBits  = srcBits - 1;
    std::uint32_t  repeatValue = value & ((1u << repeatBits) - 1);
    if (scaleBits > repeatBits)
        repeatValue <<= scaleBits - repeatBits;
    else
        repeatValue >>= repeatBits - scaleBits;
    while (repeatValue != 0) {
        shifted |= repeatValue;
        repeatValue >>= repeatBits;
    }
    return shifted;
}

std::uint32_t scaleDown(std::uint32_t value, unsigned srcBits, unsigned dstBits) noexcept {
    if (dstBits >= srcBits) return value;
    return value >> (srcBits - dstBits);
}

std::uint16_t velocity16(const MidiMessage& m) noexcept {
    if (!isChannelVoice(m)) return 0;
    if (status(m) != Status::NoteOn && status(m) != Status::NoteOff) return 0;
    if (messageType(m) == MessageType::Midi2ChannelVoice)
        return static_cast<std::uint16_t>(m.words[1] >> 16);
    return static_cast<std::uint16_t>(scaleUp(m.words[0] & 0x7F, 7, 16));
}

bool transposeNote(MidiMessage& m, int semitones) noexcept {
    if (semitones == 0 || !isChannelVoice(m)) return true;

    const Status s     = status(m);
    const bool   midi2 = messageType(m) == MessageType::Midi2ChannelVoice;
    const bool   carriesNote =
        s == Status::NoteOn || s == Status::NoteOff || s == Status::PolyPressure ||
        (midi2 && (s == Status::RegisteredPerNoteController ||
                   s == Status::AssignablePerNoteController || s == Status::PerNotePitchBend ||
                   s == Status::PerNoteManagement));
    if (!carriesNote) return true;

    const int note = noteNumber(m) + semitones;
    if (note < 0 || note > 127) return false;

    std::uint32_t w0 = (m.words[0] & ~0x7F00u) | (static_cast<std::uint32_t>(note) << 8);
    std::uint32_t w1 = m.words[1];
    if (midi2 && (s == Status::NoteOn || s == Status::NoteOff) &&
        static_cast<NoteAttribute>(m.words[0] & 0xFF) == NoteAttribute::Pitch7_9) {
        const int pitch = static_cast<int>(w1 & 0xFFFF) + semitones * 512;   // 7.9: 512 per semitone
        if (pitch < 0 || pitch > 0xFFFF) return false;
        w1 = (w1 & 0xFFFF0000u) | static_cast<std::uint32_t>(pitch);
    }
    m.words[0] = w0;
    m.words[1] = w1;
    return true;
}

std::size_t toMidi1Bytes(const MidiMessage& m, std::uint8_t out[3]) noexcept {
    const std::uint32_t w0         = m.words[0];
    const auto          statusByte = static_cast<std::uint8_t>((w0 >> 16) & 0xFF);
    out[0] = statusByte;
    out[1] = static_cast<std::uint8_t>((w0 >> 8) & 0x7F);
    out[2] = static_cast<std::uint8_t>(w0 & 0x7F);

    switch (messageType(m)) {
        case MessageType::Midi1ChannelVoice:
            return (status(m) == Status::ProgramChange || status(m) == Status::ChannelPressure) ? 2 : 3;
        case MessageType::System:
            switch (statusByte) {
                case 0xF1:
                case 0xF3: return 2;
                case 0xF2: return 3;
                default:   return statusByte >= 0xF6 ? 1 : 0;
            }
        default:
            return 0;
    }
}

// upconvert(), downconvert() and parseMidi1Byte() arrive in M5 with their
// spec-derived tests.

} // namespace stagehand::ump
