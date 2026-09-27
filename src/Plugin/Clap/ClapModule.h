#pragma once

#include <memory>
#include <string>

struct clap_plugin_entry;
struct clap_plugin_factory;

namespace stagehand {

// One loaded .clap file: dlopen'd, its clap_entry initialized once, and shared
// (via shared_ptr) by every plugin instance created from it. The last user's
// release runs clap_entry.deinit() and dlclose().
class ClapModule {
public:
    // Load and initialize. Returns null (after printing why) on failure.
    static std::shared_ptr<ClapModule> load(const std::string& path);
    ~ClapModule();

    ClapModule(const ClapModule&)            = delete;
    ClapModule& operator=(const ClapModule&) = delete;

    const clap_plugin_factory* factory() const noexcept { return pluginFactory; }
    const std::string&         path() const noexcept { return modulePath; }

private:
    ClapModule() = default;

    std::string                modulePath;
    void*                      handle        = nullptr;
    const clap_plugin_entry*   entry         = nullptr;
    const clap_plugin_factory* pluginFactory = nullptr;
    bool                       initialized   = false;
};

} // namespace stagehand
