#include "Ui/Monitor.h"

#include <X11/Xlib.h>
#ifdef STAGEHAND_HAVE_XRANDR
#include <X11/extensions/Xrandr.h>
#endif

namespace stagehand::ui {

bool monitorUnderPointer(unsigned& width, unsigned& height) {
    Display* d = XOpenDisplay(nullptr);
    if (d == nullptr) return false;
    const int    screen = DefaultScreen(d);
    const ::Window root = RootWindow(d, screen);
    width  = static_cast<unsigned>(DisplayWidth(d, screen));
    height = static_cast<unsigned>(DisplayHeight(d, screen));
#ifdef STAGEHAND_HAVE_XRANDR
    ::Window     rootReturn = 0, child = 0;
    int          x = 0, y = 0, wx = 0, wy = 0;
    unsigned int mask = 0;
    const bool   pointer = XQueryPointer(d, root, &rootReturn, &child, &x, &y, &wx, &wy, &mask) != 0;
    int          count   = 0;
    if (XRRMonitorInfo* monitors = XRRGetMonitors(d, root, True, &count); monitors != nullptr) {
        int pick = -1;
        for (int i = 0; i < count; ++i) {
            const XRRMonitorInfo& m = monitors[i];
            if (pointer && x >= m.x && x < m.x + m.width && y >= m.y && y < m.y + m.height) pick = i;
            else if (pick < 0 && m.primary) pick = i;   // no pointer on any: the primary
        }
        if (pick < 0 && count > 0) pick = 0;
        if (pick >= 0) {
            width  = static_cast<unsigned>(monitors[pick].width);
            height = static_cast<unsigned>(monitors[pick].height);
        }
        XRRFreeMonitors(monitors);
    }
#endif
    XCloseDisplay(d);
    return true;
}

} // namespace stagehand::ui
