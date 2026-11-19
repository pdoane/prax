// Render graphs: a frame's passes and resources, the dependencies between the passes, their order, and the walk over
// that order that lends textures and records barriers.

#include "pr/gpu_graph.h"

#include "pr/alloc.h"
#include "pr/arena.h"
#include "pr/assert.h"
#include "pr/mem.h"
#include "pr/raw_array.h"

namespace pr_impl {

namespace {

// Indices into a graph's passes, resources, uses, edges and lent textures, and into a walk's written ranges.
typedef uint32_t PassIndex;
typedef uint32_t ResourceIndex;
typedef uint32_t UseIndex;
typedef uint32_t EdgeIndex;
typedef uint32_t LentIndex;
typedef uint32_t WrittenIndex;

// No element, for each of the indices.
constexpr uint32_t kNone = UINT32_MAX;

// The number of frames that a lent texture may stay unused before the graph destroys it.
constexpr uint32_t kIdleFrames = 8;

// The alignment of the memory that prGpuGraphAlloc gives out.
constexpr size_t kAllocAlignment = 16;

// A range of a buffer's bytes.
struct Range {
    uint64_t offset;
    uint64_t size;
};

// The range of a use that covers a whole resource.
constexpr Range kWholeRange{0, UINT64_MAX};

// A texture or a buffer of the frame.
struct Resource {
    bool isTexture;
    bool isFrame;               // a texture of the frame, which the graph lends a device texture
    bool isLeft;                // a result of the frame, which the graph leaves in `left`

    // The caller's texture or buffer. For a texture of the frame, `texture` is the device texture that it holds, while
    // the graph records its uses.
    PrGpuTexture texture;
    PrGpuBuffer buffer;

    PrGpuAccessFlags accesses;  // the accesses that an imported resource is in when the graph is recorded
    PrGpuAccessFlags left;

    // A texture of the frame: its description, and the bytes of its first level.
    PrGpuTextureDesc desc;
    uint64_t bytes;

    // The uses of the resource, in the order that their passes were added, linked through Use::nextOfResource.
    UseIndex firstUse;
    UseIndex lastUse;

    uint32_t users;             // the recorded passes that use the resource
};

// A pass's use of a resource.
struct Use {
    PassIndex pass;
    ResourceIndex resource;
    PrGpuAccessFlags accesses;
    Range range;                // of a buffer. A texture's use covers the whole texture.
    UseIndex nextOfPass;        // the pass's next use, in the order that they were declared, or none
    UseIndex nextOfResource;
};

// A pass that draws, or work outside any pass.
struct Pass {
    PrStringView name;
    bool draws;                 // true if the pass has attachments and `draw`, false if it has `record`
    bool isRecorded;            // a result of the frame depends on the pass
    PrGpuGraphPassDesc desc;
    PrGpuGraphDrawFn PR_NULLABLE draw;
    PrGpuGraphRecordFn PR_NULLABLE record;
    void* PR_NULLABLE context;

    // The pass's uses, linked through Use::nextOfPass.
    UseIndex firstUse;
    UseIndex lastUse;

    EdgeIndex firstEdge;        // the edges from the pass, linked through Edge::nextFrom
};

// A dependency: `to` is recorded after `from`.
struct Edge {
    PassIndex from;
    PassIndex to;
    bool keeps;                 // `from` is recorded if `to` is, because `to` depends on what `from` did
    EdgeIndex nextFrom;
};

// A device texture that the graph lends to the frame's textures.
struct Lent {
    PrGpuTextureDesc desc;      // without a name
    PrGpuTexture texture;
    bool isHeld;                // a texture of the frame holds it now
    bool isUsed;                // a texture of this frame has held it
    PrGpuAccessFlags accesses;  // the accesses that the last texture to hold it left it in
    uint32_t idleFrames;
};

// A range that a pass of a walk has written, in the list of its resource's written ranges.
struct Written {
    Range range;
    WrittenIndex next;
};

// A graph: the frame that is being built, and what outlives a frame.
struct GpuGraph {
    PrGpuDevice* device;
    PrGpuGraphPriority priority;
    uint16_t generation;        // of the frame, which the ids of its textures, buffers and passes carry
    bool isRecorded;            // the frame's graph has been recorded, so nothing more is added to it

    RawArray<Resource> resources;
    RawArray<Pass> passes;
    RawArray<Use> uses;
    RawArray<Edge> edges;
    RawArray<PrGpuGraphError> errors;
    RawArray<Written> written;  // the written ranges of the walk under way

    RawArray<Lent> lent;

