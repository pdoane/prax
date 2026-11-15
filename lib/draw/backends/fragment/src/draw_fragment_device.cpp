// The fragment renderer's work on a GPU device.
//
// A frame's data, every array of its records, goes into the stream of the command buffer that prepares it, as one
// allocation, and the shaders read each array from its offset there (draw_fragment.slang). Instances and records name
// textures by their index in the frame's table. On a device with bindless, the table is an array of texture handles, so
// that any instance can read any texture. On a device of slots alone, the shaders compiled for slots read the data
// through Buffer slot 0, and each kind of texture that an instance reads has a Texture slot of its own (FragmentSlot),
// with an Image paint's sampler beside its texture, so a run of instances is split into a draw for each run of them
// that reads one texture and one sampler in each slot. A pass draws its instances as instanced strips. It issues one
// draw for each run of instances that either all read the destination or all do not, with a pipeline for its target's
// format and that mode. Within a run that does not read the destination, long runs of basic instances draw with a
// separate pipeline. Its shader handles only basic instances, which make up most of a frame of text and rectangles, and
// it is much smaller than the shader for every instance. Where the device's reads of the target are not ordered within
// a draw, a run that reads the destination is drawn in batches of instances that cover disjoint pixels.
//
// The textures that the frames request are the renderer's own, and rest in the accesses of shader reads. Layers come
// from a pool. The queue executes the commands that read a returned layer before those of the frame that takes it next,
// and a pass into a layer begins with a barrier, so a layer is free as soon as its frame returns it.

#include "draw/backends/fragment/src/draw_fragment_device.h"

#include "pr/align.h"
#include "pr/alloc.h"
#include "pr/float.h"
#include "pr/hash_index.h"
#include "pr/integer.h"
#include "pr/mem.h"
#include "pr/raw_array.h"

#include "draw/backends/fragment/src/kDrawFragmentShaders.h"
#include "draw/backends/fragment/src/kDrawFragmentSlotShaders.h"

namespace pr_impl {

// How a pipeline draws: painting blended over the destination, painting that reads the destination from the target, a
// filter step that replaces the destination, basic instances blended over the destination (FragmentInstanceFlag_Basic),
// painting that reads the destination from a snapshot of the layer, or painting that replaces the destination
// (FragmentInstanceFlag_Writes).
enum {
    FragmentMode_Blend,
    FragmentMode_Read,
    FragmentMode_Filter,
    FragmentMode_Basic,
    FragmentMode_Snapshot,
    FragmentMode_Write,
    FragmentMode_Count,
};
typedef uint8_t FragmentMode;

// The entry points of each mode.
constexpr const char* kFragmentEntries[FragmentMode_Count] = {
    "fragmentShade",
    "fragmentBlend",
    "filterStep",
    "fragmentShadeBasic",
    "fragmentBlendSnapshot",
    "fragmentShade",
};

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

// The uniforms of a frame's draws: each array's offset in the buffer that holds the frame's data, the samplers, and
// that buffer. The shaders read the arrays as arrays of 4-vectors, as root data's arrays are (pr/gpu.slang). Through
// slots, the offsets are from where the data is bound, and the samplers and the buffer are unused: each draw binds the
// sampler of its Image paints beside their texture.
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
    PrGpuBuffer data;                  // the buffer of the frame's data and its offset there, bound to Buffer slot 0
    uint64_t dataOffset;
    bool prepared;
    RawArray<PrGpuTexture> textures;   // the frame's table, by index, zero for a texture that is gone
    RawArray<uint32_t> layers;         // for each of the frame's layers, its index in the pool, or kArrayMissing if not
                                       // from the pool
    RawArray<PrGpuTexture> hostLayers; // for each of the frame's layers, the host's texture if it is one, or zero
    KeyIndex layerIds;                 // the frame's layers, by their textures
};

