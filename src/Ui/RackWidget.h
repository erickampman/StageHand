#pragma once

// The rack window's widget (internal to stagehand-ui; RackUi is the public
// face): toolbar, rack rows, the sources dock, and the status bar, drawn from
// the controller's RackViewModel. Input becomes controller intents.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "App/RackController.h"
#include "Ui/UiWidget.h"

namespace stagehand {

class RackWidget final : public UiWidget {
public:
    RackWidget(DGL_NAMESPACE::Window& window, RackController& controller);

    // A status-bar message for a few seconds.
    void showNotice(const std::string& text, double now);
    // What the drawing shows that depends on the time rather than the view
    // model: blink phases (only while something blinks), the notice's
    // visibility. RackUi repaints when this or the model changes.
    std::uint32_t timedLook(double now) const;
    // Opens (or raises) a sequence's editor window; set by RackUi.
    std::function<void(const std::string& sequenceId)> openSequenceEditor;
    // Shows the desktop's file chooser: a rack (a folder) or a setlist (a file)
    // to open, or a path to save to with `name` suggested. `chosen` runs only
    // if a path is chosen. Set by RackUi.
    enum class Choose { Rack, Setlist, Save };
    std::function<void(Choose what, const std::string& title, const std::string& folder, const std::string& name,
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
    float toolbarY() const;   // below the setlist row, when one is open
    float topH() const;       // where the rows start
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
    float rigHeight() const;   // the bus strips, when a rig is loaded
    float rigTop();            // above the dock

    // --- parts
    void meterBars(float x0, float x1, float y, const float db[2], const float hold[2]);
    void openSetlistMidiMenu(float x, float y);
    // A horizontal dB fader centred on `cy`: drag (Shift: fine), the wheel
    // (half a dB a notch), double-click for 0 dB. `current` reads the value at
    // event time; `key` tells double-clicks on different faders apart.
    void dbFader(float x0, float x1, float cy, float db, bool dim, float thumbH, const std::string& key,
                 std::function<std::optional<float>()> current, std::function<void(float)> set);
    // A row's send to a bus (docs/rig.md): drag up or down, the wheel, double-click on/off.
    void sendBox(const RowModel& row, std::size_t bus, float x, float y);
    void drawRig();
    void drawBusStrip(const BusModel& bus, std::size_t index, float y);
    void openEffectMenu(std::size_t bus, std::size_t slot, float x, float y);
    void openBusOutputMenu(std::size_t bus, float x, float y);
    void drawSetlistRow();
    void drawToolbar();
    void drawTransport(float x);
    void drawRow(const RowModel& row, std::size_t index);
    void drawRowInput(const RowModel& row, float y);
    void drawRowPlugin(const RowModel& row, float y);
    void drawRowOutput(const RowModel& row, float y);
    void drawStatus();
    void drawDock();
    void drawSequenceRow(const SequenceRowModel& seq, float y);
    // A looper's row (docs/looper.md), under the sequences: its button (the
    // looper switch's tap), state and length, Undo and Clear, its settings.
    void drawLooperRow(const LooperRowModel& looper, float y);
    void newLooper(float x, float y);                    // + Looper: what it records, where it plays, its name
    void startLooperRename(const std::string& id, const std::string& name);   // the inline text field
    void openLooperTargetsMenu(const std::string& id, float x, float y);
    void openLooperSwitchMenu(float x, float y);
    void drawFilterPanel();

    // --- actions
    // The red X: Remove / Cancel, right-aligned to `right` (the X's right edge).
    void confirmRemoveSequence(const std::string& id, const std::string& name, float right, float y);
    void startRename(const std::string& id, const std::string& name);
    void openTriggerMenu(const SequenceRowModel& seq, float x, float y);
    void openSourceMenu(const RowModel& row, float x, float y);
    void confirmRemoveRow(const std::string& id, const std::string& name, float right, float y);
    void openOutputMenu(const RowModel& row, float x, float y);
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
    // The setlist's songs, to go to one (docs/setlists.md, "The setlist row").
    void openSongMenu(float x, float y);
    // Go to another song: if an untitled rack's changes would be lost, ask
    // first (as New and Open do). `go` switches.
    void switchSong(std::function<void()> go);
    // File > New setlist... (with this rack: an untitled one is saved first).
    void newSetlist(bool withThisRack);
    void confirmRemoveSong(std::size_t index, float x, float y);
    // A missing song: find its rack (it moved), or remove it from the set.
    void openMissingSongMenu(std::size_t index, float x, float y);
    std::string setlistFolder() const;   // beside the open setlist, else where racks are
    std::string setRackFolder() const;   // the setlist's Racks/ folder, if it has one
    // Ask whether to keep an untitled rack's changes, then `then` (unless
    // cancelled). `quitting` words the discard choice for quitting.
    void keepChangesThen(std::function<void()> then, bool quitting);
    void openFileMenu(float x, float y);
    void openSwitchMenu(float x, float y);   // File > Setlist settings...
    void openTransportMenu(float x, float y);
    void openMetronomeMenu(float x, float y);
    void proceed(FileAction action);
    void saveAsThen(std::function<void()> then, const std::string& title = "Save the rack as");
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
    float         songNameX = 0;   // where the setlist row's song name is (for renaming it)
    std::string   notice;
    double        noticeUntil = 0;
};

} // namespace stagehand
