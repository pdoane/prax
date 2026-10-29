// Filters for the CPU backend: the evaluation of filter graphs over a group's offscreen, and blurs.

#pragma once

#include "pr/draw_filter.h"

#include "draw/backends/cpu/src/cpu_pixels.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Evaluates a brush's alpha over a box of target pixels, row by row, for a node that reads a brush. The filter calls
// this function because it cannot reach the backend's paint itself.
typedef void (*FilterBrushAlphaFn)(void* PR_NULLABLE context, const PrDrawBrush* brush,
                                   PrAffine2 transform, PrIRect box, float* out);

// Returns a texture of an image node's source: the image's texture, or the list's area rendered at `width` by `height`
// pixels. Writes the texels that the source spans to `texels`. Returns null if the source has no texture.
typedef const CpuTexture* PR_NULLABLE (*FilterSourceTextureFn)(void* PR_NULLABLE context,
                                                               const PrDrawSource* source,
                                                               uint32_t width, uint32_t height,
                                                               PrRect* texels);

// The inputs that a filter reads besides the offscreen that it filters.
struct CpuFilterInputs {
    PrAffine2 transform;                    // the group's space to the offscreen's pixels: a scale and translation

    // The group's bounds, which an empty filter region means. Empty bounds mean the offscreen's bounds.
    PrRect bounds;

    const CpuTarget* PR_NULLABLE backdrop;     // what lies beneath
    CpuTextureCache* textures;
    FilterBrushAlphaFn brushAlpha;
    FilterSourceTextureFn sourceTexture;
    void* PR_NULLABLE context;              // passed to brushAlpha and sourceTexture
};

// Returns how far, in target pixels, the filter may move the content that it reads under a transform of `scale`.
int32_t cpuFilterReach(const PrDrawFilter* filter, float scale);

// Replaces the offscreen's pixels with the filter's result, which is transparent outside the filter's region.
void cpuApplyFilter(const PrDrawFilter* filter, const CpuFilterInputs& inputs, const CpuTarget& layer);

// Blurs the coverage by a Gaussian with the given sigmas, in target pixels, reading zero outside its box.
void cpuBlurCoverage(const CpuCoverage& coverage, float sigmaX, float sigmaY);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
