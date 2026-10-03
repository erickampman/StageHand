#pragma once

#include <string>

namespace stagehand {

// The rack window's size, kept between runs (this machine's, not a rack's or
// the rig's), in logical pixels (before the display's scale factor), and
// whether it was in the Perform view (docs/perform.md, decision 10).
struct WindowSize {
    double width   = 0.0;
    double height  = 0.0;
    bool   perform = false;

    bool operator==(const WindowSize&) const = default;
};

// window.json in appStateDir() (Session/AppPaths.h).
std::string defaultWindowStatePath();
bool        loadWindowSize(const std::string& path, WindowSize& size);   // false: none, or unreadable
bool        saveWindowSize(const std::string& path, const WindowSize& size);

// What to open at: `wanted`, no bigger than 92% of the monitor it opens on
// (when known: a dimension of 0 isn't), and no smaller than `minimum`.
WindowSize fitWindowSize(WindowSize wanted, WindowSize minimum, WindowSize monitor);

} // namespace stagehand
