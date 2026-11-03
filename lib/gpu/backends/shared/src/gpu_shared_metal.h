// What the Metal backends share: resources and their residency, formats, the stream, uploads' staging, swap chains, and
// the commands whose Objective-C selectors both APIs spell alike.
//
// Metal has no descriptor heap, so a device keeps two buffers of 8-byte entries that shaders index by handle
// (pr/gpu.slang): resources, where an entry is a texture's MTLResourceID or a buffer's GPU address, and samplers, where
// it is a sampler's MTLResourceID. A command buffer binds push data at buffer index 0, uniforms at 1, the resource heap
// at 2 and the sampler heap at 3. Push data and uniforms go into the command buffer's stream. Resources are untracked,
// so a backend orders work by itself. Every resource is in one residency set that the queue holds.
//
// A destroyed object is kept until every command buffer that began before its destruction is done, since any of them
// may use it. A fence is reached once its submission and those before it are done and the functions that wait for the
// submission have run.
//
// Objects are held with manual reference counting: these files are compiled without ARC, so that structures that run no
// constructors can hold them.

#pragma once

#include "pr/atomic.h"
#include "pr/gpu_backend.h"
#include "pr/mutex.h"
#include "pr/raw_array.h"
#include "pr/thread.h"

#include "gpu/backends/shared/src/gpu_shared_upload.h"

// The system APIs that the structures below name.
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#if __has_feature(objc_arc)
#error "the Metal backends hold Objective-C objects in plain structures, and are compiled without ARC"
#endif

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The handles that a device gives out, of resources and of samplers, and the pipelines that it holds. Slot zero of each
// is never used, so that zero is none.
constexpr uint32_t kGpuMetalMaxResources = 1u << 16;
constexpr uint32_t kGpuMetalMaxSamplers = 2048;
constexpr uint32_t kGpuMetalMaxPipelines = 4096;

// The buffer indices of root data and heaps, as pr/gpu.slang binds them.
enum {
    GpuMetalBinding_Push = 0,
    GpuMetalBinding_Uniforms = 1,
    GpuMetalBinding_Resources = 2,
    GpuMetalBinding_Samplers = 3,
    GpuMetalBinding_Count,
};
typedef uint32_t GpuMetalBinding;

// The stream's blocks, and the alignment of its allocations, which every use of a buffer's address accepts.
constexpr uint64_t kGpuMetalStreamBlock = uint64_t(1) << 20;
constexpr uint64_t kGpuMetalStreamAlignment = 256;

// The staging of uploads: blocks of shared memory, and the number that a batch fills before it is committed without
// waiting for a submission.
constexpr uint64_t kGpuMetalStagingBlock = uint64_t(4) << 20;
constexpr uint32_t kGpuMetalStagingFlushBlocks = 4;

// What a resource slot holds.
enum {
    GpuMetalSlot_Free,
    GpuMetalSlot_Buffer,
    GpuMetalSlot_Texture,
};
typedef uint8_t GpuMetalSlot;

// A resource slot.
struct GpuMetalResource {
    GpuMetalSlot kind;
    union {
        PrGpuBufferDesc buffer;
        PrGpuTextureDesc texture;
    };
    id PR_NULLABLE object;              // id<MTLBuffer> or id<MTLTexture>, retained
};

// A pipeline of either kind, with the state that a render encoder sets for it.
struct GpuMetalPipeline {
    bool compute;
    id PR_NULLABLE state;                              // id<MTLRenderPipelineState> or id<MTLComputePipelineState>,
                                                       // retained
    id<MTLDepthStencilState> PR_NULLABLE depthStencil; // retained
    MTLPrimitiveType primitive;
    MTLCullMode cull;
    MTLWinding winding;
    float depthBias;
    float depthBiasSlope;
    float depthBiasClamp;
    MTLSize groupSize;
};

// An object that waits to be freed until every command buffer that began before `serial` is done.
enum {
    GpuMetalRetiredKind_Resource,
    GpuMetalRetiredKind_Pipeline,
};
typedef uint8_t GpuMetalRetiredKind;

// An object that waits to be freed, and the serial that it waits for.
struct GpuMetalRetired {
    uint64_t serial;
    GpuMetalRetiredKind kind;
    uint32_t slot;
};

// A block of shared memory that the stream or staging writes into, with a resource slot so that shaders can read it.
struct GpuMetalBlock {
    id<MTLBuffer> PR_NULLABLE buffer;   // retained
    uint32_t slot;
    uint64_t used;
};

