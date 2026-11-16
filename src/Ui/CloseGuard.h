#pragma once

#include <cstdint>
#include <functional>

namespace stagehand::ui {

// macOS only (docs/macos.md, milestone 6, "Closing the rack window"): asks
// `shouldClose` before a DGL window's close button closes it. DPF never calls
// Window::onClose() on macOS, since pugl's windowShouldClose: closes the
// NSWindow whatever it would say, so StageHand puts its own delegate in front
// of pugl's: `shouldClose` false keeps the window open; true lets pugl's
// delegate close it as before. Every other delegate message goes to pugl's.
//
// `nativeView` is the window's getNativeWindowHandle() (pugl's NSView), the
// window realized. An empty `shouldClose` takes the question away again (call
// that before the window goes). Main thread.
void guardWindowClose(std::uintptr_t nativeView, std::function<bool()> shouldClose);

} // namespace stagehand::ui
