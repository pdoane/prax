// The Cocoa application backend: a macOS window and the AppKit event loop.

#pragma once

#include "pr/app.h"
#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates an application that is a macOS window on the AppKit event loop, as `desc` describes.
PrApp* prAppCocoaCreate(const PrAppDesc* desc);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
