// The Win32 backend's windows: their frames, their placement, their look, and their window procedure.

#pragma once

#include "app/backends/win32/src/app_win32.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The window's client extent in logical pixels.
PrVec2 win32ExtentOf(Win32Window* window);

// The pacer: each time a frame is requested, it waits for the display's refresh and posts kMessageFrame to the
// application's message window. Waiting on a thread of its own keeps the message loop free for input, and the message
// reaches the application also during a modal loop, which dispatches posted messages as the main loop does.
void win32Pace(void* PR_NULLABLE context);

// Returns true if `window` is one of the application's open windows, and the window of `serial` rather than one that
// took its memory after it was freed.
bool win32IsOpen(const Win32App* app, const Win32Window* window, uint64_t serial);

// Runs the frame of each window that requested one when the refresh came. A frame may close another of the windows,
// whose frame then does not run.
void win32RunRequestedFrames(Win32App* app);

// Returns true if a window of the application requested a frame.
bool win32AnyFrameRequested(const Win32App* app);

// Brings a window forward, as after a click on the application's icon in the notification area, which allows the
// process to take the foreground.
void win32BringForward(Win32Window* window);

// The procedure of the application's windows.
LRESULT CALLBACK win32WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

// Frees the window, or marks it to be freed once its running callback returns (PrAppWindow::destroyFn).
void win32WindowDestroy(PrAppWindow* base);

// Returns the parameters that a frame of the window would have now.
PrAppFrame win32FrameState(PrAppWindow* base);

// Requests a frame of the window at the display's next refresh.
void win32RequestFrame(PrAppWindow* base);

// Returns the window's state, or sets it.
PrAppWindowState win32State(const PrAppWindow* base);
void win32SetState(PrAppWindow* base, PrAppWindowState wanted);

// Returns an icon of straight-alpha sRGB Rgba8 pixels, or null for none.
HICON PR_NULLABLE win32IconOf(PrPixelView pixels);

// Implements prAppWindowCreate.
PrStatus win32WindowCreate(PrApp* base, const PrAppWindowDesc* desc, PrAppWindow* PR_NULLABLE* PR_NONNULL out);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
