#include "App/AppHost.h"

#include <cstdio>
#include <utility>

namespace stagehand {

void AppHost::requestRestart() {
    restart.store(true, std::memory_order_relaxed);
}

void AppHost::log(LogLevel level, const char* message) {
    const char* tag = "info";
    switch (level) {
        case LogLevel::Debug:   tag = "debug"; break;
        case LogLevel::Info:    tag = "info"; break;
        case LogLevel::Warning: tag = "warning"; break;
        case LogLevel::Error:   tag = "error"; break;
    }
    std::fprintf(stderr, "[%s] %s\n", tag, message);
}

TimerId AppHost::addTimer(std::uint32_t periodMs, std::function<void()> callback) {
    return loop.addTimer(periodMs, std::move(callback));
}

void AppHost::removeTimer(TimerId id) {
    loop.removeTimer(id);
}

bool AppHost::watchFd(int fd, std::uint32_t events, std::function<void(std::uint32_t)> callback) {
    return loop.watchFd(fd, events, std::move(callback));
}

bool AppHost::modifyFd(int fd, std::uint32_t events) {
    return loop.modifyFd(fd, events);
}

void AppHost::unwatchFd(int fd) {
    loop.unwatchFd(fd);
}

} // namespace stagehand
