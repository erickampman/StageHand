#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <unistd.h>

#include "App/MidiMonitor.h"
#include "uw/midi/PlatformMidiInput.h"
#include "Session/Session.h"

#include "App/AppHost.h"
#include "App/MainLoop.h"
#include "App/RackController.h"
#include "App/TestTone.h"
#include "Engine/Engine.h"
#include "uw/clap/ClapPluginFactory.h"
#include "Session/RackFile.h"
#include "Session/RigFile.h"
#include "Session/SetlistFile.h"
#include "Session/WindowState.h"

#ifdef __APPLE__
#include "uw/audio/CoreAudioBackend.h"
#include "Session/AudioDeviceState.h"
#else
#include "uw/audio/JackBackend.h"
#include "Core/UwNames.h"
#endif

// The plugin editors' windows: X11 on Linux, Cocoa on macOS (the same
// interface). The rack window is DGL's on both.
#ifdef __APPLE__
#include "App/EditorWindowMac.h"
#else
#include "App/EditorWindow.h"
#endif
#include "Ui/RackUi.h"

using namespace stagehand;

namespace {

// StageHand's name where uw-core asks for the app's: JACK and ALSA clients,
// CoreMIDI ports, the CoreAudio aggregate device.
constexpr const char* kAppName = "StageHand";
#ifdef __APPLE__
constexpr const char* kAggregateUidPrefix = "com.unlikelyware.stagehand";
#endif

std::atomic<bool>      quitRequested{false};
std::atomic<MainLoop*> activeLoop{nullptr};   // --play's loop, for the signal handler

void handleSignal(int) {
    quitRequested.store(true);
    if (MainLoop* loop = activeLoop.load()) loop->requestQuit();   // async-signal-safe
}

// The modifier for StageHand's keys in its windows (docs/macos.md, decision 7).
#ifdef __APPLE__
constexpr const char* kWindowKey = "Cmd";
#else
constexpr const char* kWindowKey = "Ctrl";
#endif

// The audio backend stopped under us: JACK's server went, or the Mac's output
// device did.
#ifdef __APPLE__
bool audioStopped(const CoreAudioBackend& backend) { return backend.deviceLost(); }
constexpr const char* kAudioStoppedMessage = "The output device went away";
#else
bool audioStopped(const JackBackend& backend) { return backend.serverShutDown(); }
constexpr const char* kAudioStoppedMessage = "JACK server shut down";
#endif

void printUsage() {
    std::puts(
        "usage: stagehand [MODE] [options]\n"
        "\n"
        "Without a mode, StageHand plays (--play is the default and can be left out).\n"
        "With no arguments it reopens the last setlist at the song it was on; with\n"
        "none (or a rack open on its own last), a new, empty, untitled rack.\n"
        "\n"
        "Modes:\n"
        "  --play               (the default) host plugins: MIDI in, audio out. Plays\n"
        "                       --setlist, --rack or --plugin; with none, as above\n"
        "  --list-plugins       list installed CLAP plugins\n"
        "  --list-midi          list MIDI sources (id, protocol, hardware/software/app)\n"
        "  --monitor            print incoming MIDI exactly as it arrives\n"
        "  --tone [HZ]          play a test sine (default 440 Hz) to check the audio path\n"
        "  --list-audio         list output devices (macOS; on Linux PipeWire routes)\n"
        "  -h, --help           show this help\n"
        "\n"
        "What to play:\n"
        "  --rack PATH          a rack: a .rack bundle (see docs/rack-format.md)\n"
        "  --setlist PATH       a setlist: a .setlist file, from its first song (see\n"
        "                       docs/setlists.md)\n"
        "  --plugin PATH|ID     or a single plugin: a .clap file, or an id or name from\n"
        "                       --list-plugins (starts from the plugin's defaults)\n"
        "  --plugin-id ID       which plugin, when the .clap file holds several\n"
        "  --open-editors       open every plugin's editor at start (otherwise the rack\n"
        "                       window's Editor buttons open them)\n"
        "  --no-ui              no rack window, console only; every editor opens at start\n"
        "  --no-editor          with --no-ui: don't open the editors either\n"
        "\n"
        "Saving (when playing). A --rack is saved back to itself when StageHand quits, with\n"
        "each plugin's current state:\n"
        "  --no-save-rack       don't save anything\n"
        "  --save-rack PATH     save to PATH instead (\"save as\"); the way to keep a\n"
        "                       --plugin session\n"
        "  --overwrite          let --save-rack replace an existing rack other than the\n"
        "                       one being played (refused otherwise)\n"
        "\n"
        "Audio (playing, --tone):\n"
        "  --quantum N          force a PipeWire buffer size of N frames (4-8192) while\n"
        "                       StageHand runs, released when it exits. Treat it as\n"
        "                       affecting all audio on the system.\n"
        "  --channels N         output channels to register (default 2)\n"
        "  --no-connect         don't connect to the system playback ports\n"
        "  --device NAME        macOS: the output device, by name, UID or a unique part\n"
        "                       of its name (--list-audio). Default: the system's\n"
        "                       output when StageHand starts, not followed afterwards.\n"
        "                       On macOS --quantum sets this app's buffer on the device.\n"
        "\n"
        "MIDI (playing, --monitor). Every hardware MIDI input is opened by default, plus\n"
        "two virtual inputs other apps can connect to: 'MIDI 2.0 In' and 'MIDI 1.0 In'.\n"
        "  --midi ID            open only this source instead; repeatable. ID is from\n"
        "                       --list-midi, a client:port like 14:0, or 'all' (hardware\n"
        "                       plus software and app sources)\n"
        "  --midi-protocol 1|2  MIDI 2.0 protocol preference (default 2). With 1, every\n"
        "                       source is received as MIDI 1.0\n"
        "\n"
        "Keys in the rack window and in plugin editor windows:\n"
        "  Ctrl-Q               quit StageHand (saving, unless --no-save-rack)\n"
        "  Ctrl-Shift-S         save the rack now\n"
        "In the rack window also: Space starts or stops the transport; Ctrl-S saves,\n"
        "Ctrl-N makes a new rack, Ctrl-O opens one (in a setlist: adds one to the set),\n"
        "and the File menu has Save As and the setlist commands. An untitled rack with\n"
        "changes asks before they're lost.\n"
        "Closing the rack window quits the same way. Closing an editor window closes\n"
        "that editor only; StageHand keeps playing.\n"
        "In the terminal, Ctrl-C quits the same way as Ctrl-Q.\n"
        "On macOS, Cmd takes Ctrl's place in StageHand's windows (Cmd-Q, Cmd-Shift-S).\n"
        "\n"
        "Audio goes through PipeWire's JACK layer. Started without pw-jack and with no\n"
        "JACK server to reach, playing and --tone restart themselves under pw-jack.\n"
        "\n"
        "Examples:\n"
        "  pw-jack stagehand --rack ~/Racks/Piano.rack\n"
        "  pw-jack stagehand --setlist ~/Sets/Friday.setlist\n"
        "  pw-jack stagehand --save-rack ~/Racks/New.rack    (build it in the window)\n"
        "  pw-jack stagehand --rack ~/Racks/Gig.rack --no-save-rack --quantum 128\n"
        "  pw-jack stagehand --plugin com.unlikelyware.polygraphmodular \\\n"
        "      --save-rack ~/Racks/New.rack\n"
        "  stagehand --list-plugins");
}

enum class Mode { None, Play, ListPlugins, ListMidi, ListAudio, Monitor, Tone };

struct Options {
    Mode                     mode        = Mode::None;
    std::string              plugin;
    std::string              pluginId;
    std::string              rack;
    std::string              setlist;
    bool                     reopen      = false;   // no arguments: the last setlist, at its song
    std::string              saveRack;
    bool                     editor      = true;    // --no-ui: open the editors at start
    bool                     openEditors = false;   // with the rack window: open them all at start
    bool                     ui          = true;
    bool                     overwrite   = false;
    bool                     noSaveRack  = false;
    double                   toneHz      = 440.0;
    unsigned                 channels    = 2;
    unsigned                 quantum     = 0;   // 0 = leave the buffer size to PipeWire
    bool                     autoConnect = true;
    std::string              device;    // macOS: the output device (decision 12); "" = the system's
    std::vector<std::string> midiSources;
    MidiProtocol             preference  = MidiProtocol::Midi2;
};

// Returns 0 to run, -1 to exit successfully (help), or a process exit code.
int parseArguments(int argc, char** argv, Options& options) {
    if (argc == 1) {   // the last setlist, else a new, untitled rack
        options.mode   = Mode::Play;
        options.reopen = true;
    }
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            printUsage();
            return -1;
        } else if (arg == "--play") {
            options.mode = Mode::Play;
        } else if (arg == "--list-plugins") {
            options.mode = Mode::ListPlugins;
        } else if (arg == "--list-midi") {
            options.mode = Mode::ListMidi;
        } else if (arg == "--list-audio") {
            options.mode = Mode::ListAudio;
        } else if (arg == "--monitor") {
            options.mode = Mode::Monitor;
        } else if (arg == "--tone") {
            options.mode = Mode::Tone;
            if (i + 1 < argc && argv[i + 1][0] != '-') options.toneHz = std::atof(argv[++i]);
        } else if (arg == "--plugin" && i + 1 < argc) {
            options.plugin = argv[++i];
        } else if (arg == "--plugin-id" && i + 1 < argc) {
            options.pluginId = argv[++i];
        } else if (arg == "--open-editors") {
            options.openEditors = true;
        } else if (arg == "--no-ui") {
            options.ui = false;
        } else if (arg == "--no-editor") {
            options.editor = false;
        } else if (arg == "--rack" && i + 1 < argc) {
            options.rack = argv[++i];
        } else if (arg == "--setlist" && i + 1 < argc) {
            options.setlist = argv[++i];
        } else if (arg == "--save-rack" && i + 1 < argc) {
            options.saveRack = argv[++i];
        } else if (arg == "--overwrite") {
            options.overwrite = true;
        } else if (arg == "--no-save-rack") {
            options.noSaveRack = true;
        } else if (arg == "--channels" && i + 1 < argc) {
            options.channels = static_cast<unsigned>(std::atoi(argv[++i]));
        } else if (arg == "--quantum" && i + 1 < argc) {
            // PipeWire's documented quantum-floor and quantum-limit defaults.
            const int n = std::atoi(argv[++i]);
            if (n < 4 || n > 8192) {
                std::fprintf(stderr, "--quantum must be between 4 and 8192 frames\n");
                return 2;
            }
            options.quantum = static_cast<unsigned>(n);
        } else if (arg == "--no-connect") {
            options.autoConnect = false;
        } else if (arg == "--device" && i + 1 < argc) {
            options.device = argv[++i];
        } else if (arg == "--midi" && i + 1 < argc) {
            options.midiSources.emplace_back(argv[++i]);
        } else if (arg == "--midi-protocol" && i + 1 < argc) {
            const std::string_view value = argv[++i];
            if (value != "1" && value != "2") {
                std::fprintf(stderr, "--midi-protocol must be 1 or 2\n");
                return 2;
            }
            options.preference = value == "1" ? MidiProtocol::Midi1 : MidiProtocol::Midi2;
        } else {
            std::fprintf(stderr, "unknown option: %s\n\n", argv[i]);
            printUsage();
            return 2;
        }
    }
    if (options.mode == Mode::None) options.mode = Mode::Play;   // playing is the default
    return 0;
}

