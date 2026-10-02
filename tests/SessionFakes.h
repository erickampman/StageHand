#pragma once

// Fakes for tests that run a Session: a plugin factory with an "installed"
// list, a MIDI input that knows one device ("Keys:Keys MIDI 1", handle 42), a
// do-nothing host, and small builders. Shared by the session and controller
// tests.

#include <cstdint>
#include <map>
#include <set>
#include <memory>
#include <string>
#include <vector>

#include "FakePlugin.h"
#include "Session/Session.h"

namespace stagehand::test {

class FakeFactory final : public PluginFactory {
public:
    PluginFormat                  format() const override { return PluginFormat::Clap; }
    std::vector<PluginDescriptor> scan() override {
        ++scans;
        return installed;
    }
    std::unique_ptr<HostedPlugin> create(const PluginDescriptor& desc, HostServices&) override {
        for (const PluginDescriptor& d : installed)
            if (d.id == desc.id && d.path == desc.path) {
                const auto it = dialects.find(d.id);
                auto plugin   = std::make_unique<FakePlugin>(
                    2, it != dialects.end() ? it->second : NoteDialect::Midi | NoteDialect::Midi2, 1.0f, d.id);
                plugin->desc   = d;
                plugin->editor = withEditor.count(d.id) != 0;
                created[d.id]  = plugin.get();
                return plugin;
            }
        return nullptr;
    }

    std::vector<PluginDescriptor>       installed;
    std::map<std::string, FakePlugin*>  created;
    std::map<std::string, NoteDialectMask> dialects;   // by plugin id; default MIDI | MIDI 2.0
    std::set<std::string>               withEditor;   // plugin ids whose instances have an editor
    int                                 scans = 0;
};

class FakeMidiInput final : public MidiInput {
public:
    void                        setProtocolPreference(MidiProtocol) override {}
    void                        setSink(MidiSink*) override {}
    std::vector<MidiSourceInfo> enumerate() override { return sources; }
    MidiSourceHandle            open(const std::string& id) override {
        if (id != "Keys:Keys MIDI 1") return kInvalidMidiSource;
        opened.push_back(id);
        return 42;
    }
    MidiSourceHandle openVirtual(const std::string&, MidiProtocol) override { return kInvalidMidiSource; }
    void             close(MidiSourceHandle) override {}
    const char*      name() const override { return "fake"; }

    std::vector<std::string>    opened;
    std::vector<MidiSourceInfo> sources;   // what enumerate() lists
};

class NullHost final : public HostServices {
public:
    void requestRestart() override {}
    void requestProcess() override {}
    void requestParamFlush() override {}
    void log(LogLevel, const char*) override {}
};

inline PluginDescriptor installedPlugin(const std::string& id, const std::string& path) {
    PluginDescriptor d;
    d.format = PluginFormat::Clap;
    d.id     = id;
    d.name   = id;
    d.path   = path;
    return d;
}

inline RackSlot slot(const std::string& id, const std::string& pluginId, const std::string& pathHint = "") {
    RackSlot s;
    s.id            = id;
    s.name          = id;
    s.plugin.format = PluginFormat::Clap;
    s.plugin.id     = pluginId;
    s.plugin.path   = pathHint;
    return s;
}

// One audio block, so the engine adopts what was committed and runs it.
inline void runBlock(Engine& engine) {
    std::vector<float>   left(64), right(64);
    float*               channels[2] = {left.data(), right.data()};
    AudioCallbackContext ctx;
    ctx.frameCount      = 64;
    ctx.blockStartTime  = 1'000'000'000;
    ctx.time.sampleRate = 48000.0;
    ctx.output          = AudioBuffer{channels, 2, 64};
    engine.process(ctx);
}

} // namespace stagehand::test
