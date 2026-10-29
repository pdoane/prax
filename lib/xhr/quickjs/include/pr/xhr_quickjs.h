// JavaScript binding of XMLHttpRequest, XMLHttpRequestUpload and ProgressEvent.
//
// xhr - https://xhr.spec.whatwg.org/

#pragma once

#include "pr/base.h"

#include "quickjs.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Defines XMLHttpRequest, XMLHttpRequestEventTarget, XMLHttpRequestUpload and ProgressEvent on the global object of
// `ctx`. A request that a script constructs is the request of the global's window.
void prJsXhrBind(JSContext* ctx);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
