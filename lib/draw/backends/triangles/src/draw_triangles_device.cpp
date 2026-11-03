// The triangles renderer's work on a GPU device.
//
// The textures that frames request are the renderer's own, written by uploads, and rest in the accesses of shader
// reads. Rendering copies the frame's vertices and indices into the stream of the pass's command buffer, then draws
// each draw with one pipeline for the target's format. The vertex shader reads its vertex from the stream.

#include "draw/backends/triangles/src/draw_triangles_device.h"

#include "pr/alloc.h"
#include "pr/hash_index.h"
#include "pr/mem.h"
#include "pr/raw_array.h"

#include "draw/backends/triangles/src/kDrawTrianglesShaders.h"

namespace pr_impl {

struct TrianglesOwnTexture {
    uint32_t id;                    // the texture's number in the frames' requests
    PrGpuTexture texture;
};

struct TrianglesPipeline {
    PrGpuFormat format;
    PrGpuPipeline pipeline;
};

// The push data of each draw (draw_triangles.slang).
struct TrianglesPush {
    float size[2];
    uint32_t vertices;
    uint32_t offset;
    uint32_t texture;
    uint32_t sampler;
};

struct TrianglesDevice {
    PrGpuDevice* device;
    PrGpuShaderLibrary* library;
    uint32_t samplers[TrianglesSampler_Count];  // by TrianglesSampler
    RawArray<TrianglesPipeline> pipelines;
    RawArray<TrianglesOwnTexture> textures;
    KeyIndex textureIds;            // `textures`, by id
};

namespace {

uint32_t textureIndex(const TrianglesDevice* d, uint32_t id) {
    uint32_t at = d->textureIds.find(id, [&](uint32_t i) { return uint64_t(d->textures.data[i].id); });
    return at != kHashMissing ? at : kArrayMissing;
}

// Returns the pipeline that draws into targets of `format`, created on first use, or zero if the device cannot create
// it. Color blends by source alpha and one minus source alpha, and alpha by one and one minus source alpha.
PrGpuPipeline pipelineFor(TrianglesDevice* d, PrGpuFormat format) {
    for (uint32_t i = 0; i < d->pipelines.count; ++i) {
        if (d->pipelines.data[i].format == format)
            return d->pipelines.data[i].pipeline;
    }
    PrGpuGraphicsPipelineDesc desc = prGpuGraphicsPipelineDescDefault(d->library);
    desc.vertex = "trianglesVertex";
    desc.fragment = "trianglesFragment";
    desc.formats.colorCount = 1;
    desc.formats.color[0] = format;
    desc.blend[0] = prGpuBlendPremultiplied();
    desc.blend[0].colorSource = PrGpuBlendFactor_SourceAlpha;
    desc.name = "triangles";
    PrGpuPipeline pipeline{};
    prGpuPipelineCreateGraphics(d->device, &desc, &pipeline);
    d->pipelines.push(TrianglesPipeline{format, pipeline});
    return pipeline;
}

void destroyTextures(TrianglesDevice* d) {
    for (uint32_t i = 0; i < d->textures.count; ++i)
        prGpuTextureDestroy(d->device, d->textures.data[i].texture);
    d->textures.clear();
    d->textureIds.clear();
}

int32_t within(int64_t at, uint32_t extent) {
    return int32_t(at < 0 ? 0 : at > int64_t(extent) ? int64_t(extent) : at);
}

} // namespace

PrStatus trianglesDeviceCreate(PrGpuDevice* device, TrianglesDevice* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    PrGpuShaderLibrary* library = nullptr;
    PrStatus status = prGpuShaderLibraryCreate(device, kDrawTrianglesShaders, sizeof(kDrawTrianglesShaders), &library);
    if (status != PrStatus_Ok)
        return status;
    TrianglesDevice* d = allocZeroed<TrianglesDevice>();
    d->device = device;
    d->library = library;
    for (uint32_t i = 0; i < TrianglesSampler_Count; ++i) {
        PrGpuAddress address = PrGpuAddress_Clamp;
        if (i >= TrianglesSampler_LinearMirror)
            address = PrGpuAddress_Mirror;
        else if (i >= TrianglesSampler_LinearRepeat)
            address = PrGpuAddress_Repeat;
        PrGpuSamplerDesc desc = prGpuSamplerDescDefault(i % 2 == 0 ? PrGpuFilter_Linear : PrGpuFilter_Nearest, address);
        desc.mip = PrGpuFilter_Nearest;
        PrGpuSampler sampler{};
        status = prGpuSamplerIntern(device, &desc, &sampler);
        if (status != PrStatus_Ok) {
            trianglesDeviceDestroy(d);
            return status;
        }
        d->samplers[i] = sampler.id;
    }
    *out = d;
    return PrStatus_Ok;
}

void trianglesDeviceDestroy(TrianglesDevice* PR_NULLABLE device) {
    if (device == nullptr)
        return;
    TrianglesDevice* d = device;
    destroyTextures(d);
    d->textures.reset();
    d->textureIds.reset();
    for (uint32_t i = 0; i < d->pipelines.count; ++i)
        prGpuPipelineDestroy(d->device, d->pipelines.data[i].pipeline);
    d->pipelines.reset();
    prGpuShaderLibraryDestroy(d->library);
    prMemoryFree(d);
}

void trianglesDevicePrepare(TrianglesDevice* d, const TrianglesFrame* frame, PrGpuCommands*) {
    // The entry of a destroyed texture is emptied, and the remaining entries are compacted once the requests are done.
    bool destroyed = false;
    for (uint32_t i = 0; i < frame->requestCount; ++i) {
        const TrianglesTextureRequest& q = frame->requests[i];
        uint32_t at = textureIndex(d, q.texture);
        switch (q.op) {
            case TrianglesTextureOp_Create: {
                if (q.texels.extent.x == 0 || q.texels.extent.y == 0)
                    break;
                PrGpuTextureDesc desc = prGpuTextureDesc2d(PrGpuFormat_Rgba8, q.texels.extent,
                    PrGpuTextureUsageFlag_Sampled);
                desc.name = "triangles texture";
                PrGpuTexture texture{};
                if (prGpuTextureCreate(d->device, &desc, &texture) != PrStatus_Ok)
                    break;
                prGpuTextureWrite(d->device, texture, PrIVec2{0, 0}, q.texels);
                d->textureIds.insert(q.texture, d->textures.count);
                d->textures.push(TrianglesOwnTexture{q.texture, texture});
                break;
            }
            case TrianglesTextureOp_Update:
                if (at != kArrayMissing)
                    prGpuTextureWrite(d->device, d->textures.data[at].texture,
                        PrIVec2{int32_t(q.origin.x), int32_t(q.origin.y)}, q.texels);
                break;
            case TrianglesTextureOp_Destroy:
                if (at != kArrayMissing) {
                    prGpuTextureDestroy(d->device, d->textures.data[at].texture);
                    d->textures.data[at] = TrianglesOwnTexture{};
                    destroyed = true;
                }
                break;
            default:
                break;
        }
    }
    if (destroyed) {
        uint32_t kept = 0;
        for (uint32_t i = 0; i < d->textures.count; ++i) {
            if (d->textures.data[i].texture.id != 0)
                d->textures.data[kept++] = d->textures.data[i];
        }
        d->textures.count = kept;
        d->textureIds.rebuild(kept, [&](uint32_t i) { return uint64_t(d->textures.data[i].id); });
    }
}

void trianglesDeviceRender(TrianglesDevice* d, const TrianglesFrame* frame, PrGpuPass* pass, PrIVec2 origin) {
    if (frame->drawCount == 0 || frame->extent.x == 0 || frame->extent.y == 0)
        return;
    PrGpuPipeline pipeline = pipelineFor(d, prGpuPassFormats(pass).color[0]);
    if (pipeline.id == 0)
        return;
    PrGpuCommands* commands = prGpuPassCommands(pass);
    PrUVec2 extent = prGpuPassExtent(pass);
    // The vertices and the indices, one after the other in the stream.
    uint64_t vertexBytes = sizeof(TrianglesVertex) * uint64_t(frame->vertexCount);
    uint64_t indexAt = (vertexBytes + 255) / 256 * 256;
    uint64_t indexBytes = sizeof(uint32_t) * uint64_t(frame->indexCount);
    PrGpuBuffer buffer{};
    uint64_t offset = 0;
    uint8_t* bytes = static_cast<uint8_t*>(prGpuCommandsStream(commands, indexAt + indexBytes, &buffer, &offset));
    prMemCopy(bytes, frame->vertices, vertexBytes);
    prMemCopy(bytes + indexAt, frame->indices, indexBytes);
    prGpuCommandsSetPipeline(commands, pipeline);
    prGpuPassSetIndexBuffer(pass, buffer, offset + indexAt, true);
    prGpuPassSetViewport(pass, PrRect{
        float(origin.x), float(origin.y), float(origin.x) + float(frame->extent.x),
        float(origin.y) + float(frame->extent.y),
    }, 0, 1);
    TrianglesPush push{{float(frame->extent.x), float(frame->extent.y)}, buffer.id, uint32_t(offset), 0, 0};
    for (uint32_t i = 0; i < frame->drawCount; ++i) {
        const TrianglesDraw& draw = frame->draws[i];
        PrGpuTexture texture = draw.hostTexture != 0 ? PrGpuTexture{uint32_t(draw.hostTexture)} : PrGpuTexture{};
        uint32_t at = texture.id == 0 ? textureIndex(d, draw.texture) : kArrayMissing;
        if (at != kArrayMissing)
            texture = d->textures.data[at].texture;
        // The draw's scissor at the frame's position, within the pass's attachments.
        PrIRect scissor{
            within(int64_t(origin.x) + draw.scissor.left, extent.x),
            within(int64_t(origin.y) + draw.scissor.top, extent.y),
            within(int64_t(origin.x) + draw.scissor.right, extent.x),
            within(int64_t(origin.y) + draw.scissor.bottom, extent.y),
        };
        if (texture.id == 0 || draw.indexCount == 0 || scissor.right <= scissor.left || scissor.bottom <= scissor.top)
            continue;
        prGpuPassSetScissor(pass, scissor);
        push.texture = texture.id;
        push.sampler = d->samplers[draw.sampler];
        prGpuCommandsSetPush(commands, &push, sizeof(push));
        prGpuPassDrawIndexed(pass, draw.indexCount, 1, draw.indexOffset, 0, 0);
    }
    // The viewport and scissor over the whole attachments, so that the host can continue to draw in the pass.
    prGpuPassSetViewport(pass, PrRect{0, 0, float(extent.x), float(extent.y)}, 0, 1);
    prGpuPassSetScissor(pass, PrIRect{0, 0, int32_t(extent.x), int32_t(extent.y)});
}

void trianglesDeviceDropResources(TrianglesDevice* d) {
    destroyTextures(d);
}

} // namespace pr_impl