// What a command buffer held, which is free again once its fence is reached. A backend's own kinds start at Backend,
// and its `heldRecycle` takes them.
enum {
    GpuMetalHeldKind_StreamBlock,
    GpuMetalHeldKind_StagingBlock,
    GpuMetalHeldKind_Backend,
};
typedef uint8_t GpuMetalHeldKind;

// One thing that a command buffer held, and the fence that frees it.
struct GpuMetalHeld {
    PrGpuFence fence;
    GpuMetalHeldKind kind;
    GpuMetalBlock block;                // the blocks
    id PR_NULLABLE object;              // a backend's own object, retained
};

// A command buffer that is begun or in flight, and the fence that it completes at once it is submitted.
struct GpuMetalInFlight {
    uint64_t serial;
    PrGpuFence fence;                   // zero until submitted
};

// A function that a command buffer calls once it is done.
struct GpuMetalDone {
    PrGpuDoneFn done;
    void* PR_NULLABLE context;
};

// A shader library, one metallib that holds every entry point.
struct GpuMetalLibrary : PrGpuShaderLibrary {
    id<MTLLibrary> library;             // retained
};

// A drawable that presents with a command buffer.
struct GpuMetalDrawable {
    id drawable;                        // id<CAMetalDrawable>, retained
    bool transactional;                 // presents within the window's display transaction
};

struct GpuMetalPass;

// The state of a command buffer that the backends share, at the start of each backend's own.
struct GpuMetalCommands : PrGpuCommands {
    uint64_t serial;                    // the order in which command buffers began
    GpuMetalPass* PR_NULLABLE pass;     // the open pass
    PrGpuHostStateFlags changed;        // the host's pass's state changed since the last sync
    uint32_t pipeline;                  // the pipeline of dispatches and draws
    RawArray<GpuMetalBlock> blocks;     // the blocks of the stream
    RawArray<GpuMetalDrawable> drawables;
    RawArray<GpuMetalDone> dones;
    RawArray<void*> scratch;               // what the stream gave out once the device had no memory left
};

// A pass of either API.
struct GpuMetalPass : PrGpuPass {
    id encoder;                         // id<MTLRenderCommandEncoder> or id<MTL4RenderCommandEncoder>, retained
    PrGpuBuffer indexBuffer;
    uint64_t indexOffset;
    MTLIndexType indexType;
};

// A swap chain over a Metal layer.
struct GpuMetalSwapChain : PrGpuSwapChain {
    CAMetalLayer* layer;                      // retained
    NSView* view;                             // the view that the layer backs or the view hosts, not retained
    bool transactional;                       // true unless the swap chain was created PrGpuSwapChainFlag_Threaded
    id<CAMetalDrawable> PR_NULLABLE drawable; // the drawable of the acquired frame, retained
    PrGpuTexture texture;                     // the drawable's texture, while it is acquired
};

// A copy of a staged upload into a texture.
struct GpuMetalTextureCopy {
    id<MTLBuffer> staging;
    uint64_t at;                        // the byte offset of the first slice in `staging`
    uint32_t rowBytes;
    uint64_t sliceBytes;
    uint32_t slices;
    id<MTLTexture> texture;
    const PrGpuTextureRegion* region;
    bool is3d;
    bool barrier;                       // the batch wrote the texture since its last barrier
};

// The state of a device that the backends share, at the start of each backend's own.
struct GpuMetalDevice : PrGpuDevice {
    id<MTLDevice> device;                   // retained
    id queue;                               // id<MTLCommandQueue> or id<MTL4CommandQueue>, retained
    Atomic<uint64_t> done;                  // the fence of the last submission that is done
    PrMutex doneMutex;                      // guards raising `done`, for doneSignal
    PrCondition doneSignal;                 // broadcast when `done` rises
    RawArray<PrGpuFence> doneAhead;         // the fences that completed ahead of one before them, under doneMutex
    id<MTLResidencySet> residency;          // retained
    id<MTLBuffer> resourceHeap;             // retained
    id<MTLBuffer> samplerHeap;              // retained
    Atomic<bool> lost;
    char adapter[128];                      // the adapter's name, in UTF-8

    // What a backend does for the shared code, each with the mutex held: opens the batch of uploads if it is not open,
    // commits the open batch ahead of whatever is submitted after it, encodes a copy into a texture in the open batch,
    // and takes back what a command buffer held of the backend's own kinds.
    void (*uploadsOpen)(GpuMetalDevice* d);
    void (*uploadsSubmit)(GpuMetalDevice* d);
    void (*uploadsCopyTexture)(GpuMetalDevice* d, const GpuMetalTextureCopy& copy);
    void (*PR_NULLABLE heldRecycle)(GpuMetalDevice* d, GpuMetalHeld& held);

