// Batches of uploads and their staging (gpu_shared_upload.h).

#include "gpu/backends/shared/src/gpu_shared_upload.h"

#include "pr/align.h"
#include "pr/search.h"

namespace pr_impl {

void gpuSharedUploadAddCopy(GpuSharedUploadBatch* batch, const GpuSharedUploadCopy& copy) {
    RawArray<GpuSharedUploadCopy>& copies = batch->copies;
    uint64_t lo = copy.target;
    uint64_t hi = copy.target + copy.size;
    // The first copy that sorts at or after (copy.buffer, lo).
    uint32_t at = partitionPoint(0, copies.count, [&](uint32_t i) {
        const GpuSharedUploadCopy& c = copies.data[i];
        return c.buffer < copy.buffer || (c.buffer == copy.buffer && c.target < lo);
    });
    // The copy before may reach into this one, and keeps what lies before it, and after it if it reaches past.
    if (at > 0 && copies.data[at - 1].buffer == copy.buffer) {
        GpuSharedUploadCopy before = copies.data[at - 1];
        uint64_t end = before.target + before.size;
        if (end > lo) {
            copies.data[at - 1].size = lo - before.target;
            if (end > hi) {
                GpuSharedUploadCopy after = before;
                after.source += hi - before.target;
                after.target = hi;
                after.size = end - hi;
                copies.insert(at, after);
            }
        }
    }
    // The copies that start within this one lose what it covers, and are gone if it covers them whole.
    uint32_t end = at;
    while (end < copies.count && copies.data[end].buffer == copy.buffer && copies.data[end].target < hi) {
        GpuSharedUploadCopy& c = copies.data[end];
        uint64_t cEnd = c.target + c.size;
        if (cEnd > hi) {
            c.source += hi - c.target;
            c.size = cEnd - hi;
            c.target = hi;
            break;
        }
        ++end;
    }
    copies.erase(at, end - at);
    copies.insert(at, copy);
}

bool gpuSharedUploadNeedsTextureBarrier(GpuSharedUploadBatch* batch, uint32_t texture) {
    if (batch->textures.find(texture) == kArrayMissing) {
        batch->textures.push(texture);
        return false;
    }
    batch->textures.clear();
    batch->textures.push(texture);
    return true;
}

void gpuSharedUploadBatchClear(GpuSharedUploadBatch* batch) {
    batch->copies.clear();
    batch->textures.clear();
}

void gpuSharedUploadBatchReset(GpuSharedUploadBatch* batch) {
    batch->copies.reset();
    batch->textures.reset();
}

bool gpuSharedStage(GpuSharedStaging* staging, uint64_t size, uint64_t alignment, uint64_t* offset) {
    if (staging->open.mapped != nullptr) {
        uint64_t at = alignUp(staging->used, alignment);
        if (at + size <= staging->open.size) {
            staging->used = at + size;
            *offset = at;
            return true;
        }
        staging->submitFn(staging->context);
        // If the backend had nothing to submit, no work reads the block.
        gpuSharedStagingSubmitted(staging, 0);
    }
    if (staging->collectFn != nullptr)
        staging->collectFn(staging->context);
    GpuSharedStagingBlock block{};
    if (size <= kGpuSharedStagingBlockBytes && staging->free.count != 0) {
        block = staging->free.last();
        staging->free.pop();
        if (staging->idleFn != nullptr)
            staging->idleFn(staging->context, block, false);
    } else {
        uint64_t bytes = kGpuSharedStagingBlockBytes;
        if (size > bytes)
            bytes = alignUp(size, kGpuSharedStagingAlignment);
        if (!staging->makeFn(staging->context, bytes, &block))
            return false;
    }
    staging->open = block;
    staging->used = size;
    *offset = 0;
    return true;
}

void gpuSharedStagingSubmitted(GpuSharedStaging* staging, PrGpuFence fence) {
    if (staging->open.mapped == nullptr)
        return;
    staging->held.push(GpuSharedStagingHeld{fence, staging->open});
    staging->open = GpuSharedStagingBlock{};
    staging->used = 0;
}

void gpuSharedStagingCollect(GpuSharedStaging* staging, PrGpuFence done) {
    uint32_t taken = 0;
    while (taken < staging->held.count && staging->held.data[taken].fence <= done) {
        GpuSharedStagingBlock block = staging->held.data[taken++].block;
        if (block.size != kGpuSharedStagingBlockBytes) {
            staging->freeFn(staging->context, block);
            continue;
        }
        if (staging->idleFn != nullptr)
            staging->idleFn(staging->context, block, true);
        staging->free.push(block);
    }
    staging->held.erase(0, taken);
}

void gpuSharedStagingReset(GpuSharedStaging* staging) {
    staging->open = GpuSharedStagingBlock{};
    staging->used = 0;
    staging->held.reset();
    staging->free.reset();
}

} // namespace pr_impl
