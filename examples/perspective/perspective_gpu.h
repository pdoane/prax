// The perspective example's own pipelines on a lib/gpu device: the background that the host draws, and a copy of the
// scene texture that it renders the background into first.

#pragma once

#include "pr/gpu.h"

PR_ASSUME_NONNULL_BEGIN

namespace perspective {

// The arguments that the background's shader reads, laid out as the shaders declare them: a vertical gradient from
// `top` to `bottom` over `height` logical pixels, under a grid of cells `cell` logical pixels apart with `gap` between
// them, drifting by `drift` logical pixels a second. A pass's pixel (0, 0) is the background's target pixel `origin`.
// The background is sRGB, written encoded, as lib/draw's targets hold it.
struct BackgroundArguments {
    float palette[4][4];
    float top[4];
    float bottom[4];
    float origin[2];
    float drift[2];
    float pixelRatio;
    float time;
    float height;
    float cell;
    float gap;
    float _pad[3];
};

struct Pipelines;

// Creates the pipelines on `device`. Returns null, after printing why, if the device cannot make them.
Pipelines* PR_NULLABLE pipelinesCreate(PrGpuDevice* device);

// Frees the pipelines. The device's work that uses them must be done.
void pipelinesDestroy(Pipelines* PR_NULLABLE pipelines);

// Draws the background over the whole of the pass's target: the example's own pass, or a pass of lib/draw's renderer
// that redraws what lies beneath.
void pipelinesDrawBackground(Pipelines* pipelines, PrGpuPass* pass, const BackgroundArguments& arguments);

// Draws `scene`, a texture of the pass's target's extent that holds encoded sRGB, over the whole of the target. The
// texture is in the accesses of shader reads.
void pipelinesCopy(Pipelines* pipelines, PrGpuPass* pass, PrGpuTexture scene);

} // namespace perspective

PR_ASSUME_NONNULL_END
