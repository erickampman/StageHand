#pragma once

// The MIDI input this platform's app uses. Both have the same create(),
// describe(), threadDescription() and overrunCount() for main.cpp, beyond the
// MidiInput seam.
#ifdef __APPLE__
#include "Midi/CoreMidiInput.h"
namespace stagehand {
using PlatformMidiInput = CoreMidiInput;
}
#else
#include "Midi/AlsaSeqInput.h"
namespace stagehand {
using PlatformMidiInput = AlsaSeqInput;
}
#endif
