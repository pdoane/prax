// The UIKit backend's device: power, keeping awake, haptics, the motion sensors, notifications and the badge.

#include "app/backends/uikit/src/uikit_device.h"

#include "pr/alloc.h"
#include "pr/integer.h"

#import <CoreMotion/CoreMotion.h>
#import <UserNotifications/UserNotifications.h>

// The device's services of an application: its motion manager, made on first use, and its answers from the notification
// center, whose delegate it is while the application runs.
@interface PrUiKitDevice : NSObject <UNUserNotificationCenterDelegate>
@property(nonatomic) PrApp* app;
@property(nonatomic, strong) CMMotionManager* motion;
@property(nonatomic) uint8_t permission;    // notification permission, an UiKitPermission
@property(nonatomic, strong) NSMutableArray<UNNotificationRequest*>* waiting;
@property(nonatomic) BOOL badged;           // a badge waits for the permission
@property(nonatomic) NSInteger badge;
- (void)ask;
@end

namespace pr_impl {

namespace {

// The state of the user's permission for notifications and the badge. The application asks at the first post or badge.
enum {
    UiKitPermission_Unasked,
    UiKitPermission_Asking,
    UiKitPermission_Granted,
    UiKitPermission_Refused,
};
typedef uint8_t UiKitPermission;

// The most notifications that wait for the user's answer. If more are posted, the oldest is dropped.
constexpr NSUInteger kWaitingNotificationsMax = 16;

// The key in a notification's user info that holds its tag.
NSString* const kNotificationTagKey = @"tag";

// Standard gravity, which an accelerometer reports in units of.
constexpr double kGravity = 9.80665;

PrUiKitDevice* deviceOf(PrApp* app) {
    UiKitApp* uiKit = static_cast<UiKitApp*>(app);
    if (uiKit->device == nullptr) {
        PrUiKitDevice* device = [[PrUiKitDevice alloc] init];
        device.app = app;
        uiKit->device = (__bridge_retained void*)device;
    }
    return (__bridge PrUiKitDevice*)uiKit->device;
}

// Delivers a reading of `sensor` to the application.
void deliverReading(PrApp* app, PrAppSensor sensor, float x, float y, float z, float w = 0) {
    PrUiKitHost* host = uiKitHostOf(app);
    if (!host.running)
        return;
    PrAppEvent event{};
    event.kind = PrAppEventKind_Sensor;
    event.time = uiKitUptime();
    event.sensor.sensor = sensor;
    event.sensor.values[0] = x;
    event.sensor.values[1] = y;
    event.sensor.values[2] = z;
    event.sensor.values[3] = w;
    [host deliver:event to:nullptr];
}

} // namespace

PrAppPower uiKitPower(const PrApp*) {
    UIDevice* device = UIDevice.currentDevice;
    device.batteryMonitoringEnabled = YES;
    PrAppPower power{.source = PrAppPowerSource_Unknown, .level = -1};
    switch (device.batteryState) {
        case UIDeviceBatteryStateUnplugged: power.source = PrAppPowerSource_Battery; break;
        case UIDeviceBatteryStateCharging:
            power.source = PrAppPowerSource_External;
            power.charging = true;
            break;
        case UIDeviceBatteryStateFull: power.source = PrAppPowerSource_External; break;
        default: break;
    }
    if (device.batteryLevel >= 0)
        power.level = device.batteryLevel;
    power.lowPower = NSProcessInfo.processInfo.lowPowerModeEnabled;
    return power;
}

// The system sleeps only while the display does, so either flag keeps the display awake.
void uiKitSetAwake(PrApp* app, PrAppAwakeFlags flags) {
    if (!uiKitHostOf(app).headless)
        UIApplication.sharedApplication.idleTimerDisabled = flags != 0;
}

void uiKitHapticPlay(PrApp* app, PrAppHaptic haptic) {
    UIView* view = uiKitHostOf(app).windows.firstObject.view;
    if (view == nil || view.window == nil)
        return;
    UIImpactFeedbackStyle impact = UIImpactFeedbackStyleLight;
    UINotificationFeedbackType outcome = UINotificationFeedbackTypeSuccess;
    switch (haptic) {
        case PrAppHaptic_Selection:
            [[UISelectionFeedbackGenerator feedbackGeneratorForView:view] selectionChanged];
            return;
        case PrAppHaptic_Light: break;
        case PrAppHaptic_Medium: impact = UIImpactFeedbackStyleMedium; break;
        case PrAppHaptic_Heavy: impact = UIImpactFeedbackStyleHeavy; break;
        case PrAppHaptic_Success: break;
        case PrAppHaptic_Warning: outcome = UINotificationFeedbackTypeWarning; break;
        default: outcome = UINotificationFeedbackTypeError; break;
    }
    if (haptic <= PrAppHaptic_Heavy)
        [[UIImpactFeedbackGenerator feedbackGeneratorWithStyle:impact forView:view] impactOccurred];
    else
        [[UINotificationFeedbackGenerator feedbackGeneratorForView:view] notificationOccurred:outcome];
}

// Core Motion's axes are the device's, as the event's are. Its accelerometer reports in units of gravity, with the
// device at rest reporting gravity's pull, and the event reports the force that holds the device up, the reverse.
PrStatus uiKitSensorEnable(PrApp* app, PrAppSensor sensor, bool enable, double interval) {
    PrUiKitDevice* device = deviceOf(app);
    if (device.motion == nil)
        device.motion = [[CMMotionManager alloc] init];
    CMMotionManager* motion = device.motion;
    NSOperationQueue* main = NSOperationQueue.mainQueue;
    switch (sensor) {
        case PrAppSensor_Accelerometer:
            if (!motion.accelerometerAvailable)
                return PrStatus_NotSupported;
            if (!enable) {
                [motion stopAccelerometerUpdates];
                break;
            }
            motion.accelerometerUpdateInterval = interval;
            [motion startAccelerometerUpdatesToQueue:main withHandler:^(CMAccelerometerData* data, NSError*) {
                CMAcceleration a = data.acceleration;
                deliverReading(app, sensor, float(-a.x * kGravity), float(-a.y * kGravity), float(-a.z * kGravity));
            }];
            break;
        case PrAppSensor_Gyroscope:
            if (!motion.gyroAvailable)
                return PrStatus_NotSupported;
            if (!enable) {
                [motion stopGyroUpdates];
                break;
            }
            motion.gyroUpdateInterval = interval;
            [motion startGyroUpdatesToQueue:main withHandler:^(CMGyroData* data, NSError*) {
                CMRotationRate r = data.rotationRate;
                deliverReading(app, sensor, float(r.x), float(r.y), float(r.z));
            }];
            break;
        case PrAppSensor_Magnetometer:
            if (!motion.magnetometerAvailable)
                return PrStatus_NotSupported;
            if (!enable) {
                [motion stopMagnetometerUpdates];
                break;
            }
            motion.magnetometerUpdateInterval = interval;
            [motion startMagnetometerUpdatesToQueue:main withHandler:^(CMMagnetometerData* data, NSError*) {
                CMMagneticField m = data.magneticField;
                deliverReading(app, sensor, float(m.x), float(m.y), float(m.z));
            }];
            break;
        case PrAppSensor_Attitude:
            if (!motion.deviceMotionAvailable)
                return PrStatus_NotSupported;
            if (!enable) {
                [motion stopDeviceMotionUpdates];
                break;
            }
            motion.deviceMotionUpdateInterval = interval;
            [motion startDeviceMotionUpdatesToQueue:main withHandler:^(CMDeviceMotion* data, NSError*) {
                CMQuaternion q = data.attitude.quaternion;
                deliverReading(app, sensor, float(q.x), float(q.y), float(q.z), float(q.w));
            }];
            break;
        default: return PrStatus_NotSupported;
    }
    return PrStatus_Ok;
}

PrStatus uiKitNotificationPost(PrApp* app, const PrAppNotificationDesc* desc) {
    PrUiKitHost* host = uiKitHostOf(app);
    if (!host.running)
        return PrStatus_InvalidState;
    // A headless application posts nothing, because the permission request would go to a user who never sees it.
    if (host.headless)
        return PrStatus_NotSupported;
    PrUiKitDevice* device = deviceOf(app);
    if (device.permission == UiKitPermission_Refused)
        return PrStatus_NotAllowed;
    UNMutableNotificationContent* content = [[UNMutableNotificationContent alloc] init];
    content.title = uiKitStringOf(desc->title);
    content.body = uiKitStringOf(desc->body);
    content.userInfo = @{kNotificationTagKey: @(desc->tag)};
    UNNotificationRequest* request = [UNNotificationRequest requestWithIdentifier:NSUUID.UUID.UUIDString content:content
        trigger:nil];
    if (device.permission == UiKitPermission_Granted) {
        [UNUserNotificationCenter.currentNotificationCenter addNotificationRequest:request withCompletionHandler:nil];
        return PrStatus_Ok;
    }
    if (device.waiting == nil)
        device.waiting = [NSMutableArray array];
    if (device.waiting.count == kWaitingNotificationsMax)
        [device.waiting removeObjectAtIndex:0];
    [device.waiting addObject:request];
    [device ask];
    return PrStatus_Ok;
}

// The system's badge is a count, so a badge that is not a number shows as 1.
void uiKitSetBadge(PrApp* app, PrStringView badge) {
    if (uiKitHostOf(app).headless)
        return;
    PrUiKitDevice* device = deviceOf(app);
    uint64_t count = 0;
    if (badge.size != 0 && !prU64Parse(badge.data, badge.size, &count, 10))
        count = 1;
    device.badge = NSInteger(MIN(count, uint64_t(INT32_MAX)));
    if (device.permission == UiKitPermission_Granted) {
        [UNUserNotificationCenter.currentNotificationCenter setBadgeCount:device.badge withCompletionHandler:nil];
        return;
    }
    device.badged = YES;
    [device ask];
}

void uiKitDeviceStart(PrApp* app) {
    if (!uiKitHostOf(app).headless)
        UNUserNotificationCenter.currentNotificationCenter.delegate = deviceOf(app);
}

void uiKitDeviceStop(PrApp* app) {
    UiKitApp* uiKit = static_cast<UiKitApp*>(app);
    if (uiKit->device == nullptr)
        return;
    PrUiKitDevice* device = (__bridge PrUiKitDevice*)uiKit->device;
    [device.motion stopAccelerometerUpdates];
    [device.motion stopGyroUpdates];
    [device.motion stopMagnetometerUpdates];
    [device.motion stopDeviceMotionUpdates];
    if (uiKitHostOf(app).headless)
        return;
    UNUserNotificationCenter* center = UNUserNotificationCenter.currentNotificationCenter;
    if (center.delegate == device)
        center.delegate = nil;
}

void uiKitDeviceDestroy(PrApp* app) {
    UiKitApp* uiKit = static_cast<UiKitApp*>(app);
    if (uiKit->device == nullptr)
        return;
    PrUiKitDevice* device = (__bridge_transfer PrUiKitDevice*)uiKit->device;
    device.app = nullptr;
    uiKit->device = nullptr;
}

} // namespace pr_impl

