// The selector for lib/app's backends. It reports which application backends the build includes, and creates an
// application of a given backend. Programs and composition libraries link it.

#pragma once

#include "pr/app_application.h"
#include "pr/backend.h"
#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The application backends. Default is the backend that PR_BACKEND_APP names (pr/backend.h). If the variable chooses
// nothing, Default is the platform's backend. A program can also supply a backend of its own (pr/app_backend.h).
enum {
    PrAppBackend_Default,
    PrAppBackend_AppKit,
    PrAppBackend_UiKit,
    PrAppBackend_Win32,
    PrAppBackend_Web,
    PrAppBackend_Count,
};
typedef uint8_t PrAppBackend;

// Returns the descriptor of the application interface.
const PrBackendInterface* prAppSelectInterface(void);

// Creates an application of `backend`. Null options mean the defaults. Returns null if the build does not include the
// backend, or if the backend cannot run where the program runs, as the web backend cannot outside a page. For Default,
// it also returns null if PR_BACKEND_APP is not valid (prBackendFromEnvironment).
PrApp* PR_NULLABLE prAppCreate(PrAppBackend backend, const PrAppOptions* PR_NULLABLE options);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
