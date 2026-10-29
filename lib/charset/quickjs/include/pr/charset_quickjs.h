// JavaScript binding of TextDecoder and TextEncoder.
//
// encoding - https://encoding.spec.whatwg.org/

#pragma once

#include "pr/base.h"

#include "quickjs.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Defines the Encoding Standard's interfaces on the global object of `ctx`.
void prJsCharsetBind(JSContext* ctx);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
