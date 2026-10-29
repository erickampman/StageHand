#include "Ui/RackUi.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "Application.hpp"
#include "Window.hpp"

#include "App/FileDialog.h"
#include "App/ParameterEditor.h"
#include "App/RackController.h"
#include "App/SequenceEditor.h"
#include "Session/WindowState.h"
#include "Ui/Monitor.h"
#include "Ui/ParameterEditorWidget.h"
#ifdef __APPLE__
#include "Ui/FileChooser.h"
#endif
#include "Ui/RackWidget.h"
#include "Ui/SequenceEditorWidget.h"
#ifndef __APPLE__
#include "Ui/WindowIcon.h"
#endif

namespace stagehand {

namespace dgl = DGL_NAMESPACE;

namespace {

// DGL's Application, its windows' X11 class StageHand (WM_CLASS; DGL's own is
// its namespace), as the .desktop file's StartupWMClass has it
// (tools/install-desktop-entry.sh). Set before any window is created: DGL
// realizes a window in its constructor.
class StageHandApplication final : public dgl::Application {
public:
    StageHandApplication() : Application(true) {
#ifndef __APPLE__
        setClassName("StageHand");
#endif
    }
};

// Linux: the window's icon in the dock and the window switcher.
void setIcon([[maybe_unused]] const dgl::Window& window) {
#ifndef __APPLE__
    ui::setWindowIcon(window.getNativeWindowHandle());
#endif
}

// A window that remembers being closed by the user. DGL's Application counts
// open windows, so with an editor still open, closing the rack window alone
// wouldn't make it "quitting": the flag is what RackUi::closed() checks.
// `allowClose`, if set, can keep the window open (and ask something instead).
class TrackedWindow final : public dgl::Window {
public:
    explicit TrackedWindow(dgl::Application& app) : Window(app) {}
    bool                  closedByUser = false;
    std::function<bool()> allowClose;

protected:
    bool onClose() override {
        if (allowClose && !allowClose()) return false;
        closedByUser = true;
        return true;
    }
};

// A widget is created and destroyed with its window's GL context current (the
// NanoVG context lives in it: the spike's lesson), then the window closes.
template <typename Widget>
void destroyWith(dgl::Window& window, std::unique_ptr<Widget>& widget) {
    {
        const dgl::Window::ScopedGraphicsContext context(window);
        widget.reset();
    }
    window.close();
}

} // namespace

struct RackUi::Impl {
    explicit Impl(RackController& rackController) : controller(rackController), window(app) {}

    struct Editor {
        std::unique_ptr<TrackedWindow>        window;
        std::unique_ptr<SequenceEditor>       editor;
        std::unique_ptr<SequenceEditorWidget> widget;
        std::string                           name;   // in its title
    };

    // Generic editors (docs/generic-editor.md), by a number their handles keep.
    struct Generic {
        std::unique_ptr<TrackedWindow>         window;
        std::unique_ptr<ParameterEditor>       editor;
        std::unique_ptr<ParameterEditorWidget> widget;
        std::uint64_t                          drawn = ~std::uint64_t{0};   // the editor's version last drawn
    };
    // A handle closed: hidden now, torn down in idle(), outside DGL's event
    // handling (a click in the rack window can close one).
    void closeGeneric(int key) {
        const auto it = generic.find(key);
        if (it == generic.end()) return;
        it->second.window->hide();
        closingGeneric.push_back(std::move(it->second));
        generic.erase(it);
    }
    void destroyGeneric(Generic& g) { destroyWith(*g.window, g.widget); }

    void openEditor(const std::string& id);
    void closeEditor(Editor& e) {
        destroyWith(*e.window, e.widget);
        e.window.reset();
        e.editor.reset();
    }

    // Another rack replaced this one: its sequence editors go (a sequence id
    // may name a different sequence now), and so do menus and panels.
    void followRack() {
        if (const std::uint64_t now = controller.rackGeneration(); now != generation) {
            generation = now;
            for (auto& [id, e] : editors) closeEditor(e);
            editors.clear();
            widget->rackReplaced();
        }
        if (controller.view().rackName != shownName) setTitle();
    }

