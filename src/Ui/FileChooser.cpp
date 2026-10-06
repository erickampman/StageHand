#include "Ui/FileChooser.h"

#include "FileBrowserDialog.hpp"

namespace stagehand {

namespace dgl = DGL_NAMESPACE;

namespace {
dgl::FileBrowserHandle browser(void* handle) { return static_cast<dgl::FileBrowserHandle>(handle); }
} // namespace

FileChooser::~FileChooser() {
    if (handle != nullptr) dgl::fileBrowserClose(browser(handle));
}

bool FileChooser::start(Mode mode, const std::string& title, const std::string& folder, const std::string& name,
                        std::uintptr_t window, std::string& problem) {
    if (handle != nullptr) {
        problem = "one is already open";
        return false;
    }
    if (window == 0) {
        problem = "the window isn't open";
        return false;
    }
    std::string extension;
    if (mode == Mode::OpenFile && name.rfind("*.", 0) == 0) extension = name.substr(2);

    dgl::FileBrowserOptions options;
    options.saving      = mode == Mode::Save;
    options.directories = mode == Mode::OpenFolder;
    options.defaultName = mode == Mode::Save && !name.empty() ? name.c_str() : nullptr;
    options.extension   = extension.empty() ? nullptr : extension.c_str();
    options.startDir    = folder.empty() ? nullptr : folder.c_str();
    options.title       = title.c_str();
    handle              = dgl::fileBrowserCreate(false, window, 1.0, options);
    if (handle == nullptr) {
        problem = "the system's file browser didn't open";
        return false;
    }
    return true;
}

bool FileChooser::poll(std::string& path) {
    if (handle == nullptr || !dgl::fileBrowserIdle(browser(handle))) return false;
    const char* const chosen = dgl::fileBrowserGetPath(browser(handle));
    path                     = chosen != nullptr ? chosen : "";
    dgl::fileBrowserClose(browser(handle));
    handle = nullptr;
    return true;
}

} // namespace stagehand