struct FragmentDevice {
    PrGpuDevice* device;
    PrGpuShaderLibrary* library;
    bool targetReads;               // the device reads the target (PrGpuFeatureFlag_TargetReads)
    bool orderedReads;              // the device's reads of the target are ordered within a draw
    bool slots;                     // the device has no bindless, so the shaders read through slots
    PrGpuTexture blank;             // slots: a transparent texel, bound to the slots that a draw leaves empty
    uint32_t samplers[kFragmentSamplers];
    RawArray<FragmentPipeline> pipelines;
    RawArray<FragmentOwnTexture> textures;
    KeyIndex textureIds;            // `textures`, by id
    RawArray<FragmentFrameState> frames;
    RawArray<FragmentPooledLayer> pool;
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
    if (mode == FragmentMode_Blend || mode == FragmentMode_Basic)
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
        if (l.texture.id == 0 || l.taken || l.format != format || l.width < width || l.height < height ||
                l.exact != exact || l.mipmapped != layer.mipmapped || (exact && (l.width != width || l.height != height)))
            continue;
        if (best == kArrayMissing ||
                uint64_t(l.width) * l.height < uint64_t(d->pool.data[best].width) * d->pool.data[best].height)
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
        FragmentPooledLayer added{
            .texture = texture,
            .width = w,
            .height = h,
            .format = format,
            .exact = exact,
            .mipmapped = layer.mipmapped,
        };
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
    PrIRect c{
        a.left > b.left ? a.left : b.left, a.top > b.top ? a.top : b.top,
        a.right < b.right ? a.right : b.right, a.bottom < b.bottom ? a.bottom : b.bottom,
    };
    if (c.right < c.left)
        c.right = c.left;
    if (c.bottom < c.top)
        c.bottom = c.top;
    return c;
}

// The number of target pixels that a run of basic instances must cover to draw on its own with the basic pipeline. A
// smaller run draws with the instances around it in the blend pipeline. One draw costs the CPU about as much as the
// basic pipeline saves the GPU over tens of thousands of pixels. The threshold keeps a list that alternates between
// kinds of instances to a few draws.
constexpr float kBasicRunPixels = 65536;

// A run of basic instances: its end, and the target pixels that their quads cover.
struct BasicRun {
    uint32_t end;
    float pixels;
};

// Returns the run of basic instances that starts at `start` and stops at or before `end`. The pixel count stops once it
// reaches kBasicRunPixels, and triangles add nothing to it.
BasicRun basicRun(const FragmentFrame* frame, uint32_t start, uint32_t end) {
    BasicRun run{start, 0};
    for (; run.end < end; ++run.end) {
        const FragmentInstance& i = frame->instances[run.end];
        if ((i.flags & FragmentInstanceFlag_Basic) == 0)
            break;
        if (run.pixels < kBasicRunPixels && (i.flags & FragmentInstanceFlag_Coverage) != FragmentCoverage_Triangle) {
            const PrAffine2& m = i.transform;
            run.pixels += prF32Abs((i.bounds.right - i.bounds.left) * (i.bounds.bottom - i.bounds.top) *
                (m.a * m.d - m.b * m.c));
        }
    }
    return run;
}

// What a draw reads through its Texture slots, by FragmentSlot: each texture an index in the frame's table, and each
// sampler an index below kFragmentSamplers, or kAny where its instances read none. The snapshot's slot is the pass's,
// and is never set here.
struct SlotTextures {
    uint32_t at[FragmentSlot_Count];
    uint32_t samplers[FragmentSlot_Count];
};

constexpr uint32_t kAny = 0xffffffffu;

SlotTextures noSlotTextures() {
    SlotTextures s;
    for (uint32_t k = 0; k < FragmentSlot_Count; ++k) {
        s.at[k] = kAny;
        s.samplers[k] = kAny;
    }
    return s;
}

// Sets the Layer slot to the entry named by its index plus one, and the Under slot after it to the entry under it, as
// draw_fragment.slang's layerAt reads them.
void setLayer(const FragmentFrame* frame, FragmentSlot slot, uint32_t entry, SlotTextures& s) {
    if (entry == 0 || entry > frame->textureCount)
        return;
    s.at[slot] = entry - 1;
    uint32_t under = frame->textures[entry - 1].under;
    if (under != 0 && under <= frame->textureCount)
        s.at[slot + 1] = under - 1;
}

