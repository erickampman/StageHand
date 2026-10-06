#pragma once

namespace stagehand::ui {

// The size, in pixels, of the monitor under the mouse pointer (where a new
// window opens). Linux (Monitor.cpp): through XRandR when StageHand was built
// with it, else the whole X screen (every monitor together). macOS
// (MonitorMac.cpp): the display's, through CoreGraphics. False when there's no
// display.
bool monitorUnderPointer(unsigned& width, unsigned& height);

} // namespace stagehand::ui
