// The interface that document writers implement. Callers use the functions of pr/draw_writer.h, which call these. Only
// writers include this header.

#pragma once

#include "pr/base.h"
#include "pr/draw_writer.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The functions of a writer's backend, at the start of its state. Each implements the function of pr/draw_writer.h that
// it is named for. A null `destroyFn` means that there is nothing to free.
struct PrDrawWriter {
    void (*PR_NULLABLE destroyFn)(PrDrawWriter* writer);
    void (*beginFn)(PrDrawWriter* writer, const PrDrawDocumentDesc* desc);
    void (*addPageFn)(PrDrawWriter* writer, const PrDrawPage* page);
    void (*endFn)(PrDrawWriter* writer);
};

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
