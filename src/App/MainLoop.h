#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>

#include "Plugin/HostServices.h"

namespace stagehand {

// The app's main-thread event loop: a poll() loop that runs timers and watched
// file descriptors until asked to quit. It serves plugin editors (CLAP
// timer-support, posix-fd-support), the editor window's X11 connection, and
// StageHand's own housekeeping (engine collect(), plugin idle(), stats). --play
// runs on it; the simpler test modes keep their sleep loops.
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
};

} // namespace stagehand
