#pragma once

#include <string>
#include <sys/types.h>

namespace stagehand {

// The desktop's own file chooser, run as a separate process (zenity, or
// kdialog on KDE), so it never blocks the main loop: start() it, then poll()
// from the loop until it reports a result. DGL has no text entry, and DPF's
// built-in browser can't choose a folder (a rack is one) or suggest a name
// when saving; these tools do both. Main thread.
class FileDialog {
public:
    enum class Mode {
        OpenFolder,   // choose an existing folder (a .rack bundle)
        Save,         // choose a new path; the tool doesn't ask about replacing
    };

    FileDialog() = default;
    ~FileDialog();   // a dialog still open is closed

    FileDialog(const FileDialog&)            = delete;
    FileDialog& operator=(const FileDialog&) = delete;

    // Which tool would run ("zenity", "kdialog"), or "" if neither is installed.
    static std::string tool();

    // Show the chooser, starting in `folder` (Save: with `name` filled in).
    // False, with `problem` set, if one is already open or no tool is found.
    bool start(Mode mode, const std::string& title, const std::string& folder, const std::string& name,
               std::string& problem);
    bool running() const noexcept { return child > 0; }

    // Once the chooser closes: true, with `path` set (empty if cancelled).
    bool poll(std::string& path);

private:
    void readOutput();

    pid_t       child  = -1;
    int         output = -1;   // the chooser's stdout
    std::string buffer;
};

} // namespace stagehand
