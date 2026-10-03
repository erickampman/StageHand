#pragma once

// Control links in rack.json and rig.json ("controls", docs/rack-format.md
// and docs/rig.md). Internal to stagehand-session.

#include <algorithm>
#include <string>

#include <nlohmann/json.hpp>

#include "Session/BundleFiles.h"
#include "Session/ControlLinks.h"

namespace stagehand::bundle {

// {"source": ..., "controller": 74, "channel": 1, "group": 1, "mode": "pick-up" | "jump",
//  "passOn": true, "targets": [{"target": "parameter" | "gain" | "send" | "return" | "master",
//  "row": slot id, "plugin": plugin id, "bus": "A" | "B", "effect": 1 | 2, "param": id,
//  "name": the parameter's, "bottom": value, "top": value, "curve": "linear" | "slow start" | "fast start"}]}
// A song's targets name rows; the rig's name plugins (every row with it).
inline ControlLinkSetup parseControlLink(const nlohmann::ordered_json& j, ControlScope scope) {
    using Kind = ControlLinkTarget::Kind;
    if (!j.is_object()) fail("each control link is an object");
    ControlLinkSetup link;
    link.source = parseSource(j.value("source", std::string("all")));
    if (link.source.kind == MidiSourceRef::Kind::Sequence || link.source.kind == MidiSourceRef::Kind::None)
        fail("a control link's source must be an input, or \"all\"");
    link.number  = static_cast<std::uint8_t>(std::clamp(j.at("controller").get<int>(), 0, 127));
    link.channel = static_cast<std::uint8_t>(std::clamp(j.value("channel", 1), 1, 16) - 1);
    link.group   = static_cast<std::uint8_t>(std::clamp(j.value("group", 1), 1, 16) - 1);
    const std::string mode = j.value("mode", std::string("pick-up"));
    if (mode != "pick-up" && mode != "jump") fail("a control link's mode is \"pick-up\" or \"jump\"");
    link.pickUp = mode == "pick-up";
    link.passOn = j.value("passOn", false);
    if (!j.contains("targets") || !j.at("targets").is_array() || j.at("targets").empty())
        fail("a control link has a list of \"targets\"");
    for (const auto& tj : j.at("targets")) {
        if (!tj.is_object()) fail("each target is an object");
        ControlLinkTarget t;
        const std::string kind = tj.value("target", std::string("parameter"));
        if (kind == "parameter") t.kind = Kind::Parameter;
        else if (kind == "gain") t.kind = Kind::Gain;
        else if (kind == "send") t.kind = Kind::Send;
        else if (kind == "return") t.kind = Kind::Return;
        else if (kind == "master") t.kind = Kind::Master;
        else fail("a target is \"parameter\", \"gain\", \"send\", \"return\" or \"master\"");
        t.row    = tj.value("row", std::string{});
        t.plugin = tj.value("plugin", std::string{});
        if (tj.contains("bus")) {
            const std::string bus = tj.at("bus").get<std::string>();
            if (bus != "A" && bus != "B") fail("a target's bus is \"A\" or \"B\"");
            t.bus = bus == "A" ? 0 : 1;
        }
        if (tj.contains("effect")) {
            const int effect = tj.at("effect").get<int>();
            if (effect != 1 && effect != 2) fail("a target's effect is 1 or 2");
            t.effect = effect - 1;
        }
        t.param     = tj.value("param", 0u);
        t.paramName = tj.value("name", std::string{});
        const bool level = t.kind != Kind::Parameter;
        t.bottom    = tj.value("bottom", level ? -60.0 : 0.0);
        t.top       = tj.value("top", level ? 0.0 : 1.0);
        const std::string curve = tj.value("curve", std::string("linear"));
        if (curve == "linear") t.curve = ControlCurve::Linear;
        else if (curve == "slow start") t.curve = ControlCurve::SlowStart;
        else if (curve == "fast start") t.curve = ControlCurve::FastStart;
        else fail("a target's curve is \"linear\", \"slow start\" or \"fast start\"");

        const bool song = scope == ControlScope::Song;
        switch (t.kind) {
            case Kind::Parameter:
                if (t.bus >= 0 ? t.effect < 0 || !t.row.empty() || !t.plugin.empty()
                               : (song ? t.row.empty() || !t.plugin.empty() : t.plugin.empty() || !t.row.empty()))
                    fail(song ? "a parameter target names a \"row\", or a \"bus\" and \"effect\""
                              : "a parameter target names a \"plugin\", or a \"bus\" and \"effect\"");
                break;
            case Kind::Gain:
            case Kind::Send:
                if (!song) fail("a row's gain or send can't be a target for all songs");
                if (t.row.empty() || (t.kind == Kind::Send) != (t.bus >= 0)) fail("a gain target names a \"row\"; a send, a \"row\" and a \"bus\"");
                break;
            case Kind::Return:
                if (t.bus < 0) fail("a return target names a \"bus\"");
                break;
            case Kind::Master:
                break;
        }
        link.targets.push_back(std::move(t));
    }
    return link;
}

inline nlohmann::ordered_json controlLinkJson(const ControlLinkSetup& link) {
    using Kind = ControlLinkTarget::Kind;
    nlohmann::ordered_json j;
    j["source"]     = sourceText(link.source);
    j["controller"] = link.number;
    j["channel"]    = link.channel + 1;
    j["group"]      = link.group + 1;
    j["mode"]       = link.pickUp ? "pick-up" : "jump";
    if (link.passOn) j["passOn"] = true;
    nlohmann::ordered_json targets = nlohmann::ordered_json::array();
    for (const ControlLinkTarget& t : link.targets) {
        nlohmann::ordered_json tj;
        tj["target"] = t.kind == Kind::Parameter ? "parameter"
                       : t.kind == Kind::Gain    ? "gain"
                       : t.kind == Kind::Send    ? "send"
                       : t.kind == Kind::Return  ? "return"
                                                 : "master";
        if (!t.row.empty()) tj["row"] = t.row;
        if (!t.plugin.empty()) tj["plugin"] = t.plugin;
        if (t.bus >= 0) tj["bus"] = t.bus == 0 ? "A" : "B";
        if (t.effect >= 0) tj["effect"] = t.effect + 1;
        if (t.kind == Kind::Parameter) {
            tj["param"] = t.param;
            if (!t.paramName.empty()) tj["name"] = t.paramName;
        }
        tj["bottom"] = t.bottom;
        tj["top"]    = t.top;
        tj["curve"]  = t.curve == ControlCurve::SlowStart ? "slow start"
                       : t.curve == ControlCurve::FastStart ? "fast start"
                                                            : "linear";
        targets.push_back(tj);
    }
    j["targets"] = targets;
    return j;
}

} // namespace stagehand::bundle
