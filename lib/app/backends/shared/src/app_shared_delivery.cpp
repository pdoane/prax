// The delivery of a window's callbacks.

#include "app/backends/shared/src/app_shared_delivery.h"

#include "pr/assert.h"

namespace pr_impl {

namespace {

// Calls the handler's `open` once, with the window as its first frame finds it. A window that fails to open is
// destroyed as the delivery ends.
void open(AppWindowDelivery& delivery, PrAppWindow* window, const PrAppFrame& frame) {
    if (delivery.opened)
        return;
    delivery.opened = true;
    PrAppFrame first = frame;
    first.delta = 0;
    if (!delivery.handler.open(delivery.handler.context, window, &first))
        delivery.destroyed = true;
}

// Ends a callback, and returns false if the program destroyed the window during it.
bool endDelivery(AppWindowDelivery& delivery) {
    delivery.delivering = false;
    return !delivery.destroyed;
}

} // namespace

bool appWindowDeliveryDeferDestroy(AppWindowDelivery& delivery) {
    if (!delivery.delivering)
        return false;
    delivery.destroyed = true;
    return true;
}

bool appWindowDeliverEvent(AppWindowDelivery& delivery, PrAppWindow* window, const PrAppEvent* event,
        AppWindowFrameStateFn frameState) {
    PR_ASSERT(!delivery.delivering && !delivery.destroyed);
    delivery.delivering = true;
    if (!delivery.opened)
        open(delivery, window, frameState(window));
    if (!delivery.destroyed)
        delivery.handler.event(delivery.handler.context, window, event);
    return endDelivery(delivery);
}

bool appWindowDeliverFrame(AppWindowDelivery& delivery, PrAppWindow* window, const PrAppFrame* frame, PrAppOutput* out) {
    PR_ASSERT(!delivery.delivering && !delivery.destroyed);
    delivery.delivering = true;
    open(delivery, window, *frame);
    *out = PrAppOutput{};
    if (!delivery.destroyed)
        *out = delivery.handler.frame(delivery.handler.context, window, frame);
    return endDelivery(delivery);
}

} // namespace pr_impl
