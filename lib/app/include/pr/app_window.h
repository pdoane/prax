// Windows: surfaces on the screen, each with its own frames, input and handler.

#pragma once

#include "pr/app_application.h"
#include "pr/app_data.h"
#include "pr/app_display.h"
#include "pr/app_event.h"
#include "pr/app_input.h"
#include "pr/app_user_preferences.h"
#include "pr/base.h"
#include "pr/geom.h"
#include "pr/pixels.h"
#include "pr/string_view.h"
#include "pr/surface.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The role of a window, which decides how the system treats it.
enum {
    // An ordinary window of the application. The system lists it, for example in the Dock and in the taskbar.
    PrAppWindowRole_Normal,

    // A window that stays above its parent, such as a panel or an alert. It follows the parent to other displays and
    // desktops. A modal dialog's parent dims and takes no input while the dialog shows. With System or Custom
    // decoration, a modal dialog is the system's sheet over its parent, with no title bar. With None, it is a window of
    // the program's own drawing, centered over its parent.
    PrAppWindowRole_Dialog,

    // A transient window, such as a menu, a list or a tooltip, placed against its parent by PrAppPopupPlacement. It
    // moves with the parent and has no system decoration. A press on it does not activate the application. The system
    // may close it, for example when the user presses outside it, and then sends CloseRequested.
    PrAppWindowRole_Popup,

    // A window that floats above the windows of every application, on every desktop and above fullscreen windows, such
    // as a HUD, a launcher or a screen picker. It has no system decoration or shadow, and the system does not list it.
    // A press on it does not activate the application. It takes the keyboard only when the program focuses it.
    PrAppWindowRole_Overlay,
};
typedef uint8_t PrAppWindowRole;

// Who draws the title bar and frame of a Normal or Dialog window. With Custom, the program draws the title bar in its
// content (PrAppTitleBar). The system keeps its resize borders and shadow, and its window buttons on platforms that
// draw them. With None, the window has no title bar.
enum {
    PrAppWindowDecoration_System,
    PrAppWindowDecoration_Custom,
    PrAppWindowDecoration_None,
};
typedef uint8_t PrAppWindowDecoration;

// What shows behind a window's content. With any backdrop but Opaque, the window composites its frames over the
// backdrop with premultiplied alpha, so the program clears to transparent wherever the backdrop should show. The swap
// chain must present alpha (PrGpuSwapChainFlag_Transparent).
enum {
    PrAppWindowBackdrop_Opaque,
    PrAppWindowBackdrop_Transparent,    // whatever is behind the window, unchanged

    // Whatever is behind the window, blurred. This is the HUD material on macOS and Acrylic on Windows. The blur shows
    // whether or not the window is active.
    PrAppWindowBackdrop_Blur,
};
typedef uint8_t PrAppWindowBackdrop;

// Where a Normal, Dialog or Overlay window opens.
enum {
    PrAppWindowPlacement_Default,   // the platform's choice, such as cascaded from the previous window
    PrAppWindowPlacement_Center,    // centered on the parent, or on the display of the active window
    PrAppWindowPlacement_Position,  // at PrAppWindowDesc::position
};
typedef uint8_t PrAppWindowPlacement;

// The side of the anchor where a popup opens.
enum {
    PrAppPopupSide_Below,
    PrAppPopupSide_Above,
    PrAppPopupSide_After,           // to the right in a left-to-right layout
    PrAppPopupSide_Before,
};
typedef uint8_t PrAppPopupSide;

// The alignment of a popup along the side of its anchor.
enum {
    PrAppPopupAlign_Start,
    PrAppPopupAlign_Center,
    PrAppPopupAlign_End,
};
typedef uint8_t PrAppPopupAlign;

// The ways that the system may adjust a popup that would extend past the work area of its display. It tries them in
// order: flip to the opposite side of the anchor, slide along the side, then shrink.
enum {
    PrAppPopupAdjustFlag_Flip = 1 << 0,
    PrAppPopupAdjustFlag_Slide = 1 << 1,
    PrAppPopupAdjustFlag_Resize = 1 << 2,
};
typedef uint8_t PrAppPopupAdjustFlags;

