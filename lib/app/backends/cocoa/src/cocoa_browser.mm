// A WKWebView that the Cocoa backend's browser loads, runs and captures. When hidden, it is in a borderless window on
// the main screen below the desktop, where the window server composites it but no one sees it. When shown, it is in a
// clipping view over the app's own view.

#include "app/backends/cocoa/src/cocoa_browser.h"

#include "pr/alloc.h"
#include "pr/apple_string.h"
#include "pr/raw_array.h"

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#import <WebKit/WebKit.h>

// WebKit's calls that its clients use for tools: running a block once the page's next rendering has reached the screen,
// an image of its window as the window server composites it, of a rect in the screen's coordinates from the top left of
// the main screen, the pixels per CSS pixel that it renders at in place of the window's backing scale factor, whether
// it throttles a page whose window is hidden, and the processes that it runs the page in.
@interface WKWebView (Tools)
- (void)_doAfterNextPresentationUpdate:(void (^)(void))block;
- (NSImage*)_windowSnapshotInRect:(CGRect)rect withOptions:(CGWindowImageOption)options;
@property(nonatomic, setter=_setOverrideDeviceScaleFactor:) CGFloat _overrideDeviceScaleFactor;
@property(nonatomic, setter=_setWindowOcclusionDetectionEnabled:) BOOL _windowOcclusionDetectionEnabled;
@property(nonatomic, readonly) pid_t _webProcessIdentifier;
@property(nonatomic, readonly) pid_t _gpuProcessIdentifier;
@end

@interface WKWebsiteDataStore (Tools)
@property(nonatomic, readonly) pid_t _networkProcessIdentifier;
@end

// The view that clips a shown page, laid out from its top left like the app's view.
@interface PrCocoaBrowserClip : NSView
@end

@implementation PrCocoaBrowserClip
- (BOOL)isFlipped {
    return YES;
}
@end

// Runs a block once a screen has shown a number of frames. The display link holds the object until then.
@interface PrCocoaFrames : NSObject
@property(nonatomic) int left;
@property(nonatomic, copy, nullable) void (^then)(void);
- (void)frame:(CADisplayLink*)link;
@end

@implementation PrCocoaFrames
- (void)frame:(CADisplayLink*)link {
    if (--self.left > 0)
        return;
    [link invalidate];
    void (^then)(void) = self.then;
    self.then = nil;
    if (then != nil)
        then();
}
@end

@interface PrCocoaBrowser : NSObject <WKNavigationDelegate>
@property(nonatomic, strong) NSWindow* window;
@property(nonatomic, strong) WKWebView* view;
@property(nonatomic, weak) NSView* host;        // the app's view, which a shown page is placed over
@property(nonatomic, strong, nullable) PrCocoaBrowserClip* clip;
@property(nonatomic) NSSize viewport;           // CSS pixels
@property(nonatomic) BOOL shown;
@property(nonatomic) PrRect shownClip;
@property(nonatomic) PrRect shownFrame;
@property(nonatomic, copy, nullable) void (^loaded)(BOOL ok);
@property(nonatomic, copy, nullable) void (^printed)(BOOL ok);
- (void)layOut;
@end

