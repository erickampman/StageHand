#include "Session/AppPaths.h"

#include <cstdlib>
#include <filesystem>

#include "Session/BundleFiles.h"

namespace stagehand {

namespace fs = std::filesystem;
using namespace bundle;

namespace {

#ifdef __APPLE__
std::string applicationSupport() {
    return (fs::path(homeDir()) / "Library" / "Application Support" / "StageHand").string();
}
#else
// $variable if it's set and not empty, else ~/`fallback`; then "stagehand".
std::string xdg(const char* variable, const char* fallback) {
    const char*    set  = std::getenv(variable);
    const fs::path base = set != nullptr && *set != '\0' ? fs::path(set) : fs::path(homeDir()) / fallback;
    return (base / "stagehand").string();
}
#endif

} // namespace

std::string appDataDir() {
#ifdef __APPLE__
    return applicationSupport();
#else
    return xdg("XDG_DATA_HOME", ".local/share");
#endif
}

std::string appStateDir() {
#ifdef __APPLE__
    return applicationSupport();
#else
    return xdg("XDG_STATE_HOME", ".local/state");
#endif
}

} // namespace stagehand
