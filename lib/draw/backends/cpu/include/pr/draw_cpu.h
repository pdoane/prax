// The CPU draw backend, which renders on a memory device (PrGpuNativeKind_Pixels), drawing natively into the texels of
// the passes that frames render into (prGpuPassNativeDraw).

#pragma once

#include "pr/base.h"
#include "pr/draw_glyph.h"
#include "pr/draw_renderer.h"
#include "pr/gpu.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// How the backend draws.
typedef struct PrDrawCpuOptions {
    PrDrawGlyphDecoder* PR_NULLABLE glyphDecoder;   // draws the glyphs that are pictures; without it, they draw nothing

    // Draws each glyph at the phase of a target pixel that an atlas holds it at (prDrawGlyphPlace), as a backend that
    // draws glyphs from an atlas does, instead of exactly where its run places it. A glyph then covers the same pixels
    // wherever its run puts it within the phase, so runs that a document splits differently draw alike.
    bool glyphPhases;

    // The memory device that the renderer renders on. If it is null, the renderer creates a memory device
    // (pr/gpu_memory.h) and frees it with itself.
    PrGpuDevice* PR_NULLABLE device;
} PrDrawCpuOptions;

// Returns options with no glyph decoder, glyphs drawn where their runs place them, and a device of the renderer's own.
PrDrawCpuOptions prDrawCpuOptionsDefault(void);

// Creates a renderer that renders on a memory device into attachments of Rgba8, Bgra8 and Rgba16F, and blends
// sRGB-encoded values. A group's layer (PrDrawGroupLayer) is a texture on the device, in Rgba8 or Rgba16F. Null options
// mean the defaults. Returns null if the device is not a memory device, or if it is null and the build has no memory
// device.
PrDrawRenderer* PR_NULLABLE prDrawCpuCreate(const PrDrawCpuOptions* PR_NULLABLE options);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
