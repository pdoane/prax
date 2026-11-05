// The memory GPU backend: a memory device (PrGpuNativeKind_Pixels), which presents to no surface. An application that
// renders without a GPU, such as a test or a tool, draws into texels natively (prGpuPassNativeDraw). It reads its
// textures' texels in place (prGpuTextureNative), or creates textures over its own texels
// (prGpuTextureCreateOverPixels).
//
// The device runs a command buffer's work when it is submitted, on the thread that submits it, so every fence is
// reached by then and no wait blocks. Passes load and clear their attachments, and the device runs no shaders. Textures
// have one sample, and are of the uncompressed color formats with float and normalized components, without depth or
// stencil.

#pragma once

#include "pr/base.h"
#include "pr/gpu.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates a device. It always succeeds.
PrStatus prGpuMemoryCreate(PrGpuDevice* PR_NULLABLE* PR_NONNULL out);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
