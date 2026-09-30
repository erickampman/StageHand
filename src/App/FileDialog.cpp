#include "App/FileDialog.h"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace stagehand {

namespace {

bool onPath(const std::string& program) {
    const char* const path = std::getenv("PATH");
    if (path == nullptr) return false;
    std::string_view rest = path;
    while (!rest.empty()) {
        const std::size_t      colon = rest.find(':');
        const std::string_view dir   = rest.substr(0, colon);
        if (!dir.empty() && ::access((std::string(dir) + "/" + program).c_str(), X_OK) == 0) return true;
        if (colon == std::string_view::npos) break;
        rest.remove_prefix(colon + 1);
    }
    return false;
}

bool onKde() {
    const char* const desktop = std::getenv("XDG_CURRENT_DESKTOP");
    return desktop != nullptr && std::strstr(desktop, "KDE") != nullptr;
}

} // namespace

FileDialog::~FileDialog() {
    if (child > 0) {
        ::kill(child, SIGTERM);
        ::waitpid(child, nullptr, 0);
    }
    if (output >= 0) ::close(output);
}

std::string FileDialog::tool() {
    const bool zenity = onPath("zenity"), kdialog = onPath("kdialog");
    if (kdialog && (onKde() || !zenity)) return "kdialog";
    return zenity ? "zenity" : "";
}

bool FileDialog::start(Mode mode, const std::string& title, const std::string& folder, const std::string& name,
                       std::string& problem) {
    if (running()) {
        problem = "a file dialog is already open";
        return false;
    }
    const std::string program = tool();
    if (program.empty()) {
        problem = "no file dialog found; install zenity (or kdialog on KDE)";
        return false;
    }

    const std::string start = mode == Mode::Save ? (std::filesystem::path(folder) / name).string() : folder + "/";
    std::vector<std::string> args{program};
    if (program == "zenity") {
        args.push_back("--file-selection");
        args.push_back(mode == Mode::Save ? "--save" : "--directory");
        args.push_back("--title=" + title);
        args.push_back("--filename=" + start);
    } else {
        args.push_back(mode == Mode::Save ? "--getsavefilename" : "--getexistingdirectory");
        args.push_back(start);
        args.push_back("--title");
        args.push_back(title);
    }
    std::vector<char*> argv;
    for (std::string& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);

    int fds[2];
    if (::pipe2(fds, O_CLOEXEC) != 0) {
        problem = std::string("couldn't start ") + program + ": " + std::strerror(errno);
        return false;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);   // GTK chatter
    pid_t     pid    = -1;
    const int result = ::posix_spawnp(&pid, program.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(fds[1]);
    if (result != 0) {
        ::close(fds[0]);
        problem = "couldn't start " + program + ": " + std::strerror(result);
        return false;
    }
    ::fcntl(fds[0], F_SETFL, O_NONBLOCK);
    child  = pid;
    output = fds[0];
    buffer.clear();
    return true;
}

void FileDialog::readOutput() {
    char buf[1024];
    for (;;) {
        const ssize_t n = ::read(output, buf, sizeof buf);
        if (n > 0) {
            buffer.append(buf, static_cast<std::size_t>(n));
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return;   // EAGAIN (nothing yet) or end of file
    }
}

bool FileDialog::poll(std::string& path) {
    if (!running()) return false;
    readOutput();
    int         status = 0;
    const pid_t done   = ::waitpid(child, &status, WNOHANG);
    if (done == 0) return false;   // still open
    readOutput();                  // whatever it wrote last
    ::close(output);
    output = -1;
    child  = -1;

    // Chosen: exit status 0 and the path on one line. Cancelled: anything else.
    path.clear();
    if (done > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        path = buffer.substr(0, buffer.find('\n'));
    }
    buffer.clear();
    return true;
}

} // namespace stagehand