    // The frame's memory. The graph keeps its pages from frame to frame.
    Arena frame;
};

// What a walk has done to a resource so far.
struct ResourceState {
    PrGpuAccessFlags accesses;
    WrittenIndex written;       // the ranges that passes have written since the last barrier
    uint32_t remaining;         // the recorded passes that are still to use the resource
    LentIndex lent;             // the device texture that a texture of the frame holds, or none
    bool isBegun;               // a pass has used the texture of the frame
    bool isWritten;             // a pass has written the resource
    bool isFresh;               // the device texture holds nothing that the frame needs yet
    bool isAbsent;              // the resource has no device texture or buffer, so no pass that uses it is recorded
};

// A walk over the recorded passes in their order: the accesses that each pass finds its resources in, the barriers that
// it needs, and the device textures that the frame's textures hold.
struct Walk {
    GpuGraph* graph;
    PrGpuCommands* commands;
    const PassIndex* order;     // the recorded passes. The walk reads the passes after the one that it is at, too.
    uint32_t count;
    ResourceState* states;      // by resource
    PrGpuGraphStats stats;      // what the walk recorded
};

// Returns the bytes of the first level of a texture of `desc`.
uint64_t textureBytes(const PrGpuTextureDesc& desc) {
    uint64_t bytes = prGpuFormatLevelBytes(desc.format, PrUVec2{desc.extent.width, desc.extent.height});
    return bytes * desc.extent.depth * desc.samples;
}

// Returns true if two descriptions make textures that may stand in for each other. Their names do not matter.
bool sameTexture(const PrGpuTextureDesc& a, const PrGpuTextureDesc& b) {
    return a.kind == b.kind && a.format == b.format && a.usage == b.usage && a.levels == b.levels &&
        a.samples == b.samples && a.extent.width == b.extent.width && a.extent.height == b.extent.height &&
        a.extent.depth == b.extent.depth;
}

// Returns true if two ranges share a byte. A range's end saturates, so that the whole range overlaps every other.
bool overlaps(Range a, Range b) {
    uint64_t aEnd = a.size > UINT64_MAX - a.offset ? UINT64_MAX : a.offset + a.size;
    uint64_t bEnd = b.size > UINT64_MAX - b.offset ? UINT64_MAX : b.offset + b.size;
    return a.offset < bEnd && b.offset < aEnd;
}

bool writes(PrGpuAccessFlags accesses) {
    return (accesses & PrGpuAccessFlag_Write) != 0;
}

// Returns true if the accesses hold both a write and a read.
bool readsAndWrites(PrGpuAccessFlags accesses) {
    PrGpuAccessFlags notReads = PrGpuAccessFlag_Write | PrGpuAccessFlag_Discard;
    return writes(accesses) && (accesses & ~notReads) != 0;
}

// Returns a copy of `name` in the frame's memory.
PrStringView frameName(GpuGraph* g, PrStringView name) {
    if (name.size == 0)
        return PrStringView{};
    return PrStringView{g->frame.copy(name.data, uint32_t(name.size)), name.size};
}

// Returns the id of the frame's element at `index`. An id holds the frame's generation above the index, and is never
// zero.
uint32_t idOf(const GpuGraph* g, uint32_t index) {
    return uint32_t(g->generation) << 16 | (index + 1);
}

void report(GpuGraph* g, PrGpuGraphErrorKind kind, PassIndex pass) {
    PrGpuGraphError error{.kind = kind};
    if (pass != kNone) {
        error.pass = PrGpuGraphPass{idOf(g, pass)};
        error.name = g->passes.data[pass].name;
    }
    g->errors.push(error);
}

// Returns the index that `id` names among `count` elements of the frame, or none if it names none of them.
uint32_t indexOf(const GpuGraph* g, uint32_t id, uint32_t count) {
    uint32_t number = id & 0xffff;
    if (id >> 16 != g->generation || number == 0 || number > count)
        return kNone;
    return number - 1;
}

// Returns the pass that `id` names, or none, which it reports unless the id is zero.
PassIndex passOf(GpuGraph* g, PrGpuGraphPass pass) {
    PassIndex index = indexOf(g, pass.id, g->passes.count);
    if (index == kNone && pass.id != 0)
        report(g, PrGpuGraphErrorKind_StaleId, kNone);
    return index;
}

// Returns the texture or the buffer that `id` names, or none, which it reports for `pass` unless the id is zero.
ResourceIndex resourceOf(GpuGraph* g, uint32_t id, bool isTexture, PassIndex pass) {
    ResourceIndex index = indexOf(g, id, g->resources.count);
    if (index != kNone && g->resources.data[index].isTexture != isTexture)
        index = kNone;
    if (index == kNone && id != 0)
        report(g, PrGpuGraphErrorKind_StaleId, pass);
    return index;
}

ResourceIndex addResource(GpuGraph* g, const Resource& resource) {
    PR_ASSERT(!g->isRecorded && g->resources.count < kPrGpuGraphMaxResources);
    g->resources.push(resource);
    return g->resources.count - 1;
}

PassIndex addPass(GpuGraph* g, const Pass& pass) {
    PR_ASSERT(!g->isRecorded && g->passes.count < kPrGpuGraphMaxPasses);
    g->passes.push(pass);
    return g->passes.count - 1;
}

// Adds a pass's use of a resource, or adds the accesses to the use that the pass already has.
void addUse(GpuGraph* g, PassIndex pass, ResourceIndex resource, PrGpuAccessFlags accesses, Range range) {
    PR_ASSERT(!g->isRecorded && accesses != 0);
    if (pass == kNone || resource == kNone)
        return;
    Pass& p = g->passes.data[pass];
    for (UseIndex i = p.firstUse; i != kNone; i = g->uses.data[i].nextOfPass) {
        Use& use = g->uses.data[i];
        if (use.resource != resource)
            continue;
        bool reported = readsAndWrites(use.accesses);
        use.accesses |= accesses;
        use.range = kWholeRange;
        if (!reported && readsAndWrites(use.accesses))
            report(g, PrGpuGraphErrorKind_ReadWrite, pass);
        return;
    }
    if (readsAndWrites(accesses))
        report(g, PrGpuGraphErrorKind_ReadWrite, pass);
    g->uses.push(Use{
        .pass = pass,
        .resource = resource,
        .accesses = accesses,
        .range = range,
        .nextOfPass = kNone,
        .nextOfResource = kNone,
    });
    UseIndex added = g->uses.count - 1;
    if (p.lastUse == kNone)
        p.firstUse = added;
    else
        g->uses.data[p.lastUse].nextOfPass = added;
    p.lastUse = added;
}

// Declares that a pass writes an attachment and its resolve target. Clears an id that names no texture of the frame.
void addAttachment(GpuGraph* g, PassIndex pass, PrGpuGraphAttachment* attachment, PrGpuAccessFlags accesses) {
    PrGpuGraphTexture* const textures[] = {&attachment->texture, &attachment->resolve};
    for (PrGpuGraphTexture* texture : textures) {
        ResourceIndex resource = resourceOf(g, texture->id, true, pass);
        if (resource == kNone)
            *texture = PrGpuGraphTexture{};
        else
            addUse(g, pass, resource, accesses, kWholeRange);
    }
}

void addEdge(GpuGraph* g, PassIndex from, PassIndex to, bool keeps) {
    g->edges.push(Edge{.from = from, .to = to, .keeps = keeps, .nextFrom = kNone});
}

// Returns a pass's use of a resource, or null.
const Use* PR_NULLABLE useOf(const GpuGraph* g, PassIndex pass, ResourceIndex resource) {
    for (UseIndex i = g->passes.data[pass].firstUse; i != kNone; i = g->uses.data[i].nextOfPass) {
        if (g->uses.data[i].resource == resource)
            return &g->uses.data[i];
    }
    return nullptr;
}

// Links each resource's uses in the order that their passes were added, and adds the dependencies between the passes
// that use it. Of two uses that share bytes, the later added depends on the earlier if the earlier writes. If only the
// later writes, it follows the earlier, whose read must see the bytes as they were, but it does not need the earlier to
// be recorded.
void addDependencies(GpuGraph* g) {
    for (PassIndex pass = 0; pass < g->passes.count; ++pass) {
        for (UseIndex i = g->passes.data[pass].firstUse; i != kNone; i = g->uses.data[i].nextOfPass) {
            const Use& use = g->uses.data[i];
            Resource& resource = g->resources.data[use.resource];
            for (UseIndex e = resource.firstUse; e != kNone; e = g->uses.data[e].nextOfResource) {
                const Use& earlier = g->uses.data[e];
                if (!overlaps(earlier.range, use.range))
                    continue;
                if (writes(earlier.accesses))
                    addEdge(g, earlier.pass, pass, true);
                else if (writes(use.accesses))
                    addEdge(g, earlier.pass, pass, false);
            }
            if (resource.lastUse == kNone)
                resource.firstUse = i;
            else
                g->uses.data[resource.lastUse].nextOfResource = i;
            resource.lastUse = i;
        }
    }
    for (EdgeIndex i = 0; i < g->edges.count; ++i) {
        Edge& edge = g->edges.data[i];
        edge.nextFrom = g->passes.data[edge.from].firstEdge;
        g->passes.data[edge.from].firstEdge = i;
    }
}

// Marks the passes that a result of the frame depends on: a pass that declares no write, a pass that writes a resource
// that is left, and every pass that such a pass depends on. Counts each resource's recorded passes.
void markRecorded(GpuGraph* g) {
    for (PassIndex pass = 0; pass < g->passes.count; ++pass) {
        Pass& p = g->passes.data[pass];
        bool hasWrite = false;
        bool writesResult = false;
        for (UseIndex i = p.firstUse; i != kNone; i = g->uses.data[i].nextOfPass) {
            const Use& use = g->uses.data[i];
            if (!writes(use.accesses))
                continue;
            hasWrite = true;
            writesResult = writesResult || g->resources.data[use.resource].isLeft;
        }
        p.isRecorded = writesResult || !hasWrite;
    }
    // Most edges run from an earlier added pass to a later one, so a sweep from the last edge marks most passes at
    // once.
    for (bool changed = true; changed;) {
        changed = false;
        for (EdgeIndex i = g->edges.count; i-- > 0;) {
            const Edge& edge = g->edges.data[i];
            if (!edge.keeps || !g->passes.data[edge.to].isRecorded || g->passes.data[edge.from].isRecorded)
                continue;
            g->passes.data[edge.from].isRecorded = true;
            changed = true;
        }
    }
    for (const Use& use : g->uses.span()) {
        if (g->passes.data[use.pass].isRecorded)
            ++g->resources.data[use.resource].users;
    }
}

// Returns the accesses that a barrier brings a resource to for `first`, a use by the pass at `position` of the walk's
// order. A write goes to its own accesses. A read goes to the accesses of every read by the passes that follow, up to
// the next write, so that one barrier serves them all.
PrGpuAccessFlags accessesFor(const Walk* w, uint32_t position, const Use& first) {
    PrGpuAccessFlags accesses = first.accesses;
    if (writes(accesses))
        return accesses;
    for (uint32_t i = position + 1; i < w->count; ++i) {
        const Use* use = useOf(w->graph, w->order[i], first.resource);
        if (use == nullptr)
            continue;
        if (writes(use->accesses))
            break;
        accesses |= use->accesses;
    }
    return accesses;
}

void walkBegin(Walk* w) {
    GpuGraph* g = w->graph;
    g->written.clear();
    w->stats = PrGpuGraphStats{};
    for (ResourceIndex i = 0; i < g->resources.count; ++i) {
        const Resource& resource = g->resources.data[i];
        bool isAbsent = !resource.isFrame && (resource.isTexture ? resource.texture.id : resource.buffer.id) == 0;
        w->states[i] = ResourceState{
            .accesses = resource.accesses,
            .written = kNone,
            .remaining = resource.users,
            .lent = kNone,
            .isAbsent = isAbsent,
        };
    }
}

// Gives a texture of the frame its device texture, for its first use, by the pass at `position`: a free device texture
// as the graph's priority says, or a new one.
void lendTexture(Walk* w, uint32_t position, const Use& use) {
    GpuGraph* g = w->graph;
    Resource& resource = g->resources.data[use.resource];
    ResourceState& state = w->states[use.resource];
    PrGpuAccessFlags accesses = accessesFor(w, position, use);
    LentIndex found = kNone;
    LentIndex other = kNone;
    for (LentIndex i = 0; i < g->lent.count; ++i) {
        const Lent& lent = g->lent.data[i];
        if (lent.isHeld || !sameTexture(lent.desc, resource.desc))
            continue;
        if (lent.accesses == accesses) {
            found = i;
            break;
        }
        // A graph that favors barriers leaves a texture in accesses that write for a first use in those accesses. A
        // texture in accesses that only read matches no first use, and costs the barrier that a new texture costs.
        if (other == kNone && (g->priority == PrGpuGraphPriority_Memory || !writes(lent.accesses)))
            other = i;
    }
    if (found == kNone)
        found = other;
    if (found == kNone) {
        PrGpuTexture texture{};
        if (prGpuTextureCreate(g->device, &resource.desc, &texture) != PrStatus_Ok) {
            state.isAbsent = true;
            return;
        }
        Lent made{.desc = resource.desc, .texture = texture, .accesses = PrGpuAccessFlag_ShaderRead};
        made.desc.name = PrStringView{};
        g->lent.push(made);
        found = g->lent.count - 1;
    }
    Lent& lent = g->lent.data[found];
    if (!lent.isUsed) {
        ++w->stats.textures;
        w->stats.textureBytes += resource.bytes;
    }
    lent.isHeld = true;
    lent.isUsed = true;
    resource.texture = lent.texture;
    state.lent = found;
    state.accesses = lent.accesses;
    state.isFresh = true;
}

// Takes back the device texture of a texture of the frame after its last use, in the accesses of that use.
void returnTexture(Walk* w, ResourceIndex index) {
    GpuGraph* g = w->graph;
    Resource& resource = g->resources.data[index];
    ResourceState& state = w->states[index];
    resource.texture = PrGpuTexture{};
    if (state.lent == kNone)
        return;
    g->lent.data[state.lent].isHeld = false;
    g->lent.data[state.lent].accesses = state.accesses;
    state.lent = kNone;
}

// Returns true if a use needs a barrier on a resource in `state`.
bool useNeedsBarrier(const GpuGraph* g, const ResourceState& state, const Use& use) {
    // A read needs no barrier if an earlier barrier went to its accesses.
    if (!writes(use.accesses) && !writes(state.accesses) && (state.accesses & use.accesses) == use.accesses)
        return false;
    if (use.accesses != state.accesses)
        return true;
    // Passes run in the order of their recording, so two passes that use a texture only as an attachment need no
    // barrier between them.
    if ((use.accesses | PrGpuAccessFlag_Attachment) == PrGpuAccessFlag_Attachment)
        return false;
    // A write in the accesses of the writes before it waits for them only if it shares bytes with one of them.
    for (WrittenIndex i = state.written; i != kNone; i = g->written.data[i].next) {
        if (overlaps(g->written.data[i].range, use.range))
            return true;
    }
    return false;
}

// Records a barrier that brings a resource to `after`.
void walkBarrier(Walk* w, ResourceIndex index, PrGpuAccessFlags after) {
    const Resource& resource = w->graph->resources.data[index];
    ResourceState& state = w->states[index];
    ++w->stats.barriers;
    if (resource.isTexture) {
        // The first barrier of a texture of the frame need not keep what its device texture held.
        PrGpuAccessFlags before = state.accesses;
        if (state.isFresh)
            before |= PrGpuAccessFlag_Discard;
        prGpuCommandsTextureBarrier(w->commands, resource.texture, PrGpuSubresources{}, before, after);
    } else {
        prGpuCommandsBufferBarrier(w->commands, resource.buffer, state.accesses, after);
    }
    state.written = kNone;
    state.isFresh = false;
    state.accesses = after;
}

// Brings a resource to the accesses of a use by the pass at `position`.
void walkUse(Walk* w, uint32_t position, const Use& use) {
    GpuGraph* g = w->graph;
    ResourceState& state = w->states[use.resource];
    bool isWrite = writes(use.accesses);
    if (!isWrite && !state.isWritten && g->resources.data[use.resource].isFrame)
        report(g, PrGpuGraphErrorKind_UnwrittenRead, use.pass);
    state.isWritten = state.isWritten || isWrite;
    if (useNeedsBarrier(g, state, use))
        walkBarrier(w, use.resource, accessesFor(w, position, use));
    if (isWrite) {
        g->written.push(Written{use.range, state.written});
        state.written = g->written.count - 1;
    }
}

// Returns the texture that a texture of the graph has on the device now, or zero.
PrGpuTexture textureOf(const GpuGraph* g, PrGpuGraphTexture texture) {
    ResourceIndex index = indexOf(g, texture.id, g->resources.count);
    if (index == kNone || !g->resources.data[index].isTexture)
        return PrGpuTexture{};
    return g->resources.data[index].texture;
}

PrGpuAttachment attachmentOf(const GpuGraph* g, const PrGpuGraphAttachment& attachment) {
    return PrGpuAttachment{
        .texture = textureOf(g, attachment.texture),
        .resolve = textureOf(g, attachment.resolve),
        .level = attachment.level,
        .layer = attachment.layer,
        .load = attachment.load,
        .store = attachment.store,
    };
}

// Returns true if an attachment has the textures that a pass needs of it.
bool isComplete(const PrGpuAttachment& attachment) {
    bool resolves = attachment.store == PrGpuStore_Resolve || attachment.store == PrGpuStore_ResolveKeep;
    return attachment.texture.id != 0 && (!resolves || attachment.resolve.id != 0);
}

// Writes the device's description of a pass that draws to `out`. Returns false if the pass has no attachment, or lacks
// a texture of one.
bool passDesc(const GpuGraph* g, const Pass& pass, PrGpuPassDesc* out) {
    PrGpuPassDesc desc{
        .viewCount = pass.desc.viewCount,
        .colorCount = pass.desc.colorCount,
        .depthStencil = attachmentOf(g, pass.desc.depthStencil),
        .stencilLoad = pass.desc.stencilLoad,
        .stencilStore = pass.desc.stencilStore,
        .clearDepth = pass.desc.clearDepth,
        .clearStencil = pass.desc.clearStencil,
        .name = pass.name,
    };
    bool hasDepthStencil = pass.desc.depthStencil.texture.id != 0;
    bool complete = (desc.colorCount != 0 || hasDepthStencil) && (!hasDepthStencil || isComplete(desc.depthStencil));
    for (uint32_t i = 0; i < desc.colorCount; ++i) {
        desc.color[i] = attachmentOf(g, pass.desc.color[i]);
        desc.clearColor[i] = pass.desc.clearColor[i];
        complete = complete && isComplete(desc.color[i]);
    }
    *out = desc;
    return complete;
}

// Walks the pass at `position` of the order: lends the textures of the frame that it is the first to use, brings its
// resources to its accesses, records it, and takes back the textures that it is the last to use. A pass that uses a
// resource without a device texture or buffer changes nothing and is not recorded.
void walkPass(Walk* w, uint32_t position) {
    GpuGraph* g = w->graph;
    const Pass& pass = g->passes.data[w->order[position]];
    bool isPresent = true;
    for (UseIndex i = pass.firstUse; i != kNone; i = g->uses.data[i].nextOfPass) {
        const Use& use = g->uses.data[i];
        ResourceState& state = w->states[use.resource];
        if (g->resources.data[use.resource].isFrame && !state.isBegun) {
            state.isBegun = true;
            lendTexture(w, position, use);
        }
        isPresent = isPresent && !state.isAbsent;
    }
    PrGpuPassDesc desc{};
    if (pass.draws)
        isPresent = isPresent && passDesc(g, pass, &desc);
    if (isPresent) {
        for (UseIndex i = pass.firstUse; i != kNone; i = g->uses.data[i].nextOfPass)
            walkUse(w, position, g->uses.data[i]);
        ++w->stats.passes;
    }
    if (isPresent && pass.draws) {
        PrGpuPass* encoder = prGpuPassBegin(w->commands, &desc);
        pass.draw(pass.context, encoder);
        prGpuPassEnd(encoder);
    } else if (isPresent) {
        prGpuCommandsPushGroup(w->commands, pass.name);
        pass.record(pass.context, w->commands);
        prGpuCommandsPopGroup(w->commands);
    }
    for (UseIndex i = pass.firstUse; i != kNone; i = g->uses.data[i].nextOfPass) {
        ResourceIndex index = g->uses.data[i].resource;
        if (--w->states[index].remaining == 0 && g->resources.data[index].isFrame)
            returnTexture(w, index);
    }
}

// Ends a walk: brings each result of the frame to the accesses that the graph leaves it in.
void walkEnd(Walk* w) {
    GpuGraph* g = w->graph;
    for (ResourceIndex i = 0; i < g->resources.count; ++i) {
        const Resource& resource = g->resources.data[i];
        const ResourceState& state = w->states[i];
        if (resource.isLeft && !state.isAbsent && state.accesses != resource.left)
            walkBarrier(w, i, resource.left);
    }
}

// Fills `order` with the recorded passes: the order that they were added in, changed only where a pass depends on a
// pass that was added after it. Each pass placed is the earliest added whose dependencies are all placed. If every
// unplaced pass depends on another, the dependencies have a cycle. The earliest added of them is then reported and
// placed ahead of the passes that it depends on.
void orderPasses(GpuGraph* g, PassIndex* order, uint32_t count) {
    uint32_t* unmet = g->frame.alloc<uint32_t>(g->passes.count);    // by pass, its dependencies still to be placed
    bool* isPlaced = g->frame.alloc<bool>(g->passes.count);         // by pass
    uint32_t at = 0;
    for (PassIndex pass = 0; pass < g->passes.count; ++pass) {
        unmet[pass] = 0;
        isPlaced[pass] = false;
        if (g->passes.data[pass].isRecorded)
            order[at++] = pass;
    }
    for (const Edge& edge : g->edges.span()) {
        if (g->passes.data[edge.from].isRecorded && g->passes.data[edge.to].isRecorded)
            ++unmet[edge.to];
    }
    for (uint32_t position = 0; position < count; ++position) {
        uint32_t next = position;
        while (next < count && unmet[order[next]] != 0)
            ++next;
        if (next == count) {
            next = position;
            report(g, PrGpuGraphErrorKind_Cycle, order[next]);
        }
        // The passes that are still to be placed stay in the order that they were added.
        PassIndex pass = order[next];
        prMemMove(order + position + 1, order + position, size_t(next - position) * sizeof(PassIndex));
        order[position] = pass;
        isPlaced[pass] = true;
        for (EdgeIndex i = g->passes.data[pass].firstEdge; i != kNone; i = g->edges.data[i].nextFrom) {
            const Edge& edge = g->edges.data[i];
            if (g->passes.data[edge.to].isRecorded && !isPlaced[edge.to])
                --unmet[edge.to];
        }
    }
}

// Drops the graph's references to the resources that the frame imported.
void releaseImports(GpuGraph* graph) {
    for (const Resource& resource : graph->resources.span()) {
        if (resource.isFrame)
            continue;
        if (resource.isTexture)
            prGpuTextureRelease(graph->device, resource.texture);
        else
            prGpuBufferRelease(graph->device, resource.buffer);
    }
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrGpuGraph : GpuGraph {};

PrGpuGraphOptions prGpuGraphOptionsDefault(void) {
    return PrGpuGraphOptions{.priority = PrGpuGraphPriority_Barriers};
}

PrGpuGraph* prGpuGraphCreate(PrGpuDevice* device, const PrGpuGraphOptions* PR_NULLABLE options) {
    PrGpuGraph* graph = allocZeroed<PrGpuGraph>();
    graph->device = device;
    graph->frame.firstPage = 16 * 1024;
    graph->frame.largestPage = 256 * 1024;
    graph->priority = options != nullptr ? options->priority : prGpuGraphOptionsDefault().priority;
    return graph;
}

void prGpuGraphDestroy(PrGpuGraph* PR_NULLABLE graph) {
    if (graph == nullptr)
        return;
    if (!graph->isRecorded)
        releaseImports(graph);
    for (const Lent& lent : graph->lent.span())
        prGpuTextureRelease(graph->device, lent.texture);
    graph->resources.reset();
    graph->passes.reset();
    graph->uses.reset();
    graph->edges.reset();
    graph->errors.reset();
    graph->written.reset();
    graph->lent.reset();
    graph->frame.reset();
    prMemoryFree(graph);
}

void prGpuGraphBegin(PrGpuGraph* graph) {
    ++graph->generation;
    if (!graph->isRecorded)
        releaseImports(graph);
    graph->isRecorded = false;
    graph->resources.clear();
    graph->passes.clear();
    graph->uses.clear();
    graph->edges.clear();
    graph->errors.clear();
    graph->written.clear();
    graph->frame.clear();
    for (LentIndex i = graph->lent.count; i-- > 0;) {
        Lent& lent = graph->lent.data[i];
        lent.idleFrames = lent.isUsed ? 0 : lent.idleFrames + 1;
        lent.isHeld = false;
        lent.isUsed = false;
        if (lent.idleFrames > kIdleFrames) {
            prGpuTextureRelease(graph->device, lent.texture);
            graph->lent.erase(i);
        }
    }
}

void* prGpuGraphAlloc(PrGpuGraph* graph, size_t size) {
    return graph->frame.allocBytes(size, kAllocAlignment);
}

PrGpuGraphTexture prGpuGraphImportTexture(PrGpuGraph* graph, PrGpuTexture texture, PrGpuAccessFlags accesses) {
    prGpuTextureRetain(graph->device, texture);
    ResourceIndex index = addResource(graph, Resource{
        .isTexture = true,
        .texture = texture,
        .accesses = accesses,
        .firstUse = kNone,
        .lastUse = kNone,
    });
    return PrGpuGraphTexture{idOf(graph, index)};
}

PrGpuGraphBuffer prGpuGraphImportBuffer(PrGpuGraph* graph, PrGpuBuffer buffer, PrGpuAccessFlags accesses) {
    prGpuBufferRetain(graph->device, buffer);
    ResourceIndex index = addResource(graph, Resource{
        .buffer = buffer,
        .accesses = accesses,
        .firstUse = kNone,
        .lastUse = kNone,
    });
    return PrGpuGraphBuffer{idOf(graph, index)};
}

void prGpuGraphLeaveTexture(PrGpuGraph* graph, PrGpuGraphTexture texture, PrGpuAccessFlags accesses) {
    PR_ASSERT(!graph->isRecorded);
    ResourceIndex index = resourceOf(graph, texture.id, true, kNone);
    if (index == kNone)
        return;
    Resource& resource = graph->resources.data[index];
    PR_ASSERT(!resource.isFrame);
    // A texture of the frame has no device texture once its passes are recorded, so it cannot be left in accesses.
    if (resource.isFrame)
        return;
    resource.isLeft = true;
    resource.left = accesses;
}

void prGpuGraphLeaveBuffer(PrGpuGraph* graph, PrGpuGraphBuffer buffer, PrGpuAccessFlags accesses) {
    PR_ASSERT(!graph->isRecorded);
    ResourceIndex index = resourceOf(graph, buffer.id, false, kNone);
    if (index == kNone)
        return;
    graph->resources.data[index].isLeft = true;
    graph->resources.data[index].left = accesses;
}

PrGpuGraphTexture prGpuGraphAddTexture(PrGpuGraph* graph, const PrGpuTextureDesc* desc) {
    Resource resource{
        .isTexture = true,
        .isFrame = true,
        .desc = *desc,
        .bytes = textureBytes(*desc),
        .firstUse = kNone,
        .lastUse = kNone,
    };
    resource.desc.name = frameName(graph, desc->name);
    return PrGpuGraphTexture{idOf(graph, addResource(graph, resource))};
}

PrGpuTexture prGpuGraphTexture(const PrGpuGraph* graph, PrGpuGraphTexture texture) {
    return textureOf(graph, texture);
}

PrGpuBuffer prGpuGraphBuffer(const PrGpuGraph* graph, PrGpuGraphBuffer buffer) {
    ResourceIndex index = indexOf(graph, buffer.id, graph->resources.count);
    if (index == kNone || graph->resources.data[index].isTexture)
        return PrGpuBuffer{};
    return graph->resources.data[index].buffer;
}

PrGpuGraphPassDesc prGpuGraphPassDescColor(PrGpuGraphTexture texture, PrGpuLoad load, PrColor clear) {
    PrGpuGraphPassDesc desc{};
    desc.viewCount = 1;
    desc.colorCount = 1;
    desc.color[0] = PrGpuGraphAttachment{.texture = texture, .load = load, .store = PrGpuStore_Keep};
    desc.clearColor[0] = clear;
    return desc;
}

PrGpuGraphPass prGpuGraphAddPass(PrGpuGraph* graph, const PrGpuGraphPassDesc* desc, PrGpuGraphDrawFn draw,
        void* PR_NULLABLE context) {
    PR_ASSERT(desc->colorCount <= kPrGpuMaxColorAttachments);
    PassIndex index = addPass(graph, Pass{
        .draws = true,
        .desc = *desc,
        .draw = draw,
        .context = context,
        .firstUse = kNone,
        .lastUse = kNone,
        .firstEdge = kNone,
    });
    PrStringView name = frameName(graph, desc->name);
    Pass& pass = graph->passes.data[index];
    pass.name = name;
    pass.desc.name = name;
    for (uint32_t i = 0; i < desc->colorCount; ++i)
        addAttachment(graph, index, &pass.desc.color[i], PrGpuAccessFlag_ColorAttachment);
    addAttachment(graph, index, &pass.desc.depthStencil, PrGpuAccessFlag_DepthStencilWrite);
    return PrGpuGraphPass{idOf(graph, index)};
}

PrGpuGraphPass prGpuGraphAddWork(PrGpuGraph* graph, PrStringView name, PrGpuGraphRecordFn record, void* PR_NULLABLE context) {
    PassIndex index = addPass(graph, Pass{
        .record = record,
        .context = context,
        .firstUse = kNone,
        .lastUse = kNone,
        .firstEdge = kNone,
    });
    PrStringView copied = frameName(graph, name);
    graph->passes.data[index].name = copied;
    return PrGpuGraphPass{idOf(graph, index)};
}

void prGpuGraphUseTexture(PrGpuGraph* graph, PrGpuGraphPass pass, PrGpuGraphTexture texture, PrGpuAccessFlags accesses) {
    PassIndex index = passOf(graph, pass);
    addUse(graph, index, resourceOf(graph, texture.id, true, index), accesses, kWholeRange);
}

void prGpuGraphUseBuffer(PrGpuGraph* graph, PrGpuGraphPass pass, PrGpuGraphBuffer buffer, PrGpuAccessFlags accesses) {
    PassIndex index = passOf(graph, pass);
    addUse(graph, index, resourceOf(graph, buffer.id, false, index), accesses, kWholeRange);
}

void prGpuGraphUseBufferRange(PrGpuGraph* graph, PrGpuGraphPass pass, PrGpuGraphBuffer buffer,
        PrGpuAccessFlags accesses, uint64_t offset, uint64_t size) {
    PassIndex index = passOf(graph, pass);
    addUse(graph, index, resourceOf(graph, buffer.id, false, index), accesses, Range{offset, size});
}

void prGpuGraphFollowPass(PrGpuGraph* graph, PrGpuGraphPass pass, PrGpuGraphPass earlier) {
    PR_ASSERT(!graph->isRecorded);
    PassIndex to = passOf(graph, pass);
    PassIndex from = passOf(graph, earlier);
    if (to != kNone && from != kNone)
        addEdge(graph, from, to, true);
}

PrGpuGraphStats prGpuGraphRecord(PrGpuGraph* graph, PrGpuCommands* commands) {
    PR_ASSERT(!graph->isRecorded);
    graph->isRecorded = true;
    addDependencies(graph);
    markRecorded(graph);

    uint32_t count = 0;
    for (const Pass& pass : graph->passes.span())
        count += pass.isRecorded ? 1 : 0;
    PassIndex* order = graph->frame.alloc<PassIndex>(count);
    orderPasses(graph, order, count);

    Walk walk{
        .graph = graph,
        .commands = commands,
        .order = order,
        .count = count,
        .states = graph->frame.alloc<ResourceState>(graph->resources.count),
    };
    walkBegin(&walk);
    for (uint32_t position = 0; position < count; ++position)
        walkPass(&walk, position);
    walkEnd(&walk);
    releaseImports(graph);
    return walk.stats;
}

uint32_t prGpuGraphErrorCount(const PrGpuGraph* graph) {
    return graph->errors.count;
}

PrGpuGraphError prGpuGraphError(const PrGpuGraph* graph, uint32_t index) {
    PR_ASSERT(index < graph->errors.count);
    return graph->errors.data[index];
}