const char* protocolName(MidiProtocol protocol) {
    return protocol == MidiProtocol::Midi2 ? "MIDI 2.0" : "MIDI 1.0";
}

std::string dialectNames(NoteDialectMask mask) {
    std::string text;
    auto add = [&](NoteDialectMask bit, const char* name) {
        if ((mask & bit) == 0) return;
        if (!text.empty()) text += '|';
        text += name;
    };
    add(NoteDialect::Clap, "CLAP");
    add(NoteDialect::Midi, "MIDI");
    add(NoteDialect::MidiMpe, "MPE");
    add(NoteDialect::Midi2, "MIDI2");
    return text.empty() ? "none" : text;
}

// Virtual inputs other apps (a MIDI 2.0 test tool, later KeyEDO) connect to.
const std::vector<std::pair<std::string, MidiProtocol>> kVirtualInputs = {
    {"MIDI 2.0 In", MidiProtocol::Midi2}, {"MIDI 1.0 In", MidiProtocol::Midi1}};

// Opens the two virtual inputs plus the requested sources and prints where each
// landed. Registers each with `session` (if given) so rack sources resolve to
// it. Returns handle -> label.
std::map<MidiSourceHandle, std::string> openMidiInputs(PlatformMidiInput& input, const Options& options,
                                                       Session* session) {
    std::map<MidiSourceHandle, std::string> names;
    std::printf("MIDI protocol preference: %s\n", protocolName(options.preference));
    std::puts("MIDI inputs:");

    for (const auto& [name, protocol] : kVirtualInputs) {
        const MidiSourceHandle handle = input.openVirtual(name, protocol);
        if (handle == kInvalidMidiSource) continue;
        names[handle] = name;
        if (session) session->addOpenSource(MidiSourceRef{MidiSourceRef::Kind::Virtual, name}, handle);
        std::printf("  %-30s virtual, %s\n", name.c_str(), input.describe(handle).c_str());
    }

    // No --midi: every hardware input, as a live host should (MainStage listens to
    // all inputs unless told otherwise). --midi narrows it; 'all' adds software
    // and app sources too.
    std::vector<std::string> ids = options.midiSources;
    const bool allSources = ids.size() == 1 && ids.front() == "all";
    if (ids.empty() || allSources) {
        ids.clear();
        for (const MidiSourceInfo& source : input.enumerate())
            if (allSources || source.isHardware) ids.push_back(source.id);
        if (ids.empty() && !allSources)
            std::puts("  (no hardware MIDI inputs found; plug one in and restart, or use the virtual inputs)");
    }
    for (const std::string& id : ids) {
        const MidiSourceHandle handle = input.open(id);
        if (handle == kInvalidMidiSource) continue;
        names[handle] = id;
        if (session) session->addOpenSource(MidiSourceRef{MidiSourceRef::Kind::Device, id}, handle);
        std::printf("  %-30s %s\n", id.c_str(), input.describe(handle).c_str());
    }
    std::printf("MIDI thread: %s\n", input.threadDescription());
    return names;
}