using namespace pr_impl;

@implementation PrUiKitDevice
// Asks the user once for the permission to post notifications and show the badge.
- (void)ask {
    if (self.permission != UiKitPermission_Unasked)
        return;
    self.permission = UiKitPermission_Asking;
    __weak PrUiKitDevice* weak = self;
    UNAuthorizationOptions options = UNAuthorizationOptionAlert | UNAuthorizationOptionBadge;
    [UNUserNotificationCenter.currentNotificationCenter requestAuthorizationWithOptions:options
        completionHandler:^(BOOL granted, NSError*) {
            dispatch_async(dispatch_get_main_queue(), ^{
                [weak answered:granted];
            });
        }];
}
// Records the user's answer. If the user allowed notifications, posts those that waited, and shows the badge.
- (void)answered:(BOOL)granted {
    self.permission = granted ? UiKitPermission_Granted : UiKitPermission_Refused;
    UNUserNotificationCenter* center = UNUserNotificationCenter.currentNotificationCenter;
    if (granted) {
        for (UNNotificationRequest* request in self.waiting)
            [center addNotificationRequest:request withCompletionHandler:nil];
        if (self.badged)
            [center setBadgeCount:self.badge withCompletionHandler:nil];
    }
    [self.waiting removeAllObjects];
    self.badged = NO;
}
// Shows a notification even while the application is active, because the program chose to post it.
- (void)userNotificationCenter:(UNUserNotificationCenter*)center willPresentNotification:(UNNotification*)notification
        withCompletionHandler:(void (^)(UNNotificationPresentationOptions))completionHandler {
    (void)center;
    (void)notification;
    completionHandler(UNNotificationPresentationOptionBanner | UNNotificationPresentationOptionList);
}
// Delivers a tap on a notification, which opened the application.
- (void)userNotificationCenter:(UNUserNotificationCenter*)center
        didReceiveNotificationResponse:(UNNotificationResponse*)response
        withCompletionHandler:(void (^)(void))completionHandler {
    (void)center;
    if ([response.actionIdentifier isEqualToString:UNNotificationDefaultActionIdentifier]) {
        id tag = response.notification.request.content.userInfo[kNotificationTagKey];
        uint64_t value = [tag isKindOfClass:NSNumber.class] ? [tag unsignedLongLongValue] : 0;
        __weak PrUiKitDevice* weak = self;
        dispatch_async(dispatch_get_main_queue(), ^{
            PrUiKitDevice* device = weak;
            PrUiKitHost* host = device.app != nullptr ? uiKitHostOf(device.app) : nil;
            if (!host.running)
                return;
            PrAppEvent event{};
            event.kind = PrAppEventKind_Notification;
            event.time = uiKitUptime();
            event.tag = value;
            [host deliver:event to:nullptr];
        });
    }
    completionHandler();
}
@end
