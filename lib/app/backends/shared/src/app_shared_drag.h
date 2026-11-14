// Drags: the drag that a window starts, and a window's answer to a drag. Each backend keeps them in its state.

#pragma once

#include "pr/app_data.h"
#include "pr/base.h"
#include "pr/raw_array.h"

#include "app/backends/shared/src/app_shared_data.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// A drag that a window asked for, which the backend starts once no callback runs and keeps while it is in flight.
struct AppDragPending {
    bool pending;
    AppDataStore content;
    AppStrings promises;                // the names of the promised files
    PrAppDragOperationFlags operations;
    RawArray<uint8_t> preview;          // the preview's rows, packed
    PrUVec2 previewExtent;
    PrVec2 previewOffset;
};

// Records a copy of `desc` as the pending drag, replacing one that is pending.
void appDragPendingSet(AppDragPending& pending, const PrAppDragDesc& desc);

// Frees the pending drag.
void appDragPendingReset(AppDragPending& pending);

// Returns the operation that a window performs for a drop, given its latest output's operation and the operations that
// the source allows: the operation if the source allows it, and None if not.
PrAppDragOperation appDragAnswer(PrAppDragOperation wanted, PrAppDragOperationFlags allowed);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
