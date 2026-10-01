#include "Ui/RackUi.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <string>

#include "Application.hpp"
#include "Window.hpp"

#include "App/FileDialog.h"
#include "App/RackController.h"
#include "App/SequenceEditor.h"
#include "Ui/RackWidget.h"
#include "Ui/SequenceEditorWidget.h"

namespace stagehand {

namespace dgl = DGL_NAMESPACE;

namespace {

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
    explicit Impl(RackController& rackController) : controller(rackController), app(true), window(app) {}

    struct Editor {
        std::unique_ptr<TrackedWindow>        window;
        std::unique_ptr<SequenceEditor>       editor;
        std::unique_ptr<SequenceEditorWidget> widget;
        std::string                           name;   // in its title
    };

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
    dgl::Application              app;
    TrackedWindow                 window;   // the rack window
    std::unique_ptr<RackWidget>   widget;
    std::map<std::string, Editor> editors;  // by sequence id
    std::uint64_t                 generation = 0;   // the controller's rackGeneration() they belong to
    std::string                   shownName;        // in the title

    // The file chooser, and what to do with its answer.
    FileDialog                                   dialog;
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
    e.window->show();
    editors.emplace(id, std::move(e));
}

std::unique_ptr<RackUi> RackUi::open(RackController& controller) {
    if (std::getenv("DISPLAY") == nullptr) {
        std::fputs("No display (DISPLAY isn't set); running without the rack window.\n", stderr);
        return nullptr;
    }
    auto  impl = std::make_unique<Impl>(controller);
    Impl* raw  = impl.get();
    {
        // The NanoVG context is created in the widget's constructor, so the
        // window's GL context must be current (the spike's lesson).
        const dgl::Window::ScopedGraphicsContext context(impl->window);
        impl->widget = std::make_unique<RackWidget>(impl->window, controller);
    }
    impl->widget->openSequenceEditor = [raw](const std::string& id) { raw->openEditor(id); };
    impl->widget->chooseFile = [raw](bool save, const std::string& title, const std::string& folder,
                                     const std::string& name, std::function<void(const std::string&)> chosen) {
        std::string problem;
        if (!raw->dialog.start(save ? FileDialog::Mode::Save : FileDialog::Mode::OpenFolder, title, folder, name,
                               problem)) {
            raw->widget->showNotice("Can't show a file dialog: " + problem, ui::nowSeconds());
            return;
        }
        raw->chosen = std::move(chosen);
    };
    // Closing the window with an untitled rack's changes asks first (as Ctrl-Q does).
    impl->window.allowClose = [raw] {
        if (!raw->controller.unsavedUntitled()) return true;
        raw->widget->fileAction(RackWidget::FileAction::Quit);
        raw->widget->repaint();
        return false;
    };
    impl->generation = controller.rackGeneration();

    // Logical sizes, as RackWidget lays out: 990 wide, room for 3 to 10 rows,
    // plus the sources dock (its header, inputs row, and up to 3 sequences).
    const double sf   = impl->window.getScaleFactor();
    const auto   rows = static_cast<double>(std::clamp<std::size_t>(controller.view().rows.size(), 3, 10));
    const double dock = 28.0 + 34.0 + 44.0 * static_cast<double>(std::clamp<std::size_t>(controller.view().sequences.size(), 1, 3));
    const auto   px   = [sf](double logical) { return static_cast<unsigned>(std::lround(logical * sf)); };
    impl->setTitle();
    impl->window.setResizable(true);
    impl->window.setGeometryConstraints(px(930.0), px(44.0 + 64.0 * 2.0 + 28.0 + 34.0 + 44.0 + 30.0));
    impl->window.setSize(px(990.0), px(44.0 + 64.0 * rows + dock + 30.0));
    impl->window.show();
    return std::unique_ptr<RackUi>(new RackUi(std::move(impl)));
}

RackUi::RackUi(std::unique_ptr<Impl> ui) : impl(std::move(ui)) {}

RackUi::~RackUi() {
    // Editors first, then the rack window, then put the Application in
    // quitting state (DGL's teardown order).
    for (auto& [id, e] : impl->editors) impl->closeEditor(e);
    impl->editors.clear();
    destroyWith(impl->window, impl->widget);
    impl->app.quit();
}

void RackUi::idle() {
    impl->app.idle();

    // The file chooser's answer.
    if (std::string path; impl->dialog.poll(path)) {
        auto chosen = std::move(impl->chosen);
        impl->chosen = nullptr;
        if (!path.empty() && chosen) chosen(path);
        impl->widget->repaint();
    }

    impl->followRack();

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
    if (impl->window.isVisible()) impl->widget->repaint();
    for (auto& [id, e] : impl->editors) {
        if (const Sequence* seq = impl->controller.sequence(id); seq != nullptr && seq->name != e.name) {
            e.name                  = seq->name;   // renamed in the dock
            const std::string title = e.name + " - sequence (StageHand)";
            e.window->setTitle(title.c_str());
        }
        e.editor->sync();
        if (e.window->isVisible()) e.widget->repaint();
    }
}

void RackUi::showNotice(const std::string& text) {
    impl->widget->showNotice(text,
                             std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool RackUi::closed() const {
    // Closing the rack window quits, even with editor windows open. Not
    // visibility: a minimized window isn't visible, and must not quit.
    return impl->window.closedByUser || impl->app.isQuitting();
}

} // namespace stagehand
