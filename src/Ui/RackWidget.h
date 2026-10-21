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
    // The height (logical) that shows the rack without scrolling: its rows
    // (3 to 10), the buses, the dock with up to 4 sequences and loopers, the
    // setlist row when open. For sizing a new window.
    float wantedHeight() const;
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
    // A rack or setlist to open from outside the window (Finder, on macOS): a
    // setlist as Open setlist does, a rack on its own (leaving a setlist), as
    // Open outside a set does; asking first if an untitled rack has changes.
    void openDocument(const std::string& path);
    // Another rack replaced the running one: close menus and panels.
    void rackReplaced();

    // The Perform view (docs/perform.md, decisions 10 to 15): the main window
    // for the stage, the song and the beat large, nothing to edit. Tab or the
    // toolbar's Perform switches; RackUi keeps it with the window's size.
    bool performing() const noexcept { return perform; }
    void setPerforming(bool on);
    // The beat moves on screen (the Perform view, or the edge flash, while
    // the transport runs or counts in): RackUi repaints at the display's rate.
    bool animating() const;

protected:
    void draw() override;
    bool overlayOpen() const override { return !filterId.empty() || linkOpen.has_value(); }
    void drawOverlays() override {
        drawFilterPanel();
        drawLinkPanel();
    }
    void dismissOverlays() override {
        filterId.clear();
        linkOpen.reset();
        linkPending.reset();
    }
    void wheelUnclaimed(float x, float y, float notches, std::uint32_t mod) override;
    bool onKeyboard(const KeyboardEvent& ev) override;

private:
    bool perform = false;   // the Perform view
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
    float rigTop();            // above the dock, or after the last row when they scroll
    // The rows come first: when they and the bus strips don't all fit, the
    // strips scroll with the rows, after the last one, instead of keeping
    // their place above the dock.
    bool  busesScroll();

    // --- parts
    void meterBars(float x0, float x1, float y, const float db[2], const float hold[2]);
    void openSetlistMidiMenu(float x, float y);
    // A horizontal dB fader centred on `cy`: drag (Shift: fine), the wheel
    // (half a dB a notch), double-click for 0 dB. `current` reads the value at
    // event time; `key` tells double-clicks on different faders apart.
    // `right`, if set, takes a right-click (its Control Links menu).
    void dbFader(float x0, float x1, float cy, float db, bool dim, float thumbH, const std::string& key,
                 std::function<std::optional<float>()> current, std::function<void(float)> set,
                 std::function<void(const Press&)> right = {});
    // A row's send to a bus (docs/rig.md): drag up or down, the wheel, double-click on/off.
    void sendBox(const RowModel& row, std::size_t bus, float x, float y);
    void drawRig();
    void drawBusStrip(const BusModel& bus, std::size_t index, float y);
    void openEffectMenu(std::size_t bus, std::size_t slot, float x, float y);
    void openBusOutputMenu(std::size_t bus, float x, float y);
    void drawSetlistRow();
    void drawPerform();                     // the Perform view, the whole window
    void drawEdgeFlash();                   // over either view, on each pulse
    void drawToolbar();
    void drawTransport(float x);
    void drawRow(const RowModel& row, std::size_t index);
    // Each rack row has a colour (by its place: neighbours differ), tinting
    // its background with a stripe at its left; a sequence or looper takes the
    // colour of the rows it plays into, so it's clear where it goes.
    UiColor rowColor(const std::string& rowId) const;   // the neutral row colour if there's no such row
    // The background of a sequence or looper row tinted by its first target,
    // and the left stripe split between all its targets.
    void    sourceTint(const std::vector<std::string>& targetIds, float y, UiColor base);
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

    // --- control links (docs/controls.md, sections 8 to 10)
    // What a Control Links button and menu belong to: a row (its parameters,
    // gain and sends), a bus effect, a bus's return, or the master.
    struct LinkPlace {
        enum class Kind : std::uint8_t { Row, Effect, Return, Master };
        Kind        kind = Kind::Row;
        std::string row;
        int         bus = -1, effect = -1;
    };
    // Does a target (acting on `rows`) belong to `place`?
    static bool linkAt(const ControlLinkTarget& target, const std::vector<std::string>& rows, const LinkPlace& place);
    std::string placeName(const LinkPlace& place) const;   // "Keys", "Bus A effect 1", "Bus A return", "the master"
    // The MIDI icon: lit when links act on `place`, amber if one's input
    // isn't connected, blinking while one is learned there. Opens the menu.
    void linkButton(const LinkPlace& place, float x, float y, float w, float h);
    void openLinksMenu(const LinkPlace& place, float x, float y);
    // A plugin's parameters, filtered as you type; `chosen` gets one's id.
    void openParameterMenu(const std::vector<RackController::ParameterChoice>& params, const std::string& title,
                           float x, float y, bool overPanel, std::function<void(std::uint32_t)> chosen);
    // The Control Link dialog: one link, a panel over the rack. Changes apply at once.
    void openLink(ControlScope scope, std::size_t index);
    void drawLinkPanel();
    void editLink(const ControlLinkSetup& link);   // the dialog's link, as edited
    void openAddTargetMenu(const ControlLinkModel& link, float x, float y);
    void startRangeDrag(const ControlLinkSetup& link, std::size_t target, const ControlLinkTargetModel& shown,
                        float barX0, float barX1, const Press& p);

    // --- actions
    // The red X: Remove / Cancel, right-aligned to `right` (the X's right edge).
    void confirmRemoveSequence(const std::string& id, const std::string& name, float right, float y);
    void startRename(const std::string& id, const std::string& name);
    void openTriggerMenu(const SequenceRowModel& seq, float x, float y);
    void openSourceMenu(const RowModel& row, float x, float y);
    // A row's MIDI effects (docs/midi-effects.md): each one's Editor, Bypass,
    // Move and Remove, then Add a MIDI effect (the note effects).
    void openMidiFxMenu(const RowModel& row, float x, float y);
    void openNoteEffectMenu(const std::string& rowId, float x, float y);
    void removeRow(const std::string& id);   // at once (Shift-click on its X), or once confirmed
    void confirmRemoveRow(const std::string& id, const std::string& name, float right, float y);
    void openOutputMenu(const RowModel& row, float x, float y);
    void openAddMenu(float x, float y, bool instrumentsOnly = false);
    void drawAddArea(float top, float bottom);   // the empty space below the rows
    // The plugin list (scans once). `none`, if not empty, is a first item that
    // calls `chosen` with an empty descriptor (e.g. "No instrument for now").
    void openPluginMenu(float x, float y, const std::string& title,
                        std::function<void(const PluginDescriptor&)> chosen, const std::string& none = {},
                        bool instrumentsOnly = false);
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
    void openHelp();   // help/help.html beside the app, in the default browser
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
    struct OpenLink {
        ControlScope scope = ControlScope::Song;
        std::size_t  index = 0;
    };
    std::optional<OpenLink>         linkOpen;           // the Control Link dialog's
    std::optional<ControlLinkSetup> linkPending;        // its last edit, shown until the model has it
    double                          linkPendingUntil = 0;
    double                          linkMissingSince = -1;   // not in the model (yet: a scope just changed)
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
