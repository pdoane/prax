// Reads the texels of a CVPixelBuffer (PrGpuImportKind_CoreVideo) on the CPU (prGpuImportLock).

#pragma once

#include "pr/base.h"
#include "pr/gpu.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Locks the texels of `buffer`, a CVPixelBufferRef of one plane in BGRA, for reading, and writes them to `out`. The
// texels stay valid until gpuCoreVideoUnlock. Returns false, and locks nothing, for a buffer of any other format.
bool gpuCoreVideoLock(const void* buffer, PrGpuImportPixels* out);
void gpuCoreVideoUnlock(const void* buffer);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
