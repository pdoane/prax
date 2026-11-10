// The Cocoa backend's shell and device: notifications, URLs, the trash, recent documents, the Dock, sharing, power,
// keeping awake and haptics.

#include "app/backends/cocoa/src/app_cocoa_shell.h"

#include "app/backends/cocoa/src/app_cocoa_data.h"
#include "app/backends/cocoa/src/app_cocoa_input.h"

namespace pr_impl {

namespace {

// Holds or releases the power assertion `type` in `held`.
void holdAssertion(IOPMAssertionID& held, CFStringRef type, bool hold) {
    if (hold && held == kIOPMNullAssertionID)
        IOPMAssertionCreateWithName(type, kIOPMAssertionLevelOn, CFSTR("Prax application"), &held);
    if (!hold && held != kIOPMNullAssertionID) {
        IOPMAssertionRelease(held);
        held = kIOPMNullAssertionID;
    }
}

// Returns the items of `content` that a share picker offers: its text, its URLs, its files and its image.
NSArray* shareItemsOf(const PrAppDataContent& content) {
    NSMutableArray* items = [NSMutableArray array];
    if ((content.forms & PrAppDataFormFlag_Text) != 0 && content.text.size != 0)
        [items addObject:cocoaStringOf(content.text)];
    for (uint32_t i = 0; (content.forms & PrAppDataFormFlag_Urls) != 0 && i < content.urlCount; ++i) {
        if (NSURL* url = [NSURL URLWithString:cocoaStringOf(content.urls[i])])
            [items addObject:url];
    }
    for (uint32_t i = 0; (content.forms & PrAppDataFormFlag_Files) != 0 && i < content.pathCount; ++i)
        [items addObject:[NSURL fileURLWithPath:cocoaStringOf(content.paths[i])]];
    if ((content.forms & PrAppDataFormFlag_Image) != 0) {
        if (NSBitmapImageRep* rep = cocoaImageRepOf(content.image)) {
            NSImage* image = [[NSImage alloc] initWithSize:rep.size];
            [image addRepresentation:rep];
            [items addObject:image];
        }
    }
    return items;
}

} // namespace

PrStatus cocoaNotificationPost(PrApp* app, const PrAppNotificationDesc* desc) {
    PrCocoaHost* host = cocoaHostOf(app);
    if (!host.running)
        return PrStatus_InvalidState;
    // A headless application posts nothing, because the permission request would go to a user who never sees it.
    if (host.headless || !cocoaIsBundled())
        return PrStatus_NotSupported;
    return [host postNotification:desc];
}

PrStatus cocoaOpenUrl(PrApp*, PrStringView url) {
    NSString* string = prAppleNSString(url);
    NSURL* address = string != nil ? [NSURL URLWithString:string] : nil;
    if (address == nil)
        return PrStatus_Syntax;
    return [NSWorkspace.sharedWorkspace openURL:address] ? PrStatus_Ok : PrStatus_Io;
}

PrStatus cocoaRevealPath(PrApp* app, PrStringView path) {
    NSString* string = cocoaStringOf(path);
    if (![NSFileManager.defaultManager fileExistsAtPath:string])
        return PrStatus_NotFound;
    if (cocoaHostOf(app).headless)
        return PrStatus_NotSupported;
    [NSWorkspace.sharedWorkspace activateFileViewerSelectingURLs:@[ [NSURL fileURLWithPath:string] ]];
    return PrStatus_Ok;
}

PrStatus cocoaTrashPath(PrApp*, PrStringView path) {
    NSString* string = cocoaStringOf(path);
    if (![NSFileManager.defaultManager fileExistsAtPath:string])
        return PrStatus_NotFound;
    NSURL* url = [NSURL fileURLWithPath:string];
    return [NSFileManager.defaultManager trashItemAtURL:url resultingItemURL:nil error:nil] ? PrStatus_Ok : PrStatus_Io;
}

void cocoaRecentDocumentAdd(PrApp*, PrStringView path) {
    NSURL* url = [NSURL fileURLWithPath:cocoaStringOf(path)];
    [NSDocumentController.sharedDocumentController noteNewRecentDocumentURL:url];
}

void cocoaRecentDocumentsClear(PrApp*) {
    [NSDocumentController.sharedDocumentController clearRecentDocuments:nil];
}

void cocoaSetBadge(PrApp* app, PrStringView badge) {
    if (cocoaHostOf(app).headless)
        return;
    NSApp.dockTile.badgeLabel = badge.size != 0 ? cocoaStringOf(badge) : nil;
}

PrAppPower cocoaPower(const PrApp*) {
    PrAppPower result{.source = PrAppPowerSource_Unknown, .level = -1};
    result.lowPower = NSProcessInfo.processInfo.isLowPowerModeEnabled;
    CFTypeRef info = IOPSCopyPowerSourcesInfo();
    if (info == nullptr)
        return result;
    CFStringRef providing = IOPSGetProvidingPowerSourceType(info);
    if (providing != nullptr && CFEqual(providing, CFSTR(kIOPMACPowerKey)))
        result.source = PrAppPowerSource_External;
    else if (providing != nullptr && CFEqual(providing, CFSTR(kIOPMBatteryPowerKey)))
        result.source = PrAppPowerSource_Battery;
    CFArrayRef list = IOPSCopyPowerSourcesList(info);
    for (CFIndex i = 0; list != nullptr && i < CFArrayGetCount(list); ++i) {
        NSDictionary* description =
            (__bridge NSDictionary*)IOPSGetPowerSourceDescription(info, CFArrayGetValueAtIndex(list, i));
        if (![description[@kIOPSTypeKey] isEqual:@kIOPSInternalBatteryType])
            continue;
        double current = [description[@kIOPSCurrentCapacityKey] doubleValue];
        double maximum = [description[@kIOPSMaxCapacityKey] doubleValue];
        if (maximum > 0)
            result.level = float(current / maximum);
        result.charging = [description[@kIOPSIsChargingKey] boolValue];
        break;
    }
    if (list != nullptr)
        CFRelease(list);
    CFRelease(info);
    return result;
}

void cocoaSetAwake(PrApp* app, PrAppAwakeFlags flags) {
    CocoaApp* cocoa = static_cast<CocoaApp*>(app);
    holdAssertion(cocoa->displayAwake, kIOPMAssertionTypePreventUserIdleDisplaySleep, (flags & PrAppAwakeFlag_Display) != 0);
    holdAssertion(cocoa->systemAwake, kIOPMAssertionTypePreventUserIdleSystemSleep, (flags & PrAppAwakeFlag_System) != 0);
}

void cocoaHapticPlay(PrApp*, PrAppHaptic haptic) {
    NSHapticFeedbackPattern pattern = NSHapticFeedbackPatternGeneric;
    if (haptic == PrAppHaptic_Selection || haptic == PrAppHaptic_Light)
        pattern = NSHapticFeedbackPatternAlignment;
    else if (haptic == PrAppHaptic_Heavy)
        pattern = NSHapticFeedbackPatternLevelChange;
    [NSHapticFeedbackManager.defaultPerformer performFeedbackPattern:pattern
        performanceTime:NSHapticFeedbackPerformanceTimeNow];
}

void cocoaShare(PrAppWindow* window, const PrAppDataContent* content, PrRect anchor, PrAppShareFn done,
        void* PR_NULLABLE context) {
    PrApp* app = static_cast<CocoaWindow*>(window)->app;
    uint32_t id = appDataRequestsAdd(static_cast<CocoaApp*>(app)->requests, kPrAppDataSourceNone, AppDataAnswer_Status,
        (void (*)(void))done, context);
    PrCocoaWindow* object = cocoaObjectOf(window);
    NSArray* items = shareItemsOf(*content);
    if (object.host.headless || items.count == 0) {
        PrStatus status = object.host.headless ? PrStatus_NotSupported : PrStatus_InvalidData;
        cocoaAnswerSoon(app, appAnswerCreate(AppAnswerKind_Status, id, status));
        return;
    }
    PrCocoaShare* picker = [[PrCocoaShare alloc] init];
    picker.app = app;
    picker.request = id;
    picker.picker = [[NSSharingServicePicker alloc] initWithItems:items];
    picker.picker.delegate = picker;
    [object.host.shares addObject:picker];
    NSRect rect = NSMakeRect(CGFloat(anchor.left), CGFloat(anchor.top), CGFloat(anchor.right - anchor.left),
        CGFloat(anchor.bottom - anchor.top));
    [picker.picker showRelativeToRect:rect ofView:object.view preferredEdge:NSRectEdgeMaxY];
}

} // namespace pr_impl

using namespace pr_impl;

@implementation PrCocoaShare
// The user chose a service, or none, which ends the share either way.
- (void)sharingServicePicker:(NSSharingServicePicker*)sharingServicePicker
        didChooseSharingService:(NSSharingService*)service {
    (void)sharingServicePicker;
    (void)service;
    PrApp* app = self.app;
    PrCocoaHost* host = cocoaHostOf(app);
    cocoaAnswerSoon(app, appAnswerCreate(AppAnswerKind_Status, self.request, PrStatus_Ok));
    [host.shares removeObject:self];
}
@end