// The textures that the instance reads, as draw_fragment.slang's shade reads them.
SlotTextures instanceTextures(const FragmentFrame* frame, const FragmentInstance& i) {
    SlotTextures s = noSlotTextures();
    if ((i.flags & FragmentInstanceFlag_Coverage) == FragmentCoverage_Atlas && i.data != 0)
        s.at[FragmentSlot_Coverage] = i.data - 1;
    if (i.clip != 0)
        setLayer(frame, FragmentSlot_Layer1, frame->clips[i.clip - 1].mask, s);
    if (i.paint == 0)
        return s;
    const FragmentPaint& r = frame->paints[i.paint - 1];
    if (r.kind == FragmentPaintKind_Image && r.texture != 0) {
        s.at[FragmentSlot_Layer0] = r.texture - 1;
        // Nearest reads one texel, with no sampler.
        if (r.read == FragmentImageRead_Sampler && r.sampling != PrDrawSampling_Nearest)
            s.samplers[FragmentSlot_Layer0] = r.sampler;
    }
    if (r.kind == FragmentPaintKind_Layer)
        setLayer(frame, FragmentSlot_Layer0, r.texture, s);
    if (r.groupMask != FragmentMask_None)
        setLayer(frame, FragmentSlot_Layer2, r.groupMaskTexture, s);
    return s;
}

// Adds what `s` reads to `into`, unless a slot that both read would hold two textures or two samplers. Returns whether
// it did.
bool mergeSlotTextures(SlotTextures& into, const SlotTextures& s) {
    auto conflicts = [](uint32_t a, uint32_t b) { return a != kAny && b != kAny && a != b; };
    for (uint32_t k = 0; k < FragmentSlot_Count; ++k) {
        if (conflicts(s.at[k], into.at[k]) || conflicts(s.samplers[k], into.samplers[k]))
            return false;
    }
    for (uint32_t k = 0; k < FragmentSlot_Count; ++k) {
        if (s.at[k] != kAny)
            into.at[k] = s.at[k];
        if (s.samplers[k] != kAny)
            into.samplers[k] = s.samplers[k];
    }
    return true;
}

// What the Texture slots of a pass hold, by FragmentSlot: zero for a slot that holds no texture yet.
struct SlotBindings {
    PrGpuTexture textures[FragmentSlot_Count];
    PrGpuSampler samplers[FragmentSlot_Count];
};

// Binds what a draw reads to its slots. A slot that the draw does not read keeps what it holds, or holds the blank
// texture and the first sampler, since a draw reads every slot that its shaders use (pr/gpu.h). A texture that is gone
// reads as the blank one.
void bindSlotTextures(const FragmentDevice* d, const FragmentFrameState& state, PrGpuCommands* commands,
        const SlotTextures& s, SlotBindings& bindings) {
    for (uint32_t k = FragmentSlot_Coverage; k < FragmentSlot_Count; ++k) {
        PrGpuTexture texture = bindings.textures[k].id != 0 ? bindings.textures[k] : d->blank;
        if (s.at[k] != kAny) {
            texture = d->blank;
            if (s.at[k] < state.textures.count && state.textures.data[s.at[k]].id != 0)
                texture = state.textures.data[s.at[k]];
        }
        PrGpuSampler sampler = bindings.samplers[k].id != 0 ? bindings.samplers[k] : PrGpuSampler{d->samplers[0]};
        if (s.samplers[k] != kAny)
            sampler = PrGpuSampler{d->samplers[s.samplers[k]]};
        if (bindings.textures[k].id == texture.id && bindings.samplers[k].id == sampler.id)
            continue;
        prGpuCommandsSetTexture(commands, k, texture, sampler);
        bindings.textures[k] = texture;
        bindings.samplers[k] = sampler;
    }
}

// Draws `count` instances from `first` with the push data. Through slots, it binds the textures that they read, and
// issues a draw for each run of them that reads one texture and one sampler in each slot. A filter step's draw reads
// its inputs.
void drawInstances(const FragmentDevice* d, const FragmentFrame* frame, const FragmentFrameState& state,
        SlotBindings& bindings, PrGpuPass* pass, FragmentPush& push, uint32_t first, uint32_t count,
        const FragmentFilter* PR_NULLABLE filter) {
    PrGpuCommands* commands = prGpuPassCommands(pass);
    if (!d->slots) {
        push.first = first;
        prGpuCommandsSetPush(commands, &push, sizeof(push));
        prGpuPassDraw(pass, 4, count, 0, 0);
        return;
    }
    if (filter != nullptr) {
        SlotTextures s = noSlotTextures();
        for (uint32_t k = 0; k < 3; ++k)
            setLayer(frame, FragmentSlot_Layer0 + 2 * k, filter->inputs[k], s);
        bindSlotTextures(d, state, commands, s, bindings);
        push.first = first;
        prGpuCommandsSetPush(commands, &push, sizeof(push));
        prGpuPassDraw(pass, 4, count, 0, 0);
        return;
    }
    uint32_t end = first + count;
    while (first < end) {
        SlotTextures s = noSlotTextures();
        uint32_t stop = first;
        while (stop < end && mergeSlotTextures(s, instanceTextures(frame, frame->instances[stop])))
            ++stop;
        bindSlotTextures(d, state, commands, s, bindings);
        push.first = first;
        prGpuCommandsSetPush(commands, &push, sizeof(push));
        prGpuPassDraw(pass, 4, stop - first, 0, 0);
        first = stop;
    }
}