@implementation PrCocoaBrowser
- (void)finish:(BOOL)ok {
    void (^loaded)(BOOL) = self.loaded;
    self.loaded = nil;
    if (loaded != nil)
        loaded(ok);
}
// Places the page. While the page is shown, the CSS pixels of its viewport are scaled to its frame by the page's zoom,
// which keeps its layout, and the page is clipped. While the page is hidden, it is at its viewport's size, below the
// desktop.
- (void)layOut {
    if (self.shown) {
        PrRect clip = self.shownClip;
        PrRect frame = self.shownFrame;
        auto length = [](float from, float to) { return CGFloat(to) - CGFloat(from); };
        self.clip.frame = NSMakeRect(CGFloat(clip.left), CGFloat(clip.top), length(clip.left, clip.right),
            length(clip.top, clip.bottom));
        self.view.frame = NSMakeRect(length(clip.left, frame.left), length(clip.top, frame.top),
            length(frame.left, frame.right), length(frame.top, frame.bottom));
        CGFloat width = self.viewport.width;
        self.view.pageZoom = width > 0 ? length(frame.left, frame.right) / width : 1;
        return;
    }
    // A window keeps its top left as its size changes, which would move part of a taller page below the screen's
    // bottom, where the window server composites nothing. The window goes back to the screen's bottom left.
    [self.window setContentSize:self.viewport];
    [self.window setFrameOrigin:NSZeroPoint];
    self.view.frame = NSMakeRect(0, 0, self.viewport.width, self.viewport.height);
    self.view.pageZoom = 1;
}
// Ends a print that runOperationModalForWindow started.
- (void)printOperationDidRun:(NSPrintOperation*)operation success:(BOOL)success contextInfo:(void*)contextInfo {
    (void)operation;
    (void)contextInfo;
    void (^printed)(BOOL) = self.printed;
    self.printed = nil;
    if (printed != nil)
        printed(success);
}
- (void)webView:(WKWebView*)webView didFinishNavigation:(WKNavigation*)navigation {
    (void)webView;
    (void)navigation;
    [self finish:YES];
}
- (void)webView:(WKWebView*)webView didFailNavigation:(WKNavigation*)navigation withError:(NSError*)error {
    (void)webView;
    (void)navigation;
    (void)error;
    [self finish:NO];
}
- (void)webView:(WKWebView*)webView didFailProvisionalNavigation:(WKNavigation*)navigation withError:(NSError*)error {
    (void)webView;
    (void)navigation;
    (void)error;
    [self finish:NO];
}
@end

