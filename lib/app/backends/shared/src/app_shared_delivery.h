// The delivery of a window's callbacks, which each backend embeds in its window's state.

#pragma once

#include "pr/app.h"
#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// A window's handler and the state of its delivery.
struct AppWindowDelivery {
    PrAppWindowHandler handler;
    bool opened;                // true once `open` has been called
    bool delivering;            // true while a callback of the handler runs
    bool destroyed;             // true if the program destroyed the window during a callback
};

// Returns the parameters that a frame of the window would have now. A window that opens on an event opens with them.
typedef PrAppFrame (*AppWindowFrameStateFn)(PrAppWindow* window);

// Returns true if a callback of the window is running, and marks the window to be freed when that callback returns. A
// backend's destroyFn calls this first, and frees the window only if it returns false.
bool appWindowDeliveryDeferDestroy(AppWindowDelivery& delivery);

// Calls the handler's `event`. A window that has not opened opens first, with the parameters that `frameState` returns.
// Returns false if the program destroyed the window during the call. The caller then calls the window's destroyFn and
// does not use the window again.
bool appWindowDeliverEvent(AppWindowDelivery& delivery, PrAppWindow* window, const PrAppEvent* event,
    AppWindowFrameStateFn frameState);

// Calls the handler's `frame` and writes its output to `out` for the backend to apply. A window that has not opened
// opens first, with the frame. Returns false if the program destroyed the window during the call, as
// appWindowDeliverEvent does.
bool appWindowDeliverFrame(AppWindowDelivery& delivery, PrAppWindow* window, const PrAppFrame* frame, PrAppOutput* out);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
