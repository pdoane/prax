// The Metal GPU backend on Metal 4, which is kept for experiments. pr/gpu_metal.h is the default Metal backend.
//
// The native objects cross this header as untyped pointers. A caller bridges them to their Objective-C types, such as
// `id<MTLDevice>`.
//
// A swap chain for SDR holds Bgra8 in sRGB. A swap chain for HDR holds Rgba16F in extended sRGB, encoded, if the
// window's display has EDR headroom, and the layer then presents colors past sRGB's gamut and past SDR white. On a
// display without EDR headroom, an HDR swap chain holds what an SDR one does, whatever the display's gamut. A swap
// chain follows its window's display: each acquire on the main thread checks the display again.
//
// Every resource of the device is in one residency set, which the device's queue holds. An application that submits its
// own command buffers that use the device's resources adds the set to its queue (prGpuMetal4ResidencySet).

#pragma once

#include "pr/base.h"
#include "pr/gpu.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The Metal objects that a device is created over.
typedef struct PrGpuMetal4Options {
    void* PR_NULLABLE device;   // the application's id<MTLDevice>; null for the system's default
    void* PR_NULLABLE queue;    // the application's id<MTL4CommandQueue> on the device; null to create one
    bool validation;            // labels objects for Metal's validation, which the environment turns on
} PrGpuMetal4Options;

// Returns the default options: the system's default device, with a queue of the device's own.
PrGpuMetal4Options prGpuMetal4OptionsDefault(void);

// Creates a device over Metal 4. The device retains the application's Metal device and queue. Null `options` means the
// defaults. Returns NotSupported if the system has no Metal device that supports Metal 4. On failure, `*out` is null.
PrStatus prGpuMetal4Create(const PrGpuMetal4Options* PR_NULLABLE options, PrGpuDevice* PR_NULLABLE* PR_NONNULL out);

// Return the native objects of the device, valid while it lives: its id<MTLDevice>, its id<MTL4CommandQueue>, and the
// id<MTLResidencySet> that holds its resources.
void* prGpuMetal4Device(const PrGpuDevice* device);
void* prGpuMetal4Queue(const PrGpuDevice* device);
void* prGpuMetal4ResidencySet(const PrGpuDevice* device);

// The native objects of PrGpuNativeKind_Metal4. A texture's object is its id<MTLTexture> (void*), which is valid while
// the texture lives. A pass's object is its encoder and the command buffer that it records into. A native draw
// (prGpuPassNativeDraw) records its own work into them while the device records the draw, and they are valid only
// during the draw. A native draw leaves the encoder open.
typedef struct PrGpuMetal4NativePass {
    void* encoder;          // the pass's id<MTL4RenderCommandEncoder>
    void* commandBuffer;    // the id<MTL4CommandBuffer> that the pass records into
} PrGpuMetal4NativePass;

// The device imports a PrGpuImportKind_CoreVideo image in place. The image is a CVPixelBufferRef of one plane in
// kCVPixelFormatType_32BGRA, backed by an IOSurface. The device retains the buffer, and the texture is in Bgra8.

// Wraps the application's id<MTLTexture> as a texture of the device, which retains it until the texture is destroyed.
// The application keeps what the texture holds. `usage` says how the device may use it. Returns Limit if the device has
// no handle left.
PrStatus prGpuMetal4TextureWrap(PrGpuDevice* device, void* texture, PrGpuTextureUsageFlags usage, PrGpuTexture* out);

// A command buffer that records into the application's native one passes its id<MTL4CommandBuffer>, begun and not yet
// ended, as PrGpuCommandsOptions::native. A pass of the application's passes its id<MTL4RenderCommandEncoder> as
// PrGpuPassDesc::native.

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
