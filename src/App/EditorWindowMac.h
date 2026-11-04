#pragma once

#include <functional>
#include <memory>
#include <string>

#include "App/MainLoop.h"
#include "uw/plugin/Editor.h"
#include "uw/plugin/HostedPlugin.h"
#include "Core/UwNames.h"

namespace stagehand {

// A window hosting one plugin's editor on macOS (docs/macos.md, decision 11):
// an NSWindow whose content view is the NSView the plugin's editor goes in. The
// window only holds it; it isn't StageHand UI. Same interface as Linux's X11
// EditorWindow (EditorWindow.h), so main.cpp uses either. Main thread only;
// its events come through the MainLoop (AppKit's loop on macOS).
//
// Closing the window (its close button) closes that editor only: audio keeps
// running. Cmd-Q in the window quits StageHand the way Ctrl-C in the terminal
// does (so the rack still saves); Cmd-Shift-S runs the app's save action, if
// one was given (decision 7: Cmd, not Ctrl). Every other key goes to the plugin.
class EditorWindow final : public EditorHost {
public:
    // Open a window titled `title`, embed the plugin's editor in it, and show
    // it. Returns null (after printing why) if the plugin has no Cocoa editor
    // or it fails to open.
    static std::unique_ptr<EditorWindow> open(HostedPlugin& plugin, MainLoop& loop, const std::string& title,
                                              std::function<void()> save = {});
    ~EditorWindow() override;   // closes the editor, then the window

    EditorWindow(const EditorWindow&)            = delete;
    EditorWindow& operator=(const EditorWindow&) = delete;

    // False once the user (or the plugin) closed it.
    bool isOpen() const noexcept { return window != nullptr; }

    // Retitle the window (a MIDI effect bypassed, the row renamed). Nothing once it's closed.
    void setTitle(const std::string& title);

    // --- EditorHost ---
    bool requestEditorResize(EditorSize size) override;
    bool requestEditorShown(bool shown) override;
    void editorResizeHintsChanged() override;
    void editorClosed() override;

    // From the window's delegate and key monitor (EditorWindowMac.mm). Not for
    // other callers.
    void closeButtonPressed();
    void windowResized();
    bool handleKey(const std::string& characters, bool command, bool shift);   // true: consumed

private:
    EditorWindow(HostedPlugin& editorPlugin, MainLoop& mainLoop);

    void close(bool closeEditor);
    void applySizeHints();   // fixed-size vs resizable, from the plugin
    void setContentSize(EditorSize wanted);

    HostedPlugin&         plugin;
    MainLoop&             loop;
    std::function<void()> saveAction;
    void*                 window     = nullptr;   // NSWindow*, retained
    void*                 delegate   = nullptr;   // its delegate, retained
    void*                 keyMonitor = nullptr;   // an NSEvent local monitor
    EditorSize            size;
    bool                  editorOpen  = false;
    bool                  resizingNow = false;   // our own setContentSize, not the user's
    // Cleared when the window goes, so a confirmation queued for later finds it gone.
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
};

} // namespace stagehand
