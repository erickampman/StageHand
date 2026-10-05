#include "App/MainLoop.h"

#import <AppKit/AppKit.h>
#include <CoreFoundation/CoreFoundation.h>

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>

// MainLoop on macOS (MainLoop.h): AppKit's event loop drives. Timers are
// CFRunLoopTimers and fds CFFileDescriptors, both in the common modes, so they
// keep running while AppKit runs its own loop (a window being resized, a menu
// tracked) as well as in run()'s.

namespace stagehand {

namespace {

void fdCallback(CFFileDescriptorRef descriptor, CFOptionFlags types, void* info) {
    std::uint32_t fired = 0;
    if (types & kCFFileDescriptorReadCallBack) fired |= FdEvent::Read;
    if (types & kCFFileDescriptorWriteCallBack) fired |= FdEvent::Write;
    static_cast<MainLoop*>(info)->dispatchFd(CFFileDescriptorGetNativeDescriptor(descriptor), fired);
}

// Makes a waiting nextEventMatchingMask: return, so run() sees `quit`.
void wakeAppKit() {
    NSEvent* const event = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                              location:NSZeroPoint
                                         modifierFlags:0
                                             timestamp:0
                                          windowNumber:0
                                               context:nil
                                               subtype:0
                                                 data1:0
                                                 data2:0];
    [NSApp postEvent:event atStart:YES];
}

} // namespace

MainLoop::MainLoop() {
    int fds[2];
    if (::pipe(fds) != 0) {
        std::perror("MainLoop: pipe");
        return;
    }
    for (const int fd : fds) {
        ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
        ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
    wakeRead  = fds[0];
    wakeWrite = fds[1];
    watchFd(wakeRead, FdEvent::Read, [this](std::uint32_t) {
        char buffer[64];
        while (::read(wakeRead, buffer, sizeof buffer) > 0) {}
        if (quit.load()) wakeAppKit();
    });
}

MainLoop::~MainLoop() {
    while (!timers.empty()) removeTimer(timers.begin()->first);
    while (!watches.empty()) unwatchFd(watches.begin()->first);
    if (wakeRead >= 0) ::close(wakeRead);
    if (wakeWrite >= 0) ::close(wakeWrite);
}

TimerId MainLoop::addTimer(std::uint32_t periodMs, std::function<void()> callback) {
    if (!callback) return kInvalidTimer;
    const TimerId        id       = nextTimerId++;
    const CFTimeInterval period   = std::max<std::uint32_t>(periodMs, 1) / 1000.0;
    auto                 shared   = std::make_shared<std::function<void()>>(std::move(callback));
    // A late timer fires once and goes on from there: CFRunLoopTimer skips
    // the fires it missed, as the poll() loop reschedules from now.
    const CFRunLoopTimerRef timer = CFRunLoopTimerCreateWithHandler(
        kCFAllocatorDefault, CFAbsoluteTimeGetCurrent() + period, period, 0, 0, ^(CFRunLoopTimerRef) {
            const auto keep = shared;   // alive even if the callback removes this timer
            (*keep)();
        });
    CFRunLoopAddTimer(CFRunLoopGetMain(), timer, kCFRunLoopCommonModes);
    timers[id] = Timer{timer, shared};
    return id;
}

void MainLoop::removeTimer(TimerId id) {
    const auto it = timers.find(id);
    if (it == timers.end()) return;
    const auto timer = static_cast<CFRunLoopTimerRef>(it->second.timer);
    CFRunLoopTimerInvalidate(timer);   // the run loop keeps it alive through a callback in progress
    CFRelease(timer);
    timers.erase(it);
}

bool MainLoop::watchFd(int fd, std::uint32_t events, std::function<void(std::uint32_t)> callback) {
    if (fd < 0 || !callback || watches.count(fd) != 0) return false;
    CFFileDescriptorContext   context{0, this, nullptr, nullptr, nullptr};
    const CFFileDescriptorRef descriptor = CFFileDescriptorCreate(kCFAllocatorDefault, fd, false, &fdCallback, &context);
    if (descriptor == nullptr) return false;
    const CFRunLoopSourceRef source = CFFileDescriptorCreateRunLoopSource(kCFAllocatorDefault, descriptor, 0);
    CFRunLoopAddSource(CFRunLoopGetMain(), source, kCFRunLoopCommonModes);
    watches[fd] = Watch{descriptor, source, events,
                        std::make_shared<std::function<void(std::uint32_t)>>(std::move(callback))};
    enable(fd);
    return true;
}

bool MainLoop::modifyFd(int fd, std::uint32_t events) {
    const auto it = watches.find(fd);
    if (it == watches.end()) return false;
    const auto descriptor = static_cast<CFFileDescriptorRef>(it->second.descriptor);
    CFFileDescriptorDisableCallBacks(descriptor, kCFFileDescriptorReadCallBack | kCFFileDescriptorWriteCallBack);
    it->second.events = events;
    enable(fd);
    return true;
}

void MainLoop::unwatchFd(int fd) {
    const auto it = watches.find(fd);
    if (it == watches.end()) return;
    const auto source     = static_cast<CFRunLoopSourceRef>(it->second.source);
    const auto descriptor = static_cast<CFFileDescriptorRef>(it->second.descriptor);
    CFRunLoopRemoveSource(CFRunLoopGetMain(), source, kCFRunLoopCommonModes);
    CFRelease(source);
    CFFileDescriptorInvalidate(descriptor);   // the fd itself stays open: it's the watcher's
    CFRelease(descriptor);
    watches.erase(it);
}

void MainLoop::enable(int fd) {
    const auto it = watches.find(fd);
    if (it == watches.end()) return;
    CFOptionFlags types = 0;
    if (it->second.events & FdEvent::Read) types |= kCFFileDescriptorReadCallBack;
    if (it->second.events & FdEvent::Write) types |= kCFFileDescriptorWriteCallBack;
    if (types != 0) CFFileDescriptorEnableCallBacks(static_cast<CFFileDescriptorRef>(it->second.descriptor), types);
}

void MainLoop::dispatchFd(int fd, std::uint32_t fired) {
    const auto it = watches.find(fd);
    if (it == watches.end()) return;
    const auto callback = it->second.callback;   // alive even if it unwatches itself
    (*callback)(fired);
    enable(fd);   // if it's still watched
}

void MainLoop::requestQuit() noexcept {
    quit.store(true);
    const char byte = 0;   // a full pipe (EAGAIN) is fine: run() is already due to wake
    [[maybe_unused]] const ssize_t n = ::write(wakeWrite, &byte, 1);
}

void MainLoop::run() {
    @autoreleasepool {
        [NSApplication sharedApplication];
        static bool launched = false;
        if (!launched) {
            [NSApp finishLaunching];
            launched = true;
        }
    }
    while (!quit.load()) {
        @autoreleasepool {
            NSEvent* const event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                      untilDate:[NSDate distantFuture]
                                                         inMode:NSDefaultRunLoopMode
                                                        dequeue:YES];
            if (event != nil && event.type != NSEventTypeApplicationDefined) [NSApp sendEvent:event];
        }
    }
}

} // namespace stagehand
