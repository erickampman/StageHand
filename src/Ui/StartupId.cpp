#include "Ui/StartupId.h"

#include <algorithm>
#include <cstring>

#include <X11/Xlib.h>
#include <unistd.h>

namespace stagehand::ui {

std::string startupId(std::uint32_t time) {
    Display* const display = XOpenDisplay(nullptr);
    if (display == nullptr) return "";
    static unsigned   count = 0;
    const std::string id    = "stagehand-" + std::to_string(getpid()) + "-" + std::to_string(++count) + "_TIME" +
                           std::to_string(time);
    const std::string text = "new: ID=\"" + id + "\" NAME=\"StageHand\" SCREEN=" + std::to_string(DefaultScreen(display));
    const Window      root = DefaultRootWindow(display);
    XSetWindowAttributes attributes{};
    attributes.override_redirect = True;
    const Window from = XCreateWindow(display, root, -100, -100, 1, 1, 0, CopyFromParent, InputOnly, CopyFromParent,
                                      CWOverrideRedirect, &attributes);
    // In 20-byte pieces, the first as _NET_STARTUP_INFO_BEGIN; the last
    // carries the terminating NUL.
    const Atom begin = XInternAtom(display, "_NET_STARTUP_INFO_BEGIN", False);
    const Atom more  = XInternAtom(display, "_NET_STARTUP_INFO", False);
    for (std::size_t at = 0; at < text.size() + 1; at += 20) {
        XEvent event{};
        event.xclient.type         = ClientMessage;
        event.xclient.window       = from;
        event.xclient.message_type = at == 0 ? begin : more;
        event.xclient.format       = 8;
        std::memcpy(event.xclient.data.b, text.c_str() + at, std::min<std::size_t>(20, text.size() + 1 - at));
        XSendEvent(display, root, False, PropertyChangeMask, &event);
    }
    XDestroyWindow(display, from);
    XCloseDisplay(display);   // flushes
    return id;
}

}   // namespace stagehand::ui
