#pragma once

#include <cstdint>

namespace stagehand::ui {

// Linux: gives an X11 window StageHand's icon (_NET_WM_ICON, 128 and 48 px,
// from resources/linux/icons), which GNOME's dock and the window switcher show
// when no installed .desktop file matches the window's WM_CLASS
// (tools/install-desktop-entry.sh installs one). `window` is DGL's
// getNativeWindowHandle(). Kept apart from DGL, whose Window Xlib's would clash with.
void setWindowIcon(std::uintptr_t window);

}   // namespace stagehand::ui
