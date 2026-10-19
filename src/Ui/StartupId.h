#pragma once

#include <cstdint>
#include <string>

namespace stagehand::ui {

// Linux: announces an X11 startup sequence (the startup-notification spec's
// "new:" message) whose ID ends _TIME<time>, `time` being a click's X server
// time, and returns the ID ("" without an X display). Given to a launched
// program as XDG_ACTIVATION_TOKEN and DESKTOP_STARTUP_ID, it lets that
// program's window come to the front: GNOME on Wayland raises nothing without
// a token, StageHand is an X11 client there, and Mutter takes X11 startup IDs
// as tokens. Kept apart from DGL, whose Window Xlib's would clash with.
std::string startupId(std::uint32_t time);

}   // namespace stagehand::ui
