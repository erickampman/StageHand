#pragma once

#include <atomic>

#include "App/MainLoop.h"
#include "Plugin/HostServices.h"

namespace stagehand {

class MainLoop;

// The app's HostServices: what plugins can ask of StageHand. Timers and fd
// watches run on the app's MainLoop (plugin editors depend on them). The engine
// processes every block anyway, and HostedPlugin::idle() services flushes and
// main-thread callbacks. A restart request is recorded and reported but not yet
// acted on (it needs the engine's commit/adopt sequence around a
// deactivate/activate).
class AppHost final : public HostServices {
public:
    explicit AppHost(MainLoop& mainLoop) : loop(mainLoop) {}

    void requestRestart() override;
    void requestProcess() override {}
    void requestParamFlush() override {}
    void log(LogLevel level, const char* message) override;

    TimerId addTimer(std::uint32_t periodMs, std::function<void()> callback) override;
    void    removeTimer(TimerId id) override;
    bool    watchFd(int fd, std::uint32_t events, std::function<void(std::uint32_t)> callback) override;
    bool    modifyFd(int fd, std::uint32_t events) override;
    void    unwatchFd(int fd) override;

    bool restartRequested() const noexcept { return restart.load(std::memory_order_relaxed); }

private:
    MainLoop&         loop;
    std::atomic<bool> restart{false};
};

} // namespace stagehand
