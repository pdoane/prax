// JavaScript binding of channel messaging (html/#message-channels).

#pragma once

#include "pr/html_message.h"

#include "quickjs.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The class of MessagePort.
extern JSClassID htmlMessagePortClass;

// Returns the port that `value` is the object of, or null with no exception thrown.
PrHtmlMessagePort* PR_NULLABLE jsHtmlMessagePortOf(JSContext* ctx, JSValueConst value);

// Returns a frozen array of the objects of `ports`, which are transferred or were, with each port given to `window` if
// it has no window yet (html/#message-ports:transfer-receiving-steps).
JSValue jsHtmlMessagePortsAdopt(JSContext* ctx, PrHtmlMessagePort* PR_NONNULL const* PR_NULLABLE ports, uint32_t count,
    PrHtmlWindow* window);

// Defines the interfaces of channel messaging in the realm.
void jsHtmlMessageBind(JSContext* ctx);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