// The placement of a popup. The popup opens on `side` of `anchor`, aligned by `align`, and then moves by `offset`.
// `anchor` is in the parent's logical pixels. For example, a menu opens Below its button with Start alignment, and a
// submenu opens After its item.
typedef struct PrAppPopupPlacement {
    PrRect anchor;
    PrAppPopupSide side;
    PrAppPopupAlign align;
    PrAppPopupAdjustFlags adjust;
    PrVec2 offset;
} PrAppPopupPlacement;

// The direction of a layout's horizontal text, which decides where a popup's start and end sides are.
enum {
    PrAppLayoutDirection_LeftToRight,
    PrAppLayoutDirection_RightToLeft,
};
typedef uint8_t PrAppLayoutDirection;

// Returns the rectangle of a popup of `extent` that `placement` puts against its anchor, as a backend places a popup
// window. The popup flips, slides and shrinks to stay within `work`, as far as the placement's adjustments allow. The
// anchor, `work` and the result share one space, whose y grows downward. A program that draws a popup in its own window
// places it this way.
PrRect prAppPopupRect(const PrAppPopupPlacement* placement, PrVec2 extent, PrRect work, PrAppLayoutDirection direction);

// The state of a window on the screen.
enum {
    PrAppWindowState_Normal,
    PrAppWindowState_Minimized,     // hidden, in the Dock or the taskbar where the platform has them
    PrAppWindowState_Maximized,     // filling the work area of its display
    PrAppWindowState_Fullscreen,    // covering its display, without a frame
};
typedef uint8_t PrAppWindowState;

// Returns the name of a window state, as in "Maximized", or the null view if `state` is not one.
PrStringView prAppWindowStateName(PrAppWindowState state);

// Distances inward from each edge of a window's content, in logical pixels.
typedef struct PrAppInsets {
    float top;
    float right;
    float bottom;
    float left;
} PrAppInsets;

// ---------------------------------------------------------------------------------------------------------------------
// Title bars

// Options for a custom title bar.
enum {
    // The program draws the minimize, maximize and close buttons and marks them with title bar regions. Without this
    // flag, the system draws its own buttons in the leading or trailing inset.
    PrAppTitleBarFlag_ProgramButtons = 1 << 0,
};
typedef uint8_t PrAppTitleBarFlags;

// The geometry of a custom title bar, in logical pixels. The title bar is a band of `height` at the top of the content,
// which matches the height of the platform's own title bar. The program keeps its content out of the `leading` and
// `trailing` insets, where the system's buttons go. The title bar is zero for other windows and for a fullscreen
// window.
typedef struct PrAppTitleBar {
    float height;
    float leading;              // at the left in a left-to-right layout
    float trailing;
    PrAppTitleBarFlags flags;
} PrAppTitleBar;

// The behavior of a region of a custom title bar. The system handles presses in every region but Client, and the
// program receives no events for them. A drag moves the window. A double click maximizes or minimizes the window, as
// the user's settings choose. A button acts like the system's own button when it is released. Pointer moves over a
// button still reach the program, so that it can draw the button's hover state.
enum {
    PrAppTitleBarRegionKind_Client,     // a control of the program in the title bar
    PrAppTitleBarRegionKind_Drag,
    PrAppTitleBarRegionKind_Minimize,
    PrAppTitleBarRegionKind_Maximize,   // on Windows, it also shows the snap layouts
    PrAppTitleBarRegionKind_Close,      // sends CloseRequested
};
typedef uint8_t PrAppTitleBarRegionKind;

// A region of a custom title bar, in the window's logical pixels.
typedef struct PrAppTitleBarRegion {
    PrRect rect;
    PrAppTitleBarRegionKind kind;
} PrAppTitleBarRegion;

// ---------------------------------------------------------------------------------------------------------------------
// Frames

// The parameters of a frame. A change to any of them requests a frame.
//
// `display` is the display that shows the window when the frame begins, and `preferences` are the user's preferences at
// that time. `backdrop` is the window's backdrop, or Opaque if the platform cannot show the one that the program set.
// `safeArea` is the part of the content that is clear of the system's interface and of the display's shape, such as a
// notch or a home indicator. `keyboard` is the part of the content that the on-screen keyboard covers, or empty.
typedef struct PrAppFrame {
    double time;                // seconds, from a monotonic clock
    double delta;               // seconds since the window's previous frame, or zero for its first
    PrVec2 extent;              // logical pixels
    PrAppDisplay display;
    PrAppUserPreferences preferences;
    PrAppTitleBar titleBar;
    PrAppWindowBackdrop backdrop;
    PrAppInsets safeArea;
    PrRect keyboard;
} PrAppFrame;

