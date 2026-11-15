// The Direct3D 11 GPU backend, on Direct3D 11.1 at feature level 11_0.
//
// Direct3D 11 has no bindless, so a device binds resources only to slots (PrGpuFeatureFlag_Bindless is never set), and
// it refuses a pipeline whose shaders reach resources by handle. A fragment shader shares its RwTexture and RwBuffer
// slots with the pass's color attachments (pr/gpu.slang), so a pass with n color attachments binds no RwTexture slot
// below n.
//
// A command buffer records on any thread, and its submission replays the recording on the device's immediate context.
// Barriers do nothing, as Direct3D 11 orders the accesses of its work itself.
//
// Direct3D 11 copies between a buffer and a texture on the CPU's side. A copy into a texture reads a buffer in Upload
// memory or the command buffer's stream, and a copy out of a texture writes a buffer in Readback memory.
//
// A swap chain for SDR holds Bgra8 in sRGB. One for HDR holds HDR10, Rgb10A2 in Rec2100Pq, where the window's display
// is in HDR mode and takes it, and scRGB, Rgba16F in SrgbLinear, elsewhere.

#pragma once

#include "pr/base.h"
#include "pr/gpu.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// How a device is created.
typedef struct PrGpuD3d11Options {
    // Turns on the debug layer, and writes the layer's errors and warnings to the standard error. The layer must be
    // installed, as Windows' Graphics Tools install it. Without it the device is created as if the option were off.
    bool validation;

    // The device's creation runs on `pool`, or during the call if it is null. `ready` is called once the creation is
    // done (PrGpuDeviceOptions).
    PrTaskPool* PR_NULLABLE pool;
    PrGpuDeviceReadyFn PR_NULLABLE ready;
    void* PR_NULLABLE readyContext;
} PrGpuD3d11Options;

// Returns the default options: a device on the system's high-performance adapter, without the debug layer. The
// environment variable PR_D3D11_DEBUG overrides that: 1 turns the layer on, and 0 turns it off. prGpuDeviceCreate turns
// the layer on where its options ask for validation.
PrGpuD3d11Options prGpuD3d11OptionsDefault(void);

// Creates a device over Direct3D 11, as prGpuDeviceCreate does. Null `options` means the defaults. Its creation fails
// with NotSupported if the system has no hardware adapter of feature level 11_0 with constant buffer offsets, dynamic
// constant buffers that map without overwriting, and fences.
PrGpuDevice* prGpuD3d11Create(const PrGpuD3d11Options* PR_NULLABLE options);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
