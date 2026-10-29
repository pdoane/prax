// JavaScript binding of URL and URLSearchParams.
//
// url - https://url.spec.whatwg.org/

#pragma once

#include "pr/url.h"

#include "quickjs.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Defines the URL Standard's interfaces on the global object of `ctx`.
void prJsUrlBind(JSContext* ctx);

// Returns the handle of `value` if it is a URLSearchParams, and otherwise null, without throwing.
PrUrlSearchParams* PR_NULLABLE prJsUrlSearchParamsFromValue(JSContext* ctx, JSValueConst value);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