// The result of a frame, which the window applies. A library that draws a surface reports its own output, as prUiOutput
// and prHtmlViewOutput do. A host of several surfaces combines their outputs with prAppOutputPlaced and
// prAppOutputMerge.
typedef struct PrAppOutput {
    PrAppCursor cursor;         // the cursor to show over the window
    bool wantsFrame;            // true to request another frame at the display's next refresh

    // The text being edited, if `hasTextInput`. `textInput` is the caret, where the input method opens its windows, in
    // logical pixels of `textInputWindow`: this window, or a Popup or Dialog that it owns, such as a dialog that a
    // library draws for it. Null means this window. A window whose own output has no text input takes the one that its
    // owner's output names it for. A mobile system shows its on-screen keyboard while text is being edited.
    bool hasTextInput;
    PrAppTextInputKind textInputKind;
    PrRect textInput;
    PrAppWindow* PR_NULLABLE textInputWindow;

    // The time of the next frame for content that changes on its own at a known time, on the clock of PrAppFrame::time.
    // Zero means none. Each output replaces the previous one's time.
    double frameTime;

    // The regions of a custom title bar, in the window's logical pixels. They replace the previous output's regions,
    // and the window copies them. Where regions overlap, the later one wins, so a control in a drag region is a Client
    // region listed after it. The program receives the events at points outside every region.
    const PrAppTitleBarRegion* PR_NULLABLE titleBarRegions;
    uint32_t titleBarRegionCount;

    // The operation that a drop at the pointer would perform, or None to refuse the drop. The window answers each drag
    // event with the operation of its latest output. An operation that the source does not allow refuses the drop.
    PrAppDragOperation dropOperation;
} PrAppOutput;

// The callbacks of a window. Each one receives `context`, and every one must be set.
//
// The window calls `open` once, before its first frame, with that frame's parameters and a delta of zero. The program
// creates what the window needs for its extent, display and preferences. If `open` returns false, the window is
// destroyed without a frame. `event` receives the window's events.
//
// `frame` runs inside the window's display transaction. It runs at the display's refresh after a frame is requested,
// and at each step of a live resize. A swap chain that presents inside the transaction (prGpuSwapChainCreate) shows its
// frame together with the window at the frame's extent. The window applies the output when `frame` returns.
typedef struct PrAppWindowHandler {
    void* PR_NULLABLE context;
    bool (*open)(void* PR_NULLABLE context, PrAppWindow* window, const PrAppFrame* frame);
    PrAppOutput (*frame)(void* PR_NULLABLE context, PrAppWindow* window, const PrAppFrame* frame);
    void (*event)(void* PR_NULLABLE context, PrAppWindow* window, const PrAppEvent* event);
} PrAppWindowHandler;

// ---------------------------------------------------------------------------------------------------------------------
// Windows

// The description of a new window.
//
// `extent` is in logical pixels, and the window clamps it to the screen and to the extent limits. A zero component of a
// limit leaves that axis unlimited. A Dialog or a Popup needs a `parent`. A hidden window stays hidden until the
// program shows it (prAppWindowSetVisible). If `position` names a display that is not connected, the window opens where
// Default would place it. The window opens in `state`, and returns to `extent` and its placement when it leaves that
// state.
typedef struct PrAppWindowDesc {
    PrStringView title;
    PrUVec2 extent;
    PrUVec2 minExtent;
    PrUVec2 maxExtent;
    PrAppWindowState state;
    PrAppWindowRole role;
    PrAppWindow* PR_NULLABLE parent;
    bool modal;
    PrAppWindowDecoration decoration;
    PrAppWindowBackdrop backdrop;
    bool hidden;
    bool pointerPassthrough;
    PrAppWindowPlacement placement;
    PrAppDisplayPosition position;
    PrAppPopupPlacement popup;
    PrAppWindowHandler handler;
} PrAppWindowDesc;

// Creates a window of the running application. The window calls `open` before its first frame, never during this call.
// Returns InvalidState if the application is not running. Returns NotSupported for a second window of a Single
// application, and for a window of any role but Normal unless the application is a Desktop one (PrAppWindowModel). On
// failure, `*out` is null.
PrStatus prAppWindowCreate(PrApp* app, const PrAppWindowDesc* desc, PrAppWindow* PR_NULLABLE* PR_NONNULL out);

