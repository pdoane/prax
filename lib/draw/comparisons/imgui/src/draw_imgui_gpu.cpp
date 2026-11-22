// The Dear ImGui draw backend on a lib/gpu device, which draws a prepared frame with the renderer of imgui_gpu.h.

#include "draw/comparisons/imgui/src/draw_imgui_gpu.h"

#include "pr/alloc.h"
#include "pr/draw_renderer_backend.h"
#include "pr/mem.h"
#include "pr/texture.h"

#include "draw/comparisons/imgui/config/imgui_gpu.h"

#include "imgui.h"

namespace pr_impl {

namespace {

struct ImGuiGpu : ImGuiBackend {
    PrGpuDevice* device;
};

PrGpuDevice* rendererDevice(const PrDrawRenderer* renderer) {
    return static_cast<const ImGuiGpu*>(renderer)->device;
}

void destroyFrame(PrDrawFrame* handle) {
    ImGui::SetCurrentContext(static_cast<ImGuiFrame*>(handle)->renderer->context);
    ImGuiFrame* frame = static_cast<ImGuiFrame*>(handle);
    imguiFrameClear(frame);
    prMemoryFree(frame);
}

PrStatus prepare(PrDrawFrame* handle, const PrDrawList* list, const PrDrawPart* part, const PrDrawTarget* target) {
    ImGuiFrame* frame = static_cast<ImGuiFrame*>(handle);
    ImGuiGpu* g = static_cast<ImGuiGpu*>(frame->renderer);
    ImGui::SetCurrentContext(g->context);
    imguiFrameClear(frame);
    ++g->render;
    imguiRecord(g, frame, list, *part, target);
    return PrStatus_Ok;
}

// Draws the lists with the render's pixel (0, 0) at `origin`. Their projection starts at the negated origin over the
// whole attachments. Every draw has a clip at the render's bounds, which keeps it within the render.
void render(PrDrawFrame* handle, PrGpuPass* pass, PrIVec2 origin) {
    ImGui::SetCurrentContext(static_cast<ImGuiFrame*>(handle)->renderer->context);
    ImGuiFrame* frame = static_cast<ImGuiFrame*>(handle);
    PrUVec2 extent = prGpuPassExtent(pass);
    frame->data.DisplayPos = ImVec2(-float(origin.x), -float(origin.y));
    frame->data.DisplaySize = ImVec2(float(extent.x), float(extent.y));
    frame->data.FramebufferScale = ImVec2(1, 1);
    imguiGpuRender(&frame->data, pass);
}

PrDrawFrame* createFrame(PrDrawRenderer* renderer) {
    ImGuiFrame* frame = allocZeroed<ImGuiFrame>();
    frame->destroyFn = destroyFrame;
    frame->prepareFn = prepare;
    frame->renderFn = render;
    frame->renderer = static_cast<ImGuiGpu*>(renderer);
    return frame;
}

void destroyBackend(PrDrawRenderer* renderer) {
    ImGuiGpu* g = static_cast<ImGuiGpu*>(renderer);
    ImGui::SetCurrentContext(g->context);
    imguiGpuShutdown();
    imguiContextDestroy(g);
    prMemoryFree(g);
}

} // namespace

ImTextureID imguiHostTexture(PrGpuTexture texture, PrTextureAlpha alpha) {
    switch (alpha) {
        case PrTextureAlpha_None: return imguiGpuTexture(texture, ImGuiGpuAlpha_Opaque);
        case PrTextureAlpha_Straight: return imguiGpuTexture(texture, ImGuiGpuAlpha_Straight);
        case PrTextureAlpha_Premultiplied: break;
    }
    return imguiGpuTexture(texture, ImGuiGpuAlpha_Premultiplied);
}

ImGuiBackend* PR_NULLABLE imguiDeviceCreate(PrGpuDevice* device) {
    if (!imguiGpuInit(device))
        return nullptr;
    ImGuiGpu* g = allocZeroed<ImGuiGpu>();
    g->device = device;
    g->destroyFn = destroyBackend;
    g->frameCreateFn = createFrame;
    g->name = "imgui";
    g->deviceFn = rendererDevice;
    return g;
}

} // namespace pr_impl
