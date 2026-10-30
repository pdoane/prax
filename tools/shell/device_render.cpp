#include "tools/shell/device_render.h"

#include "pr/draw_renderer.h"
#include "pr/gpu.h"

using namespace pr;

namespace shell {

struct DeviceRender {
    PrGpuDevice* device;
    PrDrawRenderer* renderer;
    PrDrawFrame* frame;
};

DeviceRender* PR_NULLABLE deviceRenderCreate(PrDrawRendererBackend backend) {
    PrGpuDevice* device = nullptr;
    PrDrawRenderer* renderer = nullptr;
    if (prGpuDeviceCreate(PrGpuBackend_Default, nullptr, &device) != PrStatus_Ok ||
            prDrawRendererCreate(backend, device, nullptr, &renderer) != PrStatus_Ok) {
        prGpuDeviceDestroy(device);
        return nullptr;
    }
    return new DeviceRender{device, renderer, prDrawFrameCreate(renderer)};
}

void deviceRenderDestroy(DeviceRender* PR_NULLABLE render) {
    if (render == nullptr)
        return;
    prDrawFrameDestroy(render->frame);
    prDrawRendererDestroy(render->renderer);
    prGpuDeviceDestroy(render->device);
    delete render;
}

bool deviceRenderPixels(DeviceRender* render, const PrDrawList* list, PrUVec2 extent, uint8_t* rgba) {
    PrGpuTextureDesc desc = prGpuTextureDesc2d(PrGpuFormat_Rgba8, extent,
        PrGpuTextureUsageFlag_Sampled | PrGpuTextureUsageFlag_RenderTarget);
    PrGpuTexture texture{};
    if (prGpuTextureCreate(render->device, &desc, &texture) != PrStatus_Ok)
        return false;
    PrGpuCommands* commands = nullptr;
    if (!prGpuCommandsBegin(render->device, nullptr, &commands)) {
        prGpuTextureDestroy(render->device, texture);
        return false;
    }
    PrDrawTarget target{.extent = extent, .format = PrPixelFormat_Rgba8, .commands = commands};
    bool ok = prDrawFramePrepare(render->frame, list, &target) == PrStatus_Ok;
    PrGpuSubresources all{};
    prGpuCommandsTextureBarrier(commands, texture, all, PrGpuAccessFlag_ShaderRead, PrGpuAccessFlag_ColorAttachment);
    PrGpuPassDesc pass = prGpuPassDescColor(texture, PrGpuLoad_Clear, PrColor{0, 0, 0, 0});
    PrGpuPass* encoder = prGpuPassBegin(commands, &pass);
    if (ok)
        prDrawFrameRender(render->frame, encoder, PrIVec2{0, 0});
    prGpuPassEnd(encoder);
    prGpuCommandsTextureBarrier(commands, texture, all, PrGpuAccessFlag_ColorAttachment, PrGpuAccessFlag_ShaderRead);
    prGpuCommandsSubmitAndWait(commands);
    PrPixels texels{rgba, size_t(extent.x) * 4, extent};
    prGpuTextureRead(render->device, texture, PrGpuAccessFlag_ShaderRead, PrIVec2{0, 0}, texels);
    prGpuTextureDestroy(render->device, texture);
    return ok;
}

} // namespace shell