// Closes and frees the window. Its handler is not called again. If this is called during one of the window's callbacks,
// the window is freed when the callback returns.
void prAppWindowDestroy(PrAppWindow* PR_NULLABLE window);

// Returns the application that owns the window.
PrApp* prAppWindowApp(const PrAppWindow* window);

// Requests a frame at the display's next refresh. Several requests before that refresh produce one frame.
void prAppWindowRequestFrame(PrAppWindow* window);

// Sets or returns the window's title. The setter copies the title, and the returned view is valid until the title
// changes. The system shows the title in its window lists and to screen readers, even if a custom title bar draws it. A
// change requests a frame.
void prAppWindowSetTitle(PrAppWindow* window, PrStringView title);
PrStringView prAppWindowTitle(const PrAppWindow* window);

// Resizes the window's content in place, clamped to the screen and to the extent limits.
void prAppWindowSetExtent(PrAppWindow* window, PrUVec2 extent);

// Sets the minimum and maximum extent to which the user can resize the content. A zero component leaves that axis
// unlimited. If the minimum equals the maximum, the window cannot be resized or maximized. A window outside the new
// limits is resized to fit them.
void prAppWindowSetExtentLimits(PrAppWindow* window, PrUVec2 minExtent, PrUVec2 maxExtent);

// Keeps the ratio of the content's width to its height at `ratio` while the user resizes the window. Zero removes the
// constraint.
void prAppWindowSetAspectRatio(PrAppWindow* window, float ratio);

// Writes the position of the window's content, on the display that shows most of it, to `out`, and returns true.
// Returns false if the platform does not report it, as for a headless window, or for a window that fills the display of
// a phone, a console or a web page.
bool prAppWindowPosition(const PrAppWindow* window, PrAppDisplayPosition* out);

// Moves the window's content to `position`, clamped to the work area of the display. If the display is not connected,
// the window moves to the main display, at the same position if it fits and centered otherwise. A maximized window
// returns to Normal first. A fullscreen window does not move.
void prAppWindowSetPosition(PrAppWindow* window, const PrAppDisplayPosition* position);

// Converts a point in the window's content, in its logical pixels from its top left, to a position on a display, or
// back. The point may lie outside the content, as the position of a pointer that the window captures does. A point on
// no display is on the nearest one, outside its bounds. Each returns false if the platform does not report positions,
// as for a headless window.
bool prAppWindowPointToDisplay(const PrAppWindow* window, PrVec2 point, PrAppDisplayPosition* out);
bool prAppWindowPointFromDisplay(const PrAppWindow* window, const PrAppDisplayPosition* position, PrVec2* out);

// Replaces the placement of a popup against its parent (PrAppWindowDesc::popup).
void prAppWindowSetPopupPlacement(PrAppWindow* window, const PrAppPopupPlacement* placement);

// Sets or returns the window's state. Some changes take time, such as the animation into fullscreen, and the returned
// state changes when the transition finishes. A change requests a frame. The web supports only Normal and Fullscreen,
// and it enters fullscreen only while the page handles user input.
void prAppWindowSetState(PrAppWindow* window, PrAppWindowState state);
PrAppWindowState prAppWindowState(const PrAppWindow* window);

// Shows or hides the window, or returns whether it is shown. A hidden window receives no frames, and it draws a frame
// before it shows again. A minimized window, or a window of a hidden application, still counts as shown.
void prAppWindowSetVisible(PrAppWindow* window, bool visible);
bool prAppWindowIsVisible(const PrAppWindow* window);

// Returns true if no part of the window is visible, for example because other windows cover it. The program may skip
// its frames while it is occluded. A change requests a frame.
bool prAppWindowIsOccluded(const PrAppWindow* window);

// Returns true if the window is active. An active window has the keyboard and belongs to the active application, or is
// an overlay that the program focused. A change requests a frame.
bool prAppWindowIsActive(const PrAppWindow* window);

// Brings the window to the front and makes it and its application active. It first shows a hidden window or a hidden
// application and restores a minimized window. An overlay takes the keyboard without activating the application, except
// on Windows. On the web, this focuses the page, which the browser may refuse.
void prAppWindowFocus(PrAppWindow* window);

// Requests the user's attention if the window is not active, for example by bouncing the Dock icon or flashing the
// taskbar button.
void prAppWindowRequestAttention(PrAppWindow* window);

