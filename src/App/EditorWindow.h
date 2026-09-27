#pragma once

#include <memory>
#include <string>

#include "App/MainLoop.h"
#include "Plugin/Editor.h"
#include "Plugin/HostedPlugin.h"

typedef struct _XDisplay Display;

namespace stagehand {

// A top-level X11 window hosting one plugin's editor. On a Wayland session it
// runs through XWayland, as the plugin's own X11 editor does. Main thread only.
// Its X11 connection is serviced by the MainLoop.
//
// Closing the window (the window manager's close button) closes the editor and
// the window only: audio keeps running, since a live rig must not stop because
// an editor was closed. Ctrl-C still quits StageHand.
class EditorWindow final : public EditorHost {
public:
    // Open a window titled `title`, embed the plugin's editor in it, and show it.
    // Returns null (after printing why) if the plugin has no editor or X11 isn't
    // available.
    static std::unique_ptr<EditorWindow> open(HostedPlugin& plugin, MainLoop& loop,
                                              const std::string& title);
    ~EditorWindow() override;   // closes the editor, then destroys the window

    EditorWindow(const EditorWindow&)            = delete;
    EditorWindow& operator=(const EditorWindow&) = delete;

    // False once the user (or the plugin) closed it.
    bool isOpen() const noexcept { return window != 0; }

    // --- EditorHost ---
    bool requestEditorResize(EditorSize size) override;
    bool requestEditorShown(bool shown) override;
    void editorResizeHintsChanged() override;
    void editorClosed() override;

private:
    EditorWindow(HostedPlugin& editorPlugin, MainLoop& mainLoop);

    void handleEvents();          // X11 connection readable
    void close(bool closeEditor);
    void applySizeHints();        // fixed-size vs resizable, from the plugin

    HostedPlugin& plugin;
    MainLoop&     loop;
    Display*      display     = nullptr;
    unsigned long window      = 0;      // X11 Window
    unsigned long deleteAtom  = 0;      // WM_DELETE_WINDOW
    EditorSize    size;
    bool          editorOpen  = false;
    bool          confirmSize = false;  // a plugin-requested resize awaits setEditorSize()
};

} // namespace stagehand
