// Surfaces: the native objects that an application presents to, such as a window's view.

#pragma once

#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The kinds of native surface.
enum {
    PrAppSurfaceKind_NsView,   // `handle` is an NSView
    PrAppSurfaceKind_Hwnd,     // `handle` is a Win32 window's HWND
};
typedef uint8_t PrAppSurfaceKind;

// A native surface. `kind` says which native object `handle` is.
typedef struct PrAppSurface {
    PrAppSurfaceKind kind;
    void* handle;
} PrAppSurface;

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
