// The UIKit backend's objects and state, which its files share: the application, its windows and their views.
//
// app_uikit.mm holds the application, its scenes and its windows. uikit_view.mm holds a window's view and its input,
// uikit_data.mm the clipboard, files, dialogs and sharing, and uikit_device.mm the device's power, haptics, sensors and
// notifications.

#pragma once

#include "pr/app_backend.h"
#include "pr/app_uikit.h"
#include "pr/atomic.h"
#include "pr/base.h"
#include "pr/raw_string.h"

#include "app/backends/shared/src/app_shared_data.h"
#include "app/backends/shared/src/app_shared_delivery.h"

// UIKit's views, windows and scenes, which the objects below hold.
#import <UIKit/UIKit.h>

PR_ASSUME_NONNULL_BEGIN

// The application and its windows, which the views name before their interfaces.
@class PrUiKitHost;
@class PrUiKitWindow;

// The view of a window, whose layer is the CAMetalLayer that the window's swap chain presents to. It takes the window's
// touches, gestures, keys, text and drops. It is the first responder while its window shows, and the on-screen keyboard
// shows while the program edits text: at other times its input view is empty, which keeps the keyboard down.
//
// It is the text input of an input method, whose document is the text that the method composes, since the program's own
// text stays the program's. The composition shows at the caret of the window's latest output.
#if TARGET_OS_TV
@interface PrUiKitView : UIView <UITextInput, UIGestureRecognizerDelegate>
#else
@interface PrUiKitView : UIView <UITextInput, UIPointerInteractionDelegate, UIDropInteractionDelegate,
UIGestureRecognizerDelegate>
#endif
@property(nonatomic, weak) PrUiKitWindow* owner;
@property(nonatomic) BOOL editing;                  // the window's latest output has text input
@property(nonatomic) PrAppTextInputKind inputKind;  // the kind of that text
@property(nonatomic) CGRect caret;                  // where that text's caret stands, in the view
@property(nonatomic) PrAppCursor cursor;            // the cursor of the window's latest output, which the pointer shows
@property(nonatomic) UIKeyboardType keyboardType;
@property(nonatomic) UIReturnKeyType returnKeyType;
@property(nonatomic, getter=isSecureTextEntry) BOOL secureTextEntry;
@property(nonatomic) UITextAutocorrectionType autocorrectionType;
@property(nonatomic) UITextAutocapitalizationType autocapitalizationType;
@property(nonatomic) UITextSpellCheckingType spellCheckingType;
- (void)setEditing:(BOOL)editing kind:(PrAppTextInputKind)kind;
@end

// The root view controller of a window: the system bars that it hides and the orientations that it allows.
@interface PrUiKitViewController : UIViewController
@property(nonatomic, weak) PrUiKitWindow* owner;
@end

// A window: its view, the UIWindow that shows it once the scene connects, its frames, and what is held down in it.
@interface PrUiKitWindow : NSObject
@property(nonatomic) PrAppWindow* PR_NULLABLE record;   // null once the window is destroyed
@property(nonatomic, weak) PrUiKitHost* host;
@property(nonatomic, strong) PrUiKitView* view;
@property(nonatomic, strong) PrUiKitViewController* controller;
@property(nonatomic, strong) UIWindow* PR_NULLABLE window;   // nil until the scene connects, and for a headless window
@property(nonatomic, weak) UIWindowScene* PR_NULLABLE scene; // the scene that shows the window, or nil
@property(nonatomic) BOOL waiting;                           // the window waits for a scene to connect
@property(nonatomic) BOOL prominent;                         // the window's scene opens in front
                                                             // (PrAppWindowPlacement_Center)
@property(nonatomic, strong) CADisplayLink* PR_NULLABLE displayLink;
@property(nonatomic, strong) NSTimer* PR_NULLABLE timer;        // a headless window's frame
@property(nonatomic, strong) NSTimer* PR_NULLABLE frameTimer;   // the frame at the time that the last frame asked for
@property(nonatomic) BOOL frameRequested;
@property(nonatomic) double lastFrame;
@property(nonatomic) CGSize framedExtent;           // the extent of the last frame
@property(nonatomic) BOOL shown;                    // the program shows the window (prAppWindowSetVisible)
@property(nonatomic) PrAppSystemBarFlags hiddenBars;
@property(nonatomic) CGRect keyboard;                  // the on-screen keyboard in the view, or CGRectNull
@property(nonatomic) PrAppDragOperation dropOperation; // the answer to a drop, from the latest output
- (void)frame;
- (PrAppFrame)frameAt:(double)now;
- (void)requestFrame;
- (void)requestFrameAt:(double)time;
- (void)stopFrames;
- (void)attach:(UIWindowScene*)scene;
- (void)detach;
- (void)layoutChanged;
- (void)close;
- (void)deliver:(const PrAppEvent&)event;
@end

