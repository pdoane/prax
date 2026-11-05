// Reads the texels of a CVPixelBuffer on the CPU.

#include "gpu/src/gpu_corevideo.h"

#import <CoreVideo/CoreVideo.h>

namespace pr_impl {

bool gpuCoreVideoLock(const void* buffer, PrGpuImportPixels* out) {
    CVPixelBufferRef pixelBuffer = static_cast<CVPixelBufferRef>(const_cast<void*>(buffer));
    if (CVPixelBufferGetPixelFormatType(pixelBuffer) != kCVPixelFormatType_32BGRA || CVPixelBufferIsPlanar(pixelBuffer) ||
            CVPixelBufferLockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess)
        return false;
    *out = PrGpuImportPixels{
        .pixels = PrPixelView{
            static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(pixelBuffer)),
            CVPixelBufferGetBytesPerRow(pixelBuffer),
            PrUVec2{uint32_t(CVPixelBufferGetWidth(pixelBuffer)), uint32_t(CVPixelBufferGetHeight(pixelBuffer))},
        },
        .format = PrGpuFormat_Bgra8,
    };
    return true;
}

void gpuCoreVideoUnlock(const void* buffer) {
    CVPixelBufferUnlockBaseAddress(static_cast<CVPixelBufferRef>(const_cast<void*>(buffer)),
        kCVPixelBufferLock_ReadOnly);
}

} // namespace pr_impl
