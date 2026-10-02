// FileDialog with a stand-in "zenity" on PATH: the arguments it gets, a chosen
// path, a cancel, and one dialog at a time. No desktop needed.

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include <sys/stat.h>

#include "App/FileDialog.h"
#include "Check.h"

using namespace stagehand;

namespace {

namespace fs = std::filesystem;

// Poll until the dialog finishes (the stand-in exits at once).
std::string wait(FileDialog& dialog) {
    std::string path = "unset";
    for (int i = 0; i < 500 && !dialog.poll(path); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    return path;
}

void testDialog() {
    const fs::path dir = fs::temp_directory_path() / "stagehand-filedialog-test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    // Records its arguments; "cancel" in the title makes it exit 1, as a cancel does.
    {
        std::ofstream script(dir / "zenity");
        script << "#!/bin/sh\n"
                  "printf '%s\\n' \"$@\" > \"${0%/*}/args\"\n"
                  "case \"$*\" in *cancel*) exit 1;; esac\n"
                  "echo /home/me/Racks/Live.rack\n";
    }
    ::chmod((dir / "zenity").c_str(), 0755);
    ::setenv("PATH", dir.c_str(), 1);
    ::unsetenv("XDG_CURRENT_DESKTOP");
    CHECK(FileDialog::tool() == "zenity");

    FileDialog  dialog;
    std::string problem;
    CHECK(dialog.start(FileDialog::Mode::Save, "Save the rack as", "/home/me/Racks", "Gig.rack", problem));
    CHECK(!dialog.start(FileDialog::Mode::Save, "again", "/tmp", "x", problem) && !problem.empty());
    CHECK(wait(dialog) == "/home/me/Racks/Live.rack");
    CHECK(!dialog.running());
    std::ifstream args(dir / "args");
    std::string   all((std::istreambuf_iterator<char>(args)), std::istreambuf_iterator<char>());
    CHECK(all == "--file-selection\n--save\n--title=Save the rack as\n--filename=/home/me/Racks/Gig.rack\n");

    CHECK(dialog.start(FileDialog::Mode::OpenFolder, "Open (cancel)", "/home/me/Racks", "", problem));
    CHECK(wait(dialog).empty());   // cancelled
    std::ifstream args2(dir / "args");
    std::string   open((std::istreambuf_iterator<char>(args2)), std::istreambuf_iterator<char>());
    CHECK(open.find("--directory\n") != std::string::npos && open.find("--filename=/home/me/Racks/\n") != std::string::npos);

    // A file to open, with only matching files shown.
    CHECK(dialog.start(FileDialog::Mode::OpenFile, "Open a setlist", "/home/me/Sets", "*.setlist", problem));
    CHECK(wait(dialog) == "/home/me/Racks/Live.rack");
    std::ifstream args3(dir / "args");
    std::string   file((std::istreambuf_iterator<char>(args3)), std::istreambuf_iterator<char>());
    CHECK(file == "--file-selection\n--title=Open a setlist\n--filename=/home/me/Sets/\n--file-filter=*.setlist\n");

    ::setenv("PATH", "/nonexistent", 1);
    CHECK(FileDialog::tool().empty());
    CHECK(!dialog.start(FileDialog::Mode::Save, "x", "/tmp", "x", problem) && problem.find("zenity") != std::string::npos);
    fs::remove_all(dir);
}

} // namespace

int main() {
    testDialog();
    return test::finish("FileDialog");
}
