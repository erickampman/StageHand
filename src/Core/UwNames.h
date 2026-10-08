#pragma once

// uw-core (external/uw-core) holds the code StageHand shares with Umpire: UMP,
// the clock and queues, the plugin seam and CLAP host, the audio and MIDI
// backends. They were StageHand's own until 2026-10, in namespace stagehand;
// now they're in namespace uw. StageHand's code still names them unqualified
// (MidiMessage, ump::status, HostedPlugin, ...), through this directive.
namespace uw {}

namespace stagehand {
using namespace uw;
} // namespace stagehand