// --- --tone / --list-audio ------------------------------------------------------------------------------

int runTone(const Options& options) {
#ifdef __APPLE__
    CoreAudioBackend backend(options.device);
    backend.setAggregateIdentity(kAppName, kAggregateUidPrefix);
    const auto       stopped = [&] { return backend.deviceLost(); };
#else
    if (!options.device.empty()) std::puts("--device is macOS's: on Linux, PipeWire routes the outputs.");
    JackBackend backend(kAppName, options.autoConnect);
    const auto  stopped = [&] { return backend.serverShutDown(); };
#endif
    AudioConfig requested;
    requested.outputChannels = options.channels;
    requested.bufferSize     = options.quantum;
    AudioConfig negotiated;
    if (!backend.open(requested, negotiated)) return 1;

    std::printf("%s: %.0f Hz, %u-frame buffer, %u in / %u out\n", backend.name(),
                negotiated.sampleRate, negotiated.bufferSize, negotiated.inputChannels,
                negotiated.outputChannels);
#ifdef __APPLE__
    std::printf("Device: %s (%u outputs); output latency %.1f ms\n", backend.device().name.c_str(),
                backend.device().outputChannels, backend.outputLatencySeconds() * 1000.0);
#else
    std::printf("JACK clock - CLOCK_MONOTONIC: %lld ns\n",
                static_cast<long long>(backend.clockOffset()));
#endif

    TestTone tone(options.toneHz, 0.125f);   // ~-18 dBFS
    backend.setProcessor(&tone);
    if (!backend.start()) return 1;
#ifndef __APPLE__
    if (const std::uint32_t forced = backend.forcedQuantum(); forced != 0)
        std::printf("Forcing a %u-frame quantum while running (applies at activation).\n",
                    forced);
#endif
    std::printf("Playing %.1f Hz. Ctrl-C to stop.\n", options.toneHz);

    std::uint64_t lastXruns  = 0;
    std::uint32_t lastBuffer = negotiated.bufferSize;
    double        lastRate   = negotiated.sampleRate;
    while (!quitRequested.load() && !stopped()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (const std::uint64_t x = backend.xrunCount(); x != lastXruns) {
            std::printf("xruns: %llu\n", static_cast<unsigned long long>(x));
            lastXruns = x;
        }
        if (const std::uint32_t b = backend.config().bufferSize; b != lastBuffer) {
            std::printf("buffer size changed: %u -> %u frames\n", lastBuffer, b);
            lastBuffer = b;
        }
        if (const double r = backend.config().sampleRate; r != lastRate) {
            std::printf("sample rate changed: %.0f -> %.0f Hz\n", lastRate, r);
            lastRate = r;
        }
    }
#ifdef __APPLE__
    if (stopped()) std::fprintf(stderr, "The output device went away\n");
#else
    if (stopped()) std::fprintf(stderr, "JACK server shut down\n");
#endif

    const std::int64_t frames = backend.framesProcessed();
    backend.close();
    std::printf("Processed %lld frames (%.2f s).\n", static_cast<long long>(frames),
                negotiated.sampleRate > 0 ? frames / negotiated.sampleRate : 0.0);
    return 0;
}

