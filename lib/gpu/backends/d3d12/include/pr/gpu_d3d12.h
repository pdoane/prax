// The Direct3D 12 GPU backend, on shader model 6.6's descriptor heaps and enhanced barriers.
//
// The native objects cross this header as untyped pointers. A caller casts them to their D3D12 types, such as
// `ID3D12Device*`.
//
// A swap chain for SDR holds Bgra8 in sRGB. One for HDR holds HDR10, Rgb10A2 in Rec2100Pq, where the window's display
// is in HDR mode and takes it, and scRGB, Rgba16F in SrgbLinear, elsewhere. The choice is made when the swap chain is
// created.
//
// Shaders reach every resource through the device's two shader-visible descriptor heaps, which the device binds on
// every command list that it records into, its own and the application's, as it begins. An application that draws with
// heaps of its own after the device's work binds them again, and a pass of the application's has the device's heaps
// bound. The slots are parameters of the device's root signature: Buffer and RwBuffer slots are root descriptors, and
// Texture slots, their samplers and RwTexture slots are tables of descriptors that the device writes into the heaps
// past the handles.
//
// Copies between buffers and textures lay out rows at multiples of 256 bytes, and start at a multiple of 512 bytes into
// the buffer, as D3D12 requires.

#pragma once

#include "pr/base.h"
#include "pr/gpu.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The D3D12 objects that a device is created over.
typedef struct PrGpuD3d12Options {
    void* PR_NULLABLE device;   // the application's ID3D12Device; null for one on the system's high-performance adapter
    void* PR_NULLABLE queue;    // the application's direct ID3D12CommandQueue on the device; null to create one

    // Turns on the debug layer for a device that the backend creates, and writes the layer's errors and warnings to the
    // standard error. The layer must be installed, as Windows' Graphics Tools install it. Without it the device is
    // created as if the option were off.
    bool validation;
} PrGpuD3d12Options;

// Returns the default options: a device on the system's high-performance adapter, with a queue of the device's own,
// without the debug layer. The environment variable PR_D3D12_DEBUG overrides that: 1 turns the layer on, and 0 turns it
// off. prGpuDeviceCreate turns the layer on where its options ask for validation.
PrGpuD3d12Options prGpuD3d12OptionsDefault(void);

// Creates a device over D3D12. The device holds a reference to the application's device and queue. Null `options` means
// the defaults. Returns NotSupported if the system has no adapter with shader model 6.6, resource binding tier 3 and
// enhanced barriers. On failure, `*out` is null.
PrStatus prGpuD3d12Create(const PrGpuD3d12Options* PR_NULLABLE options, PrGpuDevice* PR_NULLABLE* PR_NONNULL out);

// Return the native objects of the device, valid while it lives: its ID3D12Device and its direct ID3D12CommandQueue.
void* prGpuD3d12Device(const PrGpuDevice* device);
void* prGpuD3d12Queue(const PrGpuDevice* device);

// The native objects of PrGpuNativeKind_D3d12. A texture's object is its ID3D12Resource (void*), which is valid while
// the texture lives, and which a view shares with its texture. A pass's object is the command list that the pass
// records into, with the pass's render targets set. A native draw (prGpuPassNativeDraw) records its own work into it
// while the device records the draw, and it is valid only during the draw. A native draw leaves the pass's render
// targets set.
typedef struct PrGpuD3d12NativePass {
    void* commandList;      // ID3D12GraphicsCommandList7
} PrGpuD3d12NativePass;

// Wraps the application's ID3D12Resource, a texture, as a texture of the device, which holds a reference to it until
// the texture is destroyed. The application keeps what the texture holds. `usage` says how the device may use it, and
// the resource's flags must allow it. Returns NotSupported for a resource of a format that the device has no
// PrGpuFormat for, and Limit if the device has no handle left.
PrStatus prGpuD3d12TextureWrap(PrGpuDevice* device, void* resource, PrGpuTextureUsageFlags usage, PrGpuTexture* out);

// A command buffer that records into the application's native one passes its ID3D12GraphicsCommandList7, open and not
// yet closed, as PrGpuCommandsOptions::native. A pass of the application's passes the same command list, with the
// pass's render targets set, as PrGpuPassDesc::native.

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
