// The WebGPU GPU backend, on the WebGPU of the page that runs the program.
//
// WebGPU has no bindless, so a device binds resources only to slots (PrGpuFeatureFlag_Bindless is never set), and it
// refuses a pipeline whose shaders reach resources by handle. Push data and uniforms are uniform buffers in the command
// buffer's stream. Barriers do nothing, as WebGPU orders the accesses of its work itself.
//
// WGSL fixes what a binding takes where pr/gpu.h does not, and the page's side meets pr/gpu.h's promises around it:
// - A texture of floats that a source only reads by texel binds as unfilterable, so that a texture of depth binds to
//   it. A format of 32-bit floats is sampled only where the device filters it ("float32-filterable").
// - A storage texture that a shader only writes takes any format that stores: a pipeline compiles again for the format
//   of the texture bound to it, where that differs from what the WGSL declares.
// - A pipeline without a fragment shader draws in a pass with colors, and writes none of them.
// - A copy between a buffer and a texture whose rows do not lie a multiple of 256 bytes apart copies row by row, and an
//   upload of bytes that cover part of a word merges them into the word on the GPU.
//
// The page creates its devices asynchronously, and reaches fences only between its tasks. A program that suspends while
// it waits for the page, as one linked with JSPI does (-sJSPI), creates a device as the other backends do, as a job of
// the pool or during the call, and a wait for a fence returns once the page reaches it. In any other program, a device
// is ready once the page's event loop finishes its creation, with or without a pool, and a wait returns at once. The
// page's thread is the device's only thread.
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
// adapter. Null `options` means the defaults, of which the pool and the ready callback apply.
PrGpuDevice* prGpuWebgpuCreate(const PrGpuDeviceOptions* PR_NULLABLE options);

// Returns the number of validation errors that the page has reported for the device's work, which WebGPU always
// validates. Tests check it after each case.
uint32_t prGpuWebgpuValidationErrors(const PrGpuDevice* device);

// The calls of the page's side of the backend. prGpuWebgpuCreated gives `device` the page's id of its GPUDevice, or
// zero if the page made none, its limits, and the optional features of WebGPU that it has, as bits in the order that
// gpu_webgpu.js names them. prGpuWebgpuReached says that the device's work reached `fence`, and gives the seconds that
// the work of that fence took, or zero without timestamps. prGpuWebgpuLost says that the page lost the device, and
// prGpuWebgpuError that it reported a validation error.
void prGpuWebgpuCreated(void* device, uint32_t page, uint32_t maxTextureSize, uint32_t storageOffsetAlignment, uint32_t features);
void prGpuWebgpuReached(void* device, double fence, double seconds);
void prGpuWebgpuLost(void* device);
void prGpuWebgpuError(void* device);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