int listAudio() {
#ifdef __APPLE__
    const std::vector<AudioDeviceInfo> devices = CoreAudioBackend::outputDevices();
    if (devices.empty()) std::puts("No output devices.");
    for (const AudioDeviceInfo& d : devices)
        std::printf("%s %-36s %2u out  %6.0f Hz  uid %s\n", d.isDefault ? "*" : " ", d.name.c_str(),
                    d.outputChannels, d.sampleRate, d.uid.c_str());
    if (!devices.empty()) std::puts("(* the system's default output: StageHand's unless --device says otherwise)");
#else
    std::puts("On Linux PipeWire routes StageHand's outputs: choose with qpwgraph or pavucontrol.");
#endif
    return 0;
}

// --- --list-midi / --monitor --------------------------------------------------------------

int listMidi() {
    const auto input = PlatformMidiInput::create(kAppName);
    if (!input) return 1;
    const std::vector<MidiSourceInfo> sources = input->enumerate();
    if (sources.empty()) {
        std::puts("No MIDI sources.");
        return 0;
    }
    for (const MidiSourceInfo& source : sources)
        std::printf("  %-44s %s  %s\n", source.id.c_str(), protocolName(source.protocol),
                    source.isHardware ? "hardware" : source.isVirtual ? "app" : "software");
    return 0;
}

int runMonitor(const Options& options) {
    const auto input = PlatformMidiInput::create(kAppName);
    if (!input) return 1;
    input->setProtocolPreference(options.preference);

    const auto monitor = std::make_unique<MidiMonitor>();
    input->setSink(monitor.get());
    const std::map<MidiSourceHandle, std::string> names = openMidiInputs(*input, options, nullptr);
    std::puts("Listening. Ctrl-C to stop.\n");
    std::fflush(stdout);

    std::uint64_t lastOverruns = 0;
    while (!quitRequested.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        monitor->drain(names);
        if (const std::uint64_t o = input->overrunCount(); o != lastOverruns) {
            std::printf("(ALSA input overruns: %llu)\n", static_cast<unsigned long long>(o));
            lastOverruns = o;
        }
    }
    monitor->drain(names);
    return 0;
}

// --- --list-plugins / --play ---------------------------------------------------------------

int listPlugins() {
    ClapPluginFactory factory;
    std::printf("Searching:");
    for (const std::string& path : factory.searchPaths()) std::printf(" %s", path.c_str());
    std::puts("");
    const std::vector<PluginDescriptor> plugins = factory.scan();
    if (plugins.empty()) std::puts("No CLAP plugins found.");
    for (const PluginDescriptor& d : plugins) {
        const char* kind = d.kind == PluginKind::Effect ? "effect" : d.kind == PluginKind::Other ? "other" : "instrument";
        std::printf("  %s  (%s %s, %s)\n    id   %s\n    file %s\n", d.name.c_str(), d.vendor.c_str(),
                    d.version.c_str(), kind, d.id.c_str(), d.path.c_str());
    }
    return 0;
}

