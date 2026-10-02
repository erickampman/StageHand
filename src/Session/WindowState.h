#pragma once

#include <string>

namespace stagehand {

// The rack window's size, kept between runs (this machine's, not a rack's or
// the rig's), in logical pixels (before the display's scale factor).
struct WindowSize {
    double width  = 0.0;
    double height = 0.0;

    bool operator==(const WindowSize&) const = default;
};

// $XDG_STATE_HOME/stagehand/window.json (default ~/.local/state/...).
std::string defaultWindowStatePath();
bool        loadWindowSize(const std::string& path, WindowSize& size);   // false: none, or unreadable
bool        saveWindowSize(const std::string& path, const WindowSize& size);

// What to open at: `wanted`, no bigger than 92% of the monitor it opens on
// (when known: a dimension of 0 isn't), and no smaller than `minimum`.
WindowSize fitWindowSize(WindowSize wanted, WindowSize minimum, WindowSize monitor);

} // namespace stagehand