    void setTitle() {
        const std::string& name = controller.view().rackName;
        shownName               = name;
        const std::string title = "StageHand - " + (name.empty() ? std::string("Untitled rack") : name);
        window.setTitle(title.c_str());
    }

    RackController&               controller;
    StageHandApplication          app;
    TrackedWindow                 window;   // the rack window
    std::unique_ptr<RackWidget>   widget;
    std::map<std::string, Editor> editors;  // by sequence id
    std::map<int, Generic>        generic;
    std::vector<Generic>          closingGeneric;
    int                           nextGeneric = 0;
    std::shared_ptr<int>          alive = std::make_shared<int>(0);   // the handles' sign that this is still here
    std::uint64_t                 generation = 0;   // the controller's rackGeneration() they belong to
    std::string                   shownName;        // in the title
    // What the rack window last drew, so refresh() repaints only on a change.
    RackViewModel                 drawnView;
    std::uint32_t                 drawnLook = 0;
    bool                          drawnValid = false;

    std::string                   windowStatePath;   // where its size is kept; empty: not kept

    // The file chooser, and what to do with its answer: DPF's on macOS (the
    // system's panels), zenity or kdialog on Linux (docs/macos.md, decision 10).
#ifdef __APPLE__
    FileChooser                                  dialog;
#else
    FileDialog                                   dialog;
#endif
    std::function<void(const std::string& path)> chosen;
};

// One window per sequence; opening an open one brings it forward.
void RackUi::Impl::openEditor(const std::string& id) {
    const Sequence* seq = controller.sequence(id);
    if (seq == nullptr) return;
    if (const auto it = editors.find(id); it != editors.end()) {
        it->second.window->focus();
        return;
    }
    Editor e;
    e.window = std::make_unique<TrackedWindow>(app);
    e.editor = std::make_unique<SequenceEditor>(controller, id);
    {
        const dgl::Window::ScopedGraphicsContext context(*e.window);
        e.widget = std::make_unique<SequenceEditorWidget>(*e.window, controller, *e.editor);
    }
    const double      sf    = e.window->getScaleFactor();
    const auto        px    = [sf](double logical) { return static_cast<unsigned>(std::lround(logical * sf)); };
    const std::string title = seq->name + " - sequence (StageHand)";
    e.window->setTitle(title.c_str());
    e.name = seq->name;
    e.window->setResizable(true);
    e.window->setGeometryConstraints(px(760.0), px(640.0));
    e.window->setSize(px(1040.0), px(660.0));
    setIcon(*e.window);
    e.window->show();
    editors.emplace(id, std::move(e));
}

std::unique_ptr<RackUi> RackUi::open(RackController& controller, const std::string& windowStatePath) {
#ifndef __APPLE__
    if (std::getenv("DISPLAY") == nullptr) {
        std::fputs("No display (DISPLAY isn't set); running without the rack window.\n", stderr);
        return nullptr;
    }
#endif
    auto  impl = std::make_unique<Impl>(controller);
    Impl* raw  = impl.get();
    {
        // The NanoVG context is created in the widget's constructor, so the
        // window's GL context must be current (the spike's lesson).
        const dgl::Window::ScopedGraphicsContext context(impl->window);
        impl->widget = std::make_unique<RackWidget>(impl->window, controller);
    }
    impl->widget->openSequenceEditor = [raw](const std::string& id) { raw->openEditor(id); };
    impl->widget->chooseFile = [raw](RackWidget::Choose what, const std::string& title, const std::string& folder,
                                     const std::string& name, std::function<void(const std::string&)> chosen) {
        std::string problem;
#ifdef __APPLE__
        // A set (and a rack) is a package, a file to the open panel: a set is
        // chosen as a file with no filter, so an older setlist file or a rack
        // can be opened too (docs/documents.md, decision 10). A rack to import:
        // the rack type, or a plain folder.
        const FileDialog::Mode mode = what == RackWidget::Choose::Save  ? FileDialog::Mode::Save
                                      : what == RackWidget::Choose::Set ? FileDialog::Mode::OpenFile
                                                                        : FileDialog::Mode::OpenFolder;
        const std::string pattern = what == RackWidget::Choose::Rack ? std::string("*.rack")
                                    : what == RackWidget::Choose::Save ? name
                                                                       : std::string{};
        const bool started = raw->dialog.start(mode, title, folder, pattern, raw->window.getNativeWindowHandle(), problem);
#else
        const FileDialog::Mode mode = what == RackWidget::Choose::Save ? FileDialog::Mode::Save : FileDialog::Mode::OpenFolder;
        const bool started = raw->dialog.start(mode, title, folder, what == RackWidget::Choose::Save ? name : std::string{},
                                               problem);
#endif
        if (!started) {
            raw->widget->showNotice("Can't show a file dialog: " + problem, ui::nowSeconds());
            return;
        }
        raw->chosen = std::move(chosen);
    };
    // Closing the window with unsaved changes asks first (as Ctrl-Q does).
    impl->window.allowClose = [raw] {
        if (!raw->controller.unsavedChanges()) return true;
        raw->widget->fileAction(RackWidget::FileAction::Quit);
        raw->widget->repaint();
        return false;
    };
    impl->generation = controller.rackGeneration();

    // Logical sizes: the last run's, else 1100 wide and tall enough for the
    // rack (RackWidget::wantedHeight()); within the monitor it opens on.
    const double sf      = impl->window.getScaleFactor();
    const auto   px      = [sf](double logical) { return static_cast<unsigned>(std::lround(logical * sf)); };
    const WindowSize minimum{930.0, 44.0 + 64.0 * 2.0 + 28.0 + 34.0 + 44.0 + 30.0};
    WindowSize       wanted{1100.0, impl->widget->wantedHeight()};
    impl->windowStatePath = windowStatePath;
    if (!windowStatePath.empty()) loadWindowSize(windowStatePath, wanted);
    WindowSize monitor;
    if (unsigned w = 0, h = 0; ui::monitorUnderPointer(w, h)) monitor = WindowSize{w / sf, h / sf};
    const WindowSize size = fitWindowSize(wanted, minimum, monitor);
    impl->setTitle();
    impl->window.setResizable(true);
    impl->window.setGeometryConstraints(px(minimum.width), px(minimum.height));
    impl->window.setSize(px(size.width), px(size.height));
    impl->widget->setPerforming(size.perform);   // as it was left (docs/perform.md, decision 10)
    setIcon(impl->window);
    impl->window.show();
    return std::unique_ptr<RackUi>(new RackUi(std::move(impl)));
}

RackUi::RackUi(std::unique_ptr<Impl> ui) : impl(std::move(ui)) {}

RackUi::~RackUi() {
    if (!impl->windowStatePath.empty()) {
        const double sf = impl->window.getScaleFactor();
        saveWindowSize(impl->windowStatePath, WindowSize{impl->window.getWidth() / sf, impl->window.getHeight() / sf,
                                                         impl->widget->performing()});
    }
    // Editors first, then the rack window, then put the Application in
    // quitting state (DGL's teardown order).
    for (auto& [id, e] : impl->editors) impl->closeEditor(e);
    impl->editors.clear();
    impl->alive.reset();   // the generic editors' handles report closed from here on
    for (auto& [key, g] : impl->generic) impl->destroyGeneric(g);
    for (Impl::Generic& g : impl->closingGeneric) impl->destroyGeneric(g);
    impl->generic.clear();
    impl->closingGeneric.clear();
    destroyWith(impl->window, impl->widget);
    impl->app.quit();
}

void RackUi::idle() {
    impl->app.idle();
    // The beat moving (the Perform view, the edge flash): drawn at this rate,
    // not only when the view model changes (docs/perform.md, decision 14).
    if (impl->window.isVisible() && impl->widget->animating()) impl->widget->repaint();

    // The file chooser's answer.
    if (std::string path; impl->dialog.poll(path)) {
        auto chosen = std::move(impl->chosen);
        impl->chosen = nullptr;
        if (!path.empty() && chosen) chosen(path);
        impl->widget->repaint();
    }

    impl->followRack();

    for (Impl::Generic& g : impl->closingGeneric) impl->destroyGeneric(g);
    impl->closingGeneric.clear();

    // Editors the user closed, or whose sequence is gone, are torn down here,
    // outside DGL's event handling.
    for (auto it = impl->editors.begin(); it != impl->editors.end();) {
        if (it->second.window->closedByUser || impl->controller.sequence(it->first) == nullptr) {
            impl->closeEditor(it->second);
            it = impl->editors.erase(it);
        } else {
            ++it;
        }
    }
}

void RackUi::refresh() {
    impl->followRack();
    // Only when something visible changed: every swap may wait for vsync, on
    // the thread that also runs the plugin editors.
    if (impl->window.isVisible()) {
        const RackViewModel& view = impl->controller.view();
        const std::uint32_t  look = impl->widget->timedLook(ui::nowSeconds());
        if (!impl->drawnValid || look != impl->drawnLook || !(view == impl->drawnView)) {
            impl->drawnView  = view;
            impl->drawnLook  = look;
            impl->drawnValid = true;
            impl->widget->repaint();
        }
    }
    for (auto& [id, e] : impl->editors) {
        if (const Sequence* seq = impl->controller.sequence(id); seq != nullptr && seq->name != e.name) {
            e.name                  = seq->name;   // renamed in the dock
            const std::string title = e.name + " - sequence (StageHand)";
            e.window->setTitle(title.c_str());
        }
        e.editor->sync();
        if (e.window->isVisible()) e.widget->repaint();
    }
    for (auto& [key, g] : impl->generic) {   // the plugins' own changes, a control link's
        g.editor->sync();
        if (g.window->isVisible() && g.editor->version() != g.drawn) {
            g.drawn = g.editor->version();
            g.widget->repaint();
        }
    }
}

std::unique_ptr<EditorHandle> RackUi::openParameterEditor(HostedPlugin& plugin, const std::string& title) {
    struct Handle final : EditorHandle {
        std::weak_ptr<int> alive;
        Impl*              impl = nullptr;
        int                key  = 0;
        bool isOpen() const override {
            if (alive.expired()) return false;
            const auto it = impl->generic.find(key);
            return it != impl->generic.end() && !it->second.window->closedByUser;
        }
        ~Handle() override {
            if (!alive.expired()) impl->closeGeneric(key);
        }
    };
    Impl::Generic g;
    g.window = std::make_unique<TrackedWindow>(impl->app);
    g.editor = std::make_unique<ParameterEditor>(impl->controller, plugin);
    {
        const dgl::Window::ScopedGraphicsContext context(*g.window);
        g.widget = std::make_unique<ParameterEditorWidget>(*g.window, *g.editor, plugin.descriptor().name);
    }
    const double sf = g.window->getScaleFactor();
    const auto   px = [sf](double logical) { return static_cast<unsigned>(std::lround(logical * sf)); };
    g.window->setTitle(title.c_str());
    g.window->setResizable(true);
    // Every row if it can, and room for a menu of values (up to kMenuSteps) below a short list.
    g.window->setGeometryConstraints(px(420.0), px(160.0));
    g.window->setSize(px(560.0), px(std::clamp(static_cast<double>(g.widget->wantedHeight()), 340.0, 720.0)));
    setIcon(*g.window);
    g.window->show();

    auto handle   = std::make_unique<Handle>();
    handle->alive = impl->alive;
    handle->impl  = impl.get();
    handle->key   = impl->nextGeneric++;
    impl->generic.emplace(handle->key, std::move(g));
    return handle;
}

void RackUi::showNotice(const std::string& text) {
    impl->widget->showNotice(text,
                             std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

void RackUi::quitAsInWindow() {
    if (impl->controller.unsavedChanges()) impl->window.focus();   // where it asks
    impl->widget->fileAction(RackWidget::FileAction::Quit);
    impl->widget->repaint();
}

void RackUi::openDocument(const std::string& path) {
    impl->window.focus();
    impl->widget->openDocument(path);
    impl->widget->repaint();
}

bool RackUi::closed() const {
    // Closing the rack window quits, even with editor windows open. Not
    // visibility: a minimized window isn't visible, and must not quit.
    return impl->window.closedByUser || impl->app.isQuitting();
}

} // namespace stagehand
