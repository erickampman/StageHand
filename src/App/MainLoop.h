#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>

#include "uw/plugin/HostServices.h"
#include "Core/UwNames.h"

namespace stagehand {

// The app's main-thread event loop: it runs timers and watched file
// descriptors until asked to quit. It serves plugin editors (CLAP
// timer-support, posix-fd-support), the editor windows, and StageHand's own
// housekeeping (engine collect(), plugin idle(), stats). --play runs on it; the
// simpler test modes keep their sleep loops.
//
// Linux (MainLoop.cpp): a poll() loop, the editor windows' X11 connections
// among its fds. macOS (MainLoopMac.mm): AppKit's event loop, the timers as
// CFRunLoopTimers and the fds as CFFileDescriptors in the run loop's common
// modes, so they keep running while a window is resized or a menu is tracked
// (docs/macos.md, "The app").
//
// Everything runs on the thread that calls run(), except requestQuit(), which is
// async-signal-safe so a SIGINT handler can call it.
class MainLoop {
public:
    MainLoop();    // creates the wake-up pipe
    ~MainLoop();

    MainLoop(const MainLoop&)            = delete;
    MainLoop& operator=(const MainLoop&) = delete;

    // A periodic callback. A timer that falls behind is rescheduled from now, not
    // fired back-to-back to catch up.
    TimerId addTimer(std::uint32_t periodMs, std::function<void()> callback);
    void    removeTimer(TimerId id);   // safe from inside any callback

    // Call `callback` with the FdEvent bits that fired whenever `fd` is ready for
    // `events`. Returns false if `fd` is already watched.
    bool watchFd(int fd, std::uint32_t events, std::function<void(std::uint32_t)> callback);
    bool modifyFd(int fd, std::uint32_t events);
    void unwatchFd(int fd);            // safe from inside any callback

    // Run until requestQuit().
    void run();

    // Make run() return soon. Async-signal-safe: an atomic store plus a write()
    // to the wake-up pipe.
    void requestQuit() noexcept;

private:
#ifdef __APPLE__
    struct Timer {
        void*                                  timer = nullptr;   // CFRunLoopTimerRef
        std::shared_ptr<std::function<void()>> callback;
    };
    struct Watch {
        void*                                               descriptor = nullptr;   // CFFileDescriptorRef
        void*                                               source     = nullptr;   // CFRunLoopSourceRef
        std::uint32_t                                       events     = 0;
        std::shared_ptr<std::function<void(std::uint32_t)>> callback;
    };
    void enable(int fd);   // CFFileDescriptor callbacks are one-shot

public:
    void dispatchFd(int fd, std::uint32_t fired);   // from the CFFileDescriptor callback; not for callers

private:
    std::map<TimerId, Timer> timers;
    std::map<int, Watch>     watches;
    TimerId                  nextTimerId = 1;
    int                      wakeRead    = -1;   // self-pipe: requestQuit() from a signal handler
    int                      wakeWrite   = -1;
    std::atomic<bool>        quit{false};
#else
    using Clock = std::chrono::steady_clock;

    // Callbacks are shared so a dispatch keeps its callback alive even if the
    // callback removes its own timer or watch.
    struct Timer {
        std::chrono::milliseconds              period;
        Clock::time_point                      due;
        std::shared_ptr<std::function<void()>> callback;
    };
    struct Watch {
        std::uint32_t                                       events;
        std::shared_ptr<std::function<void(std::uint32_t)>> callback;
    };

    std::map<TimerId, Timer> timers;
    std::map<int, Watch>     watches;
    TimerId                  nextTimerId = 1;
    int                      wakeRead    = -1;   // self-pipe (portable; eventfd is Linux's)
    int                      wakeWrite   = -1;
    std::atomic<bool>        quit{false};
#endif
};

} // namespace stagehand
