#include "Session/RackFile.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
#include <utility>

#include "Core/Ump.h"
#include "Session/BundleFiles.h"

namespace stagehand {

namespace fs = std::filesystem;
using Json   = nlohmann::ordered_json;   // keeps fields in the order written

using namespace bundle;

namespace {

constexpr int         kFormatVersion = 2;   // 2: sequences' "targets", "none" source
constexpr const char* kFormatName    = "stagehand-rack";

bool validSlotId(const std::string& id) {
    if (id.empty()) return false;
    for (const char c : id)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')) return false;
    return true;
}

// --- sequences (docs/sequences.md, section 2) ------------------------------------

int intIn(const Json& s, const char* field, int fallback, int low, int high) {
    const int value = s.value(field, fallback);
    if (value < low || value > high)
        fail(std::string(field) + " must be within " + std::to_string(low) + "-" + std::to_string(high));
    return value;
}

// "4", "1/2", "3/2": units per beat.
SequenceRate parseRate(const Json& value) {
    const std::string text = value.is_number() ? std::to_string(value.get<int>()) : value.get<std::string>();
    const auto        slash = text.find('/');
    char*             end   = nullptr;
    const long        num   = std::strtol(text.c_str(), &end, 10);
    long              den   = 1;
    if (slash != std::string::npos) den = std::strtol(text.c_str() + slash + 1, &end, 10);
    if (end == nullptr || *end != '\0' || num < 1 || num > 64 || den < 1 || den > 64)
        fail("rate '" + text + "' should be units per beat, like \"4\" or \"1/2\" (1-64 each side)");
    return SequenceRate{static_cast<std::uint16_t>(num), static_cast<std::uint16_t>(den)};
}

std::string rateText(const SequenceRate& rate) {
    return rate.denominator == 1 ? std::to_string(rate.numerator)
                                 : std::to_string(rate.numerator) + "/" + std::to_string(rate.denominator);
}

const char* launchText(LaunchQuantize launch) {
    return launch == LaunchQuantize::Beat ? "beat" : launch == LaunchQuantize::Immediate ? "immediate" : "bar";
}

// {"action": "toggle", "source": "device:<id>", "note": 36, "channel": 10, "group": 1},
// or "controller": 64 instead of "note".
SequenceTrigger parseTrigger(const Json& j) {
    if (!j.is_object()) fail("each trigger is an object");
    SequenceTrigger t;
    const std::string action = j.value("action", std::string("toggle"));
    if (action == "toggle") t.action = SequenceTrigger::Action::Toggle;
    else if (action == "start") t.action = SequenceTrigger::Action::Start;
    else if (action == "stop") t.action = SequenceTrigger::Action::Stop;
    else fail("trigger action must be \"toggle\", \"start\" or \"stop\"");
    t.source = parseSource(j.value("source", std::string("all")));
    if (t.source.kind == MidiSourceRef::Kind::Sequence) fail("a trigger's source must be an input, not a sequence");
    if (j.contains("note") == j.contains("controller")) fail("a trigger has a \"note\" or a \"controller\"");
    t.control = j.contains("note") ? SequenceTrigger::Control::Note : SequenceTrigger::Control::Controller;
    t.number  = static_cast<std::uint8_t>(intIn(j, j.contains("note") ? "note" : "controller", 0, 0, 127));
    t.channel = static_cast<std::uint8_t>(intIn(j, "channel", 1, 1, 16) - 1);
    t.group   = static_cast<std::uint8_t>(intIn(j, "group", 1, 1, 16) - 1);
    return t;
}

Json triggerJson(const SequenceTrigger& t) {
    Json j;
    j["action"] = t.action == SequenceTrigger::Action::Start  ? "start"
                  : t.action == SequenceTrigger::Action::Stop ? "stop"
                                                              : "toggle";
    j["source"] = sourceText(t.source);
    j[t.control == SequenceTrigger::Control::Note ? "note" : "controller"] = t.number;
    j["channel"] = t.channel + 1;
    j["group"]   = t.group + 1;
    return j;
}

// --- loopers (docs/looper.md, "In the rack file") ------------------------------------

// "1/4", "2", ... beats; "off".
double parseQuantize(const Json& j) {
    const std::string text = j.is_number() ? std::to_string(j.get<int>()) : j.get<std::string>();
    if (text == "off") return 0.0;
    char*        end = nullptr;
    const double num = std::strtod(text.c_str(), &end);
    double       den = 1.0;
    if (end != nullptr && *end == '/') den = std::strtod(end + 1, &end);
    if (end == nullptr || *end != '\0' || num <= 0.0 || den <= 0.0 || num / den > 16.0)
        fail("quantize '" + text + "' should be \"off\" or beats, like \"1/4\" or \"2\"");
    return num / den;
}

std::string quantizeText(double beats) {
    if (beats <= 0.0) return "off";
    if (beats >= 1.0 && beats == std::floor(beats)) return std::to_string(static_cast<int>(beats));
    return "1/" + std::to_string(static_cast<int>(std::lround(1.0 / beats)));
}

LooperSetup parseLooper(const Json& j) {
    if (!j.is_object()) fail("each looper is an object");
    LooperSetup l;
    l.id = j.at("id").get<std::string>();
    if (!validSlotId(l.id)) fail("looper id '" + l.id + "' must be letters, digits, '-' or '_'");
    l.name   = j.value("name", l.id);
    l.source = parseSource(j.value("source", std::string("all")));
    if (l.source.kind == MidiSourceRef::Kind::Sequence || l.source.kind == MidiSourceRef::Kind::None)
        fail("a looper records an input");
    if (j.contains("channels")) {
        l.channelMask = 0;
        for (const Json& c : j.at("channels")) {
            const int channel = c.get<int>();
            if (channel < 1 || channel > 16) fail("channels must be 1-16");
            l.channelMask |= static_cast<std::uint16_t>(1u << (channel - 1));
        }
    }
    if (j.contains("targets"))
        for (const Json& t : j.at("targets")) l.targets.push_back(t.get<std::string>());
    if (j.contains("quantize")) l.quantize = parseQuantize(j.at("quantize"));
    const std::string start = j.value("start", std::string("bar"));
    if (start != "bar" && start != "beat") fail("start must be \"bar\" or \"beat\"");
    l.atBar = start == "bar";
    if (j.contains("length")) {
        const Json& length = j.at("length");
        if (!length.is_object() || length.contains("bars") == length.contains("beats"))
            fail("length is {\"bars\": N} or {\"beats\": N}");
        l.fixedInBars = length.contains("bars");
        l.fixedLength = static_cast<std::uint32_t>(intIn(length, l.fixedInBars ? "bars" : "beats", 1, 1, 1024));
    }
    if (j.contains("cleanup")) {
        if (!j.at("cleanup").is_boolean()) fail("cleanup is true or false");
        l.cleanup = j.at("cleanup").get<bool>();
    }
    return l;
}

Json looperJson(const LooperSetup& l) {
    Json j;
    j["id"]     = l.id;
    j["name"]   = l.name;
    j["source"] = sourceText(l.source);
    if (l.channelMask != 0xFFFF) {
        Json channels = Json::array();
        for (int c = 0; c < 16; ++c)
            if ((l.channelMask >> c) & 1u) channels.push_back(c + 1);
        j["channels"] = channels;
    }
    j["targets"]  = l.targets;
    j["quantize"] = quantizeText(l.quantize);
    j["start"]    = l.atBar ? "bar" : "beat";
    if (l.fixedLength > 0) j["length"] = Json{{l.fixedInBars ? "bars" : "beats", l.fixedLength}};
    if (!l.cleanup) j["cleanup"] = false;
    return j;
}

Sequence parseSequence(const Json& s) {
    Sequence seq;
    seq.id = s.at("id").get<std::string>();
    if (!validSlotId(seq.id)) fail("sequence id '" + seq.id + "' may use only letters, digits, '-' and '_'");
    seq.name     = s.value("name", seq.id);
    const int protocol = s.value("protocol", 2);
    if (protocol != 1 && protocol != 2) fail("protocol must be 1 or 2");
    seq.protocol      = protocol == 1 ? MidiProtocol::Midi1 : MidiProtocol::Midi2;
    seq.edo           = intIn(s, "edo", 12, 1, 128);
    seq.referenceNote = intIn(s, "referenceNote", 60, 0, 127);
    seq.channel       = static_cast<std::uint8_t>(intIn(s, "channel", 1, 1, 16) - 1);
    seq.group         = static_cast<std::uint8_t>(intIn(s, "group", 1, 1, 16) - 1);
    if (s.contains("rate")) seq.rate = parseRate(s.at("rate"));
    seq.length = static_cast<std::uint32_t>(intIn(s, "length", 16, 1, 4096));
    const std::string launch = s.value("launch", std::string("bar"));
    if (launch == "bar") seq.launch = LaunchQuantize::Bar;
    else if (launch == "beat") seq.launch = LaunchQuantize::Beat;
    else if (launch == "immediate") seq.launch = LaunchQuantize::Immediate;
    else fail("launch must be \"bar\", \"beat\" or \"immediate\"");
    seq.repeat          = s.value("repeat", true);
    seq.atStart         = s.value("atStart", false);
    seq.noteOffOffsetMs = s.value("noteOffOffsetMs", 0.0);
    if (seq.noteOffOffsetMs < -1000.0 || seq.noteOffOffsetMs > 1000.0) fail("noteOffOffsetMs must be within +-1000");

    const bool midi1       = seq.protocol == MidiProtocol::Midi1;
    const int  maxVelocity = midi1 ? 127 : 65535;
    if (s.contains("notes")) {
        for (const Json& n : s.at("notes")) {
            if (!n.is_array() || n.size() != 4) fail("each note is [start, length, pitch, velocity]");
            SequenceNote note;
            const long start = n[0].get<long>(), length = n[1].get<long>(), velocity = n[3].get<long>();
            note.pitch       = n[2].get<int>();
            if (start < 0 || length < 1 || start + length > static_cast<long>(seq.length))
                fail("note at " + std::to_string(start) + ": it must lie within the sequence (length " +
                     std::to_string(seq.length) + ")");
            if (velocity < 1 || velocity > maxVelocity)
                fail("note velocity must be within 1-" + std::to_string(maxVelocity));
            const double semitones = midi1 ? note.pitch : seq.referenceNote + note.pitch * 12.0 / seq.edo;
            if (semitones < 0.0 || semitones >= 128.0) fail("note pitch " + std::to_string(note.pitch) + " is out of range");
            note.start    = static_cast<std::uint32_t>(start);
            note.length   = static_cast<std::uint32_t>(length);
            note.velocity = static_cast<std::uint16_t>(velocity);
            seq.notes.push_back(note);
        }
    }
    if (s.contains("triggers")) {
        if (!s.at("triggers").is_array()) fail("triggers should be a list");
        for (const Json& t : s.at("triggers")) seq.triggers.push_back(parseTrigger(t));
    }
    if (s.contains("targets")) {   // slot ids; checked against the slots once they're read
        if (!s.at("targets").is_array()) fail("targets should be a list of slot ids");
        for (const Json& t : s.at("targets")) {
            const std::string id = t.get<std::string>();
            if (std::find(seq.targets.begin(), seq.targets.end(), id) == seq.targets.end()) seq.targets.push_back(id);
        }
    }
    return seq;
}

Json sequenceJson(const Sequence& seq) {
    Json s;
    s["id"]       = seq.id;
    s["name"]     = seq.name;
    s["protocol"] = seq.protocol == MidiProtocol::Midi1 ? 1 : 2;
    if (seq.protocol == MidiProtocol::Midi2) {
        s["edo"]           = seq.edo;
        s["referenceNote"] = seq.referenceNote;
    }
    s["channel"]         = seq.channel + 1;
    s["group"]           = seq.group + 1;
    s["rate"]            = rateText(seq.rate);
    s["length"]          = seq.length;
    s["launch"]          = launchText(seq.launch);
    s["repeat"]          = seq.repeat;
    s["noteOffOffsetMs"] = seq.noteOffOffsetMs;
    s["atStart"]         = seq.atStart;
    Json notes           = Json::array();
    for (const SequenceNote& n : seq.notes) notes.push_back(Json::array({n.start, n.length, n.pitch, n.velocity}));
    s["notes"] = notes;
    if (!seq.triggers.empty()) {
        Json triggers = Json::array();
        for (const SequenceTrigger& t : seq.triggers) triggers.push_back(triggerJson(t));
        s["triggers"] = triggers;
    }
    if (!seq.targets.empty()) s["targets"] = seq.targets;
    return s;
}

// "transport": {"triggers": [...], "startOnNote": "off" | a source}.
TransportSettings parseTransport(const Json& j) {
    if (!j.is_object()) fail("transport should be an object");
    TransportSettings t;
    if (j.contains("triggers")) {
        if (!j.at("triggers").is_array()) fail("transport.triggers should be a list");
        for (const Json& trigger : j.at("triggers")) t.triggers.push_back(parseTrigger(trigger));
    }
    const std::string onNote = j.value("startOnNote", std::string("off"));
    if (onNote != "off") {
        t.startOnNote       = true;
        t.startOnNoteSource = parseSource(onNote);
        if (t.startOnNoteSource.kind == MidiSourceRef::Kind::Sequence)
            fail("transport.startOnNote must be \"off\" or an input, not a sequence");
    }
    return t;
}

Json transportJson(const TransportSettings& t) {
    Json j;
    Json triggers = Json::array();
    for (const SequenceTrigger& trigger : t.triggers) triggers.push_back(triggerJson(trigger));
    j["triggers"]    = triggers;
    j["startOnNote"] = t.startOnNote ? sourceText(t.startOnNoteSource) : std::string("off");
    return j;
}

// "metronome": {"count": "beats" | "notes", "click": bool, "clickLevel": dB, "clickOutput": name}.
MetronomeSettings parseMetronome(const Json& j) {
    if (!j.is_object()) fail("metronome should be an object");
    MetronomeSettings m;
    const std::string count = j.value("count", std::string("beats"));
    if (count != "beats" && count != "notes") fail("metronome.count must be \"beats\" or \"notes\"");
    m.everyNote    = count == "notes";
    m.click        = j.value("click", false);
    m.clickLevelDb = j.value("clickLevel", -12.0f);
    if (m.clickLevelDb < -60.0f || m.clickLevelDb > 0.0f) fail("metronome.clickLevel must be within -60 to 0 dB");
    m.clickOutput = j.value("clickOutput", std::string());
    return m;
}

Json metronomeJson(const MetronomeSettings& m) {
    Json j;
    j["count"]       = m.everyNote ? "notes" : "beats";
    j["click"]       = m.click;
    j["clickLevel"]  = m.clickLevelDb;
    j["clickOutput"] = m.clickOutput;
    return j;
}

// "all", or a list of numbers 1-16, as a bit mask.
std::uint16_t parseMask(const Json& value, const char* field) {
    if (value.is_string() && value.get<std::string>() == "all") return 0xFFFF;
    if (!value.is_array()) fail(std::string("midi.") + field + " should be \"all\" or a list of 1-16");
    std::uint16_t mask = 0;
    for (const Json& item : value) {
        const int n = item.get<int>();
        if (n < 1 || n > 16) fail(std::string("midi.") + field + " values must be 1-16");
        mask |= static_cast<std::uint16_t>(1u << (n - 1));
    }
    return mask;
}

Json maskJson(std::uint16_t mask) {
    if (mask == 0xFFFF) return "all";
    Json list = Json::array();
    for (int bit = 0; bit < 16; ++bit)
        if (mask & (1u << bit)) list.push_back(bit + 1);
    return list;
}

std::pair<int, int> parseRange(const Json& value, const char* field) {
    if (!value.is_array() || value.size() != 2) fail(std::string("midi.") + field + " should be [low, high]");
    const int low = value[0].get<int>(), high = value[1].get<int>();
    if (low < 0 || high > 127 || low > high) fail(std::string("midi.") + field + " must be within 0-127, low <= high");
    return {low, high};
}

RackSlot parseSlot(const Json& s, const fs::path& bundle, std::vector<std::string>& warnings) {
    RackSlot slot;
    slot.id = s.at("id").get<std::string>();
    if (!validSlotId(slot.id)) fail("slot id '" + slot.id + "' may use only letters, digits, '-' and '_'");
    slot.name = s.value("name", slot.id);

    const Json& plugin    = s.at("plugin");
    slot.plugin.format    = parseFormat(plugin.at("format").get<std::string>());
    slot.plugin.id        = plugin.at("id").get<std::string>();
    slot.plugin.path      = expandHome(plugin.value("path", std::string{}));

    if (s.contains("state")) {
        const fs::path statePath = bundle / s.at("state").get<std::string>();
        if (!readBytes(statePath, slot.state)) {
            slot.state.clear();
            warnings.push_back("slot '" + slot.id + "': state file " + statePath.string() +
                               " is missing; the plugin will start from its defaults");
        }
    }
    // "enabled": false (written until 2026-10-01) means muted now.
    slot.muted   = s.value("muted", false) || !s.value("enabled", true);
    slot.gainDb  = s.value("gainDb", 0.0f);
    if (s.contains("sends")) {
        const Json& sends = s.at("sends");
        if (!sends.is_array() || sends.size() > kRigBuses) fail("sends must be a list of up to 2 levels (dB or null)");
        for (std::size_t b = 0; b < sends.size(); ++b)
            if (!sends[b].is_null()) slot.sendDb[b] = sends[b].get<float>();
    }
    const int output = s.value("output", 1);
    if (output < 1) fail("output must be 1 or more");
    slot.firstOutput = static_cast<std::uint32_t>(output - 1);

    if (s.contains("midi")) {
        const Json& midi = s.at("midi");
        slot.source = parseSource(midi.value("source", std::string("all")));
        if (midi.contains("channels")) slot.filter.channelMask = parseMask(midi.at("channels"), "channels");
        if (midi.contains("groups")) slot.filter.groupMask = parseMask(midi.at("groups"), "groups");
        if (midi.contains("notes")) {
            const auto [low, high] = parseRange(midi.at("notes"), "notes");
            slot.filter.noteLow  = static_cast<std::uint8_t>(low);
            slot.filter.noteHigh = static_cast<std::uint8_t>(high);
        }
        if (midi.contains("velocity")) {   // 0-127 in the file, compared at 16 bits
            const auto [low, high] = parseRange(midi.at("velocity"), "velocity");
            slot.filter.velocityLow  = static_cast<std::uint16_t>(ump::scaleUp(static_cast<std::uint32_t>(low), 7, 16));
            slot.filter.velocityHigh = static_cast<std::uint16_t>(ump::scaleUp(static_cast<std::uint32_t>(high), 7, 16));
        }
        const int transpose = midi.value("transpose", 0);
        if (transpose < -127 || transpose > 127) fail("midi.transpose must be within -127..127");
        slot.filter.transpose = static_cast<std::int8_t>(transpose);
        const int notePort = midi.value("notePort", 0);
        if (notePort < 0) fail("midi.notePort must be 0 or more");
        slot.notePort = static_cast<std::uint16_t>(notePort);
    }
    return slot;
}

Json slotJson(const RackSlot& slot, bool hasState) {
    Json s;
    s["id"]   = slot.id;
    s["name"] = slot.name;
    Json plugin;
    plugin["format"] = formatName(slot.plugin.format);
    plugin["id"]     = slot.plugin.id;
    if (!slot.plugin.path.empty()) plugin["path"] = abbreviateHome(slot.plugin.path);
    s["plugin"] = plugin;
    if (hasState) s["state"] = "state/" + slot.id + ".bin";
    s["muted"]   = slot.muted;
    s["gainDb"]  = slot.gainDb;
    if (slot.sendDb[0] || slot.sendDb[1]) {
        Json sends = Json::array();
        for (const std::optional<float>& db : slot.sendDb) sends.push_back(db ? Json(*db) : Json(nullptr));
        s["sends"] = sends;
    }
    s["output"]  = slot.firstOutput + 1;

    Json midi;
    midi["source"]    = sourceText(slot.source);
    midi["channels"]  = maskJson(slot.filter.channelMask);
    midi["groups"]    = maskJson(slot.filter.groupMask);
    midi["notes"]     = Json::array({slot.filter.noteLow, slot.filter.noteHigh});
    midi["velocity"]  = Json::array({ump::scaleDown(slot.filter.velocityLow, 16, 7),
                                     ump::scaleDown(slot.filter.velocityHigh, 16, 7)});
    midi["transpose"] = static_cast<int>(slot.filter.transpose);
    midi["notePort"]  = slot.notePort;
    s["midi"]         = midi;
    return s;
}

} // namespace

RackFileResult loadRack(const std::string& bundlePath, Rack& rack) {
    RackFileResult result;
    const fs::path bundle   = bundleDirectory(bundlePath);
    const fs::path jsonPath = bundle / "rack.json";

    std::ifstream in(jsonPath);
    if (!in) {
        result.error = "cannot read " + jsonPath.string();
        return result;
    }
    Rack loaded;
    try {
        const Json doc = Json::parse(in);
        if (!doc.is_object() || doc.value("format", std::string{}) != kFormatName)
            fail("not a StageHand rack (expected \"format\": \"stagehand-rack\")");
        const int version = doc.at("version").get<int>();
        if (version > kFormatVersion)
            fail("made by a newer StageHand (format version " + std::to_string(version) +
                 "; this one reads up to " + std::to_string(kFormatVersion) + ")");
        if (version < 1) fail("invalid format version");

        std::string stem = bundle.filename().string();
        if (stem.size() > 5 && stem.compare(stem.size() - 5, 5, ".rack") == 0) stem.resize(stem.size() - 5);
        loaded.name     = doc.value("name", stem);
        loaded.tempoBpm = doc.value("tempo", 120.0);
        if (loaded.tempoBpm < 20.0 || loaded.tempoBpm > 400.0) fail("tempo must be within 20-400 BPM");
        if (doc.contains("timeSignature")) {
            const Json& sig = doc.at("timeSignature");
            if (!sig.is_array() || sig.size() != 2) fail("timeSignature must be [numerator, denominator]");
            const int n = sig[0].get<int>(), d = sig[1].get<int>();
            if (n < 1 || n > 32 || d < 1 || d > 32 || (d & (d - 1)) != 0)
                fail("timeSignature must be 1-32 over 1, 2, 4, 8, 16 or 32");
            loaded.timeSigNumerator   = n;
            loaded.timeSigDenominator = d;
        }

        std::set<std::string> sequenceIds;
        if (doc.contains("sequences")) {
            std::size_t sequenceIndex = 0;
            for (const Json& s : doc.at("sequences")) {
                ++sequenceIndex;
                try {
                    Sequence seq = parseSequence(s);
                    if (!sequenceIds.insert(seq.id).second) fail("duplicate sequence id '" + seq.id + "'");
                    loaded.sequences.push_back(std::move(seq));
                } catch (const std::exception& e) {
                    fail("sequence " + std::to_string(sequenceIndex) + ": " + e.what());
                }
            }
        }
        if (doc.contains("metronome")) {
            try {
                loaded.metronome = parseMetronome(doc.at("metronome"));
            } catch (const std::exception& e) {
                fail(std::string("metronome: ") + e.what());
            }
        }
        if (doc.contains("transport")) {
            try {
                loaded.transport = parseTransport(doc.at("transport"));
            } catch (const std::exception& e) {
                fail(std::string("transport: ") + e.what());
            }
        }

        std::set<std::string> ids;
        std::size_t           index = 0;
        for (const Json& s : doc.at("slots")) {
            ++index;
            try {
                RackSlot slot = parseSlot(s, bundle, result.warnings);
                if (!ids.insert(slot.id).second) fail("duplicate slot id '" + slot.id + "'");
                if (slot.source.kind == MidiSourceRef::Kind::Sequence) {
                    // Format 1: the slot played that sequence and nothing else. It
                    // becomes one of the sequence's targets, with no live input.
                    auto seq = std::find_if(loaded.sequences.begin(), loaded.sequences.end(),
                                            [&](const Sequence& q) { return q.id == slot.source.name; });
                    if (seq == loaded.sequences.end()) fail("there's no sequence '" + slot.source.name + "' in this rack");
                    if (std::find(seq->targets.begin(), seq->targets.end(), slot.id) == seq->targets.end())
                        seq->targets.push_back(slot.id);
                    slot.source = MidiSourceRef{MidiSourceRef::Kind::None, {}};
                }
                loaded.slots.push_back(std::move(slot));
            } catch (const std::exception& e) {
                fail("slot " + std::to_string(index) + ": " + e.what());
            }
        }
        for (const Sequence& seq : loaded.sequences)   // a target must be one of the rack's slots
            for (const std::string& target : seq.targets)
                if (ids.count(target) == 0) fail("sequence '" + seq.id + "': there's no slot '" + target + "' to play into");
        if (doc.contains("loopers")) {
            std::set<std::string> looperIds;
            std::size_t           looperIndex = 0;
            for (const Json& l : doc.at("loopers")) {
                ++looperIndex;
                try {
                    LooperSetup looper = parseLooper(l);
                    if (!looperIds.insert(looper.id).second) fail("duplicate looper id '" + looper.id + "'");
                    for (const std::string& target : looper.targets)
                        if (ids.count(target) == 0) fail("there's no slot '" + target + "' to play into");
                    loaded.loopers.push_back(std::move(looper));
                } catch (const std::exception& e) {
                    fail("looper " + std::to_string(looperIndex) + ": " + e.what());
                }
            }
        }
    } catch (const std::exception& e) {   // nlohmann's errors are std::exceptions too
        result.error = jsonPath.string() + ": " + e.what();
        return result;
    }
    rack      = std::move(loaded);
    result.ok = true;
    return result;
}

RackFileResult saveRack(const std::string& bundlePath, const Rack& rack) {
    RackFileResult result;
    const fs::path bundle = bundleDirectory(bundlePath);
    const fs::path temp   = bundle.string() + ".tmp";
    std::error_code ec;
    try {
        fs::remove_all(temp);
        fs::create_directories(temp / "state");

        Json doc;
        doc["format"]  = kFormatName;
        doc["version"] = kFormatVersion;
        doc["name"]          = rack.name;
        doc["tempo"]         = rack.tempoBpm;
        doc["timeSignature"] = Json::array({rack.timeSigNumerator, rack.timeSigDenominator});
        Json slots     = Json::array();
        std::set<std::string> ids;
        for (const RackSlot& slot : rack.slots) {
            if (!validSlotId(slot.id) || !ids.insert(slot.id).second)
                fail("invalid or duplicate slot id '" + slot.id + "'");
            if (!slot.state.empty() && !writeBytes(temp / "state" / (slot.id + ".bin"), slot.state))
                fail("cannot write the state of slot '" + slot.id + "'");
            slots.push_back(slotJson(slot, !slot.state.empty()));
        }
        doc["slots"] = slots;
        if (!rack.sequences.empty()) {
            Json sequences = Json::array();
            std::set<std::string> sequenceIds;
            for (const Sequence& seq : rack.sequences) {
                if (!validSlotId(seq.id) || !sequenceIds.insert(seq.id).second)
                    fail("invalid or duplicate sequence id '" + seq.id + "'");
                sequences.push_back(sequenceJson(seq));
            }
            doc["sequences"] = sequences;
        }
        if (!rack.loopers.empty()) {
            Json loopers = Json::array();
            std::set<std::string> looperIds;
            for (const LooperSetup& looper : rack.loopers) {
                if (!validSlotId(looper.id) || !looperIds.insert(looper.id).second)
                    fail("invalid or duplicate looper id '" + looper.id + "'");
                loopers.push_back(looperJson(looper));
            }
            doc["loopers"] = loopers;
        }
        if (rack.transport != TransportSettings{}) doc["transport"] = transportJson(rack.transport);
        if (rack.metronome != MetronomeSettings{}) doc["metronome"] = metronomeJson(rack.metronome);
        {
            std::ofstream out(temp / "rack.json", std::ios::trunc);
            out << doc.dump(2) << '\n';
            if (!out) fail("cannot write " + (temp / "rack.json").string());
        }

        swapInBundle(temp, bundle);   // a crash never leaves a half-written X.rack
    } catch (const std::exception& e) {
        result.error = e.what();
        fs::remove_all(temp, ec);
        return result;
    }
    result.ok = true;
    return result;
}

// --- paths -------------------------------------------------------------------------

std::string resolveRackPath(const std::string& text) {
    std::string expanded = text;
    if (const char* home = std::getenv("HOME"); home != nullptr && (text == "~" || text.rfind("~/", 0) == 0))
        expanded = home + text.substr(1);
    std::error_code ec;
    fs::path        path = fs::weakly_canonical(expanded, ec);
    if (ec) path = fs::absolute(expanded, ec);
    if (!path.has_filename()) path = path.parent_path();   // "X.rack/" -> "X.rack"
    return path.string();
}

std::string rackBundleOf(const std::string& chosen) {
    if (chosen.empty()) return {};
    fs::path        path = resolveRackPath(chosen);
    std::error_code ec;
    if (path.filename() == "rack.json") path = path.parent_path();
    return fs::is_regular_file(path / "rack.json", ec) ? path.string() : std::string{};
}

std::string saveAsPath(const std::string& chosen) {
    if (chosen.empty()) return {};
    fs::path path = resolveRackPath(chosen);
    if (path.extension() != ".rack") path += ".rack";
    return path.string();
}

SaveTarget classifySaveTarget(const std::string& target, const std::string& current) {
    std::error_code ec;
    const fs::path  path = resolveRackPath(target);
    if (!fs::exists(path, ec)) return SaveTarget::New;
    if (!fs::is_directory(path, ec) || !fs::exists(path / "rack.json", ec)) return SaveTarget::NotARack;
    if (!current.empty() && fs::equivalent(path, resolveRackPath(current), ec)) return SaveTarget::Current;
    return SaveTarget::OtherRack;
}

std::string rackNameOf(const std::string& bundlePath) {
    return fs::path(resolveRackPath(bundlePath)).stem().string();
}

} // namespace stagehand
