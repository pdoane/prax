// The WebGPU GPU backend, on the WebGPU of the page that runs the program.
//
// WebGPU has no bindless, so a device binds resources only to slots (PrGpuFeatureFlag_Bindless is never set), and it
// refuses a pipeline whose shaders reach resources by handle. Push data and uniforms are uniform buffers in the command
// buffer's stream. Barriers do nothing, as WebGPU orders the accesses of its work itself.
//
// The page creates its devices asynchronously, so a device is ready once the page's event loop finishes its creation,
// with or without a pool. The page's thread is the device's only thread. A wait for a fence returns at once, as the
// page reaches fences only between its tasks.
//
// A swap chain presents to a page's canvas, which a surface of PrAppSurfaceKind_HtmlCanvas names, in 8-bit sRGB.

#pragma once

#include "pr/base.h"
#include "pr/gpu.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates a device, as prGpuDeviceCreate does. Its creation fails with NotSupported if the page has no WebGPU or no
// adapter. Null `options` means the defaults, of which only the ready callback applies.
PrGpuDevice* prGpuWebgpuCreate(const PrGpuDeviceOptions* PR_NULLABLE options);

// The calls of the page's side of the backend. prGpuWebgpuCreated gives `device` the page's id of its GPUDevice, or
// zero if the page made none, its limits, and whether the device has timestamps. prGpuWebgpuReached says that the
// device's work reached `fence`, and gives the seconds that the work of that fence took, or zero without timestamps.
// prGpuWebgpuLost says that the page lost the device.
void prGpuWebgpuCreated(void* device, uint32_t page, uint32_t maxTextureSize, uint32_t storageOffsetAlignment,
    bool timestamps);
void prGpuWebgpuReached(void* device, double fence, double seconds);
void prGpuWebgpuLost(void* device);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
