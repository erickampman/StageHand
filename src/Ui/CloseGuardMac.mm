#include "Ui/CloseGuard.h"

#import <AppKit/AppKit.h>
#import <objc/runtime.h>

#include <utility>

// A window delegate in front of pugl's (PuglWindowDelegate): it answers
// windowShouldClose: itself and forwards everything else, so pugl still sees
// its resizes, moves and focus changes. NSWindow's delegate is weak: the
// window keeps this one as an associated object, and this keeps pugl's.
// Its class name is StageHand's own, so no plugin's DGL clashes with it.
@interface StageHandCloseGuard : NSObject <NSWindowDelegate>
@end

@implementation StageHandCloseGuard {
@public
    id<NSWindowDelegate>  inner;   // pugl's delegate
    std::function<bool()> shouldClose;
}

- (BOOL)windowShouldClose:(NSWindow*)sender {
    if (shouldClose && !shouldClose()) return NO;   // asked instead; the window stays
    // pugl's: PUGL_CLOSE to DGL, which hides the window, then YES.
    if ([inner respondsToSelector:@selector(windowShouldClose:)]) return [inner windowShouldClose:sender];
    return YES;
}

- (BOOL)respondsToSelector:(SEL)selector {
    return [super respondsToSelector:selector] || [inner respondsToSelector:selector];
}

- (id)forwardingTargetForSelector:(SEL)selector {
    if ([inner respondsToSelector:selector]) return inner;
    return [super forwardingTargetForSelector:selector];
}

@end

namespace stagehand::ui {

namespace {
char guardKey;   // its address names the associated object
} // namespace

void guardWindowClose(std::uintptr_t nativeView, std::function<bool()> shouldClose) {
    NSView*   view   = (__bridge NSView*)reinterpret_cast<void*>(nativeView);
    NSWindow* window = view.window;
    if (window == nil) return;   // not a top-level window (or not realized): nothing to guard
    StageHandCloseGuard* guard = objc_getAssociatedObject(window, &guardKey);
    if (guard == nil) {
        if (!shouldClose) return;
        guard        = [[StageHandCloseGuard alloc] init];
        guard->inner = window.delegate;
        objc_setAssociatedObject(window, &guardKey, guard, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        // Set after `inner`: NSWindow asks what its delegate responds to as it's set.
        window.delegate = guard;
    }
    guard->shouldClose = std::move(shouldClose);
}

} // namespace stagehand::ui
