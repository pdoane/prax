// The interface that renderer backends implement. Callers use the functions of pr/draw_renderer.h, which call these.
// Only renderers include this header.

#pragma once

#include "pr/base.h"
#include "pr/draw_renderer.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The functions of a frame's backend, at the start of its state. Each implements the function of pr/draw_renderer.h
// that it is named for. `prepareFn` renders the part of the list for `target`, and the frame keeps the target's extent
// for its renders. A null `statsFn` means that the renderer counts nothing.
struct PrDrawFrame {
    void (*destroyFn)(PrDrawFrame* frame);
    PrStatus (*prepareFn)(PrDrawFrame* frame, const PrDrawList* list, const PrDrawPart* part, const PrDrawTarget* target);
    void (*renderFn)(PrDrawFrame* frame, PrGpuPass* pass, PrIVec2 origin);
    void (*PR_NULLABLE statsFn)(const PrDrawFrame* frame, PrDrawStats* out);
};

// The functions of a renderer's backend, at the start of its state. Each implements the function of pr/draw_renderer.h
// that it is named for. A null `destroyFn` means that there is nothing to free, and a null `dropResourcesFn` that the
// renderer keeps nothing on its device.
struct PrDrawRenderer {
    void (*PR_NULLABLE destroyFn)(PrDrawRenderer* renderer);
    PrGpuDevice* PR_NONNULL (*PR_NONNULL deviceFn)(const PrDrawRenderer* renderer);
    PrDrawFrame* PR_NONNULL (*PR_NONNULL frameCreateFn)(PrDrawRenderer* renderer);
    void (*PR_NULLABLE dropResourcesFn)(PrDrawRenderer* renderer);
};

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
