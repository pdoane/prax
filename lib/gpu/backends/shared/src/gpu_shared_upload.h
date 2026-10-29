// Batches of uploads, which the backends that stage uploads share.
//
// A batch holds its copies into buffers kept disjoint, and its copies into textures ordered. Several uploads to one
// resource in a batch land as they would copied in order. Copies to buffers are kept disjoint as they are added: where
// a later copy writes bytes that an earlier one wrote, the earlier one is trimmed, which is what copying them in order
// would leave. Nothing then orders them, so they are copied all at once. Copies to textures are copied in order, with a
// barrier before a copy to a texture that the batch wrote since its last barrier.

#pragma once

#include "pr/array.h"
#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// A copy of `size` bytes from the staging block `block` at `source` into the buffer `buffer` at `target`.
struct GpuSharedUploadCopy {
    uint32_t buffer;
    uint32_t block;
    uint64_t target;
    uint64_t source;
    uint64_t size;
};

// The copies of a batch to buffers, disjoint, and the textures that the batch wrote since its last barrier.
struct GpuSharedUploadBatch {
    Array<GpuSharedUploadCopy> copies; // sorted by their buffers, then by their target offsets
    Array<uint32_t> textures;          // written since the last barrier
};

// Adds a copy into a buffer, trimming the copies already in the batch that it overwrites.
void gpuSharedUploadAddCopy(GpuSharedUploadBatch* batch, const GpuSharedUploadCopy& copy);

// Returns true if a copy into `texture` needs a barrier before it, which it does if the batch wrote the texture since
// its last barrier. A barrier starts the tracking again from the texture.
bool gpuSharedUploadNeedsTextureBarrier(GpuSharedUploadBatch* batch, uint32_t texture);

// Empties the batch, keeping its storage, or frees its storage.
void gpuSharedUploadBatchClear(GpuSharedUploadBatch* batch);
void gpuSharedUploadBatchReset(GpuSharedUploadBatch* batch);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
