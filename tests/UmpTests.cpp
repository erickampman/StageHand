// UMP helper tests. The translators and byte parser join these when
// Core/Ump.cpp lands (M5).

#include <cstdint>

#include "Check.h"
#include "Core/Ump.h"

using namespace stagehand;

namespace {

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

void testScaling() {
    // Expected values are what the ALSA kernel produced in M2.
    CHECK(ump::scaleUp(100, 7, 16) == 0xC924);
    CHECK(ump::scaleUp(100, 7, 32) == 0xC9249249);
    CHECK(ump::scaleUp(12288, 14, 32) == 0xC0020010);
    CHECK(ump::scaleUp(0, 7, 16) == 0);
    CHECK(ump::scaleUp(64, 7, 16) == 0x8000);     // center maps to center
    CHECK(ump::scaleUp(127, 7, 16) == 0xFFFF);    // max maps to max
    for (std::uint32_t v = 0; v < 128; ++v)       // round trip for 7-bit values
        CHECK(ump::scaleDown(ump::scaleUp(v, 7, 16), 16, 7) == v);
    CHECK(ump::velocity16(packet(0x20903C64)) == 0xC924);
    CHECK(ump::velocity16(packet(0x40903C00, 0x1234'0000)) == 0x1234);
}

void testToMidi1Bytes() {
    std::uint8_t bytes[3] = {};
    CHECK(ump::toMidi1Bytes(packet(0x20913C64), bytes) == 3);
    CHECK(bytes[0] == 0x91 && bytes[1] == 0x3C && bytes[2] == 0x64);
    CHECK(ump::toMidi1Bytes(packet(0x20C00500), bytes) == 2);          // program change
    CHECK(bytes[0] == 0xC0 && bytes[1] == 0x05);
    CHECK(ump::toMidi1Bytes(packet(0x10F80000), bytes) == 1);          // clock
    CHECK(bytes[0] == 0xF8);
    CHECK(ump::toMidi1Bytes(packet(0x10F20304), bytes) == 3);          // song position
    CHECK(bytes[0] == 0xF2 && bytes[1] == 0x03 && bytes[2] == 0x04);
    CHECK(ump::toMidi1Bytes(packet(0x40903C00, 0x8000'0000), bytes) == 0);   // MIDI 2.0: no
}

void testTranspose() {
    MidiMessage m1 = packet(0x20903C64);
    CHECK(ump::transposeNote(m1, 2) && ump::noteNumber(m1) == 62);
    CHECK((m1.words[0] & 0x7F) == 0x64);                               // velocity kept

    MidiMessage m2 = packet(0x40903D03, 0x8000'7B00);                   // 61 + pitch 61.5
    CHECK(ump::transposeNote(m2, -1) && ump::noteNumber(m2) == 60);
    CHECK((m2.words[1] & 0xFFFF) == ((60u << 9) | 256u));              // pitch moved too: 60.5
    CHECK((m2.words[1] >> 16) == 0x8000);                              // velocity kept

    MidiMessage top = packet(0x20907F64);
    CHECK(!ump::transposeNote(top, 1));                                // out of range...
    CHECK(ump::noteNumber(top) == 127);                                // ...and unchanged
    MidiMessage cc = packet(0x20B00764);
    CHECK(ump::transposeNote(cc, 5) && cc.words[0] == 0x20B00764);     // no note: untouched
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
    testScaling();
    testToMidi1Bytes();
    testTranspose();
    return test::finish("UMP");
}
