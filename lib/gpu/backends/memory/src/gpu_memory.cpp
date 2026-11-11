// The memory GPU backend: a memory device, without swap chains.

#include "pr/gpu_memory.h"

#include "gpu/backends/shared/src/gpu_shared_device.h"
#include "gpu/backends/shared/src/gpu_shared_memory.h"

using namespace pr_impl;

PrGpuDevice* prGpuMemoryCreate(const PrGpuDeviceOptions* PR_NULLABLE options) {
    PrGpuDeviceOptions o = options != nullptr ? *options : prGpuDeviceOptionsDefault();
    GpuSharedDevice* device = static_cast<GpuSharedDevice*>(gpuMemoryCreate("memory", nullptr));
    gpuSharedDeviceStart(device, o.pool, o.ready, o.readyContext);
    return device;
}