// Draws the pass's instances into `pass`, whose attachment is the target or the pass's layer. It issues one draw for
// each run of instances that either all read the destination or all do not, or runs the pass's filter step over them.
// In a run that does not read the destination, long runs of basic instances draw with the basic pipeline. The target's
// pixel (0, 0) is at `at` of the attachment, and `scissor` limits the draws.
void encodePass(FragmentDevice* d, const FragmentFrame* frame, const FragmentFrameState& state, const FragmentPass& fp,
        PrGpuPass* pass, PrIVec2 at, PrIRect scissor) {
    if (fp.count == 0)
        return;
    PrGpuCommands* commands = prGpuPassCommands(pass);
    if (fp.snapshot != 0)
        prGpuCommandsSetTexture(commands, FragmentSlot_Snapshot, layerTexture(d, state, fp.snapshot - 1),
            PrGpuSampler{});
    SlotBindings bindings{};
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
    FragmentPush push{
        {into[0], into[1], into[2], into[3]},
        {into[0] - view[0], into[1] - view[1]},
        fp.filter != 0 ? fp.filter - 1 : 0,
        fp.first,
    };
    if (fp.filter != 0) {
        PrGpuPipeline pipeline = pipelineFor(d, format, FragmentMode_Filter);
        if (pipeline.id == 0)
            return;
        prGpuCommandsSetPipeline(commands, pipeline);
        drawInstances(d, frame, state, bindings, pass, push, fp.first, fp.count, &frame->filters[fp.filter - 1]);
        return;
    }
    // The pixels that a draw may touch, which bound the batches of a run that reads the destination.
    PrIRect limit = intersection(PrIRect{
        int32_t(view[0]), int32_t(view[1]), int32_t(view[0] + into[2]),
        int32_t(view[1] + into[3]),
    }, scissor);
    const float offset[2] = {view[0] - into[0], view[1] - into[1]};
    constexpr uint32_t kReadBatchMax = 64;
    PrIRect rects[kReadBatchMax];
    uint32_t start = fp.first;
    uint32_t end = fp.first + fp.count;
    constexpr FragmentInstanceFlags kRun = FragmentInstanceFlag_ReadsDestination | FragmentInstanceFlag_Writes;
    while (start < end) {
        FragmentInstanceFlags kind = frame->instances[start].flags & kRun;
        bool reads = (kind & FragmentInstanceFlag_ReadsDestination) != 0;
        uint32_t stop = start + 1;
        while (stop < end && (frame->instances[stop].flags & kRun) == kind)
            ++stop;
        if ((kind & FragmentInstanceFlag_Writes) != 0) {
            PrGpuPipeline pipeline = pipelineFor(d, format, FragmentMode_Write);
            if (pipeline.id != 0) {
                prGpuCommandsSetPipeline(commands, pipeline);
                drawInstances(d, frame, state, bindings, pass, push, start, stop - start, nullptr);
            }
            start = stop;
            continue;
        }
        if (!reads) {
            // Alternates draws between the basic pipeline and the blend pipeline, which shades every kind of instance.
            while (start < stop) {
                BasicRun run = basicRun(frame, start, stop);
                uint32_t to = run.end;
                bool basic = run.pixels >= kBasicRunPixels || run.end == stop;
                if (!basic) {
                    // The blend pipeline draws up to the next long run of basic instances.
                    while (to < stop) {
                        BasicRun next = basicRun(frame, to, stop);
                        if (next.pixels >= kBasicRunPixels)
                            break;
                        to = next.end == to ? to + 1 : next.end;
                    }
                }
                PrGpuPipeline pipeline = pipelineFor(d, format, basic ? FragmentMode_Basic : FragmentMode_Blend);
                if (pipeline.id == 0 && basic)
                    pipeline = pipelineFor(d, format, FragmentMode_Blend);
                if (pipeline.id != 0) {
                    prGpuCommandsSetPipeline(commands, pipeline);
                    drawInstances(d, frame, state, bindings, pass, push, start, to - start, nullptr);
                }
                start = to;
            }
            continue;
        }
        bool snapshot = fp.snapshot != 0;
        PrGpuPipeline pipeline = pipelineFor(d, format, snapshot ? FragmentMode_Snapshot : FragmentMode_Read);
        if (pipeline.id == 0) {
            start = stop;
            continue;
        }
        prGpuCommandsSetPipeline(commands, pipeline);
        if (snapshot || d->orderedReads) {
            drawInstances(d, frame, state, bindings, pass, push, start, stop - start, nullptr);
            start = stop;
            continue;
        }
        // Batches of instances whose pixels are disjoint, each with the scissor over its pixels.
        while (start < stop) {
            uint32_t batch = start;
            uint32_t count = 0;
            PrIRect bounds{};
            while (batch < stop && count < kReadBatchMax) {
                PrIRect rect = fragmentInstancePixels(frame->instances[batch], PrVec2{offset[0], offset[1]}, limit);
                bool overlaps = false;
                for (uint32_t k = 0; k < count && !overlaps; ++k)
                    overlaps = rectsOverlap(rect, rects[k]);
                if (overlaps)
                    break;
                if (count == 0)
                    bounds = rect;
                else
                    bounds = PrIRect{
                        bounds.left < rect.left ? bounds.left : rect.left,
                        bounds.top < rect.top ? bounds.top : rect.top,
                        bounds.right > rect.right ? bounds.right : rect.right,
                        bounds.bottom > rect.bottom ? bounds.bottom : rect.bottom,
                    };
                rects[count++] = rect;
                ++batch;
            }
            prGpuPassSetScissor(pass, bounds);
            drawInstances(d, frame, state, bindings, pass, push, start, batch - start, nullptr);
            start = batch;
        }
        prGpuPassSetScissor(pass, scissor);
    }
}

