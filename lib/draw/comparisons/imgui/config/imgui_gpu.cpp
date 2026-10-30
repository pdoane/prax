// Dear ImGui's draw data drawn on a lib/gpu device.
//
// The renderer's state is the backend data of the context. Drawing copies the vertices and indices of the draw data
// into the stream of the pass's command buffer, and draws each command with one pipeline for the target's format.

#include "draw/comparisons/imgui/config/imgui_gpu.h"

#include "pr/alloc.h"
#include "pr/array.h"
#include "pr/mem.h"

#include "draw/comparisons/imgui/config/kImGuiGpuShaders.h"

namespace pr {

namespace {

struct ImGuiGpuPipeline {
    PrGpuFormat format;
    PrGpuPipeline pipeline;
};

struct ImGuiGpu {
    PrGpuDevice* device;
    PrGpuShaderLibrary* library;
    PrGpuSampler linear;
    PrGpuSampler nearest;
    uint32_t sampler;               // the sampler of the draws, which the draw data's callbacks choose
    Array<ImGuiGpuPipeline> pipelines;
};

// The push data of each draw (imgui_gpu.slang).
struct ImGuiGpuPush {
    float scale[2];
    float translate[2];
    uint32_t vertices;
    uint32_t offset;
    uint32_t texture;
    uint32_t sampler;
};

ImGuiGpu* backendOf() {
    return static_cast<ImGuiGpu*>(ImGui::GetIO().BackendRendererUserData);
}

// Returns the pipeline that draws into targets of `format`, created on first use, or zero if the device cannot create
// it. Color blends by source alpha and one minus source alpha, and alpha by one and one minus source alpha.
PrGpuPipeline pipelineFor(ImGuiGpu* g, PrGpuFormat format) {
    for (uint32_t i = 0; i < g->pipelines.count; ++i) {
        if (g->pipelines.data[i].format == format)
            return g->pipelines.data[i].pipeline;
    }
    PrGpuGraphicsPipelineDesc desc = prGpuGraphicsPipelineDescDefault(g->library);
    desc.vertex = "imguiVertex";
    desc.fragment = "imguiFragment";
    desc.formats.colorCount = 1;
    desc.formats.color[0] = format;
    desc.blend[0] = prGpuBlendPremultiplied();
    desc.blend[0].colorSource = PrGpuBlendFactor_SourceAlpha;
    desc.name = "imgui";
    PrGpuPipeline pipeline{};
    prGpuPipelineCreateGraphics(g->device, &desc, &pipeline);
    g->pipelines.push(ImGuiGpuPipeline{format, pipeline});
    return pipeline;
}

void destroyTexture(ImGuiGpu* g, ImTextureData* tex) {
    if (tex->TexID != ImTextureID_Invalid)
        prGpuTextureDestroy(g->device, PrGpuTexture{uint32_t(tex->TexID)});
    tex->SetTexID(ImTextureID_Invalid);
    tex->SetStatus(ImTextureStatus_Destroyed);
}

// Carries out what Dear ImGui asks of a texture: creating it, updating regions of it, or destroying it. Its texels are
// written by uploads, which the commands submitted after them see.
void updateTexture(ImGuiGpu* g, ImTextureData* tex) {
    if (tex->Status == ImTextureStatus_WantCreate) {
        PrUVec2 extent{uint32_t(tex->Width), uint32_t(tex->Height)};
        PrGpuTextureDesc desc = prGpuTextureDesc2d(PrGpuFormat_Rgba8, extent, PrGpuTextureUsageFlag_Sampled);
        desc.name = "imgui texture";
        PrGpuTexture texture{};
        if (prGpuTextureCreate(g->device, &desc, &texture) != PrStatus_Ok)
            return;
        prGpuTextureWrite(g->device, texture, PrIVec2{0, 0}, PrPixelView{
            static_cast<const uint8_t*>(tex->GetPixels()), size_t(tex->Width) * 4,
            {uint32_t(tex->Width), uint32_t(tex->Height)},
        });
        tex->SetTexID(ImTextureID(texture.id));
        tex->SetStatus(ImTextureStatus_OK);
    } else if (tex->Status == ImTextureStatus_WantUpdates) {
        PrGpuTexture texture{uint32_t(tex->TexID)};
        for (const ImTextureRect& r : tex->Updates) {
            prGpuTextureWrite(g->device, texture, PrIVec2{r.x, r.y}, PrPixelView{
                static_cast<const uint8_t*>(tex->GetPixelsAt(r.x, r.y)),
                size_t(tex->Width) * 4, {r.w, r.h},
            });
        }
        tex->SetStatus(ImTextureStatus_OK);
    } else if (tex->Status == ImTextureStatus_WantDestroy && tex->UnusedFrames > 0) {
        destroyTexture(g, tex);
    }
}

// The callbacks through which draw data chooses how the draws after them sample.
void setSamplerLinear(const ImDrawList*, const ImDrawCmd*) {
    backendOf()->sampler = backendOf()->linear.id;
}

void setSamplerNearest(const ImDrawList*, const ImDrawCmd*) {
    backendOf()->sampler = backendOf()->nearest.id;
}

} // namespace

bool imguiGpuInit(PrGpuDevice* device) {
    ImGuiGpu* g = allocZeroed<ImGuiGpu>();
    g->device = device;
    PrGpuSamplerDesc linear = prGpuSamplerDescDefault(PrGpuFilter_Linear, PrGpuAddress_Clamp);
    PrGpuSamplerDesc nearest = prGpuSamplerDescDefault(PrGpuFilter_Nearest, PrGpuAddress_Clamp);
    if (prGpuShaderLibraryCreate(device, kImGuiGpuShaders, sizeof(kImGuiGpuShaders), &g->library) != PrStatus_Ok ||
            prGpuSamplerIntern(device, &linear, &g->linear) != PrStatus_Ok ||
            prGpuSamplerIntern(device, &nearest, &g->nearest) != PrStatus_Ok) {
        prGpuShaderLibraryDestroy(g->library);
        prMemoryFree(g);
        return false;
    }
    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererUserData = g;
    io.BackendRendererName = "lib/gpu";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures;
    ImGuiPlatformIO& platform = ImGui::GetPlatformIO();
    platform.DrawCallback_SetSamplerLinear = setSamplerLinear;
    platform.DrawCallback_SetSamplerNearest = setSamplerNearest;
    return true;
}

void imguiGpuShutdown() {
    ImGuiGpu* g = backendOf();
    if (g == nullptr)
        return;
    for (ImTextureData* tex : ImGui::GetPlatformIO().Textures) {
        if (tex->RefCount == 1)
            destroyTexture(g, tex);
    }
    for (uint32_t i = 0; i < g->pipelines.count; ++i)
        prGpuPipelineDestroy(g->device, g->pipelines.data[i].pipeline);
    g->pipelines.reset();
    prGpuShaderLibraryDestroy(g->library);
    prMemoryFree(g);
    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererUserData = nullptr;
    io.BackendRendererName = nullptr;
    io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures);
    ImGuiPlatformIO& platform = ImGui::GetPlatformIO();
    platform.DrawCallback_SetSamplerLinear = nullptr;
    platform.DrawCallback_SetSamplerNearest = nullptr;
}

