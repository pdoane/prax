// The model of app_features, its pages, and the parts that the pages share.
//
// Each page tries one area of lib/app by hand. The control window shows the page that the sidebar selects, beside a log
// of every event that reaches the application and its windows. The pages own nothing: they read and change the model,
// which main.cpp creates and routes the events to.

#pragma once

#include "pr/app.h"
#include "pr/array.h"
#include "pr/kit.h"
#include "pr/kit_choice.h"
#include "pr/string.h"
#include "pr/studio_app.h"

PR_ASSUME_NONNULL_BEGIN

namespace features {

using namespace pr;

// The pages, in the order of the sidebar.
enum {
    Page_Lifecycle,
    Page_Timers,
    Page_Open,
    Page_Windows,
    Page_Taskbar,
    Page_Popups,
    Page_Backdrops,
    Page_Tabs,
    Page_Pointer,
    Page_Keys,
    Page_Gestures,
    Page_Clipboard,
    Page_Drags,
    Page_Files,
    Page_Menus,
    Page_StatusItems,
    Page_Shell,
    Page_Power,
    Page_Sensors,
    Page_Displays,
    Page_Count,
};
typedef uint8_t Page;

// What a hide that undoes itself hides.
enum {
    Hiding_None,
    Hiding_App,
    Hiding_Window,
};
typedef uint8_t Hiding;

// The tags of the application's timers. The program's own timers on the Timers page, the timer that asks for the user's
// attention once the user has had time to switch to another application, and the timer that undoes a hide.
constexpr uint64_t kTimerOnce = 1;
constexpr uint64_t kTimerRepeat = 2;
constexpr uint64_t kTimerAttention = 3;
constexpr uint64_t kTimerUnhide = 4;

// The tag of the global hotkey, Control-Option-Space, which toggles the overlay.
constexpr uint64_t kOverlayHotkey = 1;

// The payload kind of the tabs that move between the control window and the second window.
constexpr uint64_t kTabGroup = 0x6665617475726573ull;

// The tabs of a window's strip (PrKitTabs), which the model keeps.
struct KeptTabs {
    PrKitTabs* tabs = prKitTabsCreate();

    KeptTabs() = default;
    KeptTabs(const KeptTabs&) = delete;
    KeptTabs& operator=(const KeptTabs&) = delete;
    ~KeptTabs() { prKitTabsDestroy(tabs); }
};

// The model of every window.
struct Features {
    PrStudioApp* app = nullptr;
    PrStudioWindow* control = nullptr;
    PrStudioWindow* second = nullptr;
    PrStudioWindow* backdrop = nullptr;
    PrStudioWindow* overlay = nullptr;
    PrStudioWindow* popup = nullptr;
    PrStudioWindow* dialog = nullptr;
    PrStudioWindow* borderless = nullptr;

    Page page = Page_Lifecycle;
    PrAppFrame frame{};             // the control window's last frame, as the Displays page shows it

    // The events that reached the application and its windows, the latest last.
    Array<String> log;
    bool logMoves = false;          // true to log each pointer move, scroll, gesture and touch as well

    // Lifecycle and quitting. A quit that the program asks the user about is cancelled, and the user's answer stops the
    // run or not.
    bool accessory = false;
    bool confirmQuit = true;
    bool quitPending = false;
    bool quitSessionEnding = false;

    Hiding hiding = Hiding_None;    // what a hide that undoes itself hides, until its timer fires

    // Timers.
    uint32_t onceFired = 0;
    uint32_t repeatFired = 0;
    bool repeating = false;

    Array<String> launchArguments;  // the arguments that started this process, or the page's query on the web

    // The last request to open, and its arguments, files and URLs.
    uint32_t opens = 0;
    Array<String> openArguments;
    Array<String> openItems;

    // Windows.
    float opacity = 1;
    bool keepAbove = false;
    bool aspect = false;
    String title{"App Features"};

    // The taskbar or the Dock.
    String badge;
    PrAppProgressState progressState = PrAppProgressState_None;
    float progress = 0.5f;
    bool documentEdited = false;
    bool iconSet = false;

    // Popups: their placement against the button that opens them, and that button's bounds at the last layout.
    PrAppPopupPlacement popupPlacement{.side = PrAppPopupSide_Below, .adjust = PrAppPopupAdjustFlag_Flip};
    PrRect popupAnchor{};
    bool modal = true;
    String borderlessText;          // the text of the field in the window without decoration

    // The pointer over the control window: where it is, the movement of its last move, the sum of its movements, and
    // whether it is over the window.
    PrVec2 pointer{};
    PrVec2 movement{};
    PrVec2 travelled{};
    bool inside = false;
    bool trackScreen = false;       // true to follow the pointer anywhere on the screen, with a frame each refresh

    // Keys and text: the last key, and the text of each field, by PrAppTextInputKind.
    PrAppEvent lastKey{};
    String lastKeyText;
    String fields[PrAppTextInputKind_Search + 1];