// Copies the box of a copy pass from its layer into its snapshot layer, between the passes that draw into the layer.
// Both are read by fragments between passes, and at the same place, so a texel of one is the texel of the other.
void copySnapshot(FragmentDevice* d, const FragmentFrame* frame, const FragmentFrameState& state, const FragmentPass& fp,
        PrGpuTexture layer, PrGpuCommands* commands) {
    PrGpuTexture snapshot = layerTexture(d, state, fp.copy - 1);
    const FragmentLayer& l = frame->layers[fp.layer - 1];
    PrIRect box = intersection(fp.box, PrIRect{
        l.origin.x, l.origin.y, int32_t(int64_t(l.origin.x) + l.extent.x), int32_t(int64_t(l.origin.y) + l.extent.y),
    });
    if (snapshot.id == 0 || box.right <= box.left || box.bottom <= box.top)
        return;
    PrGpuSubresources all{};
    PrGpuTextureRegion region = prGpuTextureRegion2d(
        PrUVec2{uint32_t(box.left - l.origin.x), uint32_t(box.top - l.origin.y)},
        PrUVec2{uint32_t(box.right - box.left), uint32_t(box.bottom - box.top)});
    prGpuCommandsTextureBarrier(commands, layer, all, PrGpuAccessFlag_FragmentRead, PrGpuAccessFlag_CopyRead);
    prGpuCommandsTextureBarrier(commands, snapshot, all, PrGpuAccessFlag_FragmentRead, PrGpuAccessFlag_CopyWrite);
    prGpuCommandsCopyTexture(commands, layer, &region, snapshot, &region);
    prGpuCommandsTextureBarrier(commands, layer, all, PrGpuAccessFlag_CopyRead, PrGpuAccessFlag_FragmentRead);
    prGpuCommandsTextureBarrier(commands, snapshot, all, PrGpuAccessFlag_CopyWrite, PrGpuAccessFlag_FragmentRead);
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
                if (q.texels.extent.x == 0 || q.texels.extent.y == 0 ||
                        prGpuTextureCreate(d->device, &desc, &texture) != PrStatus_Ok)
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
// with where they are. Through slots, they are where the data is bound.
void writeData(const FragmentDevice* d, const FragmentFrame* frame, const PrDrawBeneath* PR_NULLABLE beneath,
        PrGpuTexture hostBeneath, uint32_t beneathId, PrGpuCommands* commands, FragmentFrameState* state) {
    auto view = [](const void* PR_NULLABLE bytes, size_t size) {
        return PrByteView{static_cast<const uint8_t*>(bytes), size};
    };
    // The table of texture handles is written below, once its place is known.
    const PrByteView parts[FragmentPart_Count] = {
        view(frame->instances, sizeof(FragmentInstance) * frame->instanceCount),
        view(frame->clips, sizeof(FragmentClip) * frame->clipCount),
        view(frame->paints, sizeof(FragmentPaint) * frame->paintCount),
        view(frame->clipShapes, sizeof(uint32_t) * frame->clipShapeCount),
        view(frame->shapes, sizeof(FragmentClipShape) * frame->shapeCount),
        view(frame->values, sizeof(float) * frame->valueCount),
        view(frame->rampColors, sizeof(PrColor16) * frame->rampColorCount),
        view(frame->coverage, frame->coverageCount),
        view(frame->tileData, sizeof(float) * frame->tileDataCount),
        view(frame->pieces, sizeof(FragmentPiece) * frame->pieceCount),
        view(frame->maps, sizeof(FragmentMap) * frame->mapCount),
        view(frame->mapData, sizeof(float) * frame->mapDataCount),
        view(frame->mapIndices, sizeof(uint32_t) * frame->mapIndexCount),
        view(nullptr, sizeof(uint32_t) * frame->textureCount),
        view(frame->textures, sizeof(FragmentTexture) * frame->textureCount),
        view(frame->filters, sizeof(FragmentFilter) * frame->filterCount),
        view(frame->filterData, sizeof(float) * frame->filterDataCount),
        view(frame->rampPieces, sizeof(PrDrawRampPiece) * frame->rampPieceCount),
        view(frame->rampTables, sizeof(PrColor16) * (kFragmentRampResolution + 1) * frame->rampCount),
        view(frame->rampCells, sizeof(uint32_t) * (kFragmentRampResolution + 2) * frame->rampCount),
    };
    PrGpuBuffer buffer{};
    uint64_t offsets[FragmentPart_Count];
    PrGpuBufferUsageFlags usage = d->slots ? PrGpuBufferUsageFlag_Storage : 0;
    uint8_t* bytes =
        static_cast<uint8_t*>(prGpuCommandsStreamGather(commands, parts, FragmentPart_Count, usage, &buffer, offsets));
    uint64_t base = d->slots ? offsets[0] : 0;
    for (uint32_t part = 0; part < FragmentPart_Count; ++part)
        state->uniforms.parts[part] = uint32_t(offsets[part] - base);
    state->uniforms.data = buffer.id;
    state->data = buffer;
    state->dataOffset = base;
    // The host's texture beneath the render is placed so that its texels line up with the target's pixels.
    FragmentTexture* entries = reinterpret_cast<FragmentTexture*>(bytes + (offsets[FragmentPart_Entries] - offsets[0]));
    for (uint32_t i = 0; i < frame->textureCount && hostBeneath.id != 0 && beneathId != 0; ++i) {
        if (entries[i].texture == beneathId && entries[i].hostTexture == 0) {
            entries[i].origin = -beneath->origin;
            entries[i].extent = beneath->extent;
        }
    }
    uint32_t* table = reinterpret_cast<uint32_t*>(bytes + (offsets[FragmentPart_Table] - offsets[0]));
    for (uint32_t i = 0; i < state->textures.count; ++i)
        table[i] = state->textures.data[i].id;
}

// Sets the frame's uniforms for the draws recorded after the call, and through slots binds its data.
void bindFrame(const FragmentDevice* d, const FragmentFrameState& state, PrGpuCommands* commands) {
    prGpuCommandsSetUniforms(commands, &state.uniforms, sizeof(state.uniforms));
    if (d->slots)
        prGpuCommandsSetBuffer(commands, 0, state.data, state.dataOffset);
}

} // namespace

