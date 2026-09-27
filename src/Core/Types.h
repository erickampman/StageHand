#pragma once

#include <cstdint>

namespace stagehand {

// A point in time from a monotonic clock, in nanoseconds. Used to timestamp
// MIDI at capture and to mark each audio block's start, so a MIDI message can
// be mapped to a sample-accurate offset within the block. See
// docs/domain-model.md, "Sample-accurate MIDI <-> audio alignment".
using Nanoseconds = std::int64_t;

// Severity for host-side logging (HostServices::log, added with the plugin
// seam).
enum class LogLevel { Debug, Info, Warning, Error };

} // namespace stagehand
