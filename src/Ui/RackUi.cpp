#include "Ui/RackUi.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "Application.hpp"
#include "Window.hpp"

#include "App/RackController.h"
#include "Ui/RackWidget.h"

namespace stagehand {

namespace dgl = DGL_NAMESPACE;

struct RackUi::Impl {
    Impl() : app(true), window(app) {}

    dgl::Application            app;
    dgl::Window                 window;
    std::unique_ptr<RackWidget> widget;
};

std::unique_ptr<RackUi> RackUi::open(RackController& controller) {
    if (std::getenv("DISPLAY") == nullptr) {
        std::fputs("No display (DISPLAY isn't set); running without the rack window.\n", stderr);
        return nullptr;
    }
    auto impl = std::make_unique<Impl>();
    {
        // The NanoVG context is created in the widget's constructor, so the
        // window's GL context must be current (the spike's lesson).
        const dgl::Window::ScopedGraphicsContext context(impl->window);
        impl->widget = std::make_unique<RackWidget>(impl->window, controller);
    }

    // Logical sizes, as RackWidget lays out: 990 wide, room for 3 to 10 rows.
    const double sf   = impl->window.getScaleFactor();
    const auto   rows = static_cast<double>(std::clamp<std::size_t>(controller.view().rows.size(), 3, 10));
    const auto   px   = [sf](double logical) { return static_cast<unsigned>(std::lround(logical * sf)); };
    const std::string title =
        "StageHand - " + (controller.view().rackName.empty() ? std::string("Untitled rack") : controller.view().rackName);
    impl->window.setTitle(title.c_str());
    impl->window.setResizable(true);
    impl->window.setGeometryConstraints(px(930.0), px(44.0 + 64.0 * 2.0 + 30.0));
    impl->window.setSize(px(990.0), px(44.0 + 64.0 * rows + 30.0));
    impl->window.show();
    return std::unique_ptr<RackUi>(new RackUi(std::move(impl)));
}

RackUi::RackUi(std::unique_ptr<Impl> ui) : impl(std::move(ui)) {}

RackUi::~RackUi() {
    // DGL's teardown order: the widget (its NanoVG context) with the GL context
    // current, then close the window and put the Application in quitting state.
    {
        const dgl::Window::ScopedGraphicsContext context(impl->window);
        impl->widget.reset();
    }
    impl->window.close();
    impl->app.quit();
}

void RackUi::idle() {
    impl->app.idle();
}

void RackUi::refresh() {
    if (impl->window.isVisible()) impl->widget->repaint();
}

void RackUi::showNotice(const std::string& text) {
    impl->widget->showNotice(text,
                             std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool RackUi::closed() const {
    // Closing the last window puts DGL's Application in quitting state. Not
    // visibility: a minimized window isn't visible, and must not quit.
    return impl->app.isQuitting();
}

} // namespace stagehand
