#include "pr/app_appkit.h"
#include "pr/array.h"
#include "pr/string.h"
#include "pr/thread.h"
#include "pr/vec.h"

#include "tools/harness/test.h"

#import <AppKit/AppKit.h>

// A drag over a view, as AppKit describes it to the view, with the source's operations and the drag's pasteboard.
@interface PrTestDrag : NSObject <NSDraggingInfo>
@property(nonatomic, strong) NSWindow* window;
@property(nonatomic, strong) NSPasteboard* board;
@property(nonatomic) NSDragOperation mask;
@property NSDraggingFormation draggingFormation;
@property BOOL animatesToDestination;
@property NSInteger numberOfValidItemsForDrop;
@property(nonatomic, readonly) NSSpringLoadingHighlight springLoadingHighlight;
@end

@implementation PrTestDrag
- (NSWindow*)draggingDestinationWindow {
    return self.window;
}
- (NSDragOperation)draggingSourceOperationMask {
    return self.mask;
}
- (NSPoint)draggingLocation {
    // In the window's coordinates, from its bottom left. The point is 10 px from the left and from the top of the 32 px
    // window.
    return NSMakePoint(10, 22);
}
- (NSPoint)draggedImageLocation {
    return NSZeroPoint;
}
- (NSImage*)draggedImage {
    return nil;
}
- (NSPasteboard*)draggingPasteboard {
    return self.board;
}
- (id)draggingSource {
    return nil;
}
- (NSInteger)draggingSequenceNumber {
    return 1;
}
- (void)slideDraggedImageTo:(NSPoint)screenPoint {
    (void)screenPoint;
}
- (NSArray<NSString*>*)namesOfPromisedFilesDroppedAtDestination:(NSURL*)dropDestination {
    (void)dropDestination;
    return @[];
}
- (void)enumerateDraggingItemsWithOptions:(NSDraggingItemEnumerationOptions)enumOpts forView:(NSView*)view
        classes:(NSArray<Class>*)classArray searchOptions:(NSDictionary<NSPasteboardReadingOptionKey, id>*)searchOptions
        usingBlock:(void (^)(NSDraggingItem*, NSInteger, BOOL*))block {
    (void)enumOpts;
    (void)view;
    (void)classArray;
    (void)searchOptions;
    (void)block;
}
- (void)resetSpringLoading {
}
@end

namespace pr_impl {

namespace {

// A run's calls, and the windows it made.
struct Run {
    Array<String> seen;
    PrThread waker{};
    uint32_t frames = 0;
    PrApp* app = nullptr;
    PrAppWindow* window = nullptr;
    PrAppWindow* second = nullptr;
    void (*begin)(Run& run) = nullptr;
    bool wakeOnFrame = false;       // the first frame wakes the application from another thread
    PrAppWindowModel windowModel = PrAppWindowModel_Desktop;

    // The first window's frame, and the title bar regions that its frames return, and its last frame's title bar.
    PrAppWindowDecoration decoration = PrAppWindowDecoration_System;
    Array<PrAppTitleBarRegion> regions;
    PrAppTitleBar titleBar{};

    // Called with the second frame, once the window holds the first frame's title bar regions and drop operation.
    void (*secondFrame)(Run& run) = nullptr;
    bool starting = false;
    bool nested = false;

    // The events of the first window and of the second, each in order.
    Array<PrAppEvent> events;
    Array<PrAppEvent> secondEvents;

    // What the first window's frames answer a drag with, and the drag that its first frame starts, or null.
    PrAppDragOperation dropOperation = PrAppDragOperation_None;
    const PrAppDragDesc* drag = nullptr;

