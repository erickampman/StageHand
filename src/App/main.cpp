#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "App/AppHost.h"
#include "App/EditorWindow.h"
#include "App/MainLoop.h"
#include "App/MidiMonitor.h"
#include "App/TestTone.h"
#include "Audio/JackBackend.h"
#include "Engine/Engine.h"
#include "Midi/AlsaSeqInput.h"
#include "Plugin/Clap/ClapPluginFactory.h"
#include "Session/RackFile.h"
#include "Session/Session.h"

using namespace stagehand;

namespace {

std::atomic<bool>      quitRequested{false};
std::atomic<MainLoop*> activeLoop{nullptr};   // --play's loop, for the signal handler

void handleSignal(int) {
    quitRequested.store(true);
    if (MainLoop* loop = activeLoop.load()) loop->requestQuit();   // async-signal-safe
}

void printUsage() {
    std::puts(
        "usage: stagehand MODE [options]\n"
        "\n"
        "Modes:\n"
        "  --play               host plugins: MIDI in, audio out (needs --rack or --plugin)\n"
        "  --list-plugins       list installed CLAP plugins\n"
        "  --list-midi          list MIDI sources (id, protocol, hardware/software/app)\n"
        "  --monitor            print incoming MIDI exactly as it arrives\n"
        "  --tone [HZ]          play a test sine (default 440 Hz) to check the audio path\n"
        "  -h, --help           show this help\n"
        "\n"
        "What to play (--play):\n"
        "  --rack PATH          a rack: a .rack bundle (see docs/rack-format.md)\n"
        "  --plugin PATH|ID     or a single plugin: a .clap file, or an id or name from\n"
        "                       --list-plugins (starts from the plugin's defaults)\n"
        "  --plugin-id ID       which plugin, when the .clap file holds several\n"
        "  --no-editor          don't open the plugins' editor windows\n"
        "\n"
        "Saving (--play). A --rack is saved back to itself when StageHand quits, with\n"
        "each plugin's current state:\n"
        "  --no-save-rack       don't save anything\n"
        "  --save-rack PATH     save to PATH instead (\"save as\"); the way to keep a\n"
        "                       --plugin session\n"
        "  --overwrite          let --save-rack replace an existing rack other than the\n"
        "                       one being played (refused otherwise)\n"
        "\n"
        "Audio (--play, --tone):\n"
        "  --quantum N          force a PipeWire buffer size of N frames (4-8192) while\n"
        "                       StageHand runs, released when it exits. Treat it as\n"
        "                       affecting all audio on the system.\n"
        "  --channels N         output channels to register (default 2)\n"
        "  --no-connect         don't connect to the system playback ports\n"
        "\n"
        "MIDI (--play, --monitor). Every hardware MIDI input is opened by default, plus\n"
        "two virtual inputs other apps can connect to: 'MIDI 2.0 In' and 'MIDI 1.0 In'.\n"
        "  --midi ID            open only this source instead; repeatable. ID is from\n"
        "                       --list-midi, a client:port like 14:0, or 'all' (hardware\n"
        "                       plus software and app sources)\n"
        "  --midi-protocol 1|2  MIDI 2.0 protocol preference (default 2). With 1, every\n"
        "                       source is received as MIDI 1.0\n"
        "\n"
        "Keys in a plugin editor window:\n"
        "  Ctrl-Q               quit StageHand (saving, unless --no-save-rack)\n"
        "  Ctrl-Shift-S         save the rack now\n"
        "  close button         close that editor only; StageHand keeps playing\n"
        "In the terminal, Ctrl-C quits the same way as Ctrl-Q.\n"
        "\n"
        "Examples (audio goes through PipeWire's JACK layer, hence pw-jack):\n"
        "  pw-jack stagehand --play --rack ~/Racks/Piano.rack\n"
        "  pw-jack stagehand --play --rack ~/Racks/Gig.rack --no-save-rack --quantum 128\n"
        "  pw-jack stagehand --play --plugin com.unlikelyware.polygraphmodular \\\n"
        "      --save-rack ~/Racks/New.rack\n"
        "  stagehand --list-plugins");
}

enum class Mode { None, Play, ListPlugins, ListMidi, Monitor, Tone };

struct Options {
    Mode                     mode        = Mode::None;
    std::string              plugin;
    std::string              pluginId;
    std::string              rack;
    std::string              saveRack;
    bool                     editor      = true;
    bool                     overwrite   = false;
    bool                     noSaveRack  = false;
    double                   toneHz      = 440.0;
    unsigned                 channels    = 2;
    unsigned                 quantum     = 0;   // 0 = leave the buffer size to PipeWire
    bool                     autoConnect = true;
    std::vector<std::string> midiSources;
    MidiProtocol             preference  = MidiProtocol::Midi2;
};

// Returns 0 to run, -1 to exit successfully (help), or a process exit code.
int parseArguments(int argc, char** argv, Options& options) {
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
        } else if (arg == "--monitor") {
            options.mode = Mode::Monitor;
        } else if (arg == "--tone") {
            options.mode = Mode::Tone;
            if (i + 1 < argc && argv[i + 1][0] != '-') options.toneHz = std::atof(argv[++i]);
        } else if (arg == "--plugin" && i + 1 < argc) {
            options.plugin = argv[++i];
        } else if (arg == "--plugin-id" && i + 1 < argc) {
            options.pluginId = argv[++i];
        } else if (arg == "--no-editor") {
            options.editor = false;
        } else if (arg == "--rack" && i + 1 < argc) {
            options.rack = argv[++i];
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

// Opens the two virtual inputs plus the requested sources and prints where each
// landed. Registers each with `session` (if given) so rack sources resolve to
// it. Returns handle -> label.
std::map<MidiSourceHandle, std::string> openMidiInputs(AlsaSeqInput& input, const Options& options,
                                                       Session* session) {
    std::map<MidiSourceHandle, std::string> names;
    std::printf("MIDI protocol preference: %s\n", protocolName(options.preference));
    std::puts("MIDI inputs:");

    // Virtual inputs other apps (a MIDI 2.0 test tool, later KeyEDO) connect to.
    const std::pair<const char*, MidiProtocol> virtualInputs[] = {
        {"MIDI 2.0 In", MidiProtocol::Midi2}, {"MIDI 1.0 In", MidiProtocol::Midi1}};
    for (const auto& [name, protocol] : virtualInputs) {
        const MidiSourceHandle handle = input.openVirtual(name, protocol);
        if (handle == kInvalidMidiSource) continue;
        names[handle] = name;
        if (session) session->addOpenSource(MidiSourceRef{MidiSourceRef::Kind::Virtual, name}, handle);
        std::printf("  %-30s virtual, %s\n", name, input.describe(handle).c_str());
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
    std::printf("MIDI thread: %s\n",
                input.realtimePriority()
                    ? "SCHED_FIFO"
                    : "normal priority (no real-time permission; timestamps may jitter)");
    return names;
}

// --- --tone ------------------------------------------------------------------------------

int runTone(const Options& options) {
    JackBackend backend("StageHand", options.autoConnect);
    AudioConfig requested;
    requested.outputChannels = options.channels;
    requested.bufferSize     = options.quantum;
    AudioConfig negotiated;
    if (!backend.open(requested, negotiated)) return 1;

    std::printf("%s: %.0f Hz, %u-frame buffer, %u in / %u out\n", backend.name(),
                negotiated.sampleRate, negotiated.bufferSize, negotiated.inputChannels,
                negotiated.outputChannels);
    std::printf("JACK clock - CLOCK_MONOTONIC: %lld ns\n",
                static_cast<long long>(backend.clockOffset()));

    TestTone tone(options.toneHz, 0.125f);   // ~-18 dBFS
    backend.setProcessor(&tone);
    if (!backend.start()) return 1;
    if (const std::uint32_t forced = backend.forcedQuantum(); forced != 0)
        std::printf("Forcing a %u-frame quantum while running (applies at activation).\n",
                    forced);
    std::printf("Playing %.1f Hz. Ctrl-C to stop.\n", options.toneHz);

    std::uint64_t lastXruns  = 0;
    std::uint32_t lastBuffer = negotiated.bufferSize;
    while (!quitRequested.load() && !backend.serverShutDown()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (const std::uint64_t x = backend.xrunCount(); x != lastXruns) {
            std::printf("xruns: %llu\n", static_cast<unsigned long long>(x));
            lastXruns = x;
        }
        if (const std::uint32_t b = backend.config().bufferSize; b != lastBuffer) {
            std::printf("buffer size changed: %u -> %u frames\n", lastBuffer, b);
            lastBuffer = b;
        }
    }
    if (backend.serverShutDown()) std::fprintf(stderr, "JACK server shut down\n");

    const std::int64_t frames = backend.framesProcessed();
    backend.close();
    std::printf("Processed %lld frames (%.2f s).\n", static_cast<long long>(frames),
                negotiated.sampleRate > 0 ? frames / negotiated.sampleRate : 0.0);
    return 0;
}

// --- --list-midi / --monitor --------------------------------------------------------------

int listMidi() {
    const auto input = AlsaSeqInput::create();
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
    const auto input = AlsaSeqInput::create();
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
    for (const PluginDescriptor& d : plugins)
        std::printf("  %s  (%s %s)\n    id   %s\n    file %s\n", d.name.c_str(), d.vendor.c_str(),
                    d.version.c_str(), d.id.c_str(), d.path.c_str());
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
    }
    return "?";
}

void printRack(const Session& session) {
    std::printf("Rack: %s\n", session.rackName().c_str());
    for (const Session::Slot& slot : session.slots()) {
        const RackSlot& s = slot.saved;
        std::printf("  [%s] %s: %s", s.id.c_str(), s.name.c_str(),
                    slot.plugin ? slot.plugin->descriptor().name.c_str() : "(placeholder: not loaded)");
        if (!s.enabled) std::printf(", disabled");
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

// "~/x" -> "$HOME/x", then an absolute, normalized path (the file needn't exist).
std::filesystem::path resolvePath(const std::string& text) {
    std::string expanded = text;
    if (const char* home = std::getenv("HOME"); home != nullptr && (text == "~" || text.rfind("~/", 0) == 0))
        expanded = home + text.substr(1);
    std::error_code ec;
    std::filesystem::path path = std::filesystem::weakly_canonical(expanded, ec);
    if (ec) path = std::filesystem::absolute(expanded, ec);
    if (!path.has_filename()) path = path.parent_path();   // "X.rack/" -> "X.rack"
    return path;
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
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path  target = resolvePath(saveTarget);
    if (!fs::exists(target, ec)) return true;

    if (!fs::is_directory(target, ec) || !fs::exists(target / "rack.json", ec)) {
        std::fprintf(stderr,
                     "--save-rack %s: that path exists and isn't a rack, so StageHand won't replace it.\n"
                     "Choose another path.\n",
                     saveTarget.c_str());
        return false;
    }
    const bool playingIt = !options.rack.empty() && fs::equivalent(target, resolvePath(options.rack), ec);
    if (playingIt || options.overwrite) return true;
    std::fprintf(stderr,
                 "--save-rack %s would replace an existing rack you aren't playing.\n"
                 "To edit that rack, play it: --rack %s\n"
                 "To replace it anyway, add --overwrite.\n",
                 saveTarget.c_str(), saveTarget.c_str());
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
    if (options.plugin.empty() && options.rack.empty()) {
        std::fprintf(stderr, "--play needs a rack (--rack PATH) or a plugin (--plugin PATH|ID)\n");
        return 2;
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
    } else {
        PluginDescriptor desc;
        if (!resolvePlugin(factory, options, desc)) return 1;
        rack = rackForPlugin(desc);
    }

    // Declaration order matters: destruction runs in reverse, so plugins (held
    // by the session) go before the engine, the host services they call, and
    // the backend.
    JackBackend backend("StageHand", options.autoConnect);
    AudioConfig requested;
    requested.outputChannels = options.channels;
    requested.bufferSize     = options.quantum;
    AudioConfig negotiated;
    if (!backend.open(requested, negotiated)) return 1;
    std::printf("%s: %.0f Hz, %u-frame buffer, %u out\n", backend.name(), negotiated.sampleRate,
                negotiated.bufferSize, negotiated.outputChannels);

    MainLoop   loop;
    AppHost    host(loop);
    const auto engine = std::make_unique<Engine>();
    backend.setProcessor(engine.get());

    std::unique_ptr<AlsaSeqInput> input = AlsaSeqInput::create();
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
    if (!runnable) {
        std::fprintf(stderr, "Nothing to play: no slot could be loaded and enabled.\n");
        return 1;
    }

    if (!backend.start()) return 1;
    if (const std::uint32_t forced = backend.forcedQuantum(); forced != 0)
        std::printf("Forcing a %u-frame quantum while running (applies at activation).\n", forced);

    // One save routine for Ctrl-Shift-S (editor windows) and exit. Main thread;
    // plugins are running, so their state is current.
    auto saveNow = [&]() -> bool {
        if (saveTarget.empty()) {
            std::puts(options.noSaveRack ? "Not saved: saving is off (--no-save-rack)."
                                         : "Not saved: this session has no rack; start it with --save-rack PATH.");
            std::fflush(stdout);
            return false;
        }
        const RackFileResult saved = saveRack(saveTarget, session.capture());
        if (saved.ok) {
            session.markSaved();
            std::printf("Saved the rack to %s\n", saveTarget.c_str());
        } else
            std::fprintf(stderr, "Couldn't save the rack: %s\n", saved.error.c_str());
        std::fflush(stdout);
        return saved.ok;
    };

    // Editors open after audio starts, so they show the running plugins.
    std::vector<std::unique_ptr<EditorWindow>> editors;
    if (options.editor)
        for (const Session::Slot& slot : session.slots())
            if (slot.plugin)
                if (auto editor = EditorWindow::open(*slot.plugin, loop,
                                                     slot.saved.name + " - " + slot.plugin->descriptor().name +
                                                         " (StageHand)",
                                                     [&saveNow] { saveNow(); }))
                    editors.push_back(std::move(editor));

    if (!saveTarget.empty())
        std::printf("Saves to %s on exit; Ctrl-Shift-S in an editor window saves now (--no-save-rack: off).\n",
                    saveTarget.c_str());
    else if (options.noSaveRack)
        std::puts("Not saving (--no-save-rack).");
    else
        std::puts("Not saving: this session has no rack. Add --save-rack PATH to keep it.");
    std::printf("Playing %s. Send MIDI to an input above. Ctrl-Q in an editor window, or Ctrl-C here, stops.\n",
                session.rackName().c_str());
    std::fflush(stdout);

    // Housekeeping on the main loop (engine collect, plugin requests, reporting).
    Engine::Stats lastStats;
    std::uint64_t lastXruns       = 0;
    std::uint64_t lastMidiCount   = 0;
    std::uint32_t lastBuffer      = negotiated.bufferSize;
    bool          restartReported = false;
    std::size_t   openEditors     = editors.size();
    loop.addTimer(5, [&] {
        engine->collect();
        for (const Session::Slot& slot : session.slots())
            if (slot.plugin) slot.plugin->idle();
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
        std::size_t stillOpen = 0;
        for (const auto& editor : editors) stillOpen += editor->isOpen() ? 1 : 0;
        if (stillOpen < openEditors) {
            std::puts("Editor closed; still playing. Ctrl-C to quit.");
            openEditors = stillOpen;
        }
        if (backend.serverShutDown()) loop.requestQuit();
        std::fflush(stdout);
    });
    loop.addTimer(500, [&] {   // twice a second: level and MIDI activity
        const std::uint64_t midi = engine->stats().midiReceived;
        const LevelMeter::Reading level = engine->outputMeter().take();
        const float               peak  = std::max(level.peak[0], level.peak[1]);
        if (peak > 0.0f || midi != lastMidiCount)
            std::printf("output peak %6.1f dBFS   MIDI received %llu\n",
                        peak > 0.0f ? 20.0 * std::log10(peak) : -INFINITY,
                        static_cast<unsigned long long>(midi));
        lastMidiCount = midi;
    });

    activeLoop.store(&loop);
    if (quitRequested.load()) loop.requestQuit();   // a signal that beat the line above
    loop.run();
    activeLoop.store(nullptr);
    if (backend.serverShutDown()) std::fprintf(stderr, "JACK server shut down\n");

    // Shutdown (Engine.h): close editors while the plugins are fully alive; save
    // (plugins still running, so their state is current) unless saving is off;
    // stop MIDI; run nothing so the audio thread stops the plugins; stop audio.
    // The plugins are deactivated and destroyed on this thread when the session
    // goes.
    editors.clear();
    const int exitCode = (!saveTarget.empty() && !saveNow()) ? 1 : 0;
    input.reset();
    host.onStateChanged = nullptr;   // plugins may outlive `session` (in the engine's graphs)
    session.unload();
    for (int i = 0; i < 2000 && !engine->committedGraphAdopted() && !backend.serverShutDown(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    backend.stop();
    engine->collect();
    std::puts("Stopped.");
    return exitCode;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (const int result = parseArguments(argc, argv, options); result != 0)
        return result < 0 ? 0 : result;

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    switch (options.mode) {
        case Mode::Play:        return runPlay(options);
        case Mode::ListPlugins: return listPlugins();
        case Mode::ListMidi:    return listMidi();
        case Mode::Monitor:     return runMonitor(options);
        case Mode::Tone:        return runTone(options);
        case Mode::None:        break;
    }
    printUsage();
    return 0;
}
