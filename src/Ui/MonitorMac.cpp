#include "Ui/Monitor.h"

#include <CoreGraphics/CoreGraphics.h>

#include <cstddef>
#include <cstdint>

// macOS (docs/macos.md, "The app"): the display under the pointer, through
// CoreGraphics' C API, in pixels as DGL sizes its windows (points times the
// display's scale).

namespace stagehand::ui {

bool monitorUnderPointer(unsigned& width, unsigned& height) {
    CGPoint pointer = CGPointZero;
    if (const CGEventRef event = CGEventCreate(nullptr); event != nullptr) {
        pointer = CGEventGetLocation(event);
        CFRelease(event);
    }
    CGDirectDisplayID display = CGMainDisplayID();
    std::uint32_t     count   = 0;
    if (CGGetDisplaysWithPoint(pointer, 1, &display, &count) != kCGErrorSuccess || count == 0)
        display = CGMainDisplayID();   // no pointer on any: the main display
    const CGRect bounds = CGDisplayBounds(display);   // points
    if (bounds.size.width <= 0.0 || bounds.size.height <= 0.0) return false;
    double scale = 1.0;
    if (const CGDisplayModeRef mode = CGDisplayCopyDisplayMode(display); mode != nullptr) {
        if (const std::size_t points = CGDisplayModeGetWidth(mode); points > 0)
            scale = static_cast<double>(CGDisplayModeGetPixelWidth(mode)) / static_cast<double>(points);
        CGDisplayModeRelease(mode);
    }
    width  = static_cast<unsigned>(bounds.size.width * scale);
    height = static_cast<unsigned>(bounds.size.height * scale);
    return true;
}

} // namespace stagehand::ui
