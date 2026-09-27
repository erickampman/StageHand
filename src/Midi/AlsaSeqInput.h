#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "Midi/MidiInput.h"

typedef struct _snd_seq snd_seq_t;

namespace stagehand {

// MidiInput over the ALSA sequencer in UMP mode (docs/architecture.md section 1).
// It uses two sequencer clients, so the kernel never converts between protocols:
//   "<name>"           UMP MIDI 1.0 client: MIDI 1.0 sources connect here and
//                      arrive as MT 0x2 (the kernel wraps legacy events).
//   "<name> MIDI 2.0"  UMP MIDI 2.0 client: MIDI 2.0 sources connect here and
//                      arrive as MT 0x4, untouched.
// open() picks the client from the source's own protocol and the global
// preference. Each opened source gets its own input port on the chosen client,
// and a message's handle comes from the port it arrived on.
//
// Timestamps use CLOCK_MONOTONIC (the same clock as JackBackend's block start
// times), taken as each event is read on the MIDI thread. The thread asks for
// SCHED_FIFO; without real-time permission it runs at normal priority, and the
// timestamps jitter by its wake-up latency.
class AlsaSeqInput final : public MidiInput {
public:
    // Opens both sequencer clients and starts the MIDI thread. Returns null
    // (after printing why) if the ALSA sequencer is unavailable.
    static std::unique_ptr<AlsaSeqInput> create(const std::string& clientName = "StageHand");
    ~AlsaSeqInput() override;

    AlsaSeqInput(const AlsaSeqInput&)            = delete;
    AlsaSeqInput& operator=(const AlsaSeqInput&) = delete;

    void                        setProtocolPreference(MidiProtocol pref) override;
    void                        setSink(MidiSink* midiSink) override;
    std::vector<MidiSourceInfo> enumerate() override;
    MidiSourceHandle            open(const std::string& sourceId) override;
    MidiSourceHandle            openVirtual(const std::string& name,
                                            MidiProtocol protocol) override;
    void                        close(MidiSourceHandle source) override;
    const char*                 name() const override { return "ALSA sequencer"; }

    // --- diagnostics ---
    bool          realtimePriority() const noexcept { return rtPriority; }
    std::uint64_t overrunCount() const noexcept { return overruns.load(std::memory_order_relaxed); }
    // Where a handle's port lives, e.g. "129:0 on 'StageHand MIDI 2.0' (UMP MIDI 2.0)",
    // so other tools (aseqsend, aplaymidi2) can address it. Main thread.
    std::string describe(MidiSourceHandle source) const;

private:
    enum { kMidi1Client = 0, kMidi2Client = 1, kClientCount = 2, kMaxPorts = 256 };

    struct Source {
        MidiSourceInfo info;
        int            client = -1;
        int            port   = -1;
    };
    struct OpenPort {
        int  clientIndex = 0;
        int  port        = -1;
        bool isVirtual   = false;
    };

    AlsaSeqInput() = default;

    std::vector<Source> listSources();
    bool                findSource(const std::string& sourceId, Source& found);
    int                 clientFor(MidiProtocol protocol) const;
    int                 createPort(int clientIndex, const std::string& portName, bool exported);
    MidiSourceHandle    registerPort(int clientIndex, int port, bool isVirtual);
    void                run();
    void                drain(int clientIndex);

    snd_seq_t*  seq[kClientCount]      = {nullptr, nullptr};
    int         clientId[kClientCount] = {-1, -1};
    std::string clientNames[kClientCount];

    // Port -> handle, written on the main thread, read on the MIDI thread.
    std::atomic<MidiSourceHandle> portHandles[kClientCount][kMaxPorts] = {};

    std::map<MidiSourceHandle, OpenPort> openPorts;   // main thread only
    MidiSourceHandle                     nextHandle = 1;
    MidiProtocol                         preference = MidiProtocol::Midi2;

    std::atomic<MidiSink*>     sink{nullptr};
    int                        wakeFd = -1;
    std::thread                thread;
    std::atomic<bool>          quit{false};
    bool                       rtPriority = false;
    std::atomic<std::uint64_t> overruns{0};
};

} // namespace stagehand
