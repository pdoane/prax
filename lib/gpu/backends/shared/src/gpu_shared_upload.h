// Batches of uploads and their staging, which the backends that stage uploads share.
//
// Uploads are staged in blocks of memory that the CPU writes and the GPU copies from. The open batch fills one block.
// If an upload does not fit in the rest of that block, the batch is submitted and the upload starts a new block. The
// GPU copies from a full block while the CPU fills the next, so a device that uploads steadily uses about two blocks. A
// block holds kGpuSharedStagingBlockBytes, or one larger upload. A block of the standard size is reused once the work
// of its batch is done, and a larger block is freed.
//
// A batch holds its copies into buffers kept disjoint, and its copies into textures ordered. Several uploads to one
// resource in a batch land as they would copied in order. Copies to buffers are kept disjoint as they are added: where
// a later copy writes bytes that an earlier one wrote, the earlier one is trimmed, which is what copying them in order
// would leave. Nothing then orders them, so they are copied all at once. Copies to textures are copied in order, with a
// barrier before a copy to a texture that the batch wrote since its last barrier.

#pragma once

#include "pr/base.h"
#include "pr/gpu.h"
#include "pr/raw_array.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// A copy of `size` bytes from the batch's staging block at `source` into the buffer `buffer` at `target`.
struct GpuSharedUploadCopy {
    uint32_t buffer;
    uint64_t target;
    uint64_t source;
    uint64_t size;
};

// The copies of a batch to buffers, disjoint, and the textures that the batch wrote since its last barrier.
struct GpuSharedUploadBatch {
    RawArray<GpuSharedUploadCopy> copies; // sorted by their buffers, then by their target offsets
    RawArray<uint32_t> textures;          // written since the last barrier
};

// Adds a copy into a buffer, trimming the copies already in the batch that it overwrites.
void gpuSharedUploadAddCopy(GpuSharedUploadBatch* batch, const GpuSharedUploadCopy& copy);

// Returns true if a copy into `texture` needs a barrier before it, which it does if the batch wrote the texture since
// its last barrier. A barrier starts the tracking again from the texture.
bool gpuSharedUploadNeedsTextureBarrier(GpuSharedUploadBatch* batch, uint32_t texture);

// Empties the batch, keeping its storage, or frees its storage.
void gpuSharedUploadBatchClear(GpuSharedUploadBatch* batch);
void gpuSharedUploadBatchReset(GpuSharedUploadBatch* batch);

// The bytes of a staging block, and the alignment of a larger block's size.
constexpr uint64_t kGpuSharedStagingBlockBytes = uint64_t(4) << 20;
constexpr uint64_t kGpuSharedStagingAlignment = 256;

// A block of staging memory, which `id` names to the backend that made it.
struct GpuSharedStagingBlock {
    uint8_t* PR_NULLABLE mapped;
    uint64_t size;
    uint32_t id;
};

// A block of a submitted batch, and the fence after which no work reads it.
struct GpuSharedStagingHeld {
    PrGpuFence fence;
    GpuSharedStagingBlock block;
};

// The staging of a device's uploads. The device's mutex guards it, and its functions are called with the mutex held.
//
// The backend's functions make and free blocks. The submit function submits the open batch and calls
// gpuSharedStagingSubmitted with the batch's fence. The collect function, if any, is called before a block is taken, so
// that blocks whose work is done are reused. The idle function, if any, is called with true when a block becomes free
// and with false when it is taken again.
struct GpuSharedStaging {
    void* PR_NULLABLE context;
    bool (*PR_NULLABLE makeFn)(void* PR_NULLABLE context, uint64_t size, GpuSharedStagingBlock* out);
    void (*PR_NULLABLE freeFn)(void* PR_NULLABLE context, const GpuSharedStagingBlock& block);
    void (*PR_NULLABLE submitFn)(void* PR_NULLABLE context);
    void (*PR_NULLABLE collectFn)(void* PR_NULLABLE context);
    void (*PR_NULLABLE idleFn)(void* PR_NULLABLE context, const GpuSharedStagingBlock& block, bool idle);

    GpuSharedStagingBlock open;                 // the open batch's block, with null `mapped` if it has none
    uint64_t used;                              // the bytes of `open` that the batch filled
    RawArray<GpuSharedStagingBlock> free;       // of kGpuSharedStagingBlockBytes
    RawArray<GpuSharedStagingHeld> held;        // in the order of their fences
};

// Takes `size` bytes at `alignment` in the open batch's block, at `*offset` in `staging->open`, submitting the batch
// first if they do not fit. Returns false if the device has no memory for them.
bool gpuSharedStage(GpuSharedStaging* staging, uint64_t size, uint64_t alignment, uint64_t* offset);

// Ends the open batch, which the work of `fence` copies from.
void gpuSharedStagingSubmitted(GpuSharedStaging* staging, PrGpuFence fence);

// Reclaims the blocks of the batches whose fences are at or before `done`.
void gpuSharedStagingCollect(GpuSharedStaging* staging, PrGpuFence done);

// Frees the staging's own storage, as the device is destroyed. The backend frees the blocks with the rest of its
// memory.
void gpuSharedStagingReset(GpuSharedStaging* staging);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