    // The mutex guards everything below. The tables have fixed capacities, so that recording reads them without it.
    PrMutex mutex;
    GpuMetalResource* resources;                          // by slot
    id<MTLSamplerState> PR_NULLABLE* PR_NONNULL samplers; // by slot
    GpuMetalPipeline* pipelines;                          // by id
    RawArray<uint32_t> freeResources;
    uint32_t nextResource;
    uint32_t nextSampler;
    RawArray<uint32_t> freePipelines;
    uint32_t nextPipeline;
    bool residencyDirty;

    PrGpuFence submitted;                   // the last fence that a submission returned
    uint64_t serial;                        // the serial of the next command buffer
    RawArray<GpuMetalInFlight> inFlight;    // in the order of their submission
    RawArray<GpuMetalRetired> retired;
    RawArray<GpuMetalHeld> held;
    RawArray<GpuMetalBlock> streamBlocks;      // free
    RawArray<GpuMetalBlock> stagingBlocks;     // free

    // The staging blocks and buffer copies of the open batch of uploads, and the serial of its command buffer.
    uint64_t uploadSerial;
    RawArray<GpuMetalBlock> uploadBlocks;
    GpuSharedUploadBatch uploadBatch;
};

// Returns the device's shared state.
inline GpuMetalDevice* gpuMetalDeviceFrom(PrGpuDevice* device) {
    return static_cast<GpuMetalDevice*>(device);
}

// Returns the device's shared state, for reading.
inline const GpuMetalDevice* gpuMetalDeviceFrom(const PrGpuDevice* device) {
    return static_cast<const GpuMetalDevice*>(device);
}

// Returns the command buffer's shared state.
inline GpuMetalCommands* gpuMetalCommandsFrom(PrGpuCommands* commands) {
    return static_cast<GpuMetalCommands*>(commands);
}

// Returns an autoreleased string of the view.
NSString* gpuMetalString(PrStringView view);

// Conversions from the interface's values to Metal's.
MTLPixelFormat gpuMetalPixelFormat(PrGpuFormat format);
PrGpuFormat gpuMetalFormatFromPixelFormat(MTLPixelFormat format);
MTLCompareFunction gpuMetalCompare(PrGpuCompare compare);
MTLBlendFactor gpuMetalBlendFactor(PrGpuBlendFactor factor, bool alpha);
MTLBlendOperation gpuMetalBlendOp(PrGpuBlendOp op);
MTLPrimitiveType gpuMetalPrimitive(PrGpuTopology topology);
MTLPrimitiveTopologyClass gpuMetalTopologyClass(PrGpuTopology topology);

// The stages of the work that makes accesses.
MTLStages gpuMetalStages(PrGpuAccessFlags access);

// Puts an object into a new resource slot, adds it to the residency set and writes its heap entry. Returns zero if no
// slot is left. The Locked form's caller holds the mutex.
uint32_t gpuMetalResourceAddLocked(GpuMetalDevice* d, GpuMetalSlot kind, id object, uint64_t entry);
uint32_t gpuMetalResourceAdd(GpuMetalDevice* d, GpuMetalSlot kind, id object, uint64_t entry);

// Frees what the command buffers that are done held: retired objects, and the blocks and objects of their recording.
// The caller holds the mutex.
void gpuMetalCollect(GpuMetalDevice* d);

// Retires an object, which is freed once the command buffers that began before now are done.
void gpuMetalRetire(GpuMetalDevice* d, GpuMetalRetiredKind kind, uint32_t slot);

// Commits the residency set if it changed. The caller holds the mutex.
void gpuMetalCommitResidency(GpuMetalDevice* d);

// Return the object of a resource slot.
id<MTLBuffer> gpuMetalBuffer(const GpuMetalDevice* d, uint32_t slot);
id<MTLTexture> gpuMetalTexture(const GpuMetalDevice* d, uint32_t slot);

// Implement prGpuDeviceWait and prGpuDeviceWaitIdle.
void gpuMetalWait(PrGpuDevice* device, PrGpuFence fence);
void gpuMetalWaitIdle(PrGpuDevice* device);

// Puts a pipeline into a new id, or returns zero if none is left.
uint32_t gpuMetalPipelineAdd(GpuMetalDevice* d, const GpuMetalPipeline& pipeline);

// Allocates `size` bytes of the stream, at kGpuMetalStreamAlignment, in a block that the command buffer holds.
void* gpuMetalStream(GpuMetalDevice* d, GpuMetalCommands* c, uint64_t size, uint32_t* slot, uint64_t* offset);

// Sets a graphics pipeline's state on a render encoder of either API.
void gpuMetalRenderPipelineSet(const GpuMetalPipeline& p, id encoder);

