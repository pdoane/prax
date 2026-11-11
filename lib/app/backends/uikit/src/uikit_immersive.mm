// The immersive space of the UIKit backend, on visionOS. Swift activates the space's scene, whose delegate is
// SwiftUI's, and reports its layer renderer, on the main thread. The space's scene is the one whose session has an
// immersive space's role: the backend closes the space by destroying its session, and hears that it closed when the
// scene disconnects. The host keeps the layer renderer while the space is open.

#include "app/backends/uikit/src/uikit_immersive.h"

// What uikit_immersive.swift defines with C linkage: asks the system to open the space, and gives the functions that
// hear that it opened, with its layer renderer, and that it failed to open.
extern "C" void prUiKitImmersiveActivate(void (*opened)(void* renderer), void (*failed)(void));

namespace pr_impl {

namespace {

// The host whose application opened the space, the space's scene once it opened, and the observer of its disconnect.
__weak PrUiKitHost* immersiveHost;
__weak UIScene* immersiveScene;
id<NSObject> disconnectObserver;

// Returns true for the role of an immersive space's session, which UIKit names, and which CompositorServices names for
// a space of its layer.
bool isImmersive(UISceneSessionRole role) {
    return [role isEqualToString:UISceneSessionRoleImmersiveSpaceApplication] ||
        [role isEqualToString:@"CPSceneSessionRoleImmersiveSpaceApplication"];
}

void closed() {
    PrUiKitHost* host = immersiveHost;
    if (host == nil || (!host.immersiveOpening && host.immersive == nil))
        return;
    host.immersiveOpening = NO;
    host.immersive = nil;
    immersiveScene = nil;
    [host deliverKind:PrAppEventKind_ImmersiveClosed];
}

void opened(void* renderer) {
    PrUiKitHost* host = immersiveHost;
    if (host == nil || !host.immersiveOpening)
        return;
    for (UIScene* scene in UIApplication.sharedApplication.connectedScenes) {
        if (isImmersive(scene.session.role))
            immersiveScene = scene;
    }
    host.immersiveOpening = NO;
    host.immersive = (__bridge id)renderer;
    if (!host.running)
        return;
    PrAppEvent event{};
    event.kind = PrAppEventKind_ImmersiveOpened;
    event.time = uiKitUptime();
    event.immersive.native = renderer;
    [host deliver:event to:nullptr];
}

} // namespace

PrStatus uiKitImmersiveOpen(PrApp* app) {
    PrUiKitHost* host = uiKitHostOf(app);
    if (host.headless)
        return PrStatus_NotSupported;
    if (host.immersiveOpening || host.immersive != nil)
        return PrStatus_InvalidState;
    immersiveHost = host;
    host.immersiveOpening = YES;
    if (disconnectObserver == nil) {
        disconnectObserver = [NSNotificationCenter.defaultCenter addObserverForName:UISceneDidDisconnectNotification
            object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification* note) {
                if (note.object != nil && note.object == immersiveScene)
                    closed();
            }];
    }
    prUiKitImmersiveActivate(opened, closed);
    return PrStatus_Ok;
}

void uiKitImmersiveClose(PrApp* app) {
    (void)app;
    UIScene* scene = immersiveScene;
    if (scene != nil)
        [UIApplication.sharedApplication requestSceneSessionDestruction:scene.session options:nil errorHandler:nil];
}

} // namespace pr_impl
