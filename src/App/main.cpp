#include <atomic>
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
        "  --play               host a CLAP plugin: MIDI in, audio out (needs --plugin)\n"
        "  --list-plugins       list installed CLAP plugins\n"
        "  --list-midi          list MIDI sources (id, protocol, kernel/app)\n"
        "  --monitor            print incoming MIDI exactly as it arrives\n"
        "  --tone [HZ]          play a test sine (default 440 Hz) to prove the audio path\n"
        "\n"
        "--play and --monitor always open two virtual MIDI inputs other apps can\n"
        "connect to: 'MIDI 2.0 In' and 'MIDI 1.0 In'.\n"
        "\n"
        "Plugin options (--play):\n"
        "  --plugin PATH|ID     a .clap file, or an id or name from --list-plugins\n"
        "  --plugin-id ID       which plugin, when the .clap file holds several\n"
        "  --no-editor          don't open the plugin's editor window (it opens by\n"
        "                       default; closing it keeps StageHand playing)\n"
        "\n"
        "Audio options (--play, --tone):\n"
        "  --channels N         output channels to register (default 2)\n"
        "  --quantum N          force a PipeWire buffer size of N frames (4-8192) while\n"
        "                       StageHand runs; released automatically when it exits.\n"
        "                       Treat it as affecting all audio on the system.\n"
        "  --no-connect         don't auto-connect to the system playback ports\n"
        "\n"
        "MIDI options (--play, --monitor):\n"
        "                       By default every hardware MIDI input is opened.\n"
        "  --midi ID            open only these sources instead; repeatable. ID is from\n"
        "                       --list-midi, a numeric client:port like 14:0, or 'all'\n"
        "                       (hardware plus software and app sources)\n"
        "  --midi-protocol 1|2  global MIDI 2.0 protocol preference (default 2). With 1,\n"
        "                       every source is received as MIDI 1.0\n"
        "\n"
        "Audio runs under PipeWire's JACK layer:\n"
        "  pw-jack stagehand --play --plugin ~/.clap/PolygraphModular.clap --quantum 128");
}

enum class Mode { None, Play, ListPlugins, ListMidi, Monitor, Tone };

struct Options {
    Mode                     mode        = Mode::None;
    std::string              plugin;
    std::string              pluginId;
    bool                     editor      = true;
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
// landed. Returns handle -> label.
std::map<MidiSourceHandle, std::string> openMidiInputs(AlsaSeqInput& input, const Options& options) {
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
    const std::map<MidiSourceHandle, std::string> names = openMidiInputs(*input, options);
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

void printPlugin(const HostedPlugin& plugin) {
    const PluginDescriptor& d = plugin.descriptor();
    std::printf("Plugin: %s (%s %s)\n  id   %s\n", d.name.c_str(), d.vendor.c_str(), d.version.c_str(),
                d.id.c_str());
    const PortLayout layout = plugin.portLayout();
    for (std::size_t i = 0; i < layout.audioOutputs.size(); ++i) {
        const AudioPortInfo& port = layout.audioOutputs[i];
        std::printf("  audio out %zu: '%s' %u ch%s\n", i, port.name.c_str(), port.channelCount,
                    port.isMain ? " (main)" : "");
    }
    for (std::size_t i = 0; i < layout.noteInputs.size(); ++i) {
        const NotePortInfo& port = layout.noteInputs[i];
        std::printf("  note in %zu:   '%s' %s, prefers %s\n", i, port.name.c_str(),
                    dialectNames(port.supportedDialects).c_str(),
                    dialectNames(port.preferredDialect).c_str());
    }
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

int runPlay(const Options& options) {
    if (options.plugin.empty()) {
        std::fprintf(stderr, "--play needs --plugin PATH|ID\n");
        return 2;
    }
    ClapPluginFactory factory;
    PluginDescriptor  desc;
    if (!resolvePlugin(factory, options, desc)) return 1;

    // Declaration order is destruction order in reverse: the plugin must go
    // before the host services it calls, and everything before the backend.
    JackBackend backend("StageHand", options.autoConnect);
    AudioConfig requested;
    requested.outputChannels = options.channels;
    requested.bufferSize     = options.quantum;
    AudioConfig negotiated;
    if (!backend.open(requested, negotiated)) return 1;
    std::printf("%s: %.0f Hz, %u-frame buffer, %u out\n", backend.name(), negotiated.sampleRate,
                negotiated.bufferSize, negotiated.outputChannels);

    MainLoop                      loop;
    AppHost                       host(loop);
    std::shared_ptr<HostedPlugin> plugin = factory.create(desc, host);
    if (!plugin) return 1;
    printPlugin(*plugin);
    if (!plugin->activate(negotiated.sampleRate, kMaxPluginBlockFrames)) return 1;

    const auto              engine = std::make_unique<Engine>();
    std::vector<SlotConfig> slots(1);
    slots[0].plugin = plugin;
    engine->commit(std::make_unique<EngineGraph>(std::move(slots)));
    backend.setProcessor(engine.get());

    std::unique_ptr<AlsaSeqInput> input = AlsaSeqInput::create();
    if (!input) return 1;
    input->setProtocolPreference(options.preference);
    input->setSink(engine.get());
    openMidiInputs(*input, options);

    if (!backend.start()) return 1;
    if (const std::uint32_t forced = backend.forcedQuantum(); forced != 0)
        std::printf("Forcing a %u-frame quantum while running (applies at activation).\n", forced);

    // The editor opens after audio starts, so it shows the running plugin.
    std::unique_ptr<EditorWindow> editor;
    if (options.editor) editor = EditorWindow::open(*plugin, loop, desc.name + " (StageHand)");
    std::printf("Playing %s. Send MIDI to an input above. Ctrl-C to stop.\n", desc.name.c_str());
    std::fflush(stdout);

    // Housekeeping on the main loop (engine collect, plugin requests, reporting).
    Engine::Stats lastStats;
    std::uint64_t lastXruns       = 0;
    std::uint64_t lastMidiCount   = 0;
    std::uint32_t lastBuffer      = negotiated.bufferSize;
    bool          restartReported = false;
    bool          editorReported  = false;
    loop.addTimer(5, [&] {
        engine->collect();
        plugin->idle();
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
        if (editor && !editor->isOpen() && !editorReported) {
            std::puts("Editor closed; still playing. Ctrl-C to quit.");
            editorReported = true;
        }
        if (backend.serverShutDown()) loop.requestQuit();
        std::fflush(stdout);
    });
    loop.addTimer(500, [&] {   // twice a second: level and MIDI activity
        const std::uint64_t midi = engine->stats().midiReceived;
        const float         peak = engine->takeOutputPeak();
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

    // Shutdown (Engine.h): close the editor while the plugin is fully alive, stop
    // MIDI, then run nothing so the audio thread stops the plugin, then stop
    // audio. The plugin is deactivated and destroyed on this thread when its last
    // reference goes.
    editor.reset();
    input.reset();
    engine->commit(nullptr);
    for (int i = 0; i < 2000 && !engine->committedGraphAdopted() && !backend.serverShutDown(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    backend.stop();
    engine->collect();
    std::puts("Stopped.");
    return 0;
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
