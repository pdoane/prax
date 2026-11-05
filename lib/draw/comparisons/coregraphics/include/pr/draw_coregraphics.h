// The CoreGraphics draw backend, which renders with Quartz 2D on a memory device (PrGpuNativeKind_Pixels), natively
// into the passes that frames render into.

#pragma once

#include "pr/base.h"
#include "pr/draw_glyph.h"
#include "pr/draw_renderer.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// How the backend draws.
typedef struct PrDrawCoreGraphicsOptions {
    bool fontSmoothing;                           // Quartz's font smoothing, which thickens glyphs
    PrDrawGlyphDecoder* PR_NULLABLE glyphDecoder; // draws the glyphs that are pictures; without it, they draw nothing

    // The memory device that the renderer renders on. If it is null, the renderer creates a memory device and frees it
    // with itself.
    PrGpuDevice* PR_NULLABLE device;
} PrDrawCoreGraphicsOptions;

// Returns options with font smoothing, without a glyph decoder, on a device of the renderer's own.
PrDrawCoreGraphicsOptions prDrawCoreGraphicsOptionsDefault(void);

// Creates a renderer that renders on a memory device, into attachments of Rgba8 and Bgra8, through a bitmap context in
// sRGB. Null options mean the defaults. Returns null if the device is not a memory device, or if it is null and the
// build has no memory device.
PrDrawRenderer* PR_NULLABLE prDrawCoreGraphicsCreate(const PrDrawCoreGraphicsOptions* PR_NULLABLE options);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
