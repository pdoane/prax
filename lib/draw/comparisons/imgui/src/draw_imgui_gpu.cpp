// The Dear ImGui draw backend on a lib/gpu device, which draws a prepared frame with the renderer of imgui_gpu.h.

#include "draw/comparisons/imgui/src/draw_imgui_gpu.h"

#include "pr/alloc.h"
#include "pr/draw_image.h"
#include "pr/mem.h"
#include "pr/pixels.h"
#include "pr/raw_array.h"

#include "draw/comparisons/imgui/config/imgui_gpu.h"

#include "imgui.h"

namespace pr_impl {

namespace {

// An image uploaded for the renderer to sample. It has straight alpha, which is what the renderer blends.
struct CachedTexture {
    uint64_t id;
    PrGpuTexture texture;
    PrUVec2 extent;
    uint64_t lastUsed;
};

struct ImGuiGpu : ImGuiBackend {
    RawArray<CachedTexture> textures;
};

PrDrawFrame* createFrame(PrDrawRenderer* renderer) {
    ImGuiFrame* frame = allocZeroed<ImGuiFrame>();
    frame->renderer = renderer;
    return frame;
}

void destroyFrame(PrDrawRenderer* renderer, PrDrawFrame* handle) {
    ImGui::SetCurrentContext(static_cast<ImGuiGpu*>(renderer)->context);
    ImGuiFrame* frame = static_cast<ImGuiFrame*>(handle);
    imguiFrameClear(frame);
    prMemoryFree(frame);
}

// Drops the images that the render did not use. The device keeps a destroyed texture until the work that reads it is
// done.
void dropUnusedTextures(ImGuiGpu* g) {
    uint32_t kept = 0;
    for (uint32_t i = 0; i < g->textures.count; ++i) {
        if (g->textures.data[i].lastUsed == g->render)
            g->textures.data[kept++] = g->textures.data[i];
        else
            prGpuTextureDestroy(g->device, g->textures.data[i].texture);
    }
    g->textures.count = kept;
}

PrStatus prepare(PrDrawRenderer* renderer, PrDrawFrame* handle, const PrDrawList* list, const PrDrawPart* part,
        const PrDrawTarget* target) {
    ImGuiGpu* g = static_cast<ImGuiGpu*>(renderer);
    ImGui::SetCurrentContext(g->context);
    ImGuiFrame* frame = static_cast<ImGuiFrame*>(handle);
    imguiFrameClear(frame);
    ++g->render;
    imguiRecord(g, frame, list, *part, target);
    dropUnusedTextures(g);
    return PrStatus_Ok;
}

// Draws the lists with the render's pixel (0, 0) at `origin`. Their projection starts at the negated origin over the
// whole attachments. Every draw has a clip at the render's bounds, which keeps it within the render.
void render(PrDrawRenderer* renderer, PrDrawFrame* handle, PrGpuPass* pass, PrIVec2 origin) {
    ImGui::SetCurrentContext(static_cast<ImGuiGpu*>(renderer)->context);
    ImGuiFrame* frame = static_cast<ImGuiFrame*>(handle);
    PrUVec2 extent = prGpuPassExtent(pass);
    frame->data.DisplayPos = ImVec2(-float(origin.x), -float(origin.y));
    frame->data.DisplaySize = ImVec2(float(extent.x), float(extent.y));
    frame->data.FramebufferScale = ImVec2(1, 1);
    imguiGpuRender(&frame->data, pass);
}

void dropResources(PrDrawRenderer* renderer) {
    ImGuiGpu* g = static_cast<ImGuiGpu*>(renderer);
    for (uint32_t i = 0; i < g->textures.count; ++i)
        prGpuTextureDestroy(g->device, g->textures.data[i].texture);
    g->textures.clear();
}

void destroyBackend(PrDrawRenderer* renderer) {
    ImGuiGpu* g = static_cast<ImGuiGpu*>(renderer);
    ImGui::SetCurrentContext(g->context);
    dropResources(renderer);
    g->textures.reset();
    imguiGpuShutdown();
    imguiContextDestroy(g);
    prMemoryFree(g);
}

} // namespace

// The texture is written by an upload, which the frame's commands, submitted after it, see. It is created once and kept
// for as long as renders use it.
ImTextureID imguiImageTexture(ImGuiBackend* imgui, const PrDrawImage* image, PrUVec2* extent) {
    ImGuiGpu* g = static_cast<ImGuiGpu*>(imgui);
    uint64_t key = prDrawImageId(image);
    for (uint32_t i = 0; i < g->textures.count; ++i) {
        if (g->textures.data[i].id == key) {
            g->textures.data[i].lastUsed = g->render;
            *extent = g->textures.data[i].extent;
            return imguiGpuTexture(g->textures.data[i].texture);
        }
    }
    PrUVec2 size = prDrawImageExtent(image);
    if (size.x == 0 || size.y == 0)
        return ImTextureID_Invalid;
    PrGpuTextureDesc desc = prGpuTextureDesc2d(PrGpuFormat_Rgba8, size, PrGpuTextureUsageFlag_Sampled);
    desc.name = "imgui image";
    PrGpuTexture texture{};
    if (prGpuTextureCreate(g->device, &desc, &texture) != PrStatus_Ok)
        return ImTextureID_Invalid;
    size_t row = size_t(size.x) * 4;
    uint8_t* pixels = static_cast<uint8_t*>(prMemoryAlloc(row * size.y));
    imguiImagePixels(image, PrPixels{pixels, row, size});
    prGpuTextureWrite(g->device, texture, PrIVec2{0, 0}, PrPixelView{pixels, row, size});
    prMemoryFree(pixels);
    g->textures.push(CachedTexture{key, texture, size, g->render});
    *extent = size;
    return imguiGpuTexture(texture);
}

ImTextureID imguiHostTexture(ImGuiBackend*, PrDrawTextureId texture) {
    return ImTextureID(texture);
}

ImGuiBackend* PR_NULLABLE imguiDeviceCreate(PrGpuDevice* device) {
    if (!imguiGpuInit(device))
        return nullptr;
    ImGuiGpu* g = allocZeroed<ImGuiGpu>();
    g->device = device;
    g->destroy = destroyBackend;
    g->frameCreate = createFrame;
    g->frameDestroy = destroyFrame;
    g->framePrepare = prepare;
    g->frameRender = render;
    g->dropResources = dropResources;
    return g;
}

} // namespace pr_impl