namespace pr_impl {

namespace {

struct CocoaBrowser : PrAppBrowser {
    void* object;               // the PrCocoaBrowser, retained
};

PrCocoaBrowser* objectOf(PrAppBrowser* browser) {
    return (__bridge PrCocoaBrowser*)static_cast<CocoaBrowser*>(browser)->object;
}

} // namespace

void cocoaBrowserDestroy(PrApp*, PrAppBrowser* browser) {
    CocoaBrowser* cocoa = static_cast<CocoaBrowser*>(browser);
    PrCocoaBrowser* object = (__bridge_transfer PrCocoaBrowser*)cocoa->object;
    object.loaded = nil;
    object.printed = nil;
    [object.view stopLoading];
    object.view.navigationDelegate = nil;
    [object.view removeFromSuperview];
    [object.clip removeFromSuperview];
    [object.window orderOut:nil];
    prMemoryFree(cocoa);
}

void cocoaBrowserResize(PrApp*, PrAppBrowser* browser, PrUVec2 viewport) {
    PrCocoaBrowser* object = objectOf(browser);
    object.viewport = NSMakeSize(viewport.x, viewport.y);
    [object layOut];
}

void cocoaBrowserShow(PrApp*, PrAppBrowser* browser, PrRect clip, PrRect frame) {
    PrCocoaBrowser* object = objectOf(browser);
    NSView* host = object.host;
    if (host == nil)
        return;
    if (object.clip == nil) {
        object.clip = [[PrCocoaBrowserClip alloc] initWithFrame:NSZeroRect];
        object.clip.wantsLayer = YES;
        object.clip.layer.masksToBounds = YES;
    }
    if (object.clip.superview != host)
        [host addSubview:object.clip];
    if (object.view.superview != object.clip) {
        [object.view removeFromSuperview];
        [object.clip addSubview:object.view];
    }
    object.shown = YES;
    object.shownClip = clip;
    object.shownFrame = frame;
    [object layOut];
}

void cocoaBrowserHide(PrApp*, PrAppBrowser* browser) {
    PrCocoaBrowser* object = objectOf(browser);
    if (!object.shown)
        return;
    object.shown = NO;
    [object.view removeFromSuperview];
    [object.clip removeFromSuperview];
    object.window.contentView = object.view;
    [object layOut];
}

void cocoaBrowserLoad(PrApp*, PrAppBrowser* browser, PrStringView url, PrAppBrowserDoneFn done, void* PR_NULLABLE context) {
    PrCocoaBrowser* object = objectOf(browser);
    __weak PrCocoaBrowser* weak = object;
    // A load that replaces a pending one completes the pending one as failed.
    [object finish:NO];
    object.loaded = ^(BOOL ok) {
        if (weak != nil)
            done(context, ok ? PrStatus_Ok : PrStatus_NotFound);
    };
    NSURL* target = [NSURL URLWithString:prAppleNSString(url)];
    if (target == nil) {
        dispatch_async(dispatch_get_main_queue(), ^{
            if (weak != nil)
                [weak finish:NO];
        });
        return;
    }
    if (target.isFileURL)
        [object.view loadFileURL:target allowingReadAccessToURL:[NSURL fileURLWithPath:@"/"]];
    else
        [object.view loadRequest:[NSURLRequest requestWithURL:target]];
}

void cocoaBrowserEvaluate(PrApp*, PrAppBrowser* browser, PrStringView script, PrAppBrowserResultFn done,
        void* PR_NULLABLE context) {
    PrCocoaBrowser* object = objectOf(browser);
    __weak PrCocoaBrowser* weak = object;
    // The script runs as global code and is awaited, so that a promise's result is returned once it settles.
    [object.view callAsyncJavaScript:@"return await (0, eval)(script);" arguments:@{@"script" : prAppleNSString(script)}
        inFrame:nil inContentWorld:WKContentWorld.pageWorld completionHandler:^(id result, NSError* error) {
            if (weak == nil)
                return;
            if (error != nil) {
                NSString* message = error.userInfo[@"WKJavaScriptExceptionMessage"];
                PrRawString text = prAppleNSStringToRawString(message != nil ? message : error.localizedDescription);
                done(context, PrStatus_InvalidData, prRawStringView(&text));
                prRawStringReset(&text);
                return;
            }
            NSData* json;
            if (result == nil)
                json = nil;
            else
                json = [NSJSONSerialization dataWithJSONObject:result options:NSJSONWritingFragmentsAllowed error:nil];
            done(context, PrStatus_Ok,
                json == nil ? PrStringView{} : PrStringView{static_cast<const char*>(json.bytes), json.length});
        }];
}

void cocoaBrowserCapture(PrApp*, PrAppBrowser* browser, PrRect rect, PrUVec2 size, PrAppBrowserCaptureFn done,
        void* PR_NULLABLE context) {
    PrCocoaBrowser* object = objectOf(browser);
    __weak PrCocoaBrowser* weak = object;
    // The capture is of the window as the window server composites it, which is what the display shows. A view draws
    // itself into a bitmap (cacheDisplayInRect) without the 3D transforms that the window server applies, and WebKit's
    // snapshot (takeSnapshotWithConfiguration) paints the page again, also flat, and draws a square cap of zero length
    // as a square and its 45° turn. The capture is taken once the page's latest rendering has reached the screen, so
    // that it shows the changes that its scripts have made.
    void (^capture)(void) = ^{
        PrCocoaBrowser* strong = weak;
        if (strong == nil)
            return;
        WKWebView* view = strong.view;
        // Convert the rect to the view's points, whether or not the viewport is shown at some scale, and then to the
        // screen's coordinates from the main screen's top left.
        NSSize viewport = strong.viewport;
        NSSize bounds = view.bounds.size;
        CGFloat sx = viewport.width > 0 ? bounds.width / viewport.width : 1;
        CGFloat sy = viewport.height > 0 ? bounds.height / viewport.height : 1;
        NSRect points = NSMakeRect(CGFloat(rect.left) * sx, CGFloat(rect.top) * sy,
            CGFloat(rect.right - rect.left) * sx, CGFloat(rect.bottom - rect.top) * sy);
        NSRect screen = [view.window convertRectToScreen:[view convertRect:points toView:nil]];
        CGFloat top = NSScreen.screens.count > 0 ? NSScreen.screens[0].frame.size.height : 0;
        CGRect global = CGRectMake(screen.origin.x, top - screen.origin.y - screen.size.height, screen.size.width,
            screen.size.height);
        NSImage* image = nil;
        if ([view respondsToSelector:@selector(_windowSnapshotInRect:withOptions:)])
            image = [view _windowSnapshotInRect:global withOptions:kCGWindowImageBestResolution];
        CGImageRef cg = image != nil ? [image CGImageForProposedRect:nullptr context:nil hints:nil] : nullptr;
        if (cg == nullptr) {
            done(context, PrStatus_NotFound, PrPixelView{});
            return;
        }
        // The image has the screen's pixels, and is drawn at the size asked for, in sRGB.
        uint32_t width = size.x;
        uint32_t height = size.y;
        RawArray<uint8_t> bgra;
        bgra.reserve(width * height * 4);
        bgra.count = width * height * 4;
        CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGContextRef bitmap = CGBitmapContextCreate(bgra.data, width, height, 8, size_t(width) * 4, space,
            uint32_t(kCGImageAlphaPremultipliedFirst) | uint32_t(kCGBitmapByteOrder32Little));
        CGContextSetInterpolationQuality(bitmap, kCGInterpolationHigh);
        CGContextDrawImage(bitmap, CGRectMake(0, 0, width, height), cg);
        CGContextRelease(bitmap);
        CGColorSpaceRelease(space);
        done(context, PrStatus_Ok, PrPixelView{bgra.data, size_t(width) * 4, {width, height}});
        bgra.reset();
    };
    // The window server composites the page's latest rendering on a frame after it reaches the view, so the capture
    // waits for two of the screen's frames. The screen's display link runs whether or not the window is seen.
    __weak NSScreen* screen = object.view.window.screen != nil ? object.view.window.screen : NSScreen.mainScreen;
    void (^composited)(void) = ^{
        NSScreen* strong = screen;
        if (strong == nil) {
            capture();
            return;
        }
        PrCocoaFrames* frames = [[PrCocoaFrames alloc] init];
        frames.left = 2;
        frames.then = capture;
        [[strong displayLinkWithTarget:frames selector:@selector(frame:)] addToRunLoop:NSRunLoop.mainRunLoop
            forMode:NSRunLoopCommonModes];
    };
    if ([object.view respondsToSelector:@selector(_doAfterNextPresentationUpdate:)])
        [object.view _doAfterNextPresentationUpdate:composited];
    else
        dispatch_async(dispatch_get_main_queue(), composited);
}

void cocoaBrowserPrint(PrApp*, PrAppBrowser* browser, PrStringView path, PrVec2 paper, PrAppBrowserDoneFn done,
        void* PR_NULLABLE context) {
    PrCocoaBrowser* object = objectOf(browser);
    NSURL* url = prAppleNSFileUrl(path);
    NSString* saved = prAppleNSString(path);
    if (url == nil || saved == nil) {
        done(context, PrStatus_InvalidData);
        return;
    }
    NSPrintInfo* info = [[NSPrintInfo alloc] init];
    info.jobDisposition = NSPrintSaveJob;
    info.dictionary[NSPrintJobSavingURL] = url;
    // A CSS px is three quarters of a point.
    if (paper.x > 0 && paper.y > 0)
        info.paperSize = NSMakeSize(CGFloat(paper.x) * 0.75, CGFloat(paper.y) * 0.75);
    info.topMargin = 0;
    info.bottomMargin = 0;
    info.leftMargin = 0;
    info.rightMargin = 0;
    info.horizontallyCentered = NO;
    info.verticallyCentered = NO;
    object.printed = ^(BOOL ok) {
        bool written = ok && [[NSFileManager defaultManager] fileExistsAtPath:saved];
        done(context, written ? PrStatus_Ok : PrStatus_Io);
    };
    NSPrintOperation* operation = [object.view printOperationWithPrintInfo:info];
    operation.showsPrintPanel = NO;
    operation.showsProgressPanel = NO;
    operation.view.frame = object.view.bounds;
    // WebKit lays out the pages in its content process, so the print runs with the event loop instead of blocking it.
    [operation runOperationModalForWindow:object.window delegate:object
        didRunSelector:@selector(printOperationDidRun:success:contextInfo:) contextInfo:nullptr];
}

uint32_t cocoaBrowserProcesses(PrApp*, PrAppBrowser* browser, PrAppBrowserProcess* PR_NULLABLE out, uint32_t capacity) {
    PrCocoaBrowser* object = objectOf(browser);
    WKWebView* view = object.view;
    WKWebsiteDataStore* store = view.configuration.websiteDataStore;
    // A process that WebKit does not name, as an older WebKit does not, or that it has not started, is left out.
    pid_t content = [view respondsToSelector:@selector(_webProcessIdentifier)] ? view._webProcessIdentifier : 0;
    pid_t gpu = [view respondsToSelector:@selector(_gpuProcessIdentifier)] ? view._gpuProcessIdentifier : 0;
    pid_t network = [store respondsToSelector:@selector(_networkProcessIdentifier)] ? store._networkProcessIdentifier : 0;
    const PrAppBrowserProcess found[] = {
        {PrAppBrowserProcessRole_Content, content},
        {PrAppBrowserProcessRole_Gpu, gpu},
        {PrAppBrowserProcessRole_Network, network},
    };
    uint32_t count = 0;
    for (const PrAppBrowserProcess& process : found) {
        if (process.id <= 0)
            continue;
        if (count < capacity)
            out[count] = process;
        ++count;
    }
    return count;
}

PrStatus cocoaBrowserCreate(PrApp* app, void* PR_NULLABLE host, const PrAppBrowserDesc* desc,
        PrAppBrowser* PR_NULLABLE* PR_NONNULL out) {
    PrCocoaBrowser* object = [[PrCocoaBrowser alloc] init];
    object.host = (__bridge NSView*)host;
    object.viewport = NSMakeSize(desc->viewport.x, desc->viewport.y);
    NSRect frame = NSMakeRect(0, 0, desc->viewport.x, desc->viewport.y);
    object.window = [[NSWindow alloc] initWithContentRect:frame styleMask:NSWindowStyleMaskBorderless
        backing:NSBackingStoreBuffered defer:NO];
    object.window.releasedWhenClosed = NO;
    object.window.excludedFromWindowsMenu = YES;
    // On the main screen, so that the window server composites the page, and below the desktop, so that no one sees it,
    // in every space.
    object.window.level = CGWindowLevelForKey(kCGDesktopWindowLevelKey) - 1;
    object.window.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
        NSWindowCollectionBehaviorStationary | NSWindowCollectionBehaviorIgnoresCycle;
    object.window.ignoresMouseEvents = YES;
    [object.window setFrameOrigin:NSZeroPoint];
    WKWebViewConfiguration* configuration = [[WKWebViewConfiguration alloc] init];
    // The page keeps nothing once the browser is gone: no cookies, storage or caches carry over to the next.
    configuration.websiteDataStore = [WKWebsiteDataStore nonPersistentDataStore];
    // A page from a file can read the files beside it. The viewer's players rely on this to read the documents that
    // they play.
    [configuration.preferences setValue:@YES forKey:@"allowFileAccessFromFileURLs"];
    // A print draws backgrounds as the screen does.
    configuration.preferences.shouldPrintBackgrounds = YES;
    object.view = [[WKWebView alloc] initWithFrame:frame configuration:configuration];
    object.view.navigationDelegate = object;
    if (desc->pixelRatio > 0 && [object.view respondsToSelector:@selector(_setOverrideDeviceScaleFactor:)])
        object.view._overrideDeviceScaleFactor = CGFloat(desc->pixelRatio);
    // The window below the desktop counts as hidden, which would throttle the page's rendering and timers.
    if ([object.view respondsToSelector:@selector(_setWindowOcclusionDetectionEnabled:)])
        object.view._windowOcclusionDetectionEnabled = NO;
    if (desc->preferences != nullptr) {
        bool dark = desc->preferences->colorScheme == PrAppColorScheme_Dark;
        object.view.appearance = [NSAppearance appearanceNamed:dark ? NSAppearanceNameDarkAqua : NSAppearanceNameAqua];
    }
    object.window.contentView = object.view;
    // The window is ordered in, so that it renders.
    [object.window orderFrontRegardless];
    CocoaBrowser* browser = allocZeroed<CocoaBrowser>();
    browser->app = app;
    browser->object = (__bridge_retained void*)object;
    *out = browser;
    return PrStatus_Ok;
}

} // namespace pr_impl
