#include "Ui/WindowIcon.h"

#include <vector>

#include <X11/Xatom.h>
#include <X11/Xlib.h>

// The PNGs, embedded (CMakeLists.txt).
extern const unsigned char kWindowIcon128[];
extern const unsigned int  kWindowIcon128Size;
extern const unsigned char kWindowIcon48[];
extern const unsigned int  kWindowIcon48Size;

// DGL's stb_image (dgl/src/nanovg/nanovg.c renames it dpf_stbi_*), which the
// rack window links anyway.
extern "C" {
unsigned char* dpf_stbi_load_from_memory(const unsigned char* buffer, int len, int* x, int* y, int* comp, int req_comp);
void           dpf_stbi_image_free(void* data);
}

namespace stagehand::ui {

namespace {

// Appends one icon as _NET_WM_ICON has it: width, height, then the pixels as
// ARGB, each in a long (Xlib's format 32), row by row.
void appendIcon(std::vector<unsigned long>& data, const unsigned char* png, unsigned int size) {
    int            w = 0, h = 0, comp = 0;
    unsigned char* rgba = dpf_stbi_load_from_memory(png, static_cast<int>(size), &w, &h, &comp, 4);
    if (rgba == nullptr) return;
    data.push_back(static_cast<unsigned long>(w));
    data.push_back(static_cast<unsigned long>(h));
    for (int i = 0; i < w * h; ++i) {
        const unsigned char* p = rgba + 4 * i;
        data.push_back(static_cast<unsigned long>(p[3]) << 24 | static_cast<unsigned long>(p[0]) << 16 |
                       static_cast<unsigned long>(p[1]) << 8 | p[2]);
    }
    dpf_stbi_image_free(rgba);
}

}   // namespace

void setWindowIcon(std::uintptr_t window) {
    if (window == 0) return;
    std::vector<unsigned long> data;
    appendIcon(data, kWindowIcon128, kWindowIcon128Size);
    appendIcon(data, kWindowIcon48, kWindowIcon48Size);
    if (data.empty()) return;
    Display* const display = XOpenDisplay(nullptr);
    if (display == nullptr) return;
    XChangeProperty(display, static_cast<Window>(window), XInternAtom(display, "_NET_WM_ICON", False), XA_CARDINAL, 32,
                    PropModeReplace, reinterpret_cast<const unsigned char*>(data.data()), static_cast<int>(data.size()));
    XCloseDisplay(display);   // flushes
}

}   // namespace stagehand::ui
