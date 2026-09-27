#include "Plugin/Clap/ClapPluginFactory.h"

#include <clap/clap.h>

#include <cstdlib>
#include <filesystem>
#include <sstream>

#include "Plugin/Clap/ClapModule.h"
#include "Plugin/Clap/ClapPlugin.h"

namespace stagehand {

namespace fs = std::filesystem;

ClapPluginFactory::ClapPluginFactory() {
    if (const char* home = std::getenv("HOME")) paths.push_back(std::string(home) + "/.clap");
    paths.emplace_back("/usr/lib/clap");
    if (const char* extra = std::getenv("CLAP_PATH")) {
        std::stringstream list(extra);
        std::string       dir;
        while (std::getline(list, dir, ':'))
            if (!dir.empty()) paths.push_back(dir);
    }
}

std::vector<PluginDescriptor> ClapPluginFactory::scan() {
    std::vector<PluginDescriptor> found;
    for (const std::string& dir : paths) {
        std::error_code ec;
        fs::recursive_directory_iterator it(
            dir, fs::directory_options::follow_directory_symlink | fs::directory_options::skip_permission_denied,
            ec);
        for (const fs::recursive_directory_iterator end; !ec && it != end; it.increment(ec)) {
            std::error_code fileError;
            if (it->path().extension() != ".clap" || !fs::is_regular_file(it->path(), fileError)) continue;
            std::vector<PluginDescriptor> more = scanFile(it->path().string());
            found.insert(found.end(), more.begin(), more.end());
        }
    }
    return found;
}

std::vector<PluginDescriptor> ClapPluginFactory::scanFile(const std::string& path) {
    std::vector<PluginDescriptor> found;
    std::error_code   ec;
    const std::string absolute = fs::absolute(path, ec).string();
    const auto        loaded   = module(absolute);
    if (!loaded) return found;

    const clap_plugin_factory_t* factory = loaded->factory();
    const std::uint32_t          count   = factory->get_plugin_count(factory);
    for (std::uint32_t i = 0; i < count; ++i) {
        const clap_plugin_descriptor_t* d = factory->get_plugin_descriptor(factory, i);
        if (d == nullptr || !clap_version_is_compatible(d->clap_version)) continue;
        PluginDescriptor desc;
        desc.format  = PluginFormat::Clap;
        desc.id      = d->id != nullptr ? d->id : "";
        desc.name    = d->name != nullptr ? d->name : "";
        desc.vendor  = d->vendor != nullptr ? d->vendor : "";
        desc.version = d->version != nullptr ? d->version : "";
        desc.path    = absolute;
        found.push_back(desc);
    }
    return found;
}

std::unique_ptr<HostedPlugin> ClapPluginFactory::create(const PluginDescriptor& desc, HostServices& host) {
    if (desc.format != PluginFormat::Clap) return nullptr;
    std::shared_ptr<ClapModule> loaded = module(desc.path);
    if (!loaded) return nullptr;
    return ClapPlugin::create(std::move(loaded), desc, host);
}

std::shared_ptr<ClapModule> ClapPluginFactory::module(const std::string& path) {
    if (std::shared_ptr<ClapModule> existing = modules[path].lock()) return existing;
    std::shared_ptr<ClapModule> loaded = ClapModule::load(path);
    modules[path] = loaded;
    return loaded;
}

} // namespace stagehand
