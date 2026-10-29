// Batches of uploads (gpu_shared_upload.h).

#include "gpu/backends/shared/src/gpu_shared_upload.h"

#include "pr/search.h"

namespace pr_impl {

void gpuSharedUploadAddCopy(GpuSharedUploadBatch* batch, const GpuSharedUploadCopy& copy) {
    Array<GpuSharedUploadCopy>& copies = batch->copies;
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

} // namespace pr_impl
