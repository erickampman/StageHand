#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <thread>

#include "App/TestTone.h"
#include "Audio/JackBackend.h"

using namespace stagehand;

namespace {

std::atomic<bool> quitRequested{false};

void handleSignal(int) {
    quitRequested.store(true);
}

void printUsage() {
    std::puts(
        "usage: stagehand [options]\n"
        "\n"
        "  --tone [HZ]      play a test sine (default 440 Hz) to prove the audio path\n"
        "  --channels N     output channels to register (default 2)\n"
        "  --quantum N      force a PipeWire buffer size of N frames (4-8192) while\n"
        "                   StageHand runs; released automatically when it exits.\n"
        "                   Affects every app on the same audio device.\n"
        "  --no-connect     don't auto-connect to the system playback ports\n"
        "  --help           show this help\n"
        "\n"
        "Run under PipeWire's JACK layer:  pw-jack stagehand --tone --quantum 128");
}

} // namespace

int main(int argc, char** argv) {
    bool     playTone    = false;
    double   toneHz      = 440.0;
    unsigned channels    = 2;
    unsigned quantum     = 0;      // 0 = leave the buffer size to PipeWire
    bool     autoConnect = true;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            printUsage();
            return 0;
        } else if (arg == "--tone") {
            playTone = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') toneHz = std::atof(argv[++i]);
        } else if (arg == "--channels" && i + 1 < argc) {
            channels = static_cast<unsigned>(std::atoi(argv[++i]));
        } else if (arg == "--quantum" && i + 1 < argc) {
            // PipeWire's documented quantum-floor and quantum-limit defaults.
            const int n = std::atoi(argv[++i]);
            if (n < 4 || n > 8192) {
                std::fprintf(stderr, "--quantum must be between 4 and 8192 frames\n");
                return 2;
            }
            quantum = static_cast<unsigned>(n);
        } else if (arg == "--no-connect") {
            autoConnect = false;
        } else {
            std::fprintf(stderr, "unknown option: %s\n\n", argv[i]);
            printUsage();
            return 2;
        }
    }
    if (!playTone) {   // M1: the test tone is the only mode so far
        printUsage();
        return 0;
    }

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    JackBackend backend("StageHand", autoConnect);
    AudioConfig requested;
    requested.outputChannels = channels;
    requested.bufferSize     = quantum;
    AudioConfig negotiated;
    if (!backend.open(requested, negotiated)) return 1;

    std::printf("%s: %.0f Hz, %u-frame buffer, %u in / %u out\n", backend.name(),
                negotiated.sampleRate, negotiated.bufferSize, negotiated.inputChannels,
                negotiated.outputChannels);
    std::printf("JACK clock - CLOCK_MONOTONIC: %lld ns\n",
                static_cast<long long>(backend.clockOffset()));

    TestTone tone(toneHz, 0.125f);   // ~-18 dBFS
    backend.setProcessor(&tone);
    if (!backend.start()) return 1;
    if (const std::uint32_t forced = backend.forcedQuantum(); forced != 0)
        std::printf("Forcing a %u-frame quantum while running (applies at activation).\n",
                    forced);
    std::printf("Playing %.1f Hz. Ctrl-C to stop.\n", toneHz);

    // M1 main loop: report xruns and buffer-size changes. Replaced by a poll()
    // loop (plugin timers, GUI) in M4.
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
