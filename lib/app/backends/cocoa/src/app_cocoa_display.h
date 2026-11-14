// The Cocoa backend's displays: their list, their properties, and positions on them.

#pragma once

#include "app/backends/cocoa/src/app_cocoa.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Returns the id of a screen: its UUID folded to 64 bits, which stays the same across connections, or its display
// number if it has none. Neither is ever zero.
PrAppDisplayId cocoaDisplayIdOf(NSScreen* screen);

// Returns the screen with `id`, or nil if none is connected.
NSScreen* PR_NULLABLE cocoaScreenOf(PrAppDisplayId id);

// Returns what `screen` shows at `pixelRatio`.
PrAppDisplay cocoaDisplayOf(NSScreen* PR_NULLABLE screen, CGFloat pixelRatio);

// Returns `rect`, in AppKit's global points, from the top left of `screen`, in the screen's logical pixels.
PrRect cocoaRectOnScreen(NSScreen* screen, NSRect rect);

// Moves `window` so that the top left of its content is at `position` on `screen`, within the screen's work area.
void cocoaPlaceOnScreen(NSWindow* window, NSScreen* screen, PrVec2 position);

// Centers `window` on the work area of `screen`.
void cocoaCenterOnScreen(NSWindow* window, NSScreen* screen);

// Returns the frame of a popup's content of `size`, in AppKit's global points, as `placement` puts it against the
// content of `parent`, within the work area of the parent's screen.
NSRect cocoaPopupFrame(NSWindow* parent, NSSize size, const PrAppPopupPlacement& placement);

// Returns the position of `at`, in AppKit's global coordinates, on the screen that holds it, or on the nearest screen
// if none does. Returns false if there are no screens.
bool cocoaDisplayPositionAt(NSPoint at, PrAppDisplayPosition* out);

// Implement the application's functions of their names (PrApp::pointerPositionFn and the others).
bool cocoaPointerPosition(const PrApp* app, PrAppDisplayPosition* out);
uint32_t cocoaDisplays(const PrApp* app, PrAppDisplayInfo* PR_NULLABLE out, uint32_t capacity);
bool cocoaDisplayInfo(const PrApp* app, PrAppDisplayId id, PrAppDisplayInfo* out);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
