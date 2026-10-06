#include "App/AppDelegateMac.h"

#import <AppKit/AppKit.h>

#include <utility>

@interface StageHandAppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation StageHandAppDelegate {
@public
    std::function<void()>                        quit;
    std::function<void(const std::string& path)> open;
}

// Not NSTerminateLater: AppKit would wait in a modal loop of its own for the
// reply, and StageHand's shutdown runs only once MainLoop::run() returns.
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
    (void)sender;
    if (quit) quit();
    return NSTerminateCancel;
}

- (void)application:(NSApplication*)application openURLs:(NSArray<NSURL*>*)urls {
    (void)application;
    for (NSURL* url in urls)
        if (url.isFileURL && open) open(url.path.UTF8String);
}

@end

namespace stagehand {

void installAppHandlers(std::function<void()> quit, std::function<void(const std::string& path)> open) {
    static StageHandAppDelegate* delegate = nil;   // NSApp's delegate is weak: kept here
    [NSApplication sharedApplication];
    if (delegate == nil) delegate = [[StageHandAppDelegate alloc] init];
    delegate->quit = std::move(quit);
    delegate->open = std::move(open);
    NSApp.delegate = delegate;
}

} // namespace stagehand
