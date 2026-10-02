#pragma once

namespace stagehand::ui {

// The size, in pixels, of the monitor under the mouse pointer (where a new
// window opens), through XRandR when StageHand was built with it, else the
// whole X screen (every monitor together). False when there's no display.
bool monitorUnderPointer(unsigned& width, unsigned& height);

} // namespace stagehand::ui
