// The viewer's controls drawn by the Dear ImGui renderer on lib/gpu.

#include "tools/viewer/ui_renderer.h"

#include "draw/comparisons/imgui/config/imgui_gpu.h"

using namespace pr;

namespace viewer {

// The renderer keeps its state in the current context, so it holds nothing of its own.
struct UiRenderer {};

UiRenderer* uiRendererCreate(PrGpuDevice* device) {
    imguiGpuInit(device);
    return new UiRenderer{};
}

void uiRendererDestroy(UiRenderer* PR_NULLABLE renderer) {
    if (renderer == nullptr)
        return;
    imguiGpuShutdown();
    delete renderer;
}

ImTextureID uiRendererTexture(UiRenderer*, PrGpuTexture texture) {
    return imguiGpuTexture(texture);
}

void uiRendererRender(UiRenderer*, ImDrawData* data, PrGpuPass* pass) {
    imguiGpuRender(data, pass);
}

} // namespace viewer
