#include "pr/gpu.h"
#include "pr/gpu_backend.h"
#include "pr/gpu_graph.h"

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
    PrGpuGraphPassDesc pass = {
        .viewCount = 1,
        .colorCount = 1,
        .color = {{.load = PrGpuLoad_Clear, .store = PrGpuStore_Keep}},
    };
    PrGpuGraphOptions options = prGpuGraphOptionsDefault();
    prGpuDeviceDestroy(NULL);
    prGpuSwapChainDestroy(NULL);
    prGpuShaderLibraryDestroy(NULL);
    prGpuGraphDestroy(NULL);
    return desc.extent.width != 64 || buffer.size != 256 || none.id != 0 || pass.color[0].texture.id != 0 ||
        options.priority != PrGpuGraphPriority_Barriers;
}
