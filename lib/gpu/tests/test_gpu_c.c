#include "pr/gpu.h"
#include "pr/gpu_backend.h"

int prTestGpuCApi(void);

// The public headers compiled as C. Checks the descriptions a caller fills in.
int prTestGpuCApi(void) {
    PrGpuTextureDesc desc = {
        .kind = PrGpuTextureKind_2d,
        .format = PrGpuFormat_Bgra8,
        .usage = PrGpuTextureUsageFlag_Sampled | PrGpuTextureUsageFlag_RenderTarget,
        .levels = 1,
        .samples = 1,
        .extent = {64, 32, 1},
    };
    PrGpuBufferDesc buffer = {.size = 256, .memory = PrGpuMemory_Upload};
    PrGpuTexture none = {0};
    prGpuDeviceDestroy(NULL);
    prGpuSwapChainDestroy(NULL);
    prGpuShaderLibraryDestroy(NULL);
    return desc.extent.width != 64 || buffer.size != 256 || none.id != 0;
}