// Acquires a swap chain's next drawable, at `extent`.
PrStatus gpuMetalSwapChainAcquire(PrGpuDevice* device, PrGpuSwapChain* handle, PrUVec2 extent, PrGpuTexture* out);

// Runs what a submission's completion does: reports an error, calls `dones` and frees them, and raises the device's
// count of fences done. Called once for each fence, in any order.
void gpuMetalSubmissionDone(GpuMetalDevice* d, PrGpuFence fence, GpuMetalDone* PR_NULLABLE dones, uint32_t doneCount,
    NSError* PR_NULLABLE error, double seconds);

// Records that a submission committed at `fence`: its command buffer's serial, and the stream blocks that it holds. The
// caller holds the mutex.
void gpuMetalSubmissionRecord(GpuMetalDevice* d, GpuMetalCommands* c, PrGpuFence fence);

// Records that the open batch of uploads committed at `fence`, and empties the batch. The caller holds the mutex.
void gpuMetalUploadsRecord(GpuMetalDevice* d, PrGpuFence fence);

// Returns a copy of the command buffer's functions to call once it is done, for its submission's completion to free.
GpuMetalDone* PR_NULLABLE gpuMetalDonesCopy(const GpuMetalCommands* c);

// Fills in a new command buffer's device and serial. The caller holds the mutex.
void gpuMetalCommandsInit(GpuMetalDevice* d, GpuMetalCommands* c);

// Frees a command buffer after its submission. The caller has released its drawables.
void gpuMetalCommandsDestroy(GpuMetalCommands* c);

// Fills a graphics pipeline's state that both APIs describe alike: its depth and stencil state, and what the render
// encoder sets. The caller sets `state`.
GpuMetalPipeline gpuMetalPipelineFromDesc(GpuMetalDevice* d, const PrGpuGraphicsPipelineDesc* desc);

// Sets the pipeline of the command buffer's next dispatches and draws, on the open pass if there is one.
void gpuMetalCommandsSetPipeline(PrGpuDevice* device, PrGpuCommands* commands, PrGpuPipeline pipeline);

// Encode copies on an encoder of either API that takes copies.
void gpuMetalEncodeCopyTexture(const GpuMetalDevice* d, id encoder, PrGpuTexture source,
    const PrGpuTextureRegion* sourceRegion, PrGpuTexture target, const PrGpuTextureRegion* targetRegion);
void gpuMetalEncodeCopyBufferToTexture(const GpuMetalDevice* d, id encoder, PrGpuBuffer source, uint64_t sourceOffset,
    uint32_t rowBytes, uint32_t layerBytes, PrGpuTexture target, const PrGpuTextureRegion* targetRegion);
void gpuMetalEncodeCopyTextureToBuffer(const GpuMetalDevice* d, id encoder, PrGpuTexture source,
    const PrGpuTextureRegion* sourceRegion, PrGpuBuffer target, uint64_t targetOffset, uint32_t rowBytes,
    uint32_t layerBytes);
void gpuMetalEncodeUploadCopies(const GpuMetalDevice* d, id encoder);
void gpuMetalEncodeTextureCopy(id encoder, const GpuMetalTextureCopy& copy);

// Marks the state of the host's pass that the device changed. The state of the device's own encoders ends with them.
void gpuMetalPassChanged(GpuMetalPass* pass, PrGpuHostStateFlags flags);

// Fills a device's shared state and the functions that the backends share, over `device`, which it takes. The backend
// then creates the queue and calls gpuMetalDeviceAddQueue, which takes the queue and adds the residency set to it.
void gpuMetalDeviceInit(GpuMetalDevice* d, id<MTLDevice> device);
void gpuMetalDeviceAddQueue(GpuMetalDevice* d, id queue);

// Frees a device's shared state. The backend has freed its own.
void gpuMetalDeviceReset(GpuMetalDevice* d);

// Wraps the host's texture as a texture of the device.
PrStatus gpuMetalTextureWrap(GpuMetalDevice* d, id<MTLTexture> texture, PrGpuTextureUsageFlags usage, PrGpuTexture* out);

