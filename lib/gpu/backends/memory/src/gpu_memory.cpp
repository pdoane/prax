// The memory GPU backend: a memory device, without swap chains.

#include "pr/gpu_memory.h"

#include "gpu/backends/shared/src/gpu_shared_memory.h"

using namespace pr_impl;

PrStatus prGpuMemoryCreate(PrGpuDevice* PR_NULLABLE* PR_NONNULL out) {
    *out = gpuMemoryCreate("memory", nullptr);
    return PrStatus_Ok;
}
