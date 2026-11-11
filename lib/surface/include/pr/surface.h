// Surfaces: the native objects that a device presents to, such as a window's view. A surface names its object by kind,
// so that a library that makes surfaces, such as lib/app, and a library that presents to them, such as lib/gpu, need
// not know each other.

#pragma once

#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The kinds of native surface.
enum {
    PrSurfaceKind_NsView,       // `handle` is an NSView
    PrSurfaceKind_UiView,       // `handle` is a UIView whose layer is a CAMetalLayer
    PrSurfaceKind_Hwnd,         // `handle` is a Win32 window's HWND
    PrSurfaceKind_HtmlCanvas,   // `handle` is the CSS selector of a page's canvas, a NUL-terminated string
};
typedef uint8_t PrSurfaceKind;

// A native surface. `kind` says which native object `handle` is.
typedef struct PrSurface {
    PrSurfaceKind kind;
    void* handle;
} PrSurface;

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