ImTextureID imguiGpuTexture(PrGpuTexture texture) {
    return ImTextureID(texture.id);
}

void imguiGpuRender(ImDrawData* data, PrGpuPass* pass) {
    ImGuiGpu* g = backendOf();
    PrUVec2 extent = prGpuPassExtent(pass);
    int width = int(data->DisplaySize.x * data->FramebufferScale.x + 0.5f);
    int height = int(data->DisplaySize.y * data->FramebufferScale.y + 0.5f);
    if (width <= 0 || height <= 0 || data->CmdLists.Size == 0)
        return;
    if (data->Textures != nullptr) {
        for (ImTextureData* tex : *data->Textures) {
            if (tex->Status != ImTextureStatus_OK)
                updateTexture(g, tex);
        }
    }
    PrGpuPipeline pipeline = pipelineFor(g, prGpuPassFormats(pass).color[0]);
    if (pipeline.id == 0)
        return;
    // The vertices, then the indices, in the stream.
    PrGpuCommands* commands = prGpuPassCommands(pass);
    uint64_t vertexBytes = uint64_t(data->TotalVtxCount) * sizeof(ImDrawVert);
    uint64_t indexAt = (vertexBytes + 255) / 256 * 256;
    uint64_t indexBytes = uint64_t(data->TotalIdxCount) * sizeof(ImDrawIdx);
    PrGpuBuffer buffer{};
    uint64_t offset = 0;
    uint8_t* bytes = static_cast<uint8_t*>(prGpuCommandsStream(commands, indexAt + indexBytes + 16, &buffer, &offset));
    uint64_t vertexAt = 0;
    uint64_t indexOffset = indexAt;
    for (const ImDrawList* list : data->CmdLists) {
        memCopy(bytes + vertexAt, list->VtxBuffer.Data, size_t(list->VtxBuffer.Size) * sizeof(ImDrawVert));
        memCopy(bytes + indexOffset, list->IdxBuffer.Data, size_t(list->IdxBuffer.Size) * sizeof(ImDrawIdx));
        vertexAt += uint64_t(list->VtxBuffer.Size) * sizeof(ImDrawVert);
        indexOffset += uint64_t(list->IdxBuffer.Size) * sizeof(ImDrawIdx);
    }
    prGpuCommandsSetPipeline(commands, pipeline);
    prGpuPassSetIndexBuffer(pass, buffer, offset + indexAt, sizeof(ImDrawIdx) == 4);
    prGpuPassSetViewport(pass, PrRect{0, 0, float(width), float(height)}, 0, 1);
    // Clip space from the display's coordinates, with y up.
    ImGuiGpuPush push{
        {2 / data->DisplaySize.x, -2 / data->DisplaySize.y},
        {-1 - data->DisplayPos.x * 2 / data->DisplaySize.x, 1 + data->DisplayPos.y * 2 / data->DisplaySize.y},
        buffer.id,
        0,
        0,
        0,
    };
    g->sampler = g->linear.id;
    ImVec2 clipOffset = data->DisplayPos;
    ImVec2 clipScale = data->FramebufferScale;
    uint64_t listVertices = 0;
    uint64_t listIndices = 0;
    for (const ImDrawList* list : data->CmdLists) {
        for (const ImDrawCmd& cmd : list->CmdBuffer) {
            if (cmd.UserCallback != nullptr) {
                if (cmd.UserCallback != ImDrawCallback_ResetRenderState)
                    cmd.UserCallback(list, &cmd);
                continue;
            }
            float left = (cmd.ClipRect.x - clipOffset.x) * clipScale.x;
            float top = (cmd.ClipRect.y - clipOffset.y) * clipScale.y;
            float right = (cmd.ClipRect.z - clipOffset.x) * clipScale.x;
            float bottom = (cmd.ClipRect.w - clipOffset.y) * clipScale.y;
            left = left < 0 ? 0 : left;
            top = top < 0 ? 0 : top;
            right = right > float(width) ? float(width) : right;
            bottom = bottom > float(height) ? float(height) : bottom;
            ImTextureID texture = cmd.GetTexID();
            if (right <= left || bottom <= top || cmd.ElemCount == 0 || texture == ImTextureID_Invalid)
                continue;
            prGpuPassSetScissor(pass, PrIRect{int32_t(left), int32_t(top), int32_t(right), int32_t(bottom)});
            push.offset = uint32_t(offset + (listVertices + cmd.VtxOffset) * sizeof(ImDrawVert));
            push.texture = uint32_t(texture);
            push.sampler = g->sampler;
            prGpuCommandsSetPush(commands, &push, sizeof(push));
            prGpuPassDrawIndexed(pass, cmd.ElemCount, 1, uint32_t(listIndices + cmd.IdxOffset), 0, 0);
        }
        listVertices += uint64_t(list->VtxBuffer.Size);
        listIndices += uint64_t(list->IdxBuffer.Size);
    }
    prGpuPassSetViewport(pass, PrRect{0, 0, float(extent.x), float(extent.y)}, 0, 1);
    prGpuPassSetScissor(pass, PrIRect{0, 0, int32_t(extent.x), int32_t(extent.y)});
}

} // namespace pr
