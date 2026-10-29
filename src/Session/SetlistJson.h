#pragma once

// The parts of setlist.json shared by the set package (Session/SetFile.h) and
// the older `.setlist` file (Session/SetlistFile.h): the switch settings and
// the MIDI triggers. Internal to stagehand-session.

#include <cstddef>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "Session/BundleFiles.h"
#include "Session/Setlist.h"

namespace stagehand::bundle {

using Json = nlohmann::ordered_json;

inline constexpr std::pair<const char*, SwitchSettings::Transport> kTransports[] = {
    {"keep", SwitchSettings::Transport::Keep}, {"stop", SwitchSettings::Transport::Stop}};
inline constexpr std::pair<const char*, SwitchSettings::When> kWhens[] = {   // the first is the default
    {"nextBar", SwitchSettings::When::NextBar}, {"now", SwitchSettings::When::Now}};
inline constexpr std::pair<const char*, SwitchSettings::OldRack> kOldRacks[] = {
    {"ringOut", SwitchSettings::OldRack::RingOut}, {"cut", SwitchSettings::OldRack::Cut}};

template <typename Enum, std::size_t N>
Enum choice(const Json& j, const char* field, const std::pair<const char*, Enum> (&values)[N]) {
    if (!j.contains(field)) return values[0].second;   // the first is the default
    const std::string text = j.at(field).get<std::string>();
    for (const auto& [name, value] : values)
        if (text == name) return value;
    fail(std::string("unknown \"") + field + "\": \"" + text + "\"");
}

inline constexpr std::pair<const char*, SetlistTrigger::Action> kTriggerActions[] = {
    {"next", SetlistTrigger::Action::Next}, {"previous", SetlistTrigger::Action::Previous},
    {"song", SetlistTrigger::Action::Song}};

inline int numberIn(const Json& j, const char* field, int fallback, int low, int high) {
    const int value = j.value(field, fallback);
    if (value < low || value > high)
        fail(std::string(field) + " must be within " + std::to_string(low) + "-" + std::to_string(high));
    return value;
}

template <typename Enum, std::size_t N>
const char* nameOf(const std::pair<const char*, Enum> (&values)[N], Enum value) {
    for (const auto& [name, v] : values)
        if (v == value) return name;
    return values[0].first;
}

// {"action": "next", "source": "device:<id>", "controller": 80, "channel": 1,
// "group": 1}, or "note" instead of "controller"; "song" (a Program Change)
// has neither.
inline SetlistTrigger parseTrigger(const Json& j) {
    if (!j.is_object()) fail("each trigger is an object");
    SetlistTrigger t;
    t.action = choice(j, "action", kTriggerActions);
    t.source = parseSource(j.value("source", std::string("all")));
    if (t.source.kind == MidiSourceRef::Kind::Sequence || t.source.kind == MidiSourceRef::Kind::None)
        fail("a trigger's source must be an input");
    if (t.action != SetlistTrigger::Action::Song) {
        if (j.contains("note") == j.contains("controller")) fail("a Next or Previous trigger has a \"note\" or a \"controller\"");
        t.control = j.contains("note") ? SequenceTrigger::Control::Note : SequenceTrigger::Control::Controller;
        t.number  = static_cast<std::uint8_t>(numberIn(j, j.contains("note") ? "note" : "controller", 0, 0, 127));
    }
    t.channel = static_cast<std::uint8_t>(numberIn(j, "channel", 1, 1, 16) - 1);
    t.group   = static_cast<std::uint8_t>(numberIn(j, "group", 1, 1, 16) - 1);
    return t;
}

inline Json triggerJson(const SetlistTrigger& t) {
    Json j;
    j["action"] = nameOf(kTriggerActions, t.action);
    j["source"] = sourceText(t.source);
    if (t.action != SetlistTrigger::Action::Song)
        j[t.control == SequenceTrigger::Control::Note ? "note" : "controller"] = t.number;
    j["channel"] = t.channel + 1;
    j["group"]   = t.group + 1;
    return j;
}

// {"transport": "keep" | "stop", "at": "nextBar" | "now", "oldRack": "ringOut" | "cut"}
inline SwitchSettings parseSwitching(const Json& sw) {
    if (!sw.is_object()) fail("\"switching\" must be an object");
    SwitchSettings s;
    s.transport = choice(sw, "transport", kTransports);
    s.when      = choice(sw, "at", kWhens);
    s.oldRack   = choice(sw, "oldRack", kOldRacks);
    return s;
}

inline Json switchingJson(const SwitchSettings& s) {
    Json sw;
    sw["transport"] = nameOf(kTransports, s.transport);
    sw["at"]        = nameOf(kWhens, s.when);
    sw["oldRack"]   = nameOf(kOldRacks, s.oldRack);
    return sw;
}

} // namespace stagehand::bundle