    // What the first window heard of its drops and of its drags' ends: their operations, the forms of a drag that
    // ended, and the text and the paths that a drop's reads answered.
    Array<String> dropped;
    Array<PrAppDataFormFlags> contents;    // the content's flags in each drag event
};

Run& runOf(void* context) {
    return *static_cast<Run*>(context);
}

// The number of the elements of `items` that equal `value`.
template <typename T, typename Value>
uint32_t occurrences(const Array<T>& items, const Value& value) {
    uint32_t count = 0;
    for (const T& item : items)
        count += item == value ? 1 : 0;
    return count;
}

NSView* viewOf(PrAppWindow* window) {
    PrAppSurface surface{};
    REQUIRE(prAppWindowSurface(window, &surface) == PrStatus_Ok);
    return (__bridge NSView*)surface.handle;
}

// A press of the A key, as AppKit would dispatch it to the window.
NSEvent* keyA(NSWindow* window, NSEventType type) {
    return [NSEvent keyEventWithType:type location:NSZeroPoint modifierFlags:0 timestamp:0
        windowNumber:window.windowNumber context:nil characters:@"a" charactersIgnoringModifiers:@"a" isARepeat:NO
        keyCode:0];
}

NSEvent* leftMouse(NSWindow* window, NSEventType type) {
    return [NSEvent mouseEventWithType:type location:NSMakePoint(10, 10) modifierFlags:0 timestamp:0
        windowNumber:window.windowNumber context:nil eventNumber:0 clickCount:1 pressure:1];
}

// The events of a run, without Text, which the input method types as it will, and without the pointer's crossings,
// which a test of their own checks.
Array<PrAppEventKind> kindsOf(const Array<PrAppEvent>& events) {
    Array<PrAppEventKind> kinds;
    for (const PrAppEvent& event : events) {
        bool crossing = event.kind == PrAppEventKind_PointerEnter || event.kind == PrAppEventKind_PointerLeave;
        if (event.kind != PrAppEventKind_Text && !crossing)
            kinds.push(event.kind);
    }
    return kinds;
}

// Every event kind of a run, in order.
Array<PrAppEventKind> allKindsOf(const Array<PrAppEvent>& events) {
    Array<PrAppEventKind> kinds;
    for (const PrAppEvent& event : events)
        kinds.push(event.kind);
    return kinds;
}

// A window's handler that records the window's events in the run, and stops on a frame after its first if asked.
PrAppWindowHandler handlerOf(Run& run, bool second = false) {
    PrAppWindowHandler handler{
        .context = &run,
        .open = [](void* context, PrAppWindow*, const PrAppFrame* frame) {
            Run& r = runOf(context);
            r.seen.push("open");
            CHECK(frame->extent.x == 64.0f);
            CHECK(frame->delta == 0.0);
            return true;
        },
        .frame = [](void* context, PrAppWindow* window, const PrAppFrame* frame) {
            Run& r = runOf(context);
            if (r.frames == 0 && r.drag != nullptr)
                prAppWindowDragStart(window, r.drag);
            if (r.frames++ == 0 && r.wakeOnFrame)
                prThreadStart(&r.waker, [](void* PR_NULLABLE app) { prAppWake(static_cast<PrApp*>(app)); }, r.app);
            r.titleBar = frame->titleBar;
            if (r.frames == 2 && r.secondFrame != nullptr)
                r.secondFrame(r);
            return PrAppOutput{
                .wantsFrame = r.frames == 1 && r.secondFrame != nullptr,
                .titleBarRegions = r.regions.data(),
                .titleBarRegionCount = r.regions.count(),
                .dropOperation = r.dropOperation,
            };
        },
        .event = [](void* context, PrAppWindow*, const PrAppEvent* event) {
            Run& r = runOf(context);
            r.nested = r.nested || r.starting;
            r.events.push(*event);
            bool dragged = event->kind >= PrAppEventKind_DragEnter && event->kind <= PrAppEventKind_DragEnd;
            if (dragged && event->kind != PrAppEventKind_DragEnd)
                r.contents.push(event->drag.forms);
            if (event->kind == PrAppEventKind_DragEnd)
                r.dropped.push(format("end {} forms {}", uint32_t(event->drag.operation), uint32_t(event->drag.forms)));
            if (event->kind != PrAppEventKind_Drop)
                return;
            // The drop's content is read from its source, its text first and then its files, which stop the run.
            r.dropped.push(format("drop {}", uint32_t(event->drag.operation)));
            prAppDataReadText(r.app, event->drag.drop, [](void* context, PrStatus status, PrStringView text) {
                CHECK(status == PrStatus_Ok);
                runOf(context).dropped.push(format("text {}", text));
            }, &r);
            prAppDataReadFiles(r.app, event->drag.drop,
                [](void* context, PrStatus status, const PrAppDataFile* files, uint32_t count) {
                    CHECK(status == PrStatus_Ok);
                    Run& run = runOf(context);
                    for (uint32_t i = 0; i < count; ++i)
                        run.dropped.push(format("path {}", files[i].path));
                    prAppWake(run.app);
                }, &r);
        },
    };
    if (second) {
        handler.open = [](void*, PrAppWindow*, const PrAppFrame*) { return true; };
        handler.event = [](void* context, PrAppWindow*, const PrAppEvent* event) {
            runOf(context).secondEvents.push(*event);
        };
    }
    return handler;
}

// Runs a headless application that creates a window of 64 by 32 and calls `run.begin` in its start. The application
// stops on a Wake event.
void runHeadless(Run& run) {
    PrAppOptions options{.headless = true, .windowModel = run.windowModel};
    run.app = prAppAppKitCreate(&options);
    prAppRun(run.app, {
        .context = &run,
        .start = [](void* context, PrApp* app) {
            Run& r = runOf(context);
            r.seen.push("start");
            PrAppWindowDesc desc{
                .title = "macOS test",
                .extent = {64, 32},
                .decoration = r.decoration,
                .handler = handlerOf(r),
            };
            REQUIRE(prAppWindowCreate(app, &desc, &r.window) == PrStatus_Ok);
            if (r.begin != nullptr)
                r.begin(r);
            return true;
        },
        .event = [](void* context, PrApp* app, const PrAppEvent* event) {
            Run& r = runOf(context);
            if (event->kind != PrAppEventKind_Wake)
                return;
            r.seen.push("wake");
            prAppStop(app);
        },
        .shutdown = [](void* context, PrApp*) { runOf(context).seen.push("shutdown"); },
    });
    prAppDestroy(run.app);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a headless run starts, opens and frames a window, wakes from another thread and shuts down") {
    Run run;
    run.wakeOnFrame = true;
    run.begin = [](Run& r) {
        CHECK(viewOf(r.window).bounds.size.width == 64);
        // The window opens before its first frame, after the start returns.
        CHECK(r.seen == Array<String>({"start"}));
    };
    runHeadless(run);
    prThreadJoin(&run.waker);
    CHECK(run.seen == Array<String>({"start", "open", "wake", "shutdown"}));
}

TEST_CASE("a window is created only while its application runs") {
    PrAppOptions options{.headless = true, .windowModel = PrAppWindowModel_Desktop};
    PrApp* app = prAppAppKitCreate(&options);
    Run run;
    PrAppWindowDesc desc{.extent = {64, 32}, .handler = handlerOf(run)};
    PrAppWindow* window = nullptr;
    CHECK(prAppWindowCreate(app, &desc, &window) == PrStatus_InvalidState);
    CHECK(window == nullptr);
    prAppDestroy(app);
}

TEST_CASE("a Single application refuses a second window, a Multiple one refuses only popups, and a Desktop one neither") {
    Run run;
    run.windowModel = PrAppWindowModel_Single;
    run.begin = [](Run& r) {
        CHECK(prAppWindowModel(r.app) == PrAppWindowModel_Single);
        PrAppWindowDesc desc{.extent = {32, 32}, .handler = handlerOf(r, true)};
        CHECK(prAppWindowCreate(r.app, &desc, &r.second) == PrStatus_NotSupported);
        CHECK(r.second == nullptr);
        prAppWake(r.app);
    };
    runHeadless(run);
    Run multiple;
    multiple.windowModel = PrAppWindowModel_Multiple;
    multiple.begin = [](Run& r) {
        CHECK(prAppWindowModel(r.app) == PrAppWindowModel_Multiple);
        PrAppWindowDesc desc{.extent = {32, 32}, .handler = handlerOf(r, true)};
        CHECK(prAppWindowCreate(r.app, &desc, &r.second) == PrStatus_Ok);
        PrAppWindowDesc popup{
            .extent = {32, 32},
            .role = PrAppWindowRole_Popup,
            .parent = r.window,
            .handler = handlerOf(r, true),
        };
        PrAppWindow* refused = nullptr;
        CHECK(prAppWindowCreate(r.app, &popup, &refused) == PrStatus_NotSupported);
        CHECK(refused == nullptr);
        prAppWake(r.app);
    };
    runHeadless(multiple);
    Run desktop;
    desktop.begin = [](Run& r) {
        CHECK(prAppWindowModel(r.app) == PrAppWindowModel_Desktop);
        PrAppWindowDesc popup{
            .extent = {32, 32},
            .role = PrAppWindowRole_Popup,
            .parent = r.window,
            .handler = handlerOf(r, true),
        };
        CHECK(prAppWindowCreate(r.app, &popup, &r.second) == PrStatus_Ok);
        prAppWake(r.app);
    };
    runHeadless(desktop);
}

TEST_CASE("a process outside an application bundle posts no notification, and does not crash trying") {
    PrAppOptions options{.headless = true, .windowModel = PrAppWindowModel_Desktop};
    PrApp* app = prAppAppKitCreate(&options);
    PrAppNotificationDesc desc{.title = "Title", .body = "Body", .tag = 1};
    CHECK(prAppNotificationPost(app, &desc) == PrStatus_InvalidState);
    prAppDestroy(app);
    Run run;
    run.begin = [](Run& r) {
        PrAppNotificationDesc desc{.title = "Title", .body = "Body", .tag = 1};
        CHECK(prAppNotificationPost(r.app, &desc) == PrStatus_NotSupported);
        // A window that is never shown is never the active window.
        CHECK_FALSE(prAppWindowIsActive(r.window));
        prAppWake(r.app);
    };
    runHeadless(run);
}

TEST_CASE("input that a callback's own run loop dispatches arrives after the callback, in order") {
    Run run;
    run.begin = [](Run& r) {
        NSView* view = viewOf(r.window);
        // A modal panel that the callback opens runs a run loop of its own, which dispatches input and the main queue's
        // blocks, as this one does.
        r.starting = true;
        [view keyDown:keyA(view.window, NSEventTypeKeyDown)];
        [view mouseDown:leftMouse(view.window, NSEventTypeLeftMouseDown)];
        [view mouseUp:leftMouse(view.window, NSEventTypeLeftMouseUp)];
        prAppWake(r.app);
        [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
        r.starting = false;
    };
    runHeadless(run);
    CHECK(!run.nested);
    CHECK(kindsOf(run.events) == Array<PrAppEventKind>({
        PrAppEventKind_KeyDown, PrAppEventKind_PointerDown, PrAppEventKind_PointerUp,
    }));
    CHECK(run.seen == Array<String>({"start", "open", "wake", "shutdown"}));
}

TEST_CASE("a key with Option held means the key that it types without Option, as Alt-1 means 1 on other systems") {
    Run run;
    run.begin = [](Run& r) {
        NSView* view = viewOf(r.window);
        const NSEventType types[] = {NSEventTypeKeyDown, NSEventTypeKeyUp};
        for (NSEventType type : types) {
            NSEvent* optionOne = [NSEvent keyEventWithType:type location:NSZeroPoint
                modifierFlags:NSEventModifierFlagOption timestamp:0 windowNumber:view.window.windowNumber context:nil
                characters:@"¡" charactersIgnoringModifiers:@"1" isARepeat:NO keyCode:18];
            if (type == NSEventTypeKeyDown)
                [view keyDown:optionOne];
            else
                [view keyUp:optionOne];
        }
        prAppWake(r.app);
    };
    runHeadless(run);
    REQUIRE(kindsOf(run.events) == Array<PrAppEventKind>({PrAppEventKind_KeyDown, PrAppEventKind_KeyUp}));
    for (const PrAppEvent& e : run.events) {
        if (e.kind != PrAppEventKind_KeyDown && e.kind != PrAppEventKind_KeyUp)
            continue;
        CHECK(e.keyboard.key == PrAppKey('1'));
        CHECK(e.keyboard.code == PrAppKeyCode_Digit1);
        CHECK(e.modifiers == PrAppModifierFlag_Alt);
    }
}

TEST_CASE("a pen's press reports its pressure, tilt and twist, and its eraser presses the eraser button") {
    Run run;
    run.begin = [](Run& r) {
        NSView* view = viewOf(r.window);
        // A press of a pen on a tablet, as the tablet's driver posts it.
        auto pen = [&](CGEventType type) {
            CGEventRef source = CGEventCreateMouseEvent(nullptr, type, CGPointMake(10, 10), kCGMouseButtonLeft);
            CGEventSetIntegerValueField(source, kCGMouseEventSubtype, kCGEventMouseSubtypeTabletPoint);
            CGEventSetDoubleValueField(source, kCGMouseEventPressure, 0.75);
            CGEventSetDoubleValueField(source, kCGTabletEventTiltX, 0.5);
            CGEventSetDoubleValueField(source, kCGTabletEventTiltY, 0.25);
            CGEventSetDoubleValueField(source, kCGTabletEventRotation, 90);
            NSEvent* event = [NSEvent eventWithCGEvent:source];
            CFRelease(source);
            return event;
        };
        [view mouseDown:pen(kCGEventLeftMouseDown)];
        [view mouseUp:pen(kCGEventLeftMouseUp)];
        // The pen turns to its eraser as it comes near the tablet.
        CGEventRef near = CGEventCreate(nullptr);
        CGEventSetType(near, kCGEventTabletProximity);
        CGEventSetIntegerValueField(near, kCGTabletProximityEventEnterProximity, 1);
        CGEventSetIntegerValueField(near, kCGTabletProximityEventPointerType, NSPointingDeviceTypeEraser);
        [view tabletProximity:[NSEvent eventWithCGEvent:near]];
        CFRelease(near);
        [view mouseDown:pen(kCGEventLeftMouseDown)];
        [view mouseUp:pen(kCGEventLeftMouseUp)];
        prAppWake(r.app);
    };
    runHeadless(run);
    REQUIRE(kindsOf(run.events) == Array<PrAppEventKind>({
        PrAppEventKind_PointerDown, PrAppEventKind_PointerUp, PrAppEventKind_PointerDown, PrAppEventKind_PointerUp,
    }));
    // The pointer's crossing into the view comes first.
    Array<PrAppEvent> presses;
    for (const PrAppEvent& event : run.events) {
        if (event.kind == PrAppEventKind_PointerDown || event.kind == PrAppEventKind_PointerUp)
            presses.push(event);
    }
    const PrAppEvent& press = presses[0];
    CHECK(press.pointer.type == PrAppPointerType_Pen);
    CHECK(press.pointer.id != kPrAppPointerMouse);
    CHECK(press.pointer.button == 0);
    CHECK(press.pointer.pressure == doctest::Approx(0.75).epsilon(0.01));
    CHECK(press.pointer.tilt.x == doctest::Approx(45).epsilon(0.01));
    CHECK(press.pointer.tilt.y == doctest::Approx(-22.5).epsilon(0.01));
    CHECK(press.pointer.twist == doctest::Approx(270));
    CHECK(presses[2].pointer.button == kPrAppPointerButtonEraser);
    CHECK(presses[3].pointer.button == kPrAppPointerButtonEraser);
}

TEST_CASE("the displays have distinct ids, the main display comes first, and a headless window has no position") {
    PrAppOptions options{.headless = true, .windowModel = PrAppWindowModel_Desktop};
    PrApp* app = prAppAppKitCreate(&options);
    uint32_t count = prAppDisplays(app, nullptr, 0);
    Array<PrAppDisplayInfo> displays(count);
    CHECK(prAppDisplays(app, displays.data(), count) == count);
    for (uint32_t i = 0; i < count; ++i) {
        CHECK(displays[i].id != kPrAppDisplayNone);
        CHECK(displays[i].main == (i == 0));
        CHECK(displays[i].display.pixelRatio > 0);
        CHECK(displays[i].workArea.left >= 0);
        CHECK(displays[i].workArea.top >= 0);
        CHECK(displays[i].workArea.right <= displays[i].extent.x);
        CHECK(displays[i].workArea.bottom <= displays[i].extent.y);
        for (uint32_t j = 0; j < i; ++j)
            CHECK(displays[j].id != displays[i].id);
    }
    prAppDestroy(app);
    Run run;
    run.begin = [](Run& r) {
        PrAppDisplayPosition position{};
        CHECK_FALSE(prAppWindowPosition(r.window, &position));
        CHECK_FALSE(prAppWindowPointToDisplay(r.window, PrVec2{1, 2}, &position));
        PrVec2 point{};
        CHECK_FALSE(prAppWindowPointFromDisplay(r.window, &position, &point));
        prAppWake(r.app);
    };
    runHeadless(run);
}

TEST_CASE("a window's extent stays within its limits, and a window of one extent cannot be resized") {
    Run run;
    run.begin = [](Run& r) {
        NSWindow* window = viewOf(r.window).window;
        // The window opened at 64 by 32 and has no limits.
        CHECK((window.styleMask & NSWindowStyleMaskResizable) != 0);
        prAppWindowSetExtentLimits(r.window, PrUVec2{80, 40}, PrUVec2{0, 0});
        CHECK(viewOf(r.window).bounds.size.width == 80);
        CHECK(viewOf(r.window).bounds.size.height == 40);
        prAppWindowSetExtent(r.window, PrUVec2{50, 500});
        CHECK(viewOf(r.window).bounds.size.width == 80);
        CHECK(viewOf(r.window).bounds.size.height == 500);
        prAppWindowSetExtentLimits(r.window, PrUVec2{100, 100}, PrUVec2{100, 100});
        CHECK(viewOf(r.window).bounds.size.width == 100);
        CHECK(viewOf(r.window).bounds.size.height == 100);
        CHECK((window.styleMask & NSWindowStyleMaskResizable) == 0);
        // Without limits, the window can be resized again, and it opens at its first extent.
        prAppWindowSetExtentLimits(r.window, PrUVec2{0, 0}, PrUVec2{0, 0});
        CHECK((window.styleMask & NSWindowStyleMaskResizable) != 0);
        prAppWindowSetExtent(r.window, PrUVec2{64, 32});
        // A headless window takes no state but Normal.
        prAppWindowSetState(r.window, PrAppWindowState_Maximized);
        CHECK(prAppWindowState(r.window) == PrAppWindowState_Normal);
        prAppWake(r.app);
    };
    runHeadless(run);
}

TEST_CASE("a custom title bar leaves room for the system's buttons, and its regions take the system's presses") {
    Run run;
    run.decoration = PrAppWindowDecoration_Custom;
    const PrAppTitleBarRegion regions[] = {
        {.rect = {0, 0, 64, 20}, .kind = PrAppTitleBarRegionKind_Close},
        {.rect = {0, 0, 10, 20}, .kind = PrAppTitleBarRegionKind_Client},
    };
    run.regions = Array<PrAppTitleBarRegion>(regions);
    run.wakeOnFrame = true;
    runHeadless(run);
    prThreadJoin(&run.waker);
    CHECK(run.titleBar.height > 0);
    CHECK(run.titleBar.leading > 0);
    CHECK(run.titleBar.flags == 0);

    // The first frame took the regions. A click in the close region asks the program to close, a press there that is
    // released off the button does nothing, and a press in the Client region within it reaches the program.
    Run second;
    second.decoration = PrAppWindowDecoration_Custom;
    second.regions = run.regions;
    second.secondFrame = [](Run& r) {
        NSView* view = viewOf(r.window);
        auto at = [&](NSEventType type, CGFloat x) {
            return [NSEvent mouseEventWithType:type location:[view convertPoint:NSMakePoint(x, 10) toView:nil]
                modifierFlags:0 timestamp:0 windowNumber:view.window.windowNumber context:nil eventNumber:0 clickCount:1
                pressure:1];
        };
        // The frame's callback is running, so the events that the presses make wait for it to return.
        [view mouseDown:at(NSEventTypeLeftMouseDown, 30)];
        [view mouseUp:at(NSEventTypeLeftMouseUp, 30)];
        [view mouseDown:at(NSEventTypeLeftMouseDown, 30)];
        [view mouseUp:at(NSEventTypeLeftMouseUp, 5)];
        [view mouseDown:at(NSEventTypeLeftMouseDown, 5)];
        prAppWake(r.app);
    };
    runHeadless(second);
    Array<PrAppEventKind> kinds = kindsOf(second.events);
    CHECK(occurrences(kinds, PrAppEventKind_CloseRequested) == 1);
    CHECK(occurrences(kinds, PrAppEventKind_PointerDown) == 1);
}

TEST_CASE("a window that stops being the key window releases the buttons and keys that are down") {
    Run run;
    run.begin = [](Run& r) {
        NSView* view = viewOf(r.window);
        [view keyDown:keyA(view.window, NSEventTypeKeyDown)];
        [view mouseDown:leftMouse(view.window, NSEventTypeLeftMouseDown)];
        // Another window becomes the key window, and the button's release goes to it, or arrives late.
        [NSNotificationCenter.defaultCenter postNotificationName:NSWindowDidResignKeyNotification object:view.window];
        [view mouseUp:leftMouse(view.window, NSEventTypeLeftMouseUp)];
        prAppWake(r.app);
    };
    runHeadless(run);
    CHECK(kindsOf(run.events) == Array<PrAppEventKind>({
        PrAppEventKind_KeyDown, PrAppEventKind_PointerDown, PrAppEventKind_PointerUp, PrAppEventKind_KeyUp,
    }));
    for (const PrAppEvent& event : run.events) {
        if (event.kind == PrAppEventKind_KeyUp)
            CHECK(event.keyboard.code == PrAppKeyCode_KeyA);
        if (event.kind == PrAppEventKind_PointerUp)
            CHECK(event.pointer.button == 0);
        // The mouse is the primary pointer, and has no pressure of its own: half while a button is down.
        bool pointer = event.kind == PrAppEventKind_PointerDown || event.kind == PrAppEventKind_PointerUp;
        if (pointer) {
            CHECK(event.pointer.type == PrAppPointerType_Mouse);
            CHECK(event.pointer.id == uint32_t(kPrAppPointerMouse));
            CHECK(event.pointer.primary);
            CHECK(event.pointer.pressure == (event.kind == PrAppEventKind_PointerDown ? 0.5f : 0.0f));
        }
    }
}

TEST_CASE("each window receives its own input, and the close button asks its program instead of closing it") {
    Run run;
    run.begin = [](Run& r) {
        PrAppWindowDesc desc{.title = "Second", .extent = {64, 32}, .handler = handlerOf(r, true)};
        REQUIRE(prAppWindowCreate(r.app, &desc, &r.second) == PrStatus_Ok);
        NSView* first = viewOf(r.window);
        NSView* second = viewOf(r.second);
        [first keyDown:keyA(first.window, NSEventTypeKeyDown)];
        [second mouseDown:leftMouse(second.window, NSEventTypeLeftMouseDown)];
        [second.window performClose:nil];
        prAppWake(r.app);
    };
    runHeadless(run);
    CHECK(kindsOf(run.events) == Array<PrAppEventKind>({PrAppEventKind_KeyDown}));
    CHECK(kindsOf(run.secondEvents) == Array<PrAppEventKind>({
        PrAppEventKind_PointerDown, PrAppEventKind_CloseRequested,
    }));
}

TEST_CASE("a window destroyed during a callback receives none of the events that waited for it") {
    Run run;
    run.begin = [](Run& r) {
        PrAppWindowDesc desc{.title = "Second", .extent = {64, 32}, .handler = handlerOf(r, true)};
        REQUIRE(prAppWindowCreate(r.app, &desc, &r.second) == PrStatus_Ok);
        NSView* second = viewOf(r.second);
        // The start callback is running, so the press waits for it to return.
        [second mouseDown:leftMouse(second.window, NSEventTypeLeftMouseDown)];
        prAppWindowDestroy(r.second);
        r.second = nullptr;
        prAppWake(r.app);
    };
    runHeadless(run);
    CHECK(run.secondEvents.isEmpty());
}

TEST_CASE("a clipboard read answers after the callback that started it, and a cancelled read never answers") {
    Run run;
    run.begin = [](Run& r) {
        prAppDataReadText(r.app, kPrAppDataSourceClipboard, [](void* context, PrStatus status, PrStringView) {
            CHECK(status == PrStatus_Ok);
            runOf(context).seen.push("read");
        }, &r);
        static int cancelled = 0;
        prAppDataReadImage(r.app, kPrAppDataSourceClipboard, [](void*, PrStatus, PrPixelView) { FAIL("a cancelled read answered"); },
            &cancelled);
        prAppCancel(r.app, &cancelled);
        r.seen.push("started");
        // The wake comes after the read's answer, since the main queue runs both in order.
        prAppWake(r.app);
    };
    runHeadless(run);
    Array<String> order;
    for (const String& call : run.seen) {
        if (call != "open")
            order.push(call);
    }
    CHECK(order == Array<String>({"start", "started", "read", "wake", "shutdown"}));
}

TEST_CASE("a drag over a window reports its content, and the window answers with its latest drop operation") {
    Run run;
    run.dropOperation = PrAppDragOperation_Copy;
    run.secondFrame = [](Run& r) {
        NSView* view = viewOf(r.window);
        PrTestDrag* drag = [[PrTestDrag alloc] init];
        drag.window = view.window;
        drag.board = [NSPasteboard pasteboardWithUniqueName];
        drag.mask = NSDragOperationCopy | NSDragOperationMove;
        NSPasteboardItem* item = [[NSPasteboardItem alloc] init];
        [item setString:@"hello" forType:NSPasteboardTypeString];
        [item setString:[NSURL fileURLWithPath:@"/usr"].absoluteString forType:NSPasteboardTypeFileURL];
        // A token that names no drag of this application carries no payload.
        uint64_t token = 7;
        [item setData:[NSData dataWithBytes:&token length:sizeof(token)] forType:@"org.prax.app.drag-payload"];
        [drag.board clearContents];
        [drag.board writeObjects:@[ item ]];
        // The first frame answered Copy, which the source allows.
        CHECK([view draggingEntered:drag] == NSDragOperationCopy);
        CHECK([view draggingUpdated:drag] == NSDragOperationCopy);
        // A source that allows only a link is refused.
        drag.mask = NSDragOperationLink;
        CHECK([view draggingUpdated:drag] == NSDragOperationNone);
        CHECK(![view prepareForDragOperation:drag]);
        drag.mask = NSDragOperationCopy;
        CHECK([view prepareForDragOperation:drag]);
        // The frame's callback is running, so the drop waits for it to return. Its content is read once the drag's
        // pasteboard is gone.
        CHECK([view performDragOperation:drag]);
        [drag.board releaseGlobally];
    };
    runHeadless(run);
    REQUIRE(kindsOf(run.events) == Array<PrAppEventKind>({
        PrAppEventKind_DragEnter, PrAppEventKind_DragOver, PrAppEventKind_DragOver, PrAppEventKind_Drop,
    }));
    CHECK(run.events[0].drag.position.x == 10);
    CHECK(run.events[0].drag.position.y == 10);
    CHECK(run.contents == Array<PrAppDataFormFlags>({
        PrAppDataFormFlag_Text | PrAppDataFormFlag_Files, PrAppDataFormFlag_Text | PrAppDataFormFlag_Files,
        PrAppDataFormFlag_Text | PrAppDataFormFlag_Files, PrAppDataFormFlag_Text | PrAppDataFormFlag_Files,
    }));
    CHECK(run.dropped == Array<String>({"drop 1", "text hello", "path /usr"}));
}

TEST_CASE("a drag that a frame starts without the primary button down ends at once with no operation") {
    Run run;
    run.wakeOnFrame = true;
    PrAppDragDesc desc{
        .content = {.forms = PrAppDataFormFlag_Text, .text = "dragged"},
        .operations = PrAppDragOperationFlag_Copy,
    };
    run.drag = &desc;
    runHeadless(run);
    prThreadJoin(&run.waker);
    CHECK(run.dropped == Array<String>({"end 0 forms 1"}));
}

TEST_CASE("the pointer enters before its first move, and a pointer that leaves with a button down leaves on release") {
    Run run;
    run.begin = [](Run& r) {
        NSView* view = viewOf(r.window);
        NSWindow* window = view.window;
        auto crossing = [&](NSEventType type) {
            return [NSEvent enterExitEventWithType:type location:NSMakePoint(10, 10) modifierFlags:0 timestamp:0
                windowNumber:window.windowNumber context:nil eventNumber:0 trackingNumber:0 userData:nullptr];
        };
        // A move with no crossing before it enters first, and an enter that follows adds nothing.
        [view mouseMoved:leftMouse(window, NSEventTypeMouseMoved)];
        [view mouseEntered:crossing(NSEventTypeMouseEntered)];
        [view mouseExited:crossing(NSEventTypeMouseExited)];
        // A press that leaves leaves once released, and one that comes back before its release never left.
        [view mouseEntered:crossing(NSEventTypeMouseEntered)];
        [view mouseDown:leftMouse(window, NSEventTypeLeftMouseDown)];
        [view mouseExited:crossing(NSEventTypeMouseExited)];
        [view mouseEntered:crossing(NSEventTypeMouseEntered)];
        [view mouseExited:crossing(NSEventTypeMouseExited)];
        [view mouseUp:leftMouse(window, NSEventTypeLeftMouseUp)];
        prAppWake(r.app);
    };
    runHeadless(run);
    CHECK(allKindsOf(run.events) == Array<PrAppEventKind>({
        PrAppEventKind_PointerEnter,
        PrAppEventKind_PointerMove,
        PrAppEventKind_PointerLeave,
        PrAppEventKind_PointerEnter,
        PrAppEventKind_PointerDown,
        PrAppEventKind_PointerUp,
        PrAppEventKind_PointerLeave,
    }));
}

TEST_CASE("a window hides and shows, and a headless application has no dialogs, hotkeys or pointer position") {
    Run run;
    run.begin = [](Run& r) {
        CHECK(prAppWindowIsVisible(r.window));
        prAppWindowSetVisible(r.window, false);
        CHECK_FALSE(prAppWindowIsVisible(r.window));
        prAppWindowSetVisible(r.window, true);
        CHECK(prAppWindowIsVisible(r.window));
        PrAppFileDialogDesc desc{.kind = PrAppFileDialogKind_Open};
        prAppFileDialog(r.app, &desc, [](void* context, PrStatus status, PrAppDataSource source) {
            Run& answered = runOf(context);
            bool none = source == kPrAppDataSourceNone;
            answered.seen.push(status == PrStatus_NotSupported && none ? "dialog" : "dialog?");
            prAppWake(answered.app);
        }, &r);
        CHECK(prAppHotkeyRegister(r.app, {.code = PrAppKeyCode_Space, .modifiers = PrAppModifierFlag_Control}, 1) ==
            PrStatus_NotSupported);
        CHECK(prAppRevealPath(r.app, "/no/such/path") == PrStatus_NotFound);
        PrAppDisplayPosition at{};
        CHECK_FALSE(prAppPointerPosition(r.app, &at));
    };
    runHeadless(run);
    CHECK(occurrences(run.seen, "dialog") == 1);
}

TEST_CASE("a timer fires after its delay and then at its interval, and a cancelled timer stops firing") {
    struct Timers {
        Array<uint64_t> fired;
    } timers;
    PrAppOptions options{.headless = true, .windowModel = PrAppWindowModel_Desktop};
    PrApp* app = prAppAppKitCreate(&options);
    prAppRun(app, {
        .context = &timers,
        .start = [](void*, PrApp* app) {
            prAppTimerSet(app, 1, 0.01, 0.01);
            prAppTimerSet(app, 2, 0.02, 0);
            prAppTimerSet(app, 3, 0.01, 0);
            prAppTimerCancel(app, 3);
            CHECK(prAppLifecycle(app) == PrAppLifecycle_Background);
            return true;
        },
        .event = [](void* context, PrApp* app, const PrAppEvent* event) {
            if (event->kind != PrAppEventKind_Timer)
                return;
            Array<uint64_t>& fired = static_cast<Timers*>(context)->fired;
            fired.push(event->tag);
            if (occurrences(fired, 1u) == 3)
                prAppStop(app);
        },
        .shutdown = [](void*, PrApp*) {},
    });
    prAppDestroy(app);
    CHECK(occurrences(timers.fired, 1u) == 3);
    CHECK(occurrences(timers.fired, 3u) == 0);
    CHECK(occurrences(timers.fired, 2u) <= 1);
}

TEST_CASE("a key's label comes from the layout, a key that types nothing shows its name, and power has a level") {
    PrAppOptions options{.headless = true, .windowModel = PrAppWindowModel_Desktop};
    PrApp* app = prAppAppKitCreate(&options);
    PrStringView letter = prAppKeyCodeLabel(app, PrAppKeyCode_KeyA);
    CHECK(letter.size != 0);
    CHECK(prAppKeyCodeLabel(app, PrAppKeyCode_Enter) == "Enter");
    CHECK(prAppKeyCodeLabel(app, PrAppKeyCode_Space) == "Space");

    PrAppPower power = prAppPower(app);
    CHECK((power.level == -1.0f || (power.level >= 0.0f && power.level <= 1.0f)));

    // A test does not trash a real file, which would land in the user's Trash.
    CHECK(prAppTrashPath(app, "/no/such/path") == PrStatus_NotFound);
    prAppDestroy(app);
}

TEST_CASE("each display has a name and a refresh rate, and a window takes the roles, ratios and states it is given") {
    Run run;
    run.begin = [](Run& r) {
        PrAppDisplayInfo displays[8];
        uint32_t count = prAppDisplays(r.app, displays, 8);
        for (uint32_t i = 0; i < count && i < 8; ++i) {
            CHECK(displays[i].name.size != 0);
            CHECK(displays[i].refreshRate > 0);
        }
        prAppWindowSetAspectRatio(r.window, 2.0f);
        CHECK(viewOf(r.window).window.contentAspectRatio.width == 2);
        prAppWindowSetAspectRatio(r.window, 0);
        prAppWindowSetOpacity(r.window, 0.5f);
        CHECK(viewOf(r.window).window.alphaValue == 0.5);
        prAppWindowSetKeepAbove(r.window, true);
        CHECK(viewOf(r.window).window.level == NSFloatingWindowLevel);
        prAppWindowSetDocument(r.window, "/tmp/document.txt", true);
        CHECK(viewOf(r.window).window.documentEdited);
        CHECK_FALSE(prAppWindowIsOccluded(r.window));

        PrAppWindowHandler quiet{
            .context = nullptr,
            .open = [](void*, PrAppWindow*, const PrAppFrame*) { return true; },
            .frame = [](void*, PrAppWindow*, const PrAppFrame*) { return PrAppOutput{}; },
            .event = [](void*, PrAppWindow*, const PrAppEvent*) {},
        };
        PrAppWindow* dialog = nullptr;
        PrAppWindowDesc sheet{
            .extent = {32, 16},
            .role = PrAppWindowRole_Dialog,
            .parent = r.window,
            .modal = true,
            .handler = quiet,
        };
        REQUIRE(prAppWindowCreate(r.app, &sheet, &dialog) == PrStatus_Ok);
        PrAppWindow* popup = nullptr;
        PrAppWindowDesc menu{
            .extent = {40, 80},
            .role = PrAppWindowRole_Popup,
            .parent = r.window,
            .popup = {.anchor = {4, 4, 20, 12}, .adjust = PrAppPopupAdjustFlag_Flip},
            .handler = quiet,
        };
        REQUIRE(prAppWindowCreate(r.app, &menu, &popup) == PrStatus_Ok);
        CHECK_FALSE(viewOf(popup).window.canBecomeKeyWindow);
        PrAppWindow* bare = nullptr;
        PrAppWindowDesc borderless{.extent = {16, 16}, .decoration = PrAppWindowDecoration_None, .handler = quiet};
        REQUIRE(prAppWindowCreate(r.app, &borderless, &bare) == PrStatus_Ok);
        CHECK((viewOf(bare).window.styleMask & NSWindowStyleMaskTitled) == 0);
        CHECK(viewOf(bare).window.canBecomeKeyWindow);
        prAppWindowDestroy(bare);
        prAppWindowDestroy(popup);
        prAppWindowDestroy(dialog);
        prAppWake(r.app);
    };
    runHeadless(run);
}

TEST_CASE("a headless window has no share sheet, and its share answers after the callback that started it") {
    Run run;
    run.begin = [](Run& r) {
        PrAppDataContent content{.forms = PrAppDataFormFlag_Text, .text = "shared"};
        prAppWindowShare(r.window, &content, PrRect{0, 0, 10, 10}, [](void* context, PrStatus status) {
            Run& answered = runOf(context);
            answered.seen.push(status == PrStatus_NotSupported ? "share" : "share?");
            prAppWake(answered.app);
        }, &r);
        r.seen.push("asked");
    };
    runHeadless(run);
    // The answer comes after the callback that asked, and once, whatever the window's open does meanwhile.
    uint32_t asked = run.seen.find("asked");
    uint32_t shared = run.seen.find("share");
    CHECK(asked != kArrayMissing);
    CHECK(asked < shared);
    CHECK(occurrences(run.seen, "share") == 1);
}

TEST_CASE("an editing item sends its tag to the window, and a headless application has menus but no status items") {
    Run run;
    run.begin = [](Run& r) {
        PrAppMenuItem fileItems[] =
            {{.tag = 11, .title = "Open", .key = PrAppKey('O'), .modifiers = PrAppModifierFlag_Super}};
        PrAppMenu file{.tag = 1, .title = "File", .items = fileItems, .count = 1};
        PrAppMenuItem barItems[] = {{.submenu = &file}};
        PrAppMenu bar{.tag = 0, .items = barItems, .count = 1};
        prAppSetMenuBar(r.app, &bar);
        prAppWindowSetMenuBar(r.window, &bar);
        prAppWindowShowMenu(r.window, &file, PrVec2{4, 4});
        prAppWindowSetMenuBar(r.window, nullptr);

        PrAppStatusItemDesc desc{.tag = 3, .title = "Prax"};
        PrAppStatusItem* item = nullptr;
        CHECK(prAppStatusItemCreate(r.app, &desc, &item) == PrStatus_NotSupported);

        NSMenuItem* copy = [[NSMenuItem alloc] initWithTitle:@"Copy" action:@selector(copy:) keyEquivalent:@"c"];
        copy.tag = 42;
        [viewOf(r.window) performSelector:@selector(copy:) withObject:copy];
        prAppWake(r.app);
    };
    runHeadless(run);
    bool copied = false;
    for (const PrAppEvent& event : run.events)
        copied = copied || (event.kind == PrAppEventKind_MenuItem && event.tag == 42);
    CHECK(copied);
}
