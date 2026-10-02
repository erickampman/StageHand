#pragma once

#include <memory>
#include <string>

namespace stagehand {

class RackController;

// StageHand's main window: the rack view (docs/rack-view.md, section 6). One
// row per plugin slot, input | plugin | output, plus a toolbar and a status
// bar. It draws the controller's RackViewModel and turns mouse and keyboard
// input into controller intents; it holds no rack state of its own.
//
// DGL (DPF: pugl + NanoVG over OpenGL) stays behind this class, so nothing
// else includes DGL. Main thread only; driven by the app's MainLoop through
// idle(), like the spike (no event loop of its own).
class RackUi {
public:
    // Opens and shows the window. Null (after printing why) if there's no
    // display to open it on.
    // It opens at the size kept in `windowStatePath` (the last run's, if any;
    // empty: none) or one that fits the rack, within the monitor it opens on;
    // its size is kept there as it closes.
    static std::unique_ptr<RackUi> open(RackController& controller, const std::string& windowStatePath = {});
    ~RackUi();   // keeps its size, closes the window; tears DGL down in the order it requires

    RackUi(const RackUi&)            = delete;
    RackUi& operator=(const RackUi&) = delete;

    // Process window events. Call every ~16 ms.
    void idle();
    // Redraw from the controller's current view model. Call after each tick.
    void refresh();
    // Show a message in the status bar for a few seconds ("Saved ...").
    void showNotice(const std::string& text);
    // True once the user closed the window: time to quit.
    bool closed() const;

    struct Impl;

private:
    explicit RackUi(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl;
};

} // namespace stagehand
