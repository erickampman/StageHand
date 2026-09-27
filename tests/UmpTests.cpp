// UMP helper tests. A minimal self-contained runner (no third-party framework,
// per the minimize-dependencies principle): exits non-zero if any check fails.
// The translators and byte parser join these tests when Core/Ump.cpp lands (M5).

#include <cstdint>
#include <cstdio>

#include "Core/Ump.h"

using namespace stagehand;

namespace {

int failures = 0;

#define CHECK(expr)                                                              \
    do {                                                                         \
        if (!(expr)) {                                                           \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                         #expr);                                                 \
            ++failures;                                                          \
        }                                                                        \
    } while (0)

MidiMessage packet(std::uint32_t w0, std::uint32_t w1 = 0) {
    MidiMessage m;
    m.words[0]  = w0;
    m.words[1]  = w1;
    m.wordCount = ump::wordCountForType(static_cast<std::uint8_t>(w0 >> 28));
    return m;
}

void testFieldAccess() {
    // MIDI 1.0 CV in UMP: group 3, note-on, channel 5, note 60, velocity 100.
    const MidiMessage m1 = packet(0x23953C64);
    CHECK(ump::messageType(m1) == ump::MessageType::Midi1ChannelVoice);
    CHECK(ump::group(m1) == 3);
    CHECK(ump::status(m1) == ump::Status::NoteOn);
    CHECK(ump::channel(m1) == 5);
    CHECK(ump::noteNumber(m1) == 60);
    CHECK(m1.wordCount == 1);

    // MIDI 2.0 CV: group 0, note-on, channel 0, note 61, pitch 7.9 attribute.
    const MidiMessage m2 = packet(0x40903D03, 0x8000'7A00);
    CHECK(ump::messageType(m2) == ump::MessageType::Midi2ChannelVoice);
    CHECK(ump::noteNumber(m2) == 61);
    CHECK(static_cast<ump::NoteAttribute>(m2.words[0] & 0xFF) == ump::NoteAttribute::Pitch7_9);
    CHECK(m2.wordCount == 2);
}

void testNoteOnOffSemantics() {
    CHECK(ump::isNoteOn(packet(0x20903C64)));                      // 1.0 note-on
    CHECK(!ump::isNoteOn(packet(0x20903C00)));                     // 1.0 vel 0 ...
    CHECK(ump::isNoteOff(packet(0x20903C00)));                     // ... is a note-off
    CHECK(ump::isNoteOff(packet(0x20803C40)));                     // 1.0 note-off
    CHECK(ump::isNoteOn(packet(0x40903C00, 0x0000'0000)));         // 2.0 vel 0 is still ON
    CHECK(!ump::isNoteOff(packet(0x40903C00, 0x0000'0000)));
    CHECK(ump::isNoteOff(packet(0x40803C00, 0x8000'0000)));        // 2.0 note-off
    CHECK(!ump::isNoteOn(packet(0x10F80000)));                     // clock: not a note
}

void testProtocolClassification() {
    CHECK(ump::protocolOf(packet(0x20903C64)) == MidiProtocol::Midi1);
    CHECK(ump::protocolOf(packet(0x40903C00)) == MidiProtocol::Midi2);
    CHECK(ump::isProtocolNeutral(packet(0x10F80000)));             // timing clock
    CHECK(ump::isProtocolNeutral(packet(0x30160000)));             // SysEx7 complete
    CHECK(!ump::isProtocolNeutral(packet(0x20903C64)));
    CHECK(!ump::isChannelVoice(packet(0x10F80000)));
}

void testWordCounts() {
    // UMP spec packet sizes by message type 0x0..0xF.
    const std::uint8_t expected[16] = {1, 1, 1, 2, 2, 4, 1, 1, 2, 2, 2, 3, 3, 4, 4, 4};
    for (std::uint8_t mt = 0; mt < 16; ++mt)
        CHECK(ump::wordCountForType(mt) == expected[mt]);
}

} // namespace

int main() {
    testFieldAccess();
    testNoteOnOffSemantics();
    testProtocolClassification();
    testWordCounts();

    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("UMP tests passed");
    return 0;
}
