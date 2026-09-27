#pragma once

#include <cstdint>

namespace stagehand {

// A native parent-window handle to embed a plugin editor in: an X11 Window id
// on Linux (an NSView* on macOS, later). The window system is the build
// platform's. CLAP and VST3 both embed this way.
using NativeWindow = std::uintptr_t;

// Editor size in the window system's pixels (physical pixels on X11).
struct EditorSize {
    std::uint32_t width  = 0;
    std::uint32_t height = 0;
};

// Implemented by whatever hosts one plugin's editor (a host window). It receives
// the plugin's requests about its own editor. Main thread only.
class EditorHost {
public:
    virtual ~EditorHost() = default;

    // The plugin wants its editor resized. Return true if the host will do it;
    // the host then resizes its window and confirms with
    // HostedPlugin::setEditorSize().
    virtual bool requestEditorResize(EditorSize size) = 0;

    // The plugin wants its editor shown or hidden. Return true if honored.
    virtual bool requestEditorShown(bool shown) = 0;

    // The plugin changed whether (or how) the user may resize its editor.
    virtual void editorResizeHintsChanged() = 0;

    // The plugin's editor went away on its own. The host should close its
    // window, and must not call HostedPlugin::closeEditor() for it.
    virtual void editorClosed() = 0;
};

} // namespace stagehand