// Sets the window's backdrop. The next frame reports the backdrop that the window shows.
void prAppWindowSetBackdrop(PrAppWindow* window, PrAppWindowBackdrop backdrop);

// Sets the opacity of the whole window, including its frame, in [0, 1].
void prAppWindowSetOpacity(PrAppWindow* window, float opacity);

// Keeps a Normal window above the windows of other applications, or returns it to the normal order.
void prAppWindowSetKeepAbove(PrAppWindow* window, bool above);

// Sets whether pointer input passes through the window to whatever is below it. A window that the pointer passes
// through receives no pointer events.
void prAppWindowSetPointerPassthrough(PrAppWindow* window, bool passthrough);

// Sets or returns the window's pointer mode. A change requests a frame.
void prAppWindowSetPointerMode(PrAppWindow* window, PrAppPointerMode mode);
PrAppPointerMode prAppWindowPointerMode(const PrAppWindow* window);

// Sets the icon that the window's title bar and the taskbar show. Empty pixels restore the application's icon. The
// pixels are straight-alpha sRGB Rgba8, and the window copies them. macOS shows only the application's icon.
void prAppWindowSetIcon(PrAppWindow* window, PrPixelView pixels);

// The system bars that can show over a window on a mobile system.
enum {
    PrAppSystemBarFlag_Status = 1 << 0,
    PrAppSystemBarFlag_Navigation = 1 << 1,
    PrAppSystemBarFlag_HomeIndicator = 1 << 2,
    PrAppSystemBarFlag_All = 0x7,
};
typedef uint8_t PrAppSystemBarFlags;

// Hides the system bars in `hidden` while the window shows, and shows the others.
void prAppWindowSetHiddenSystemBars(PrAppWindow* window, PrAppSystemBarFlags hidden);

// The kind of progress that a window shows on its taskbar button or Dock icon. Normal shows a fraction of the work.
// Indeterminate shows that work continues without a known amount.
enum {
    PrAppProgressState_None,
    PrAppProgressState_Normal,
    PrAppProgressState_Indeterminate,
    PrAppProgressState_Paused,
    PrAppProgressState_Error,
};
typedef uint8_t PrAppProgressState;

// Shows the progress of the window's work on its taskbar button or Dock icon. `progress` is the completed fraction, in
// [0, 1], and only Normal, Paused and Error use it.
void prAppWindowSetProgress(PrAppWindow* window, PrAppProgressState state, float progress);

// Sets the file that the window shows, by absolute path, and whether it has unsaved changes. An empty path means no
// file. macOS shows the file's icon in the title bar and a dot in the close button for unsaved changes.
void prAppWindowSetDocument(PrAppWindow* window, PrStringView path, bool edited);

// Writes the window's native surface to `out`. The surface is valid until the window is destroyed.
PrStatus prAppWindowSurface(const PrAppWindow* window, PrSurface* out);

// ---------------------------------------------------------------------------------------------------------------------
// Drags

// Starts a drag of `desc` from the current press of the pointer's primary button. The window copies the description,
// and starts the drag after the current callback returns. The window receives PointerCancel, and DragEnd when the drag
// ends. If the button is no longer down, or the backend cannot start a drag, DragEnd follows with None.
void prAppWindowDragStart(PrAppWindow* window, const PrAppDragDesc* desc);

// Ends the promise of a DragPromise event. `status` is Ok if the program wrote the file, or the error that prevented
// it.
void prAppWindowDragPromiseEnd(PrAppWindow* window, uint32_t index, PrStatus status);

// ---------------------------------------------------------------------------------------------------------------------
// Outputs

// Returns a surface's output in the window's logical pixels. `origin` is the surface's origin in the window, and
// `scale` is the number of window logical pixels per surface logical pixel. A caret in another window
// (PrAppOutput::textInputWindow) stays as it is. The title bar belongs to the window, so the result has no title bar
// regions.
PrAppOutput prAppOutputPlaced(PrAppOutput output, PrVec2 origin, float scale);

// Combines the outputs of two surfaces of one window. The cursor and the drop operation come from the surface under the
// pointer. The text input comes from the second surface if it has one, and from the first otherwise. The title bar
// regions come from the first surface that has them. The result wants a frame if either output does, and its frame time
// is the earlier of the two that are set.
PrAppOutput prAppOutputMerge(PrAppOutput first, PrAppOutput second, bool pointerInSecond);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