// --plugin is a .clap path, or an id or name from --list-plugins.
bool resolvePlugin(ClapPluginFactory& factory, const Options& options, PluginDescriptor& chosen) {
    std::error_code               ec;
    std::vector<PluginDescriptor> candidates;
    if (std::filesystem::exists(options.plugin, ec)) {
        candidates = factory.scanFile(options.plugin);
    } else {
        for (const PluginDescriptor& d : factory.scan())
            if (d.id == options.plugin || d.name == options.plugin) candidates.push_back(d);
    }
    if (candidates.empty()) {
        std::fprintf(stderr, "No CLAP plugin '%s' (give a .clap path, or an id or name from --list-plugins)\n",
                     options.plugin.c_str());
        return false;
    }
    if (!options.pluginId.empty()) {
        for (const PluginDescriptor& d : candidates)
            if (d.id == options.pluginId) {
                chosen = d;
                return true;
            }
        std::fprintf(stderr, "No plugin with id '%s' in '%s'\n", options.pluginId.c_str(),
                     options.plugin.c_str());
        return false;
    }
    if (candidates.size() > 1)
        std::printf("'%s' holds %zu plugins; using the first (choose with --plugin-id)\n",
                    options.plugin.c_str(), candidates.size());
    chosen = candidates.front();
    return true;
}

// Print problem counters as they change.
void reportStats(const Engine::Stats& now, Engine::Stats& last) {
    auto delta = [](std::uint64_t a, std::uint64_t b) { return static_cast<unsigned long long>(a - b); };
    if (now.unsupportedDialect != last.unsupportedDialect)
        std::printf("dropped %llu message(s) in a protocol the plugin doesn't accept (translation: M5)\n",
                    delta(now.unsupportedDialect, last.unsupportedDialect));
    if (now.lateMidi != last.lateMidi)
        std::printf("%llu late MIDI message(s) placed at the start of their block\n",
                    delta(now.lateMidi, last.lateMidi));
    if (now.midiQueueFull != last.midiQueueFull)
        std::printf("MIDI queue full: %llu message(s) dropped\n", delta(now.midiQueueFull, last.midiQueueFull));
    if (now.slotEventsFull != last.slotEventsFull)
        std::printf("slot event buffer full: %llu message(s) dropped\n",
                    delta(now.slotEventsFull, last.slotEventsFull));
    last = now;
}

// A slot id from a display name: lowercase letters and digits, '-' between words.
// --plugin as a one-slot rack; --save-rack then writes it out as a starting point.
Rack rackForPlugin(const PluginDescriptor& desc) {
    Rack rack;
    rack.name = desc.name;
    RackSlot slot;
    slot.id     = slotIdFrom(desc.name);
    slot.name   = desc.name;
    slot.plugin = desc;
    rack.slots.push_back(slot);
    return rack;
}

std::string sourceLabel(const MidiSourceRef& ref) {
    switch (ref.kind) {
        case MidiSourceRef::Kind::All:     return "all inputs";
        case MidiSourceRef::Kind::Device:  return ref.name;
        case MidiSourceRef::Kind::Virtual: return "virtual " + ref.name;
        case MidiSourceRef::Kind::Sequence: return "sequence " + ref.name;
        case MidiSourceRef::Kind::None:    return "no live input";
    }
    return "?";
}

void printRack(const Session& session) {
    std::printf("Rack: %s\n", session.rackName().c_str());
    for (const Session::Slot& slot : session.slots()) {
        const RackSlot& s = slot.saved;
        std::printf("  [%s] %s: %s", s.id.c_str(), s.name.c_str(),
                    slot.plugin ? slot.plugin->descriptor().name.c_str() : "(placeholder: not loaded)");
        if (s.muted) std::printf(", muted");
        std::printf(", from %s", sourceLabel(s.source).c_str());
        if (s.filter.noteLow != 0 || s.filter.noteHigh != 127)
            std::printf(", notes %u-%u", s.filter.noteLow, s.filter.noteHigh);
        if (s.filter.transpose != 0) std::printf(", transpose %+d", s.filter.transpose);
        if (s.gainDb != 0.0f) std::printf(", %+.1f dB", static_cast<double>(s.gainDb));
        std::puts("");
        if (slot.plugin) {
            const PortLayout layout = slot.plugin->portLayout();
            if (!layout.noteInputs.empty())
                std::printf("      takes %s, prefers %s\n",
                            dialectNames(layout.noteInputs[0].supportedDialects).c_str(),
                            dialectNames(layout.noteInputs[0].preferredDialect).c_str());
        }
    }
}

// Where this session saves: the --save-rack path if given, else the --rack being
// played; nothing with --no-save-rack (or a --plugin session without --save-rack).
std::string saveTargetOf(const Options& options) {
    if (options.noSaveRack) return {};
    return !options.saveRack.empty() ? options.saveRack : options.rack;
}

// Refuse, before any audio starts, a save that would destroy something by
// accident: a path that exists but isn't a rack (never replaced), or an existing
// rack other than the one being played (only with --overwrite). Saving back to
// the rack being played is always allowed.
bool saveTargetAllowed(const std::string& saveTarget, const Options& options) {
    if (saveTarget.empty()) return true;
    switch (classifySaveTarget(saveTarget, options.rack)) {
        case SaveTarget::New:
        case SaveTarget::Current: return true;
        case SaveTarget::NotARack:
            std::fprintf(stderr,
                         "--save-rack %s: that path exists and isn't a rack, so StageHand won't replace it.\n"
                         "Choose another path.\n",
                         saveTarget.c_str());
            return false;
        case SaveTarget::OtherRack:
            if (options.overwrite) return true;
            std::fprintf(stderr,
                         "--save-rack %s would replace an existing rack you aren't playing.\n"
                         "To edit that rack, play it: --rack %s\n"
                         "To replace it anyway, add --overwrite.\n",
                         saveTarget.c_str(), saveTarget.c_str());
            return false;
    }
    return false;
}

