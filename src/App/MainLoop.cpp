#include "App/MainLoop.h"

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <vector>

namespace stagehand {

namespace {

short toPoll(std::uint32_t events) noexcept {
    short flags = 0;
    if (events & FdEvent::Read) flags |= POLLIN;
    if (events & FdEvent::Write) flags |= POLLOUT;
    return flags;   // POLLERR/POLLHUP are always reported
}

std::uint32_t fromPoll(short revents) noexcept {
    std::uint32_t events = 0;
    if (revents & (POLLIN | POLLHUP)) events |= FdEvent::Read;
    if (revents & POLLOUT) events |= FdEvent::Write;
    if (revents & (POLLERR | POLLHUP | POLLNVAL)) events |= FdEvent::Error;
    return events;
}

} // namespace

MainLoop::MainLoop() {
    int fds[2];
    if (::pipe(fds) != 0) {
        std::perror("MainLoop: pipe");
        return;
    }
    for (const int fd : fds) {   // no pipe2() on macOS
        ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
        ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
    wakeRead  = fds[0];
    wakeWrite = fds[1];
}

MainLoop::~MainLoop() {
    if (wakeRead >= 0) ::close(wakeRead);
    if (wakeWrite >= 0) ::close(wakeWrite);
}

TimerId MainLoop::addTimer(std::uint32_t periodMs, std::function<void()> callback) {
    if (!callback) return kInvalidTimer;
    const TimerId id     = nextTimerId++;
    const auto    period = std::chrono::milliseconds(std::max<std::uint32_t>(periodMs, 1));
    timers[id] = Timer{period, Clock::now() + period,
                       std::make_shared<std::function<void()>>(std::move(callback))};
    return id;
}

void MainLoop::removeTimer(TimerId id) {
    timers.erase(id);
}

bool MainLoop::watchFd(int fd, std::uint32_t events, std::function<void(std::uint32_t)> callback) {
    if (fd < 0 || !callback || watches.count(fd) != 0) return false;
    watches[fd] = Watch{events, std::make_shared<std::function<void(std::uint32_t)>>(std::move(callback))};
    return true;
}

bool MainLoop::modifyFd(int fd, std::uint32_t events) {
    const auto it = watches.find(fd);
    if (it == watches.end()) return false;
    it->second.events = events;
    return true;
}

void MainLoop::unwatchFd(int fd) {
    watches.erase(fd);
}

void MainLoop::requestQuit() noexcept {
    quit.store(true);
    const char byte = 0;   // a full pipe (EAGAIN) is fine: run() is already due to wake
    [[maybe_unused]] const ssize_t n = ::write(wakeWrite, &byte, 1);
}

void MainLoop::run() {
    std::vector<pollfd>  fds;
    std::vector<TimerId> due;

    while (!quit.load()) {
        // Sleep until the next timer is due (or forever if there are none).
        const Clock::time_point now     = Clock::now();
        int                     timeout = -1;
        for (const auto& [id, timer] : timers) {
            const auto wait =
                std::max<long long>(0, std::chrono::ceil<std::chrono::milliseconds>(timer.due - now).count());
            timeout = timeout < 0 ? static_cast<int>(wait) : std::min(timeout, static_cast<int>(wait));
        }

        fds.clear();
        fds.push_back(pollfd{wakeRead, POLLIN, 0});
        for (const auto& [fd, watch] : watches) fds.push_back(pollfd{fd, toPoll(watch.events), 0});

        if (::poll(fds.data(), fds.size(), timeout) < 0 && errno != EINTR) {
            std::perror("MainLoop: poll");
            break;
        }
        if (quit.load()) break;

        if (fds[0].revents & POLLIN) {
            char buffer[64];
            while (::read(wakeRead, buffer, sizeof buffer) > 0) {}
        }

        for (std::size_t i = 1; i < fds.size(); ++i) {
            if (fds[i].revents == 0) continue;
            const auto it = watches.find(fds[i].fd);   // may have been removed meanwhile
            if (it == watches.end()) continue;
            const auto callback = it->second.callback;
            (*callback)(fromPoll(fds[i].revents));
        }

        const Clock::time_point fired = Clock::now();
        due.clear();
        for (const auto& [id, timer] : timers)
            if (timer.due <= fired) due.push_back(id);
        for (const TimerId id : due) {
            const auto it = timers.find(id);   // may have been removed meanwhile
            if (it == timers.end()) continue;
            Timer& timer = it->second;
            timer.due += timer.period;         // keep cadence...
            if (timer.due <= fired) timer.due = fired + timer.period;   // ...but never catch up
            const auto callback = timer.callback;
            (*callback)();
        }
    }
}

} // namespace stagehand
