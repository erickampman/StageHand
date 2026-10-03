#include "Engine/ControlLink.h"

#include <algorithm>
#include <cmath>

#include "Core/Sequence.h"
#include "Core/Ump.h"

namespace stagehand {

double CompiledControlLink::Target::map(double travel) const noexcept {
    travel = std::clamp(travel, 0.0, 1.0);
    if (curve == ControlCurve::SlowStart) travel *= travel;
    else if (curve == ControlCurve::FastStart) travel = std::sqrt(travel);
    const double value = bottom + (top - bottom) * travel;
    return stepped ? std::round(value) : value;
}

bool CompiledControlLink::Target::pickUpAt(double mapped, double now) noexcept {
    const double tolerance = std::max(0.02 * std::fabs(top - bottom), 1e-9);
    const bool   crossed   = hasLast && (last - now) * (mapped - now) <= 0.0;
    picked                 = std::fabs(mapped - now) <= tolerance || crossed;
    hasLast                = !picked;
    last                   = mapped;
    return picked;
}

void CompiledControlLink::Target::moved(double value) noexcept {
    current = value;
    picked  = false;
    hasLast = false;
}

bool CompiledControlLink::Target::sameTarget(const Target& other) const noexcept {
    if (kind != other.kind) return false;
    switch (kind) {
        case Kind::Parameter: return plugin == other.plugin && param == other.param;
        case Kind::Gain:
        case Kind::Return:    return runtime == other.runtime;
        case Kind::Send:      return runtime == other.runtime && param == other.param;
        case Kind::Master:    return true;
    }
    return false;
}

bool CompiledControlLink::matches(MidiSourceHandle from, const MidiMessage& m, double& travel) const noexcept {
    if (!ump::isChannelVoice(m) || isSequenceSource(from) || ump::status(m) != ump::Status::ControlChange) return false;
    if ((source != kInvalidMidiSource && source != from) || ump::group(m) != group || ump::channel(m) != channel ||
        ((m.words[0] >> 8) & 0x7F) != number)
        return false;
    travel = ump::messageType(m) == ump::MessageType::Midi2ChannelVoice ? m.words[1] / 4294967295.0
                                                                        : (m.words[0] & 0x7F) / 127.0;
    return true;
}

bool CompiledControlLink::sameSource(const CompiledControlLink& other) const noexcept {
    return source == other.source && group == other.group && channel == other.channel && number == other.number;
}

float linkLevelGain(double db) noexcept {
    return db <= -60.0 ? 0.0f : static_cast<float>(std::pow(10.0, db / 20.0));
}

} // namespace stagehand
