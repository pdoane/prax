// The Blend2D draw backend, which renders with Blend2D on a memory device (PrGpuNativeKind_Pixels), natively into the
// passes that frames render into.

#pragma once

#include "pr/base.h"
#include "pr/draw_glyph.h"
#include "pr/draw_renderer.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// How the backend draws.
typedef struct PrDrawBlend2dOptions {
    // Zero renders synchronously on the calling thread. Any other count renders asynchronously on that many workers,
    // including the calling thread.
    uint32_t threads;

    PrDrawGlyphDecoder* PR_NULLABLE glyphDecoder;   // draws the glyphs that are pictures; without it, they draw nothing

    // The memory device that the renderer renders on. If it is null, the renderer creates a memory device and frees it
    // with itself.
    PrGpuDevice* PR_NULLABLE device;
} PrDrawBlend2dOptions;

// Returns options for synchronous rendering, without a glyph decoder, on a device of the renderer's own.
PrDrawBlend2dOptions prDrawBlend2dOptionsDefault(void);

// Creates a renderer that renders on a memory device, into attachments of Rgba8 and Bgra8. Null options mean the
// defaults. Returns null if the device is not a memory device, or if it is null and the build has no memory device.
PrDrawRenderer* PR_NULLABLE prDrawBlend2dCreate(const PrDrawBlend2dOptions* PR_NULLABLE options);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