PrStatus fragmentDeviceCreate(PrGpuDevice* device, FragmentDevice* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    // The shaders reach the frame's resources by handle, or through slots: the data's buffer, the snapshot's texture
    // and a draw's textures.
    const PrGpuDeviceInfo& info = prGpuDeviceInfo(device);
    bool slots = (info.features & PrGpuFeatureFlag_Bindless) == 0;
    if (slots && (info.maxBufferSlots < 1 || info.maxTextureSlots < FragmentSlot_Count))
        return PrStatus_NotSupported;
    const unsigned char* shaders = slots ? kDrawFragmentSlotShaders : kDrawFragmentShaders;
    uint64_t size = slots ? sizeof(kDrawFragmentSlotShaders) : sizeof(kDrawFragmentShaders);
    PrGpuShaderLibrary* library = nullptr;
    PrStatus status = prGpuShaderLibraryCreate(device, shaders, size, &library);
    if (status != PrStatus_Ok)
        return status;
    FragmentDevice* d = allocZeroed<FragmentDevice>();
    d->device = device;
    d->library = library;
    d->slots = slots;
    d->targetReads = (info.features & PrGpuFeatureFlag_TargetReads) != 0;
    d->orderedReads = (info.features & PrGpuFeatureFlag_OrderedTargetReads) != 0;
    if (slots) {
        // The texture of the slots that a draw does not read.
        PrGpuTextureDesc desc = prGpuTextureDesc2d(PrGpuFormat_Rgba8, PrUVec2{1, 1}, PrGpuTextureUsageFlag_Sampled);
        desc.name = "fragment blank";
        status = prGpuTextureCreate(device, &desc, &d->blank);
        if (status != PrStatus_Ok) {
            fragmentDeviceDestroy(d);
            return status;
        }
        const uint8_t transparent[4] = {};
        prGpuTextureWrite(device, d->blank, PrIVec2{0, 0}, PrPixelView{transparent, 4, {1, 1}});
    }
    // By FragmentAddress.
    const PrGpuAddress modes[4] = {PrGpuAddress_Clamp, PrGpuAddress_Repeat, PrGpuAddress_Mirror, PrGpuAddress_Border};
    for (uint32_t i = 0; i < kFragmentSamplers; ++i) {
        bool anisotropic = i >= kFragmentAnisotropicSamplers;
        uint32_t pair = i % kFragmentAnisotropicSamplers;
        PrGpuSamplerDesc desc = prGpuSamplerDescDefault(PrGpuFilter_Linear, modes[pair / 4]);
        desc.address[1] = modes[pair % 4];
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
    prGpuTextureDestroy(d->device, d->blank);
    prGpuShaderLibraryDestroy(d->library);
    prMemoryFree(d);
}

bool fragmentDeviceReadsTarget(const FragmentDevice* device) {
    return device->targetReads;
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
    PrGpuTexture hostBeneath;
    if (beneath != nullptr && beneath->kind == PrDrawBeneathKind_Texture)
        hostBeneath = PrGpuTexture{uint32_t(beneath->texture)};
    else
        hostBeneath = PrGpuTexture{};
    uint32_t beneathId = frame->beneath != 0 ? frame->layers[frame->beneath - 1].texture : 0;
    state->hostLayers.clear();
    for (uint32_t i = 0; i < frame->layerCount; ++i) {
        const FragmentLayer& l = frame->layers[i];
        PrGpuTexture hostLayer =
            l.hostTexture != 0 ? PrGpuTexture{uint32_t(frame->hostTextures[l.hostTexture - 1])} : PrGpuTexture{};
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
        PrGpuTexture texture =
            hostIndex != 0 ? PrGpuTexture{uint32_t(frame->hostTextures[hostIndex - 1])} : PrGpuTexture{};
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
    writeData(d, frame, beneath, hostBeneath, beneathId, commands, state);
    state->prepared = true;
    bindFrame(d, *state, commands);
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
        if (fp.copy != 0) {
            copySnapshot(d, frame, *state, fp, layer, commands);
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
            // The host draws first, in the same pass, and may leave any state, so the pass binds its frame again.
            const FragmentLayer& l = frame->layers[fp.layer - 1];
            beneath->draw(beneath->context, pass, prIRectAt(l.origin, l.extent));
            bindFrame(d, *state, commands);
        }
        encodePass(d, frame, *state, fp, pass, PrIVec2{0, 0}, whole);
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
    PrIRect scissor = intersection(PrIRect{
        origin.x, origin.y, int32_t(int64_t(origin.x) + frame->extent.x),
        int32_t(int64_t(origin.y) + frame->extent.y),
    }, whole);
    if (scissor.right <= scissor.left || scissor.bottom <= scissor.top)
        return;
    bindFrame(d, *state, prGpuPassCommands(pass));
    for (uint32_t i = 0; i < frame->passCount; ++i) {
        if (frame->passes[i].layer == 0)
            encodePass(d, frame, *state, frame->passes[i], pass, origin, scissor);
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
