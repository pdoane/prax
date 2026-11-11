// The Metal GPU backend, on Metal 3, which is the default Metal backend. pr/gpu_metal4.h is the same interface on Metal
// 4.
//
// The native objects cross this header as untyped pointers. A caller bridges them to their Objective-C types, such as
// `id<MTLDevice>`.
//
// A swap chain for SDR holds Bgra8 in sRGB. A swap chain for HDR holds Rgba16F in extended sRGB, encoded, if the
// window's display has EDR headroom, and the layer then presents colors past sRGB's gamut and past SDR white. On a
// display without EDR headroom, an HDR swap chain holds what an SDR one does, whatever the display's gamut. A swap
// chain follows its window's display: each acquire on the main thread checks the display again.
//
// A device on Metal 3 has PrGpuFeatureFlag_Bindless, and every resource of the device is in one residency set, which
// the device's queue holds. An application that submits its own command buffers that use the device's resources adds
// the set to its queue (prGpuMetalResidencySet). A device without Metal 3, such as the iOS simulator's, or one created
// `slotsOnly`, binds resources only to slots, which makes them resident in the encoders that bind them.

#pragma once

#include "pr/base.h"
#include "pr/gpu.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The Metal objects that a device is created over.
typedef struct PrGpuMetalOptions {
    void* PR_NULLABLE device;   // the application's id<MTLDevice>; null for the system's default
    void* PR_NULLABLE queue;    // the application's id<MTLCommandQueue> on the device; null to create one
    bool validation;            // labels objects for Metal's validation, which the environment turns on
    bool slotsOnly;             // creates a device without PrGpuFeatureFlag_Bindless on Metal 3 too
} PrGpuMetalOptions;

// Returns the default options: the system's default device, with a queue of the device's own.
PrGpuMetalOptions prGpuMetalOptionsDefault(void);

// Creates a device over Metal. The device retains the application's Metal device and queue. Null `options` means the
// defaults. Returns NotSupported if the system has no Metal device. On failure, `*out` is null.
PrStatus prGpuMetalCreate(const PrGpuMetalOptions* PR_NULLABLE options, PrGpuDevice* PR_NULLABLE* PR_NONNULL out);

// Return the native objects of the device, valid while it lives: its id<MTLDevice>, its id<MTLCommandQueue>, and the
// id<MTLResidencySet> that holds its resources, which is null on a device without PrGpuFeatureFlag_Bindless.
void* prGpuMetalDevice(const PrGpuDevice* device);
void* prGpuMetalQueue(const PrGpuDevice* device);
void* prGpuMetalResidencySet(const PrGpuDevice* device);

// Returns the id<MTLFence> that orders the device's encoders, valid while the device lives. An encoder of the
// application's on the device's queue that shares resources with the device waits on the fence at its start, and
// updates it at its end. A pass of the application's that the device draws in does this through prGpuPassBegin and
// prGpuPassEnd.
void* prGpuMetalOrderFence(const PrGpuDevice* device);

// The native objects of PrGpuNativeKind_Metal. A texture's object is its id<MTLTexture> (void*), which is valid while
// the texture lives. A pass's object is its encoder and the command buffer that it records into. A native draw
// (prGpuPassNativeDraw) records its own work into them while the device records the draw, and they are valid only
// during the draw. A native draw leaves the encoder open.
typedef struct PrGpuMetalNativePass {
    void* encoder;          // the pass's id<MTLRenderCommandEncoder>
    void* commandBuffer;    // the id<MTLCommandBuffer> that the pass records into
} PrGpuMetalNativePass;

// The device imports a PrGpuImportKind_CoreVideo image in place. The image is a CVPixelBufferRef of one plane in
// kCVPixelFormatType_32BGRA, backed by an IOSurface. The device retains the buffer, and the texture is in Bgra8.

// Wraps the application's id<MTLTexture> as a texture of the device, which retains it until the texture is destroyed.
// The application keeps what the texture holds. `usage` says how the device may use it. Returns Limit if the device has
// no handle left.
PrStatus prGpuMetalTextureWrap(PrGpuDevice* device, void* texture, PrGpuTextureUsageFlags usage, PrGpuTexture* out);

// A command buffer that records into the application's native one passes its id<MTLCommandBuffer>, not yet committed,
// as PrGpuCommandsOptions::native. A pass of the application's passes its id<MTLRenderCommandEncoder>, before its first
// draw, as PrGpuPassDesc::native, and the application ends the encoder after prGpuPassEnd.

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