int runPlay(const Options& options) {
    if (!options.plugin.empty() && !options.rack.empty()) {
        std::fprintf(stderr,
                     "Use --rack or --plugin, not both: a rack already names its plugins.\n"
                     "  play the rack:                  --rack %s\n"
                     "  start fresh from the plugin:    --plugin %s\n",
                     options.rack.c_str(), options.plugin.c_str());
        return 2;
    }
    if (!options.setlist.empty() && (!options.rack.empty() || !options.plugin.empty() || !options.saveRack.empty())) {
        std::fprintf(stderr,
                     "--setlist chooses its racks itself: don't combine it with --rack, --plugin or --save-rack.\n");
        return 2;
    }
    if (!options.setlist.empty() && !isSetlistFile(options.setlist)) {
        std::fprintf(stderr, "--setlist %s: no setlist file there.\n", options.setlist.c_str());
        return 1;
    }
    if (options.noSaveRack && !options.saveRack.empty()) {
        std::fprintf(stderr, "--save-rack and --no-save-rack contradict each other; use one.\n");
        return 2;
    }
    const std::string saveTarget = saveTargetOf(options);
    if (!saveTargetAllowed(saveTarget, options)) return 2;
    ClapPluginFactory factory;
    Rack              rack;
    if (!options.rack.empty()) {
        const RackFileResult result = loadRack(options.rack, rack);
        if (!result.ok) {
            std::fprintf(stderr, "Can't load the rack: %s\n", result.error.c_str());
            return 1;
        }
        for (const std::string& warning : result.warnings) std::fprintf(stderr, "warning: %s\n", warning.c_str());
    } else if (!options.plugin.empty()) {
        PluginDescriptor desc;
        if (!resolvePlugin(factory, options, desc)) return 1;
        rack = rackForPlugin(desc);
    } else if (!options.saveRack.empty()) {   // --save-rack alone: a new, empty rack, named after its file
        rack.name = rackNameOf(options.saveRack);
    } else {   // nothing to play: a new, untitled rack (Save As names it)
        rack.name = "Untitled";
    }

    // Declaration order matters: destruction runs in reverse, so plugins (held
    // by the session) go before the engine, the host services they call, and
    // the backend.
#ifdef __APPLE__
    // The output device (decision 12): --device chooses it and is remembered for
    // this Mac; without it, the one remembered, or the system's default (the
    // default stands in while a remembered one isn't there).
    AudioDeviceChoice remembered;
    const bool        haveRemembered = options.device.empty() &&
                                loadAudioDeviceChoice(defaultAudioDevicePath(), remembered);
    CoreAudioBackend  backend(!options.device.empty() ? options.device
                              : haveRemembered       ? (remembered.uid.empty() ? remembered.name : remembered.uid)
                                                     : std::string{},
                              haveRemembered);
    backend.setAggregateIdentity(kAppName, kAggregateUidPrefix);
#else
    if (!options.device.empty()) std::puts("--device is macOS's: on Linux, PipeWire routes the outputs.");
    JackBackend backend(kAppName, options.autoConnect);
#endif
    AudioConfig requested;
    requested.outputChannels    = options.channels;
    requested.auxOutputChannels = 2;   // the click, connected only where the rack says
    requested.bufferSize        = options.quantum;
    AudioConfig negotiated;
    if (!backend.open(requested, negotiated)) return 1;
    std::printf("%s: %.0f Hz, %u-frame buffer, %u out\n", backend.name(), negotiated.sampleRate,
                negotiated.bufferSize, negotiated.outputChannels);
#ifdef __APPLE__
    std::printf("Output device: %s\n", backend.device().name.c_str());
    if (backend.usingFallback())
        std::printf("The output device chosen for this Mac, '%s', isn't here: playing on '%s' until it is.\n",
                    remembered.name.c_str(), backend.device().name.c_str());
    if (!options.device.empty() &&
        !saveAudioDeviceChoice(defaultAudioDevicePath(), AudioDeviceChoice{backend.device().name, backend.device().uid}))
        std::fprintf(stderr, "warning: couldn't remember '%s' as this Mac's output device\n",
                     backend.device().name.c_str());
#endif

    MainLoop   loop;
    AppHost    host(loop);
    const auto engine = std::make_unique<Engine>();
    backend.setProcessor(engine.get());

    std::unique_ptr<PlatformMidiInput> input = PlatformMidiInput::create(kAppName);
    if (!input) return 1;
    input->setProtocolPreference(options.preference);
    input->setSink(engine.get());

    Session session(factory, host, *input, *engine);
    host.onStateChanged = [&session] { session.markModified(); };
    openMidiInputs(*input, options, &session);

    Session::LoadReport report;
    const bool          runnable = session.load(rack, negotiated.sampleRate, report);
    printRack(session);
    for (const std::string& warning : report.warnings) std::fprintf(stderr, "warning: %s\n", warning.c_str());
    // With the rack window, an empty or broken rack can be fixed there; the
    // console alone can't.
    if (!runnable && !options.ui) {
        std::fprintf(stderr, "Nothing to play: no slot could be loaded.\n");
        return 1;
    }
    if (!runnable) std::puts("Nothing plays yet: add plugins in the rack window.");

    if (!backend.start()) return 1;
#ifndef __APPLE__
    if (const std::uint32_t forced = backend.forcedQuantum(); forced != 0)
        std::printf("Forcing a %u-frame quantum while running (applies at activation).\n", forced);
#endif

    // Editors, saving, and the rack view's model (docs/rack-view.md, section 5).
    RackController* controllerRef = nullptr;   // for the editors' save key
    RackController::Config config;
    config.openEditor = [&loop, &controllerRef](HostedPlugin& plugin,
                                                const std::string& title) -> std::unique_ptr<EditorHandle> {
        struct Handle final : EditorHandle {
            std::unique_ptr<EditorWindow> window;
            bool isOpen() const override { return window->isOpen(); }
        };
        auto window = EditorWindow::open(plugin, loop, title, [&controllerRef] {
            if (controllerRef != nullptr) controllerRef->save();
        });
        if (!window) return nullptr;
        auto handle    = std::make_unique<Handle>();
        handle->window = std::move(window);
        return handle;
    };
    config.audioStatus = [&backend] {
        const AudioConfig audio = backend.config();
        return RackController::AudioStatus{audio.sampleRate, audio.bufferSize, backend.xrunCount(),
                                           audio.outputChannels};
    };
    config.quit               = [&loop] { loop.requestQuit(); };
    config.virtualInputs      = kVirtualInputs;
    config.protocolPreference = options.preference;
    config.saveTarget         = saveTarget.empty() ? std::string{} : resolveRackPath(saveTarget);
    config.savingOff          = options.noSaveRack;
    config.clickRouting       = &backend;
    config.outputLatencySeconds = backend.outputLatencySeconds();   // started and connected by now
    config.lastPositionPath   = defaultLastPositionPath();
    // Devices plugged in later are opened as at start: hardware, or everything
    // with --midi all; not when --midi named the inputs.
    const bool allMidi     = options.midiSources.size() == 1 && options.midiSources.front() == "all";
    config.openNewInputs   = options.midiSources.empty() || allMidi;
    config.openNewSoftware = allMidi;
    config.defaultRigPath     = defaultRigPath();
    RackController controller(session, *engine, *input, factory, std::move(config));
    // A setlist: --setlist from its first song, or (no arguments) the last one
    // at its song. Its rack replaces the untitled one loaded above.
    if (!options.setlist.empty()) {
        controller.openSetlist(options.setlist);
    } else if (LastPosition last; options.reopen && loadLastPosition(defaultLastPositionPath(), last) &&
                                  !last.setlist.empty()) {
        if (isSetlistFile(last.setlist))
            controller.openSetlist(last.setlist, last.song);
        else
            std::printf("The last setlist, %s, isn't there any more; starting with an untitled rack.\n",
                        last.setlist.c_str());
    }
    controllerRef = &controller;
    std::unique_ptr<RackUi>* uiRef = nullptr;   // set once the window exists
    auto printNotices = [&controller, &uiRef] {
        for (const std::string& text : controller.takeNotices()) {
            std::puts(text.c_str());
            if (uiRef != nullptr && *uiRef) (*uiRef)->showNotice(text);
        }
        std::fflush(stdout);
    };

    // The rack window, unless --no-ui (or there's no display).
    std::unique_ptr<RackUi> ui = options.ui ? RackUi::open(controller, defaultWindowStatePath()) : nullptr;
    uiRef                      = &ui;

    // Editors open after audio starts, so they show the running plugins. With
    // the rack window they open on request; without it, at start.
    if (ui ? options.openEditors : options.editor) controller.openAllEditors();

    if (!saveTarget.empty())
        std::printf("Saves to %s on exit; %s-Shift-S in %s saves now (--no-save-rack: off).\n", saveTarget.c_str(), kWindowKey,
                    ui ? "StageHand's windows" : "an editor window");
    else if (options.noSaveRack)
        std::puts("Not saving (--no-save-rack).");
    else if (ui)
        std::puts("Untitled rack: Save As in the rack window's File menu keeps it.");
    else
        std::puts("Not saving: this session has no rack. Add --save-rack PATH to keep it.");
    const std::string stopHow = ui ? std::string(kWindowKey) + "-Q in StageHand's windows, or Ctrl-C here,"
                                : controller.openEditorCount() > 0
                                    ? std::string(kWindowKey) + "-Q in an editor window, or Ctrl-C here,"
                                    : std::string("Ctrl-C");
    std::printf("Playing %s. Send MIDI to an input above. %s stops.\n", session.rackName().c_str(), stopHow.c_str());
    std::fflush(stdout);

    // Housekeeping on the main loop: the controller's service and tick, plus the
    // console reporting that stands in for a UI.
    Engine::Stats lastStats;
    std::uint64_t lastXruns       = 0;
    std::uint32_t lastBuffer      = negotiated.bufferSize;
    bool          restartReported = false;
    std::size_t   openEditors     = controller.openEditorCount();
    loop.addTimer(5, [&] {
        controller.service();
        reportStats(engine->stats(), lastStats);
        if (const std::uint64_t x = backend.xrunCount(); x != lastXruns) {
            std::printf("xruns: %llu\n", static_cast<unsigned long long>(x));
            lastXruns = x;
        }
        if (const std::uint32_t b = backend.config().bufferSize; b != lastBuffer) {
            std::printf("buffer size changed: %u -> %u frames\n", lastBuffer, b);
            lastBuffer = b;
        }
        if (host.restartRequested() && !restartReported) {
            std::puts("The plugin asked for a restart; not supported yet (reload StageHand).");
            restartReported = true;
        }
#ifdef __APPLE__
        for (const std::string& text : backend.service()) std::puts(text.c_str());   // devices going and coming
#else
        if (audioStopped(backend)) loop.requestQuit();
#endif
        std::fflush(stdout);
    });
    const auto started = std::chrono::steady_clock::now();
    loop.addTimer(33, [&] {
        controller.tick(std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
        printNotices();
        if (ui) ui->refresh();
        const std::size_t stillOpen = controller.openEditorCount();
        if (stillOpen < openEditors && !ui) {   // with the window, it's plain to see
            std::puts("Editor closed; still playing. Ctrl-C to quit.");
            std::fflush(stdout);
        }
        openEditors = stillOpen;
    });
    if (ui)
        loop.addTimer(16, [&] {   // the rack window's events
            ui->idle();
            if (ui->closed()) loop.requestQuit();   // closing it quits, like Ctrl-Q
        });

    activeLoop.store(&loop);
    if (quitRequested.load()) loop.requestQuit();   // a signal that beat the line above
    loop.run();
    activeLoop.store(nullptr);
    if (audioStopped(backend)) std::fprintf(stderr, "%s\n", kAudioStoppedMessage);

    // Shutdown (Engine.h): close the rack window, then the editors while the
    // plugins are fully alive; save
    // (plugins still running, so their state is current) unless saving is off;
    // stop MIDI; run nothing so the audio thread stops the plugins; stop audio.
    // The plugins are deactivated and destroyed on this thread when the session
    // goes.
    ui.reset();
    controller.closeAllEditors();
    const bool unsaved  = controller.unsavedUntitled();
    const int  exitCode = controller.saveForQuit() ? 0 : 1;
    printNotices();
    if (unsaved) std::puts("The untitled rack's changes weren't saved.");
    input.reset();
    host.onStateChanged = nullptr;   // plugins may outlive `session` (in the engine's graphs)
    session.unload();
    for (int i = 0; i < 2000 && !engine->committedGraphAdopted() && !audioStopped(backend); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    backend.stop();
    engine->collect();
    std::puts("Stopped.");
    return exitCode;
}

#ifndef __APPLE__
// Started without pw-jack (from a launcher, say) and no JACK server in reach:
// run again under pw-jack, once. Returns if that isn't needed or didn't work;
// the backend then reports the missing server as usual.
void restartUnderPwJackIfNeeded(int argc, char** argv) {
    constexpr const char* kMarker = "STAGEHAND_PW_JACK";
    if (std::getenv(kMarker) != nullptr || JackBackend::serverAvailable()) return;

    std::error_code   ec;
    const std::string self = std::filesystem::read_symlink("/proc/self/exe", ec).string();
    std::vector<char*> args{const_cast<char*>("pw-jack"), ec ? argv[0] : const_cast<char*>(self.c_str())};
    for (int i = 1; i < argc; ++i) args.push_back(argv[i]);
    args.push_back(nullptr);

    std::puts("No JACK server in reach; restarting under pw-jack.");
    std::fflush(stdout);
    setenv(kMarker, "1", 1);
    execvp("pw-jack", args.data());   // returns only on failure
    std::fprintf(stderr, "Couldn't run pw-jack: %s\n", std::strerror(errno));
    unsetenv(kMarker);
}
#endif

} // namespace

int main(int argc, char** argv) {
    // No vsync for any GL window in this process (ours and the plugin editors'):
    // they all draw on the one main-loop thread, where a swap that waits for
    // vblank stalls every other window and makes editor drags jerky. Before any
    // GL context exists; set either variable in the environment to override.
    setenv("vblank_mode", "0", 0);            // Mesa
    setenv("__GL_SYNC_TO_VBLANK", "0", 0);    // NVIDIA

    Options options;
    if (const int result = parseArguments(argc, argv, options); result != 0)
        return result < 0 ? 0 : result;
#ifndef __APPLE__
    if (options.mode == Mode::Play || options.mode == Mode::Tone) restartUnderPwJackIfNeeded(argc, argv);
#endif

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    switch (options.mode) {
        case Mode::Play:        return runPlay(options);
        case Mode::ListPlugins: return listPlugins();
        case Mode::Tone:        return runTone(options);
        case Mode::ListAudio:   return listAudio();
        case Mode::ListMidi:    return listMidi();
        case Mode::Monitor:     return runMonitor(options);
        case Mode::None:        break;
    }
    printUsage();
    return 0;
}
