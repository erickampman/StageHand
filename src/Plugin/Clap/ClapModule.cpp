#include "Plugin/Clap/ClapModule.h"

#include <clap/clap.h>
#include <dlfcn.h>

#include <cstdio>

namespace stagehand {

std::shared_ptr<ClapModule> ClapModule::load(const std::string& path) {
    std::shared_ptr<ClapModule> module(new ClapModule());
    module->modulePath = path;

    module->handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (module->handle == nullptr) {
        std::fprintf(stderr, "CLAP: cannot load %s: %s\n", path.c_str(), dlerror());
        return nullptr;
    }
    module->entry = static_cast<const clap_plugin_entry_t*>(dlsym(module->handle, "clap_entry"));
    if (module->entry == nullptr) {
        std::fprintf(stderr, "CLAP: %s has no clap_entry\n", path.c_str());
        return nullptr;
    }
    if (!clap_version_is_compatible(module->entry->clap_version)) {
        std::fprintf(stderr, "CLAP: %s uses an incompatible CLAP version\n", path.c_str());
        return nullptr;
    }
    if (!module->entry->init(path.c_str())) {
        std::fprintf(stderr, "CLAP: %s failed to initialize\n", path.c_str());
        return nullptr;
    }
    module->initialized   = true;
    module->pluginFactory = static_cast<const clap_plugin_factory_t*>(
        module->entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    if (module->pluginFactory == nullptr) {
        std::fprintf(stderr, "CLAP: %s has no plugin factory\n", path.c_str());
        return nullptr;
    }
    return module;
}

ClapModule::~ClapModule() {
    if (initialized) entry->deinit();
    if (handle != nullptr) dlclose(handle);
}

} // namespace stagehand