    // Gestures: the scale and the angle that the gestures add up to, the last swipe, and the trackpad's touches.
    float scale = 1;
    float rotation = 0;
    PrVec2 swipe{};
    uint32_t smartMagnifies = 0;
    Array<PrAppTrackpadTouch> touches;

    // The tabs of the control window's strip and of the second window's, by id.
    KeptTabs controlTabs;
    KeptTabs secondTabs;
    uint64_t nextTab = 1;

    // The global hotkey, and the status of its last registration, if one was tried.
    bool hotkeyRegistered = false;
    bool hotkeyTried = false;
    PrStatus hotkeyStatus = PrStatus_Ok;

    // The overlay: whether the pointer passes through it, and the keys typed into it.
    bool passthrough = false;
    String typed;
    PrAppWindowBackdrop chosenBackdrop = PrAppWindowBackdrop_Blur;

    // The clipboard: how often it changed, and what its reads answered.
    uint32_t clipboardChanges = 0;
    String clipboardRead;

    // What the last drop into the window held, a line for its text and for each of its files, as its reads answer.
    Array<String> dropped;

    uint32_t promiseIndex = 0;      // the promise of a drag that the drop asked for

    // The last path that a dialog or a drop gave, which the file manager can reveal. A Save dialog's answer is written
    // to, and the temporary files that the Files page trashes are numbered.
    String lastPath;
    bool saving = false;
    uint32_t trashed = 0;

    // Menus: whether the application and this window show menu bars, the menu's checked item, and the last menu event.
    bool appMenuBar = false;
    bool windowMenuBar = false;
    bool toolbarShown = true;
    uint32_t menuOpenings = 0;
    String lastMenu;

    // The status item, and whether it has a menu.
    PrAppStatusItem* PR_NULLABLE statusItem = nullptr;
    bool statusMenu = false;
    uint32_t statusClicks = 0;

    // The shell.
    String url{"https://example.com/"};
    String shareText{"Shared from app_features"};

    // The device: what stays awake, the sensors that are on and their last readings, and the counts of the system's
    // notices.
    PrAppAwakeFlags awake = 0;
    uint32_t orientations = 0;      // the Sensors page's choice: any, the portraits, or the landscapes
    bool sensors[PrAppSensor_Count] = {};
    float readings[PrAppSensor_Count][4] = {};
    bool hapticHovered[PrAppHaptic_Error + 1] = {};    // the pointer was over each haptic's button, by PrAppHaptic
    uint32_t suspends = 0;
    uint32_t resumes = 0;
    uint32_t locks = 0;
};

// Returns the model that a callback's context points to.
Features& featuresOf(void* context);

// The platform's application, and the control window's platform window, or null once it is closed.
PrApp* appOf(const Features& f);
PrAppWindow* PR_NULLABLE controlOf(const Features& f);

// Adds a line to the log, and asks the control window for a frame that shows it.
void logLine(Features& f, PrStringView line);

// The name of a status that the pages and the log show, which is Ok for success.
PrStringView statusName(PrStatus status);

// Logs an event that reached `who`, the application or a window, by its kind and the details that the kind has.
void logEvent(Features& f, PrStringView who, const PrAppEvent& e);

// ---------------------------------------------------------------------------------------------------------------------
// Building blocks

// Shows the window's title in its custom title bar (PrStudioWindowDesc::titleBar).
void titleBar(void* context, PrStudioWindow* window, PrLayoutNode bar);

// A panel with a caption, which holds one feature's controls.
PrLayoutNode section(PrKit* kit, PrLayoutNode parent, PrStringView title);

// A row of controls that wraps.
PrLayoutNode row(PrKit* kit, PrLayoutNode parent);

// Creates a button, and returns true if it was activated in the current round.
bool button(PrKit* kit, PrLayoutNode parent, PrStringView text, bool disabled = false);

// A line of monospace text, and a note in small muted text.
void line(PrKit* kit, PrLayoutNode parent, PrStringView text);
void note(PrKit* kit, PrLayoutNode parent, PrStringView text);

// Creates a segmented control of `count` labels with `*selected` selected, and returns true if the user selected
// another, which it writes to `*selected`.
bool segmented(PrKit* kit, PrLayoutNode parent, const PrStringView* labels, uint32_t count, uint32_t* selected);

// Creates a text field that edits `text`, of `kind`, and returns true if the user changed it.
bool textField(PrKit* kit, PrLayoutNode parent, PrUiKey key, String& text, PrStringView placeholder,
    PrAppTextInputKind kind = PrAppTextInputKind_Text);

// Returns the bounds of `node` at the last layout, in the window's logical pixels.
PrRect boundsOf(PrKit* kit, PrLayoutNode node);

// Returns an icon of `size` square pixels, straight-alpha sRGB Rgba8 in `pixels`: a colored disc with a lighter ring.
PrPixelView iconPixels(Array<uint8_t>& pixels, uint32_t size, PrColor color);

} // namespace features

PR_ASSUME_NONNULL_END
