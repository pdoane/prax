// The Metal swap chain over a CompositorServices session's surface, on visionOS.

#pragma once

#include "gpu/backends/shared/src/gpu_shared_metal.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Creates a swap chain over a CompositorServices session's surface, for a backend that encodes a compositor's present
// (GpuMetalDevice::compositorPresents).
PrStatus gpuMetalCompositorSwapChainCreate(PrGpuDevice* device, const PrSurface* surface,
    PrGpuSwapChain* PR_NULLABLE* PR_NONNULL out);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
