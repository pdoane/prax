// The Cocoa backend's displays: their list, their properties, and positions on them.

#include "app/backends/cocoa/src/app_cocoa_display.h"

namespace pr_impl {

namespace {

// Returns how `screen` is turned. A screen whose rotation leaves it taller than wide is a portrait.
PrAppOrientation orientationOf(NSScreen* screen) {
    CGDirectDisplayID number = [screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue];
    double rotation = CGDisplayRotation(number);
    if (rotation == 90)
        return PrAppOrientation_Portrait;
    if (rotation == 180)
        return PrAppOrientation_LandscapeFlipped;
    if (rotation == 270)
        return PrAppOrientation_PortraitFlipped;
    return screen.frame.size.height > screen.frame.size.width ? PrAppOrientation_Portrait : PrAppOrientation_Landscape;
}

// Returns the connected screen. `name` is the screen's name, which the caller keeps for as long as the view must last.
PrAppDisplayInfo infoOf(NSScreen* screen, bool main, PrStringView name) {
    return PrAppDisplayInfo{
        .id = cocoaDisplayIdOf(screen),
        .name = name,
        .display = cocoaDisplayOf(screen, screen.backingScaleFactor),
        .extent = PrVec2{float(screen.frame.size.width), float(screen.frame.size.height)},
        .workArea = cocoaRectOnScreen(screen, screen.visibleFrame),
        .refreshRate = float(screen.maximumFramesPerSecond),
        .orientation = orientationOf(screen),
        .main = main,
    };
}

// Returns what the application reports of `screen`, whose name it keeps until the displays change. The first screen
// holds the menu bar.
PrAppDisplayInfo displayInfoOf(const PrApp* app, NSScreen* screen) {
    AppDisplayList& displays = static_cast<const CocoaApp*>(app)->displays;
    PrAppDisplayId id = cocoaDisplayIdOf(screen);
    PrStringView name{};
    if (!appDisplayListFind(displays, id, &name)) {
        PrRawString utf8 = prAppleNSStringToRawString(screen.localizedName);
        name = appDisplayListAdd(displays, id, prRawStringView(&utf8));
        prRawStringReset(&utf8);
    }
    return infoOf(screen, screen == NSScreen.screens.firstObject, name);
}

} // namespace

PrAppDisplayId cocoaDisplayIdOf(NSScreen* screen) {
    CGDirectDisplayID number = [screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue];
    CFUUIDRef uuid = CGDisplayCreateUUIDFromDisplayID(number);
    if (uuid == nullptr)
        return PrAppDisplayId(number) | (PrAppDisplayId(1) << 63);
    CFUUIDBytes bytes = CFUUIDGetUUIDBytes(uuid);
    CFRelease(uuid);
    uint64_t halves[2];
    static_assert(sizeof(halves) == sizeof(bytes), "a UUID is 16 bytes");
    prMemCopy(halves, &bytes, sizeof(bytes));
    PrAppDisplayId id = halves[0] ^ halves[1];
    return id != kPrAppDisplayNone ? id : 1;
}

NSScreen* PR_NULLABLE cocoaScreenOf(PrAppDisplayId id) {
    for (NSScreen* screen in NSScreen.screens) {
        if (cocoaDisplayIdOf(screen) == id)
            return screen;
    }
    return nil;
}

PrAppDisplay cocoaDisplayOf(NSScreen* PR_NULLABLE screen, CGFloat pixelRatio) {
    PrAppDisplay display = prAppDisplayDefault();
    display.pixelRatio = float(pixelRatio);
    if (screen != nil) {
        display.headroom = float(screen.maximumExtendedDynamicRangeColorComponentValue);
        display.potentialHeadroom = float(screen.maximumPotentialExtendedDynamicRangeColorComponentValue);
        if ([screen canRepresentDisplayGamut:NSDisplayGamutP3])
            display.gamut = PrColorSpace_DisplayP3;
    }
    return display;
}

PrRect cocoaRectOnScreen(NSScreen* screen, NSRect rect) {
    NSRect frame = screen.frame;
    float left = float(NSMinX(rect) - NSMinX(frame));
    float top = float(NSMaxY(frame) - NSMaxY(rect));
    return PrRect{left, top, left + float(rect.size.width), top + float(rect.size.height)};
}

void cocoaPlaceOnScreen(NSWindow* window, NSScreen* screen, PrVec2 position) {
    NSRect content = [window contentRectForFrameRect:window.frame];
    NSRect frame = screen.frame;
    NSRect work = screen.visibleFrame;
    CGFloat left = NSMinX(frame) + CGFloat(position.x);
    CGFloat top = NSMaxY(frame) - CGFloat(position.y);
    left = MAX(MIN(left, NSMaxX(work) - content.size.width), NSMinX(work));
    top = MIN(MAX(top, NSMinY(work) + content.size.height), NSMaxY(work));
    content.origin = NSMakePoint(left, top - content.size.height);
    [window setFrameOrigin:[window frameRectForContentRect:content].origin];
}

void cocoaCenterOnScreen(NSWindow* window, NSScreen* screen) {
    NSRect content = [window contentRectForFrameRect:window.frame];
    NSRect work = screen.visibleFrame;
    float x = float((work.size.width - content.size.width) / 2 + NSMinX(work) - NSMinX(screen.frame));
    float y = float(NSMaxY(screen.frame) - NSMaxY(work) + (work.size.height - content.size.height) / 2);
    cocoaPlaceOnScreen(window, screen, PrVec2{x, y});
}

NSRect cocoaPopupFrame(NSWindow* parent, NSSize size, const PrAppPopupPlacement& placement) {
    // AppKit's global y grows upward from the bottom of the first screen, and the placement's grows downward.
    CGFloat top = NSMaxY(NSScreen.screens.firstObject.frame);
    auto downward = [top](NSRect rect) {
        return PrRect{float(NSMinX(rect)), float(top - NSMaxY(rect)), float(NSMaxX(rect)), float(top - NSMinY(rect))};
    };
    PrRect content = downward([parent contentRectForFrameRect:parent.frame]);
    const PrRect& a = placement.anchor;
    PrRect anchor{content.left + a.left, content.top + a.top, content.left + a.right, content.top + a.bottom};
    NSScreen* screen = parent.screen != nil ? parent.screen : NSScreen.mainScreen;
    bool rightToLeft = NSApp.userInterfaceLayoutDirection == NSUserInterfaceLayoutDirectionRightToLeft;
    PrAppPopupPlacement onScreen = placement;
    onScreen.anchor = anchor;
    PrRect placed = prAppPopupRect(&onScreen, PrVec2{float(size.width), float(size.height)},
        downward(screen.visibleFrame),
        PrAppLayoutDirection(rightToLeft ? PrAppLayoutDirection_RightToLeft : PrAppLayoutDirection_LeftToRight));
    return NSMakeRect(CGFloat(placed.left), top - CGFloat(placed.bottom), CGFloat(placed.right - placed.left),
        CGFloat(placed.bottom - placed.top));
}

bool cocoaDisplayPositionAt(NSPoint at, PrAppDisplayPosition* out) {
    NSScreen* nearest = nil;
    CGFloat nearestDistance = 0;
    for (NSScreen* screen in NSScreen.screens) {
        NSRect frame = screen.frame;
        CGFloat dx = MAX(MAX(NSMinX(frame) - at.x, at.x - NSMaxX(frame)), 0);
        CGFloat dy = MAX(MAX(NSMinY(frame) - at.y, at.y - NSMaxY(frame)), 0);
        CGFloat distance = dx * dx + dy * dy;
        if (nearest == nil || distance < nearestDistance) {
            nearest = screen;
            nearestDistance = distance;
        }
    }
    if (nearest == nil)
        return false;
    NSRect frame = nearest.frame;
    *out = PrAppDisplayPosition{
        .display = cocoaDisplayIdOf(nearest),
        .position = PrVec2{float(at.x - NSMinX(frame)), float(NSMaxY(frame) - at.y)},
    };
    return true;
}

bool cocoaPointerPosition(const PrApp* app, PrAppDisplayPosition* out) {
    return !cocoaHostOf(app).headless && cocoaDisplayPositionAt(NSEvent.mouseLocation, out);
}

uint32_t cocoaDisplays(const PrApp* app, PrAppDisplayInfo* PR_NULLABLE out, uint32_t capacity) {
    NSArray<NSScreen*>* screens = NSScreen.screens;
    for (NSUInteger i = 0; i < screens.count && i < capacity; ++i)
        out[i] = displayInfoOf(app, screens[i]);
    return uint32_t(screens.count);
}

bool cocoaDisplayInfo(const PrApp* app, PrAppDisplayId id, PrAppDisplayInfo* out) {
    NSScreen* screen = cocoaScreenOf(id);
    if (screen == nil)
        return false;
    *out = displayInfoOf(app, screen);
    return true;
}

} // namespace pr_impl
