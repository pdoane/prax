// The fragment renderer's work on a GPU device.
//
// A frame's data, every array of its records, goes into the stream of the command buffer that prepares it, as one
// allocation, and the shaders read each array from its offset there (draw_fragment.slang). The frame's texture table is
// an array of texture handles, which instances and records index, so that any instance can read any texture. A pass
// draws its instances as instanced strips. It issues one draw for each run of instances that either all read the
// destination or all do not, with a pipeline for its target's format and that mode. Where the device's reads of the
// target are not ordered within a draw, a run that reads the destination is drawn in batches of instances that cover
// disjoint pixels.
//
// The textures that the frames request are the renderer's own, and rest in the accesses of shader reads. Layers come
// from a pool. The queue executes the commands that read a returned layer before those of the frame that takes it next,
// and a pass into a layer begins with a barrier, so a layer is free as soon as its frame returns it.

#include "draw/backends/fragment/src/draw_fragment_device.h"

#include "pr/alloc.h"
#include "pr/array.h"
#include "pr/float.h"
#include "pr/hash_index.h"
#include "pr/integer.h"
#include "pr/mem.h"

#include "draw/backends/fragment/src/kDrawFragmentShaders.h"

namespace pr_impl {

// How a pipeline draws: painting blended over the destination, painting that reads the destination, or a filter step
// that replaces the destination.
enum {
    FragmentMode_Blend,
    FragmentMode_Read,
    FragmentMode_Filter,
    FragmentMode_Count,
};
typedef uint8_t FragmentMode;

// The entry points of each mode.
constexpr const char* kFragmentEntries[FragmentMode_Count] = {"fragmentShade", "fragmentBlend", "filterStep"};

struct FragmentPipeline {
    PrGpuFormat format;
    FragmentMode mode;
    PrGpuPipeline pipeline;
};

// A texture that a frame's requests created.
struct FragmentOwnTexture {
    uint32_t id;                    // the texture's number in the frames' requests
    PrGpuTexture texture;
};

// The granularity of a pooled layer's size, so that frames of similar sizes reuse layers, and the number of prepares
// for which a pooled layer stays unused before it is destroyed.
constexpr uint32_t kLayerGranule = 64;
constexpr uint64_t kLayerIdlePrepares = 8;

// A layer texture that the renderer keeps between frames, or a free slot in the pool if it has no texture.
struct FragmentPooledLayer {
    PrGpuTexture texture;
    uint32_t width;
    uint32_t height;
    PrGpuFormat format;
    bool exact;                     // made for a layer whose texture is exactly its extent
    bool mipmapped;
    uint64_t lastUsed;              // the prepare that last used the texture
    bool taken;                     // currently a layer of a frame
};

// The frame's arrays, in the order of the shaders' FrameParts.
enum {
    FragmentPart_Instances,
    FragmentPart_Clips,
    FragmentPart_Paints,
    FragmentPart_ClipShapes,
    FragmentPart_Shapes,
    FragmentPart_Values,
    FragmentPart_RampColors,
    FragmentPart_Coverage,
    FragmentPart_Tiles,
    FragmentPart_Pieces,
    FragmentPart_Maps,
    FragmentPart_MapData,
    FragmentPart_MapIndices,
    FragmentPart_Table,
    FragmentPart_Entries,
    FragmentPart_Filters,
    FragmentPart_FilterData,
    FragmentPart_RampPieces,
    FragmentPart_RampTables,
    FragmentPart_RampCells,
    FragmentPart_Count,
};
typedef uint32_t FragmentPart;

// The samplers that the shaders sample images with: one for each pair of address modes along x and y, at x * 4 + y,
// filtering bilinearly from the first level, then the same pairs filtering trilinearly and anisotropically.
constexpr uint32_t kFragmentSamplers = 32;

// The uniforms of a frame's draws: each array's offset in the buffer that holds the frame's data, the samplers, and
// that buffer. The shaders read the arrays as arrays of 4-vectors, as root data's arrays are (pr/gpu.slang).
struct FragmentUniforms {
    uint32_t parts[FragmentPart_Count];   // by FragmentPart, in bytes
    uint32_t samplers[kFragmentSamplers];
    uint32_t data;
    uint32_t _pad[3];
};
static_assert(FragmentPart_Count % 4 == 0 && kFragmentSamplers % 4 == 0);

// The push data of each draw: the pass's target, its top left in the target's pixels and its size; the offset from a
// pixel of the pass's attachment to the target's pixel; the filter step; and the draw's first instance.
struct FragmentPush {
    float into[4];
    float origin[2];
    uint32_t filter;
    uint32_t first;
};

// The resources that a prepared frame draws with until it is prepared again or released.
struct FragmentFrameState {
    const FragmentFrame* frame;
    FragmentUniforms uniforms;
    bool prepared;
    Array<PrGpuTexture> textures;   // the frame's table, by index, zero for a texture that is gone
    Array<uint32_t> layers;         // for each of the frame's layers, its index in the pool, or kArrayMissing if not
                                    // from the pool
    Array<PrGpuTexture> hostLayers; // for each of the frame's layers, the host's texture if it is one, or zero
    KeyIndex layerIds;              // the frame's layers, by their textures
};

struct FragmentDevice {
    PrGpuDevice* device;
    PrGpuShaderLibrary* library;
    bool orderedReads;              // the device's reads of the target are ordered within a draw
    uint32_t samplers[kFragmentSamplers];
    Array<FragmentPipeline> pipelines;
    Array<FragmentOwnTexture> textures;
    KeyIndex textureIds;            // `textures`, by id
    Array<FragmentFrameState> frames;
    Array<FragmentPooledLayer> pool;
    uint64_t prepares;
};

namespace {

PrGpuFormat gpuFormatOf(PrPixelFormat format) {
    PrGpuFormat f = prGpuFormatFromPixelFormat(format);
    return f != PrGpuFormat_None ? f : PrGpuFormat_Rgba8;
}

uint32_t textureIndex(const FragmentDevice* d, uint32_t id) {
    uint32_t at = d->textureIds.find(id, [&](uint32_t i) { return uint64_t(d->textures.data[i].id); });
    return at != kHashMissing ? at : kArrayMissing;
}

// Returns the pipeline that draws into targets of `format` in `mode`, created on first use, or zero if the device
// cannot create it.
PrGpuPipeline pipelineFor(FragmentDevice* d, PrGpuFormat format, FragmentMode mode) {
    for (uint32_t i = 0; i < d->pipelines.count; ++i) {
        const FragmentPipeline& p = d->pipelines.data[i];
        if (p.format == format && p.mode == mode)
            return p.pipeline;
    }
    PrGpuGraphicsPipelineDesc desc = prGpuGraphicsPipelineDescDefault(d->library);
    desc.vertex = "instanceVertex";
    desc.fragment = kFragmentEntries[mode];
    desc.topology = PrGpuTopology_TriangleStrip;
    desc.formats.colorCount = 1;
    desc.formats.color[0] = format;
    if (mode == FragmentMode_Blend)
        desc.blend[0] = prGpuBlendPremultiplied();
    desc.readsTarget = mode == FragmentMode_Read;
    desc.name = "fragment";
    PrGpuPipeline pipeline{};
    prGpuPipelineCreateGraphics(d->device, &desc, &pipeline);
    d->pipelines.push(FragmentPipeline{format, mode, pipeline});
    return pipeline;
}

// Returns the texture that the frame's layer draws into: the host's, or the pool's, or zero for none.
PrGpuTexture layerTexture(const FragmentDevice* d, const FragmentFrameState& state, uint32_t layer) {
    if (state.hostLayers.data[layer].id != 0)
        return state.hostLayers.data[layer];
    uint32_t pooled = state.layers.data[layer];
    return pooled == kArrayMissing ? PrGpuTexture{} : d->pool.data[pooled].texture;
}

// Returns the frame's layers to the pool.
void returnLayers(FragmentDevice* d, FragmentFrameState& f) {
    for (uint32_t i = 0; i < f.layers.count; ++i) {
        if (f.layers.data[i] != kArrayMissing)
            d->pool.data[f.layers.data[i]].taken = false;
    }
    f.layers.clear();
}

// Takes a texture for the layer, from the pool or newly created, and returns its index in the pool. The texture is of
// `format`, and is exactly the layer's extent if the layer needs that, or at least its extent otherwise. Returns
// kArrayMissing if the device cannot create one.
uint32_t takeLayer(FragmentDevice* d, const FragmentLayer& layer, PrGpuFormat format) {
    const uint32_t width = layer.extent.x;
    const uint32_t height = layer.extent.y;
    const bool exact = layer.exact || layer.mipmapped;
    uint32_t best = kArrayMissing;
    for (uint32_t i = 0; i < d->pool.count; ++i) {
        const FragmentPooledLayer& l = d->pool.data[i];
        if (l.texture.id == 0 || l.taken || l.format != format || l.width < width || l.height < height
            || l.exact != exact || l.mipmapped != layer.mipmapped
            || (exact && (l.width != width || l.height != height)))
            continue;
        if (best == kArrayMissing
            || uint64_t(l.width) * l.height < uint64_t(d->pool.data[best].width) * d->pool.data[best].height)
            best = i;
    }
    if (best == kArrayMissing) {
        uint32_t w = exact ? width : alignUp(width, kLayerGranule);
        uint32_t h = exact ? height : alignUp(height, kLayerGranule);
        if (w == 0 || h == 0)
            return kArrayMissing;
        PrGpuTextureDesc desc = prGpuTextureDesc2d(
            format, PrUVec2{w, h}, PrGpuTextureUsageFlag_Sampled | PrGpuTextureUsageFlag_RenderTarget);
        desc.name = "fragment layer";
        if (layer.mipmapped)
            desc.levels = prGpuMipLevels(PrUVec2{w, h});
        PrGpuTexture texture{};
        if (prGpuTextureCreate(d->device, &desc, &texture) != PrStatus_Ok)
            return kArrayMissing;
        best = 0;
        while (best < d->pool.count && d->pool.data[best].texture.id != 0)
            ++best;
        FragmentPooledLayer added{.texture = texture,
                                  .width = w,
                                  .height = h,
                                  .format = format,
                                  .exact = exact,
                                  .mipmapped = layer.mipmapped};
        if (best == d->pool.count)
            d->pool.push(added);
        else
            d->pool.data[best] = added;
    }
    FragmentPooledLayer& l = d->pool.data[best];
    l.taken = true;
    l.lastUsed = d->prepares;
    return best;
}

// Destroys the pooled layers that have gone unused too long, leaving their slots free.
void trimPool(FragmentDevice* d) {
    for (uint32_t i = 0; i < d->pool.count; ++i) {
        FragmentPooledLayer& l = d->pool.data[i];
        if (l.texture.id != 0 && !l.taken && d->prepares - l.lastUsed > kLayerIdlePrepares) {
            prGpuTextureDestroy(d->device, l.texture);
            l = FragmentPooledLayer{};
        }
    }
}

FragmentFrameState* PR_NULLABLE stateOf(FragmentDevice* d, const FragmentFrame* frame) {
    for (uint32_t i = 0; i < d->frames.count; ++i) {
        if (d->frames.data[i].frame == frame)
            return &d->frames.data[i];
    }
    return nullptr;
}

// Returns the pool's format for a layer of the frame. If the host draws into the layer, it is the target's format, so
// that the host draws as it draws into the target. Otherwise the layer's precision decides it.
PrGpuFormat layerFormat(const FragmentFrame* frame, uint32_t layer, PrGpuFormat target) {
    for (uint32_t i = 0; i < frame->passCount; ++i) {
        if (frame->passes[i].beneath && frame->passes[i].layer == layer + 1)
            return target;
    }
    return frame->layers[layer].precise ? PrGpuFormat_Rgba16F : PrGpuFormat_Rgba8;
}

bool rectsOverlap(const PrIRect& a, const PrIRect& b) {
    return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
}

PrIRect intersection(const PrIRect& a, const PrIRect& b) {
    PrIRect c{a.left > b.left ? a.left : b.left, a.top > b.top ? a.top : b.top,
              a.right < b.right ? a.right : b.right, a.bottom < b.bottom ? a.bottom : b.bottom};
    if (c.right < c.left)
        c.right = c.left;
    if (c.bottom < c.top)
        c.bottom = c.top;
    return c;
}

// The pixels of the pass's attachment that the instance can draw, within `limit`: the bounds of its corners, moved by
// `offset` from the target's pixels to the attachment's, and widened by a pixel. An instance whose corners are not
// finite may draw anywhere within `limit`.
PrIRect instancePixels(const FragmentInstance& i, const float offset[2], const PrIRect& limit) {
    PrVec2 corners[4];
    uint32_t count = 0;
    if ((i.flags & FragmentInstanceFlag_Coverage) == FragmentCoverage_Triangle) {
        for (uint32_t k = 0; k < 3; ++k)
            corners[count++] = i.corners[k];
    } else {
        const PrAffine2& m = i.transform;
        const float xs[2] = {i.bounds.left, i.bounds.right};
        const float ys[2] = {i.bounds.top, i.bounds.bottom};
        for (float y : ys) {
            for (float x : xs)
                corners[count++] = PrVec2{m.a * x + m.c * y + m.e, m.b * x + m.d * y + m.f};
        }
    }
    float lo[2] = {corners[0].x, corners[0].y};
    float hi[2] = {corners[0].x, corners[0].y};
    for (uint32_t k = 0; k < count; ++k) {
        const float p[2] = {corners[k].x, corners[k].y};
        for (uint32_t axis = 0; axis < 2; ++axis) {
            // A comparison with NaN is false, so a corner that is not finite fails both tests and the limit stands.
            if (!(p[axis] > -1e9f && p[axis] < 1e9f))
                return limit;
            lo[axis] = p[axis] < lo[axis] ? p[axis] : lo[axis];
            hi[axis] = p[axis] > hi[axis] ? p[axis] : hi[axis];
        }
    }
    PrIRect box{int32_t(floorF32(lo[0] + offset[0])) - 1, int32_t(floorF32(lo[1] + offset[1])) - 1,
                int32_t(ceilF32(hi[0] + offset[0])) + 1, int32_t(ceilF32(hi[1] + offset[1])) + 1};
    return intersection(box, limit);
}

// Draws the pass's instances into `pass`, whose attachment is the target or the pass's layer. It issues one draw for
// each run of instances that either all read the destination or all do not, or runs the pass's filter step over them.
// The target's pixel (0, 0) is at `at` of the attachment, and `scissor` limits the draws.
void encodePass(FragmentDevice* d, const FragmentFrame* frame, const FragmentPass& fp, PrGpuPass* pass,
                PrIVec2 at, PrIRect scissor) {
    if (fp.count == 0)
        return;
    PrGpuCommands* commands = prGpuPassCommands(pass);
    PrGpuFormat format = prGpuPassFormats(pass).color[0];
    // The target's pixels that are drawn into, and their position in the pass's attachment.
    float into[4] = {0, 0, float(frame->extent.x), float(frame->extent.y)};
    float view[2] = {float(at.x), float(at.y)};
    if (fp.layer != 0) {
        const FragmentLayer& l = frame->layers[fp.layer - 1];
        float place[4] = {float(l.origin.x), float(l.origin.y), float(l.extent.x), float(l.extent.y)};
        for (uint32_t k = 0; k < 4; ++k)
            into[k] = place[k];
        view[0] = 0;
        view[1] = 0;
    }
    prGpuPassSetViewport(pass, PrRect{view[0], view[1], view[0] + into[2], view[1] + into[3]}, 0, 1);
    prGpuPassSetScissor(pass, scissor);
    FragmentPush push{{into[0], into[1], into[2], into[3]},
                      {into[0] - view[0], into[1] - view[1]},
                      fp.filter != 0 ? fp.filter - 1 : 0,
                      fp.first};
    if (fp.filter != 0) {
        PrGpuPipeline pipeline = pipelineFor(d, format, FragmentMode_Filter);
        if (pipeline.id == 0)
            return;
        prGpuCommandsSetPipeline(commands, pipeline);
        prGpuCommandsSetPush(commands, &push, sizeof(push));
        prGpuPassDraw(pass, 4, fp.count, 0, 0);
        return;
    }
    // The pixels that a draw may touch, which bound the batches of a run that reads the destination.
    PrIRect limit = intersection(PrIRect{int32_t(view[0]), int32_t(view[1]), int32_t(view[0] + into[2]),
                                         int32_t(view[1] + into[3])},
                                 scissor);
    const float offset[2] = {view[0] - into[0], view[1] - into[1]};
    constexpr uint32_t kReadBatchMax = 64;
    PrIRect rects[kReadBatchMax];
    uint32_t start = fp.first;
    uint32_t end = fp.first + fp.count;
    while (start < end) {
        bool reads = (frame->instances[start].flags & FragmentInstanceFlag_ReadsDestination) != 0;
        uint32_t stop = start + 1;
        while (stop < end && ((frame->instances[stop].flags & FragmentInstanceFlag_ReadsDestination) != 0) == reads)
            ++stop;
        PrGpuPipeline pipeline = pipelineFor(d, format, reads ? FragmentMode_Read : FragmentMode_Blend);
        if (pipeline.id == 0 && reads)
            pipeline = pipelineFor(d, format, FragmentMode_Blend);
        if (pipeline.id == 0) {
            start = stop;
            continue;
        }
        prGpuCommandsSetPipeline(commands, pipeline);
        if (!reads || d->orderedReads) {
            push.first = start;
            prGpuCommandsSetPush(commands, &push, sizeof(push));
            prGpuPassDraw(pass, 4, stop - start, 0, 0);
            start = stop;
            continue;
        }
        // Batches of instances whose pixels are disjoint, each with the scissor over its pixels.
        while (start < stop) {
            uint32_t batch = start;
            uint32_t count = 0;
            PrIRect bounds{};
            while (batch < stop && count < kReadBatchMax) {
                PrIRect rect = instancePixels(frame->instances[batch], offset, limit);
                bool overlaps = false;
                for (uint32_t k = 0; k < count && !overlaps; ++k)
                    overlaps = rectsOverlap(rect, rects[k]);
                if (overlaps)
                    break;
                bounds = count == 0 ? rect
                                    : PrIRect{bounds.left < rect.left ? bounds.left : rect.left,
                                              bounds.top < rect.top ? bounds.top : rect.top,
                                              bounds.right > rect.right ? bounds.right : rect.right,
                                              bounds.bottom > rect.bottom ? bounds.bottom : rect.bottom};
                rects[count++] = rect;
                ++batch;
            }
            prGpuPassSetScissor(pass, bounds);
            push.first = start;
            prGpuCommandsSetPush(commands, &push, sizeof(push));
            prGpuPassDraw(pass, 4, batch - start, 0, 0);
            start = batch;
        }
        prGpuPassSetScissor(pass, scissor);
    }
}

// Carries out the frame's texture requests: textures created and written by uploads, which the frame's commands read,
// and the levels of mipmapped ones filled in `commands`.
void runRequests(FragmentDevice* d, const FragmentFrame* frame, PrGpuCommands* commands) {
    bool destroyed = false;
    for (uint32_t i = 0; i < frame->requestCount; ++i) {
        const FragmentTextureRequest& q = frame->requests[i];
        uint32_t at = textureIndex(d, q.texture);
        switch (q.op) {
            case FragmentTextureOp_Create: {
                PrGpuTextureDesc desc = prGpuTextureDesc2d(gpuFormatOf(q.format), q.texels.extent,
                                                           PrGpuTextureUsageFlag_Sampled);
                desc.name = "fragment texture";
                if (q.mipmapped) {
                    desc.usage |= PrGpuTextureUsageFlag_RenderTarget;
                    desc.levels = prGpuMipLevels(q.texels.extent);
                }
                PrGpuTexture texture{};
                if (q.texels.extent.x == 0 || q.texels.extent.y == 0
                    || prGpuTextureCreate(d->device, &desc, &texture) != PrStatus_Ok)
                    break;
                prGpuTextureWrite(d->device, texture, PrIVec2{0, 0}, q.texels);
                d->textureIds.insert(q.texture, d->textures.count);
                d->textures.push(FragmentOwnTexture{q.texture, texture});
                if (q.mipmapped)
                    prGpuCommandsGenerateMipmaps(commands, texture, PrGpuAccessFlag_ShaderRead);
                break;
            }
            case FragmentTextureOp_Update:
                if (at != kArrayMissing)
                    prGpuTextureWrite(d->device, d->textures.data[at].texture,
                                      PrIVec2{int32_t(q.origin.x), int32_t(q.origin.y)}, q.texels);
                break;
            case FragmentTextureOp_Destroy:
                if (at != kArrayMissing) {
                    prGpuTextureDestroy(d->device, d->textures.data[at].texture);
                    d->textures.data[at] = FragmentOwnTexture{};
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

// Copies the frame's arrays into the stream of `commands`, with the table of texture handles, and fills the uniforms
// with where they are.
void writeData(const FragmentFrame* frame, const PrDrawBeneath* PR_NULLABLE beneath, PrGpuTexture hostBeneath,
               uint32_t beneathId, PrGpuCommands* commands, FragmentFrameState* state) {
    struct Source {
        const void* PR_NULLABLE bytes;
        size_t size;
    };
    const Source sources[FragmentPart_Count] = {
        {frame->instances, sizeof(FragmentInstance) * frame->instanceCount},
        {frame->clips, sizeof(FragmentClip) * frame->clipCount},
        {frame->paints, sizeof(FragmentPaint) * frame->paintCount},
        {frame->clipShapes, sizeof(uint32_t) * frame->clipShapeCount},
        {frame->shapes, sizeof(FragmentClipShape) * frame->shapeCount},
        {frame->values, sizeof(float) * frame->valueCount},
        {frame->rampColors, sizeof(PrColor16) * frame->rampColorCount},
        {frame->coverage, frame->coverageCount},
        {frame->tileData, sizeof(float) * frame->tileDataCount},
        {frame->pieces, sizeof(FragmentPiece) * frame->pieceCount},
        {frame->maps, sizeof(FragmentMap) * frame->mapCount},
        {frame->mapData, sizeof(float) * frame->mapDataCount},
        {frame->mapIndices, sizeof(uint32_t) * frame->mapIndexCount},
        {nullptr, sizeof(uint32_t) * frame->textureCount},
        {frame->textures, sizeof(FragmentTexture) * frame->textureCount},
        {frame->filters, sizeof(FragmentFilter) * frame->filterCount},
        {frame->filterData, sizeof(float) * frame->filterDataCount},
        {frame->rampPieces, sizeof(PrDrawRampPiece) * frame->rampPieceCount},
        {frame->rampTables, sizeof(PrColor16) * (kFragmentRampResolution + 1) * frame->rampCount},
        {frame->rampCells, sizeof(uint32_t) * (kFragmentRampResolution + 2) * frame->rampCount},
    };
    uint32_t at[FragmentPart_Count];
    uint64_t total = 0;
    for (uint32_t part = 0; part < FragmentPart_Count; ++part) {
        at[part] = uint32_t(total);
        total += (sources[part].size + 15) / 16 * 16;
    }
    PrGpuBuffer buffer{};
    uint64_t offset = 0;
    uint8_t* bytes = static_cast<uint8_t*>(prGpuCommandsStream(commands, total != 0 ? total : 16, &buffer, &offset));
    for (uint32_t part = 0; part < FragmentPart_Count; ++part) {
        if (sources[part].bytes != nullptr && sources[part].size != 0)
            memCopy(bytes + at[part], sources[part].bytes, sources[part].size);
        state->uniforms.parts[part] = uint32_t(offset) + at[part];
    }
    state->uniforms.data = buffer.id;
    // The host's texture beneath the render is placed so that its texels line up with the target's pixels.
    FragmentTexture* entries = reinterpret_cast<FragmentTexture*>(bytes + at[FragmentPart_Entries]);
    for (uint32_t i = 0; i < frame->textureCount && hostBeneath.id != 0 && beneathId != 0; ++i) {
        if (entries[i].texture == beneathId && entries[i].hostTexture == 0) {
            entries[i].origin = -beneath->origin;
            entries[i].extent = beneath->extent;
        }
    }
    uint32_t* table = reinterpret_cast<uint32_t*>(bytes + at[FragmentPart_Table]);
    for (uint32_t i = 0; i < state->textures.count; ++i)
        table[i] = state->textures.data[i].id;
}

} // namespace

PrStatus fragmentDeviceCreate(PrGpuDevice* device, FragmentDevice* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    PrGpuShaderLibrary* library = nullptr;
    PrStatus status = prGpuShaderLibraryCreate(device, kDrawFragmentShaders, sizeof(kDrawFragmentShaders), &library);
    if (status != PrStatus_Ok)
        return status;
    FragmentDevice* d = allocZeroed<FragmentDevice>();
    d->device = device;
    d->library = library;
    d->orderedReads = (prGpuDeviceInfo(device).features & PrGpuFeatureFlag_OrderedTargetReads) != 0;
    const PrGpuAddress modes[4] = {PrGpuAddress_Clamp, PrGpuAddress_Repeat, PrGpuAddress_Mirror, PrGpuAddress_Border};
    for (uint32_t i = 0; i < kFragmentSamplers; ++i) {
        bool anisotropic = i >= 16;
        PrGpuSamplerDesc desc = prGpuSamplerDescDefault(PrGpuFilter_Linear, modes[(i % 16) / 4]);
        desc.address[1] = modes[i % 4];
        desc.maxAnisotropy = anisotropic ? 16 : 1;
        PrGpuSampler sampler{};
        status = prGpuSamplerIntern(device, &desc, &sampler);
        if (status != PrStatus_Ok) {
            fragmentDeviceDestroy(d);
            return status;
        }
        d->samplers[i] = sampler.id;
    }
    *out = d;
    return PrStatus_Ok;
}

void fragmentDeviceDestroy(FragmentDevice* PR_NULLABLE device) {
    if (device == nullptr)
        return;
    FragmentDevice* d = device;
    fragmentDeviceDropResources(d);
    for (uint32_t i = 0; i < d->pipelines.count; ++i)
        prGpuPipelineDestroy(d->device, d->pipelines.data[i].pipeline);
    d->pipelines.reset();
    prGpuShaderLibraryDestroy(d->library);
    prMemoryFree(d);
}

void fragmentDevicePrepare(FragmentDevice* d, const FragmentFrame* frame, PrGpuCommands* commands,
                           PrPixelFormat format, const PrDrawBeneath* PR_NULLABLE beneath) {
    PrGpuFormat target = gpuFormatOf(format);
    prGpuCommandsPushGroup(commands, "fragment prepare");
    runRequests(d, frame, commands);
    // The frame's layers, which replace those that it last drew with.
    ++d->prepares;
    FragmentFrameState* state = stateOf(d, frame);
    if (state == nullptr) {
        state = &d->frames.emplace();
        *state = FragmentFrameState{};
        state->frame = frame;
    } else {
        returnLayers(d, *state);
    }
    trimPool(d);
    // The beneath layer is the host's texture.
    PrGpuTexture hostBeneath = beneath != nullptr && beneath->kind == PrDrawBeneathKind_Texture
                                   ? PrGpuTexture{uint32_t(beneath->texture)}
                                   : PrGpuTexture{};
    uint32_t beneathId = frame->beneath != 0 ? frame->layers[frame->beneath - 1].texture : 0;
    state->hostLayers.clear();
    for (uint32_t i = 0; i < frame->layerCount; ++i) {
        const FragmentLayer& l = frame->layers[i];
        PrGpuTexture hostLayer = l.hostTexture != 0 ? PrGpuTexture{uint32_t(frame->hostTextures[l.hostTexture - 1])}
                                                    : PrGpuTexture{};
        bool host = (i + 1 == frame->beneath && hostBeneath.id != 0) || hostLayer.id != 0;
        state->layers.push(host ? kArrayMissing : takeLayer(d, l, layerFormat(frame, i, target)));
        state->hostLayers.push(hostLayer);
    }
    // The table's textures: a host texture, a texture that the requests created, or a layer's texture.
    state->textures.clear();
    state->layerIds.rebuild(frame->layerCount, [&](uint32_t k) { return uint64_t(frame->layers[k].texture); });
    for (uint32_t i = 0; i < frame->textureCount; ++i) {
        uint32_t id = frame->textures[i].texture;
        uint32_t hostIndex = frame->textures[i].hostTexture;
        PrGpuTexture texture = hostIndex != 0 ? PrGpuTexture{uint32_t(frame->hostTextures[hostIndex - 1])}
                                              : PrGpuTexture{};
        if (texture.id == 0 && id == beneathId && beneathId != 0)
            texture = hostBeneath;
        if (texture.id == 0) {
            uint32_t k = state->layerIds.find(id, [&](uint32_t at) { return uint64_t(frame->layers[at].texture); });
            if (k != kHashMissing)
                texture = layerTexture(d, *state, k);
        }
        uint32_t at = texture.id == 0 ? textureIndex(d, id) : kArrayMissing;
        if (at != kArrayMissing)
            texture = d->textures.data[at].texture;
        state->textures.push(texture);
    }
    for (uint32_t i = 0; i < kFragmentSamplers; ++i)
        state->uniforms.samplers[i] = d->samplers[i];
    writeData(frame, beneath, hostBeneath, beneathId, commands, state);
    state->prepared = true;
    prGpuCommandsSetUniforms(commands, &state->uniforms, sizeof(state->uniforms));
    // The passes into layers. Each begins and ends with a barrier, since the passes after read what it draws.
    for (uint32_t i = 0; i < frame->passCount; ++i) {
        const FragmentPass& fp = frame->passes[i];
        if (fp.layer == 0)
            continue;
        PrGpuTexture layer = layerTexture(d, *state, fp.layer - 1);
        if (layer.id == 0)
            continue;
        PrGpuSubresources all{};
        if (fp.mipmaps) {
            prGpuCommandsGenerateMipmaps(commands, layer, PrGpuAccessFlag_FragmentRead);
            continue;
        }
        prGpuCommandsTextureBarrier(commands, layer, all, PrGpuAccessFlag_FragmentRead,
                                    PrGpuAccessFlag_ColorAttachment);
        PrGpuPassDesc desc = prGpuPassDescColor(layer, fp.clear ? PrGpuLoad_Clear : PrGpuLoad_Keep, PrColor{});
        desc.name = "fragment layer";
        PrGpuPass* pass = prGpuPassBegin(commands, &desc);
        PrUVec2 extent = prGpuPassExtent(pass);
        PrIRect whole{0, 0, int32_t(extent.x), int32_t(extent.y)};
        if (fp.beneath && beneath != nullptr && beneath->kind == PrDrawBeneathKind_Draw) {
            // The host draws first, in the same pass, and may leave any state, so the pass sets its uniforms again.
            const FragmentLayer& l = frame->layers[fp.layer - 1];
            beneath->draw(beneath->context, pass, prIRectAt(l.origin, l.extent));
            prGpuCommandsSetUniforms(commands, &state->uniforms, sizeof(state->uniforms));
        }
        encodePass(d, frame, fp, pass, PrIVec2{0, 0}, whole);
        prGpuPassEnd(pass);
        prGpuCommandsTextureBarrier(commands, layer, all, PrGpuAccessFlag_ColorAttachment,
                                    PrGpuAccessFlag_FragmentRead);
    }
    prGpuCommandsPopGroup(commands);
}

void fragmentDeviceRender(FragmentDevice* d, const FragmentFrame* frame, PrGpuPass* pass, PrIVec2 origin) {
    FragmentFrameState* state = stateOf(d, frame);
    if (state == nullptr || !state->prepared || frame->extent.x == 0 || frame->extent.y == 0)
        return;
    PrUVec2 extent = prGpuPassExtent(pass);
    // The scissor keeps the render within its rectangle, and within the attachments.
    PrIRect whole{0, 0, int32_t(extent.x), int32_t(extent.y)};
    PrIRect scissor = intersection(PrIRect{origin.x, origin.y, int32_t(int64_t(origin.x) + frame->extent.x),
                                           int32_t(int64_t(origin.y) + frame->extent.y)},
                                   whole);
    if (scissor.right <= scissor.left || scissor.bottom <= scissor.top)
        return;
    prGpuCommandsSetUniforms(prGpuPassCommands(pass), &state->uniforms, sizeof(state->uniforms));
    for (uint32_t i = 0; i < frame->passCount; ++i) {
        if (frame->passes[i].layer == 0)
            encodePass(d, frame, frame->passes[i], pass, origin, scissor);
    }
    // The viewport and scissor over the whole attachments, so that the host can continue to draw in the pass.
    prGpuPassSetViewport(pass, PrRect{0, 0, float(extent.x), float(extent.y)}, 0, 1);
    prGpuPassSetScissor(pass, whole);
}

void fragmentDeviceReleaseFrame(FragmentDevice* d, const FragmentFrame* frame) {
    FragmentFrameState* state = stateOf(d, frame);
    if (state == nullptr)
        return;
    returnLayers(d, *state);
    state->layers.reset();
    state->hostLayers.reset();
    state->layerIds.reset();
    state->textures.reset();
    *state = d->frames.last();
    d->frames.pop();
}

void fragmentDeviceDropResources(FragmentDevice* d) {
    for (uint32_t i = 0; i < d->frames.count; ++i) {
        FragmentFrameState& f = d->frames.data[i];
        f.layers.reset();
        f.hostLayers.reset();
        f.layerIds.reset();
        f.textures.reset();
    }
    d->frames.reset();
    for (uint32_t i = 0; i < d->pool.count; ++i)
        prGpuTextureDestroy(d->device, d->pool.data[i].texture);
    d->pool.reset();
    for (uint32_t i = 0; i < d->textures.count; ++i)
        prGpuTextureDestroy(d->device, d->textures.data[i].texture);
    d->textures.reset();
    d->textureIds.reset();
}

} // namespace pr_impl
