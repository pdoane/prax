// The Metal GPU backend, on Metal 4.
//
// The native objects cross this header as untyped pointers. A caller bridges them to their Objective-C types, such as
// `id<MTLDevice>`.
//
// A swap chain for SDR holds Bgra8 in sRGB, and one for HDR holds Rgba16F in extended sRGB, encoded, which the layer
// presents past SDR white.
//
// Every resource of the device is in one residency set, which the device's queue holds. A host that submits its own
// command buffers that use the device's resources adds the set to its queue (prGpuMetalResidencySet).

#pragma once

#include "pr/base.h"
#include "pr/gpu.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The Metal objects that a device is created over.
typedef struct PrGpuMetalOptions {
    void* PR_NULLABLE device;   // the host's id<MTLDevice>; null for the system's default
    void* PR_NULLABLE queue;    // the host's id<MTL4CommandQueue> on the device; null to create one
    bool validation;            // labels objects for Metal's validation, which the environment turns on
} PrGpuMetalOptions;

// Returns the default options: the system's default device, with a queue of the device's own.
PrGpuMetalOptions prGpuMetalOptionsDefault(void);

// Creates a device over Metal 4. The device retains the host's Metal device and queue. Null `options` means the
// defaults. Returns NotSupported if the system has no Metal device that supports Metal 4. On failure, `*out` is null.
PrStatus prGpuMetalCreate(const PrGpuMetalOptions* PR_NULLABLE options, PrGpuDevice* PR_NULLABLE* PR_NONNULL out);

// Return the native objects of the device, valid while it lives: its id<MTLDevice>, its id<MTL4CommandQueue>, and the
// id<MTLResidencySet> that holds its resources.
void* prGpuMetalDevice(const PrGpuDevice* device);
void* prGpuMetalQueue(const PrGpuDevice* device);
void* prGpuMetalResidencySet(const PrGpuDevice* device);

// Returns the id<MTLTexture> of a texture, valid while the texture lives.
void* prGpuMetalTexture(const PrGpuDevice* device, PrGpuTexture texture);

// Wraps the host's id<MTLTexture> as a texture of the device, which retains it until the texture is destroyed. The host
// keeps what the texture holds. `usage` says how the device may use it. Returns Limit if the device has no handle left.
PrStatus prGpuMetalTextureWrap(PrGpuDevice* device, void* texture, PrGpuTextureUsageFlags usage, PrGpuTexture* out);

// A command buffer that records into the host's native one passes its id<MTL4CommandBuffer>, begun and not yet ended,
// as PrGpuCommandsOptions::native. A pass of the host's passes its id<MTL4RenderCommandEncoder> as PrGpuPassDesc::host.

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
