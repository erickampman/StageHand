#pragma once

#include <cstdint>
#include <string>

#include "App/FileDialog.h"

namespace stagehand {

// DPF's file browser (docs/macos.md, decision 10) with FileDialog's shape:
// start() it, then poll() from the loop until it reports a result. On macOS
// it's the system's open and save panels, as a sheet on the window given; the
// vendored DPF is patched for choosing a folder (a rack), the extension filter
// and the suggested name (external/dpf/README.StageHand.md). macOS only so
// far; Linux keeps FileDialog until DPF's portal path does the same. Main
// thread.
class FileChooser {
public:
    using Mode = FileDialog::Mode;

    FileChooser() = default;
    ~FileChooser();   // a chooser still open is closed

    FileChooser(const FileChooser&)            = delete;
    FileChooser& operator=(const FileChooser&) = delete;

    // As FileDialog::start(), the sheet on `window` (DGL's native handle:
    // the window's NSView). Opening, `name` is a pattern, "*.setlist" or
    // "*.rack": only its extension is used (a rack, a package once StageHand.app
    // has declared it, is chosen as a folder or as one).
    bool start(Mode mode, const std::string& title, const std::string& folder, const std::string& name,
               std::uintptr_t window, std::string& problem);
    bool running() const noexcept { return handle != nullptr; }

    // Once the chooser closes: true, with `path` set (empty if cancelled).
    bool poll(std::string& path);

private:
    void* handle = nullptr;   // DPF's FileBrowserHandle
};

} // namespace stagehand
