#pragma once

#include <string>

namespace stagehand {

enum class PluginFormat { Clap, Vst3, Auv2 };

// Format-neutral identity of a plugin: enough to list it and to re-instantiate
// the exact plugin later from a saved Rack. `id` is an opaque, format-tagged
// string (a CLAP string id, or a VST3 128-bit TUID rendered as hex).
struct PluginDescriptor {
    PluginFormat format = PluginFormat::Clap;
    std::string  id;
    std::string  name;
    std::string  vendor;
    std::string  version;
    std::string  path;    // file/bundle to load
};

} // namespace stagehand