// Fills a pass's color attachment in a pipeline descriptor of either API, other than whether it blends.
template <typename Attachment>
void gpuMetalColorAttachmentFill(Attachment* PR_NONNULL a, PrGpuFormat format, const PrGpuBlend& blend) {
    a.pixelFormat = gpuMetalPixelFormat(format);
    a.sourceRGBBlendFactor = gpuMetalBlendFactor(blend.colorSource, false);
    a.destinationRGBBlendFactor = gpuMetalBlendFactor(blend.colorTarget, false);
    a.rgbBlendOperation = gpuMetalBlendOp(blend.colorOp);
    a.sourceAlphaBlendFactor = gpuMetalBlendFactor(blend.alphaSource, true);
    a.destinationAlphaBlendFactor = gpuMetalBlendFactor(blend.alphaTarget, true);
    a.alphaBlendOperation = gpuMetalBlendOp(blend.alphaOp);
    PrGpuColorWriteFlags write = blend.write != 0 ? blend.write : PrGpuColorWriteFlag_All;
    a.writeMask = ((write & PrGpuColorWriteFlag_Red) ? MTLColorWriteMaskRed : 0) |
        ((write & PrGpuColorWriteFlag_Green) ? MTLColorWriteMaskGreen : 0) |
        ((write & PrGpuColorWriteFlag_Blue) ? MTLColorWriteMaskBlue : 0) |
        ((write & PrGpuColorWriteFlag_Alpha) ? MTLColorWriteMaskAlpha : 0);
}

// Returns the load action of a load.
inline MTLLoadAction gpuMetalLoadAction(PrGpuLoad load) {
    if (load == PrGpuLoad_Clear)
        return MTLLoadActionClear;
    return load == PrGpuLoad_Keep ? MTLLoadActionLoad : MTLLoadActionDontCare;
}

// Fills a render pass descriptor of either API from a pass's attachments, and the pass's formats and extent.
template <typename Descriptor>
void gpuMetalPassAttachmentsFill(const GpuMetalDevice* d, Descriptor* PR_NONNULL rp, const PrGpuPassDesc* desc,
        GpuMetalPass* pass) {
    pass->formats.samples = 1;
    for (uint8_t i = 0; i < desc->colorCount; ++i) {
        const PrGpuAttachment& a = desc->color[i];
        MTLRenderPassColorAttachmentDescriptor* ca = rp.colorAttachments[i];
        ca.texture = gpuMetalTexture(d, a.texture.id);
        ca.level = a.level;
        ca.slice = a.layer;
        ca.loadAction = gpuMetalLoadAction(a.load);
        const PrColor& clear = desc->clearColor[i];
        ca.clearColor = MTLClearColorMake(double(clear.r), double(clear.g), double(clear.b), double(clear.a));
        if (a.store == PrGpuStore_Resolve) {
            ca.resolveTexture = gpuMetalTexture(d, a.resolve.id);
            ca.storeAction = MTLStoreActionMultisampleResolve;
        } else {
            ca.storeAction = a.store == PrGpuStore_Keep ? MTLStoreActionStore : MTLStoreActionDontCare;
        }
        const PrGpuTextureDesc& t = d->resources[a.texture.id].texture;
        pass->formats.color[pass->formats.colorCount++] = t.format;
        pass->formats.samples = t.samples;
        uint32_t width = t.extent.width >> a.level;
        uint32_t height = t.extent.height >> a.level;
        pass->extent = PrUVec2{width != 0 ? width : 1, height != 0 ? height : 1};
    }
    if (desc->depthStencil.texture.id == 0)
        return;
    const PrGpuAttachment& a = desc->depthStencil;
    const PrGpuTextureDesc& t = d->resources[a.texture.id].texture;
    MTLRenderPassDepthAttachmentDescriptor* da = rp.depthAttachment;
    da.texture = gpuMetalTexture(d, a.texture.id);
    da.level = a.level;
    da.slice = a.layer;
    da.loadAction = gpuMetalLoadAction(a.load);
    da.storeAction = a.store == PrGpuStore_Keep ? MTLStoreActionStore : MTLStoreActionDontCare;
    da.clearDepth = double(desc->clearDepth);
    if (t.format == PrGpuFormat_D32FS8) {
        MTLRenderPassStencilAttachmentDescriptor* sa = rp.stencilAttachment;
        sa.texture = da.texture;
        sa.level = a.level;
        sa.slice = a.layer;
        sa.loadAction = gpuMetalLoadAction(desc->stencilLoad);
        sa.storeAction = desc->stencilStore == PrGpuStore_Keep ? MTLStoreActionStore : MTLStoreActionDontCare;
        sa.clearStencil = desc->clearStencil;
    }
    pass->formats.depthStencil = t.format;
    pass->formats.samples = t.samples;
    if (desc->colorCount == 0) {
        uint32_t width = t.extent.width >> a.level;
        uint32_t height = t.extent.height >> a.level;
        pass->extent = PrUVec2{width != 0 ? width : 1, height != 0 ? height : 1};
    }
}

} // namespace pr_impl

PR_ASSUME_NONNULL_END