// The application: its windows and their scenes, and the events that wait for a callback to return. Each window shows
// in a scene of its own.
@interface PrUiKitHost : NSObject
@property(nonatomic) PrApp* PR_NULLABLE app;    // null once the application is destroyed
@property(nonatomic) BOOL headless;
@property(nonatomic) BOOL running;
@property(nonatomic) BOOL inCallback;           // a callback is running, and no other may start
@property(nonatomic, strong) NSMutableArray<PrUiKitWindow*>* windows;
@property(nonatomic) PrAppOrientationFlags orientations;
@property(nonatomic) PrAppWindowModel requestedModel;        // the window model that the options asked for
@property(nonatomic) BOOL launched;                          // a scene has connected
@property(nonatomic, weak) UIWindowScene* PR_NULLABLE spare; // a connected scene that no window shows, or nil
@property(nonatomic, strong) NSMutableArray* presented;      // the dialogs that wait for the user, kept until then
- (void)started;
- (UIWindowScene* PR_NULLABLE)anyScene;
- (PrAppWindowModel)windowModel;
- (void)place:(PrUiKitWindow*)window;
- (void)closed:(UIWindowScene* PR_NULLABLE)scene;
- (void)connect:(UIWindowScene*)scene;
- (void)disconnect:(UIScene*)scene;
- (void)scenesChanged;
- (void)open:(NSSet<UIOpenURLContext*>*)contexts;
- (void)lifecycleMayChange;
- (PrAppLifecycle)lifecycle;
- (void)deliverKind:(PrAppEventKind)kind;
- (void)deliver:(const PrAppEvent&)event to:(PrAppWindow* PR_NULLABLE)window;
- (void)beginCallback;
- (void)endCallback;
- (void)forget:(PrAppWindow*)window;
- (void)stop;
- (void)setTimer:(uint64_t)tag delay:(double)delay interval:(double)interval;
- (void)cancelTimer:(uint64_t)tag;
- (void)startServices;
- (void)stopServices;

// Delivers `answer`, a UiKitAnswer that the host owns from here on, now or once no callback runs.
- (void)answer:(void*)answer;
@end

namespace pr_impl {

// The id of the only display, the device's own screen.
constexpr PrAppDisplayId kUiKitScreenId = 1;

// The state of an application, after its function table.
struct UiKitApp : PrApp {
    void* host;                 // the PrUiKitHost, retained
    void* PR_NULLABLE device;   // the PrUiKitDevice of the sensors and notifications, retained, made on first use
    PrAppHandler handler;       // the handler of the run
    Atomic<bool> wakePending;
    AppDataSources sources;
    AppDataRequests requests;
};

// The state of a window, after its function table.
struct UiKitWindow : PrAppWindow {
    void* object;               // the PrUiKitWindow, retained
    PrApp* app;
    AppWindowDelivery delivery;
    PrRawString title;
};

// Returns the host of an application, or the object of a window.
PrUiKitHost* uiKitHostOf(const PrApp* app);
PrUiKitWindow* uiKitObjectOf(const PrAppWindow* window);

// Returns the seconds since the system started, the clock of events and frames.
double uiKitUptime();

// Returns `text` as an NSString, or the empty string if it is not UTF-8.
NSString* uiKitStringOf(PrStringView text);

// Returns `string` as UTF-8, which the caller frees: the empty string for nil, and the null string if it is ill-formed.
PrRawString uiKitTextOf(NSString* PR_NULLABLE string);

// Returns the bytes of UTF-8 that the first `units` UTF-16 units of `string` take, or zero if they are ill-formed.
uint32_t uiKitUtf8Length(NSString* string, NSUInteger units);

// Returns the modifiers of UIKit's modifier flags.
PrAppModifierFlags uiKitModifiersOf(UIKeyModifierFlags flags);

// Returns the screen that shows `view`, or that of one of the application's scenes, or nil before a scene connects.
// visionOS has no screens.
#if !TARGET_OS_VISION
UIScreen* PR_NULLABLE uiKitScreenOf(PrUiKitHost* host, UIView* PR_NULLABLE view);
#endif

} // namespace pr_impl

PR_ASSUME_NONNULL_END
