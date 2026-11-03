// The state that pr/gpu.h keeps for a device beside the backend's: what every backend gets from the core.

#pragma once

#include "pr/gpu.h"
#include "pr/mutex.h"
#include "pr/raw_array.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// A sampler that the device made for a description.
struct GpuSamplerEntry {
    PrGpuSamplerDesc desc;
    PrGpuSampler sampler;
};

// A pipeline that draws mip levels into a format.
struct GpuMipmapPipeline {
    PrGpuFormat format;
    PrGpuPipeline pipeline;
};

// pr/gpu.h's own state of a device: samplers by description, and what it draws mip levels with. The mutex guards it.
struct GpuDeviceShared {
    PrMutex mutex;
    RawArray<GpuSamplerEntry> samplers;
    PrGpuShaderLibrary* PR_NULLABLE mipmapLibrary;
    RawArray<GpuMipmapPipeline> mipmapPipelines;
};

} // namespace pr_impl

// The handle that a device holds its shared state by (PrGpuDevice::shared).
struct PrGpuDeviceShared : pr_impl::GpuDeviceShared {};

PR_ASSUME_NONNULL_END
