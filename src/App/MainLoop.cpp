#include "App/MainLoop.h"

#include <poll.h>
#include <sys/eventfd.h>
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

MainLoop::MainLoop() : wakeFd(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) {
    if (wakeFd < 0) std::fprintf(stderr, "MainLoop: eventfd failed\n");
}

MainLoop::~MainLoop() {
    if (wakeFd >= 0) ::close(wakeFd);
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
    const std::uint64_t one = 1;
    [[maybe_unused]] const ssize_t n = ::write(wakeFd, &one, sizeof one);
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
        fds.push_back(pollfd{wakeFd, POLLIN, 0});
        for (const auto& [fd, watch] : watches) fds.push_back(pollfd{fd, toPoll(watch.events), 0});

        if (::poll(fds.data(), fds.size(), timeout) < 0 && errno != EINTR) {
            std::perror("MainLoop: poll");
            break;
        }
        if (quit.load()) break;

        if (fds[0].revents & POLLIN) {
            std::uint64_t value = 0;
            [[maybe_unused]] const ssize_t n = ::read(wakeFd, &value, sizeof value);
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
