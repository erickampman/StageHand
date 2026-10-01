#pragma once

// The rack window's widget (internal to stagehand-ui; RackUi is the public
// face): toolbar, rack rows, the sources dock, and the status bar, drawn from
// the controller's RackViewModel. Input becomes controller intents.

#include <cstdint>
#include <functional>
#include <string>

#include "App/RackController.h"
#include "Ui/UiWidget.h"

namespace stagehand {

class RackWidget final : public UiWidget {
public:
    RackWidget(DGL_NAMESPACE::Window& window, RackController& controller);

    // A status-bar message for a few seconds.
    void showNotice(const std::string& text, double now);
    // Opens (or raises) a sequence's editor window; set by RackUi.
    std::function<void(const std::string& sequenceId)> openSequenceEditor;
    // Shows the desktop's file chooser (a folder to open, or a path to save
    // to, with `name` suggested); `chosen` runs only if a path is chosen. Set
    // by RackUi.
    std::function<void(bool save, const std::string& title, const std::string& folder, const std::string& name,
                       std::function<void(const std::string& path)> chosen)>
        chooseFile;

    // New, Open, Save, Save As, Quit: from the File menu and keys, and Quit
    // when the window is closed. New, Open and Quit ask first if an untitled
    // rack has changes.
    enum class FileAction { New, Open, Save, SaveAs, Quit };
    void fileAction(FileAction action);
    // Another rack replaced the running one: close menus and panels.
    void rackReplaced();

protected:
    void draw() override;
    bool overlayOpen() const override { return !filterId.empty(); }
    void drawOverlays() override { drawFilterPanel(); }
    void dismissOverlays() override { filterId.clear(); }
    void wheelUnclaimed(float x, float y, float notches, std::uint32_t mod) override;
    bool onKeyboard(const KeyboardEvent& ev) override;

private:
    // --- geometry (logical pixels)
    float outputX() const;
    float outputW();
    float rowsHeight();
    float rowY(std::size_t row) const;
    float faderX0() const;
    float faderX1();
    float maxScroll();
    float dockHeight();
    float dockTop();
    float dockMaxScroll();

    // --- parts
    void meterBars(float x0, float x1, float y, const float db[2], const float hold[2]);
    void drawToolbar();
    void drawTransport(float x);
    void drawRow(const RowModel& row, std::size_t index);
    void drawRowInput(const RowModel& row, float y);
    void drawRowPlugin(const RowModel& row, float y);
    void drawRowOutput(const RowModel& row, float y);
    void drawStatus();
    void drawDock();
    void drawSequenceRow(const SequenceRowModel& seq, float y);
    void drawFilterPanel();

    // --- actions
    void openSequenceMenu(const SequenceRowModel& seq, float x, float y);
    void startRename(const std::string& id, const std::string& name);
    void openTriggerMenu(const SequenceRowModel& seq, float x, float y);
    void openSourceMenu(const RowModel& row, float x, float y);
    void openRowMenu(const RowModel& row, float x, float y);
    void openAddMenu(float x, float y);
    // The plugin list (scans once). `none`, if not empty, is a first item that
    // calls `chosen` with an empty descriptor (e.g. "No instrument for now").
    void openPluginMenu(float x, float y, const std::string& title,
                        std::function<void(const PluginDescriptor&)> chosen, const std::string& none = {});
    // Where a sequence plays: its rows (ticked = playing into), and New row....
    void openTargetsMenu(const std::string& seqId, float x, float y);
    // "+ Sequence": after the protocol, choose where it plays (a row, a new row,
    // or nowhere yet); with no rows yet, straight to the plugin list. Then name it.
    void newSequence(MidiProtocol protocol, float x, float y);
    void openFileMenu(float x, float y);
    void openTransportMenu(float x, float y);
    void openMetronomeMenu(float x, float y);
    void proceed(FileAction action);
    void saveAsThen(std::function<void()> then);
    std::string startFolder() const;
    const RowModel* rowById(const std::string& id) const;   // the current model's row, at event time
    // Filter edits go through here, so the filter panel shows them before the
    // next model rebuild.
    MidiFilter filterOf(const RowModel& row) const;
    void       applyFilter(const std::string& id, const MidiFilter& filter);
    void       startKeyRangeDrag(const RowModel& row, float barX, const Press& p);

    RackController& controller;

    std::string   filterId;   // the row whose filter panel is open
    MidiFilter    panelFilter;
    float         scrollY    = 0;
    float         dockScroll = 0;   // the dock's sequence rows
    std::string   lastFaderId;      // double-click on a fader
    std::uint32_t lastFaderTime = 0;
    std::uint32_t lastTempoTime = 0;       // double-click on the tempo...
    bool          tempoClicked  = false;   // ...once there's been a first click
    std::string   renamingId;   // the sequence whose name is being typed
    std::string   notice;
    double        noticeUntil = 0;
};

} // namespace stagehand
