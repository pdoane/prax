// The Metal GPU backend, on Metal 4.
//
// Metal has no descriptor heap, so the device keeps two buffers of 8-byte entries that shaders index by handle
// (pr/gpu.slang): resources, where an entry is a texture's MTLResourceID or a buffer's GPU address, and samplers, where
// it is a sampler's MTLResourceID. One argument table per command buffer serves every encoder: push data at buffer
// index 0, uniforms at 1, the resource heap at 2 and the sampler heap at 3. Metal 4 has no setBytes, so push data and
// uniforms go into the command buffer's stream, and the table points at them. Metal snapshots the table at each draw
// and dispatch.
//
// Resources are untracked, and Metal 4 orders nothing by itself. A barrier is a producer barrier on the command
// buffer's last encoder, which holds the stages that it names of every later encoder in the queue until those stages of
// every encoder before are done. So the last encoder stays open until the next one begins, and a barrier at the start
// of a command buffer goes on an empty encoder. Dispatches and copies between two barriers share an encoder, and run
// concurrently. Every resource is in one residency set that the queue holds.
//
// A destroyed object is kept until every command buffer that began before its destruction is done, since any of them
// may use it. A fence is reached when each submission's feedback raises the device's count of fences done, once the
// functions that wait for the submission have run, so that a fence is reached only after them. The queue delivers
// feedback in order, on a serial queue.
//
// Uploads are copied in a command buffer of the device's own, committed just before the next submission, or sooner once
// its staging fills a few blocks. It waits for all the work before it, and all the work after it waits for it.
//
// Objects are held with manual reference counting: this file is compiled without ARC, so that structures that run no
// constructors can hold them.

#include "pr/gpu_metal.h"

#include "pr/alloc.h"
#include "pr/array.h"
#include "pr/assert.h"
#include "pr/atomic.h"
#include "pr/gpu_backend.h"
#include "pr/integer.h"
#include "pr/mem.h"
#include "pr/mutex.h"
#include "pr/search.h"
#include "pr/thread.h"

#include "gpu/backends/shared/src/gpu_shared_upload.h"

#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#if __has_feature(objc_arc)
#error "gpu_metal.mm holds Objective-C objects in plain structures, and is compiled without ARC"
#endif

namespace pr_impl {

namespace {

// The handles that a device gives out, of resources and of samplers, and the pipelines that it holds. Slot zero of each
// is never used, so that zero is none.
constexpr uint32_t kMaxResources = 1u << 16;
constexpr uint32_t kMaxSamplers = 2048;
constexpr uint32_t kMaxPipelines = 4096;

// The argument table's buffer indices, as pr/gpu.slang binds them.
enum {
    MetalBinding_Push = 0,
    MetalBinding_Uniforms = 1,
    MetalBinding_Resources = 2,
    MetalBinding_Samplers = 3,
    MetalBinding_Count,
};
typedef uint32_t MetalBinding;

// The stream's blocks, and the alignment of its allocations, which every use of a buffer's address accepts.
constexpr uint64_t kStreamBlock = uint64_t(1) << 20;
constexpr uint64_t kStreamAlignment = 256;

// The staging of uploads: blocks of shared memory, and the number that a batch fills before it is committed without
// waiting for a submission.
constexpr uint64_t kStagingBlock = uint64_t(4) << 20;
constexpr uint32_t kStagingFlushBlocks = 4;

constexpr MTLStages kAllStages = MTLStageVertex | MTLStageFragment | MTLStageDispatch | MTLStageBlit;

// What a resource slot holds.
enum {
    MetalSlot_Free,
    MetalSlot_Buffer,
    MetalSlot_Texture,
};
typedef uint8_t MetalSlot;

struct MetalResource {
    MetalSlot kind;
    union {
        PrGpuBufferDesc buffer;
        PrGpuTextureDesc texture;
    };
    id PR_NULLABLE object;              // id<MTLBuffer> or id<MTLTexture>, retained
};

struct MetalPipeline {
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
    MetalRetiredKind_Resource,
    MetalRetiredKind_Pipeline,
};
typedef uint8_t MetalRetiredKind;

struct MetalRetired {
    uint64_t serial;
    MetalRetiredKind kind;
    uint32_t slot;
};

// A block of shared memory that the stream or staging writes into, with a resource slot so that shaders can read it.
struct MetalBlock {
    id<MTLBuffer> PR_NULLABLE buffer;   // retained
    uint32_t slot;
    uint64_t used;
};

// What a command buffer held, which is free again once its fence is reached.
enum {
    MetalHeldKind_StreamBlock,
    MetalHeldKind_StagingBlock,
    MetalHeldKind_Allocator,
    MetalHeldKind_Table,
};
typedef uint8_t MetalHeldKind;

struct MetalHeld {
    PrGpuFence fence;
    MetalHeldKind kind;
    MetalBlock block;                   // the blocks
    id PR_NULLABLE object;              // the allocator or the table, retained
};

// A command buffer that is begun or in flight, and the fence that it completes at once it is submitted.
struct MetalInFlight {
    uint64_t serial;
    PrGpuFence fence;                   // zero until submitted
};

// A function that a command buffer calls once it is done.
struct MetalDone {
    PrGpuDoneFn done;
    void* PR_NULLABLE context;
};

struct MetalLibrary : PrGpuShaderLibrary {
    id<MTLLibrary> library;             // retained
};

struct MetalPass;

struct MetalCommands : PrGpuCommands {
    uint64_t serial;                                   // the order in which command buffers began
    MetalPass* PR_NULLABLE pass;                       // the open pass
    id<MTL4CommandBuffer> buffer;                      // retained
    id<MTL4CommandAllocator> PR_NULLABLE allocator;    // retained; null for the host's command buffer
    id<MTL4ArgumentTable> table;                       // retained
    id<MTL4ComputeCommandEncoder> PR_NULLABLE compute; // the encoder that takes dispatches and copies, retained
    id<MTL4CommandEncoder> PR_NULLABLE tail;           // the last encoder, open for barriers, retained
    PrGpuHostStateFlags changed;                       // the host's pass's state changed since the last sync
    uint32_t pipeline;                                 // the pipeline of dispatches and draws
    Array<MetalBlock> blocks;                          // the blocks of the stream
    Array<id> drawables;                               // the id<CAMetalDrawable>s that present with the command buffer,
                                                       // retained
    Array<MetalDone> dones;
    Array<void*> scratch;               // what the stream gave out once the device had no memory left
};

struct MetalPass : PrGpuPass {
    id<MTL4RenderCommandEncoder> encoder;   // retained
    PrGpuBuffer indexBuffer;
    uint64_t indexOffset;
    MTLIndexType indexType;
};

struct MetalSwapChain : PrGpuSwapChain {
    CAMetalLayer* layer;                      // retained
    NSView* view;                             // the view that the layer backs or the view hosts, not retained
    bool transactional;                       // true unless the swap chain was created PrGpuSwapChainFlag_Threaded
    id<CAMetalDrawable> PR_NULLABLE drawable; // the drawable of the acquired frame, retained
    PrGpuTexture texture;                     // the drawable's texture, while it is acquired
};

struct MetalDevice : PrGpuDevice {
    id<MTLDevice> device;                   // retained
    id<MTL4CommandQueue> queue;             // retained
    id<MTL4Compiler> compiler;              // retained
    Atomic<uint64_t> done;                  // the fence of the last submission that is done
    PrMutex doneMutex;                      // guards raising `done`, for doneSignal
    PrCondition doneSignal;                 // broadcast when `done` rises
    dispatch_queue_t feedback;              // the serial queue of the submissions' feedback, retained
    id<MTLResidencySet> residency;          // retained
    id<MTLBuffer> resourceHeap;             // retained
    id<MTLBuffer> samplerHeap;              // retained
    Atomic<bool> lost;
    char adapter[128];                      // the adapter's name, in UTF-8

    // The mutex guards everything below. The tables have fixed capacities, so that recording reads them without it.
    PrMutex mutex;
    MetalResource* resources;                  // by slot
    id<MTLSamplerState> PR_NULLABLE* samplers; // by slot
    MetalPipeline* pipelines;                  // by id
    Array<uint32_t> freeResources;
    uint32_t nextResource;
    uint32_t nextSampler;
    Array<uint32_t> freePipelines;
    uint32_t nextPipeline;
    bool residencyDirty;

    PrGpuFence submitted;                   // the last fence that a submission returned
    uint64_t serial;                        // the serial of the next command buffer
    Array<MetalInFlight> inFlight;          // in the order of their submission
    Array<MetalRetired> retired;
    Array<MetalHeld> held;
    Array<id> allocators;                   // free id<MTL4CommandAllocator>s, retained
    Array<id> tables;                       // free id<MTL4ArgumentTable>s, retained
    Array<MetalBlock> streamBlocks;         // free
    Array<MetalBlock> stagingBlocks;        // free

    // The open batch of uploads: its command buffer and encoder, the staging blocks that it reads, and its copies.
    id<MTL4CommandBuffer> PR_NULLABLE uploads;               // retained
    id<MTL4CommandAllocator> PR_NULLABLE uploadAllocator;    // retained
    id<MTL4ComputeCommandEncoder> PR_NULLABLE uploadEncoder; // retained
    uint64_t uploadSerial;
    Array<MetalBlock> uploadBlocks;
    GpuSharedUploadBatch uploadBatch;
};

MetalDevice* metalOf(PrGpuDevice* device) {
    return static_cast<MetalDevice*>(device);
}

const MetalDevice* metalOf(const PrGpuDevice* device) {
    return static_cast<const MetalDevice*>(device);
}

NSString* stringOf(PrStringView view) {
    return [[[NSString alloc] initWithBytes:view.data length:view.size encoding:NSUTF8StringEncoding] autorelease];
}

// ---------------------------------------------------------------------------------------------------------------------
// Formats and states

MTLPixelFormat formatOf(PrGpuFormat format) {
    switch (format) {
        case PrGpuFormat_R8: return MTLPixelFormatR8Unorm;
        case PrGpuFormat_Rg8: return MTLPixelFormatRG8Unorm;
        case PrGpuFormat_Rgba8: return MTLPixelFormatRGBA8Unorm;
        case PrGpuFormat_Rgba8Srgb: return MTLPixelFormatRGBA8Unorm_sRGB;
        case PrGpuFormat_Bgra8: return MTLPixelFormatBGRA8Unorm;
        case PrGpuFormat_Bgra8Srgb: return MTLPixelFormatBGRA8Unorm_sRGB;
        case PrGpuFormat_Rgb10A2: return MTLPixelFormatRGB10A2Unorm;
        case PrGpuFormat_Rg11B10F: return MTLPixelFormatRG11B10Float;
        case PrGpuFormat_R16F: return MTLPixelFormatR16Float;
        case PrGpuFormat_Rg16F: return MTLPixelFormatRG16Float;
        case PrGpuFormat_Rgba16F: return MTLPixelFormatRGBA16Float;
        case PrGpuFormat_R32F: return MTLPixelFormatR32Float;
        case PrGpuFormat_Rg32F: return MTLPixelFormatRG32Float;
        case PrGpuFormat_Rgba32F: return MTLPixelFormatRGBA32Float;
        case PrGpuFormat_R8Uint: return MTLPixelFormatR8Uint;
        case PrGpuFormat_R16Uint: return MTLPixelFormatR16Uint;
        case PrGpuFormat_R32Uint: return MTLPixelFormatR32Uint;
        case PrGpuFormat_Rg32Uint: return MTLPixelFormatRG32Uint;
        case PrGpuFormat_Rgba32Uint: return MTLPixelFormatRGBA32Uint;
        case PrGpuFormat_D16: return MTLPixelFormatDepth16Unorm;
        case PrGpuFormat_D32F: return MTLPixelFormatDepth32Float;
        case PrGpuFormat_D32FS8: return MTLPixelFormatDepth32Float_Stencil8;
        case PrGpuFormat_Bc1: return MTLPixelFormatBC1_RGBA;
        case PrGpuFormat_Bc1Srgb: return MTLPixelFormatBC1_RGBA_sRGB;
        case PrGpuFormat_Bc3: return MTLPixelFormatBC3_RGBA;
        case PrGpuFormat_Bc3Srgb: return MTLPixelFormatBC3_RGBA_sRGB;
        case PrGpuFormat_Bc4: return MTLPixelFormatBC4_RUnorm;
        case PrGpuFormat_Bc5: return MTLPixelFormatBC5_RGUnorm;
        case PrGpuFormat_Bc6HUfloat: return MTLPixelFormatBC6H_RGBUfloat;
        case PrGpuFormat_Bc7: return MTLPixelFormatBC7_RGBAUnorm;
        case PrGpuFormat_Bc7Srgb: return MTLPixelFormatBC7_RGBAUnorm_sRGB;
        case PrGpuFormat_Astc4x4: return MTLPixelFormatASTC_4x4_LDR;
        case PrGpuFormat_Astc4x4Srgb: return MTLPixelFormatASTC_4x4_sRGB;
        default: return MTLPixelFormatInvalid;
    }
}

PrGpuFormat gpuFormatOf(MTLPixelFormat format) {
    for (uint8_t f = 1; f < PrGpuFormat_Count; ++f) {
        if (formatOf(PrGpuFormat(f)) == format)
            return PrGpuFormat(f);
    }
    return PrGpuFormat_None;
}

MTLTextureType textureTypeOf(PrGpuTextureKind kind, uint8_t samples) {
    switch (kind) {
        case PrGpuTextureKind_2dArray: return samples > 1 ? MTLTextureType2DMultisampleArray : MTLTextureType2DArray;
        case PrGpuTextureKind_Cube: return MTLTextureTypeCube;
        case PrGpuTextureKind_CubeArray: return MTLTextureTypeCubeArray;
        case PrGpuTextureKind_3d: return MTLTextureType3D;
        default: return samples > 1 ? MTLTextureType2DMultisample : MTLTextureType2D;
    }
}

MTLCompareFunction compareOf(PrGpuCompare compare) {
    switch (compare) {
        case PrGpuCompare_Never: return MTLCompareFunctionNever;
        case PrGpuCompare_Less: return MTLCompareFunctionLess;
        case PrGpuCompare_Equal: return MTLCompareFunctionEqual;
        case PrGpuCompare_LessEqual: return MTLCompareFunctionLessEqual;
        case PrGpuCompare_Greater: return MTLCompareFunctionGreater;
        case PrGpuCompare_NotEqual: return MTLCompareFunctionNotEqual;
        case PrGpuCompare_GreaterEqual: return MTLCompareFunctionGreaterEqual;
        default: return MTLCompareFunctionAlways;
    }
}

MTLBlendFactor blendFactorOf(PrGpuBlendFactor factor, bool alpha) {
    switch (factor) {
        case PrGpuBlendFactor_Zero: return MTLBlendFactorZero;
        case PrGpuBlendFactor_Source: return alpha ? MTLBlendFactorSourceAlpha : MTLBlendFactorSourceColor;
        case PrGpuBlendFactor_OneMinusSource:
            return alpha ? MTLBlendFactorOneMinusSourceAlpha : MTLBlendFactorOneMinusSourceColor;
        case PrGpuBlendFactor_SourceAlpha: return MTLBlendFactorSourceAlpha;
        case PrGpuBlendFactor_OneMinusSourceAlpha: return MTLBlendFactorOneMinusSourceAlpha;
        case PrGpuBlendFactor_Target: return alpha ? MTLBlendFactorDestinationAlpha : MTLBlendFactorDestinationColor;
        case PrGpuBlendFactor_OneMinusTarget:
            return alpha ? MTLBlendFactorOneMinusDestinationAlpha : MTLBlendFactorOneMinusDestinationColor;
        case PrGpuBlendFactor_TargetAlpha: return MTLBlendFactorDestinationAlpha;
        case PrGpuBlendFactor_OneMinusTargetAlpha: return MTLBlendFactorOneMinusDestinationAlpha;
        case PrGpuBlendFactor_SourceAlphaSaturated: return MTLBlendFactorSourceAlphaSaturated;
        case PrGpuBlendFactor_Constant: return alpha ? MTLBlendFactorBlendAlpha : MTLBlendFactorBlendColor;
        case PrGpuBlendFactor_OneMinusConstant:
            return alpha ? MTLBlendFactorOneMinusBlendAlpha : MTLBlendFactorOneMinusBlendColor;
        case PrGpuBlendFactor_Source1: return alpha ? MTLBlendFactorSource1Alpha : MTLBlendFactorSource1Color;
        case PrGpuBlendFactor_OneMinusSource1:
            return alpha ? MTLBlendFactorOneMinusSource1Alpha : MTLBlendFactorOneMinusSource1Color;
        default: return MTLBlendFactorOne;
    }
}

MTLBlendOperation blendOpOf(PrGpuBlendOp op) {
    switch (op) {
        case PrGpuBlendOp_Subtract: return MTLBlendOperationSubtract;
        case PrGpuBlendOp_ReverseSubtract: return MTLBlendOperationReverseSubtract;
        case PrGpuBlendOp_Min: return MTLBlendOperationMin;
        case PrGpuBlendOp_Max: return MTLBlendOperationMax;
        default: return MTLBlendOperationAdd;
    }
}

MTLStencilOperation stencilOpOf(PrGpuStencilOp op) {
    switch (op) {
        case PrGpuStencilOp_Zero: return MTLStencilOperationZero;
        case PrGpuStencilOp_Replace: return MTLStencilOperationReplace;
        case PrGpuStencilOp_IncrementClamp: return MTLStencilOperationIncrementClamp;
        case PrGpuStencilOp_DecrementClamp: return MTLStencilOperationDecrementClamp;
        case PrGpuStencilOp_Invert: return MTLStencilOperationInvert;
        case PrGpuStencilOp_IncrementWrap: return MTLStencilOperationIncrementWrap;
        case PrGpuStencilOp_DecrementWrap: return MTLStencilOperationDecrementWrap;
        default: return MTLStencilOperationKeep;
    }
}

MTLSamplerAddressMode addressOf(PrGpuAddress address) {
    switch (address) {
        case PrGpuAddress_Repeat: return MTLSamplerAddressModeRepeat;
        case PrGpuAddress_Mirror: return MTLSamplerAddressModeMirrorRepeat;
        case PrGpuAddress_Border: return MTLSamplerAddressModeClampToZero;
        default: return MTLSamplerAddressModeClampToEdge;
    }
}

MTLPrimitiveType primitiveOf(PrGpuTopology topology) {
    switch (topology) {
        case PrGpuTopology_TriangleStrip: return MTLPrimitiveTypeTriangleStrip;
        case PrGpuTopology_LineList: return MTLPrimitiveTypeLine;
        case PrGpuTopology_LineStrip: return MTLPrimitiveTypeLineStrip;
        case PrGpuTopology_PointList: return MTLPrimitiveTypePoint;
        default: return MTLPrimitiveTypeTriangle;
    }
}

MTLPrimitiveTopologyClass topologyClassOf(PrGpuTopology topology) {
    switch (topology) {
        case PrGpuTopology_LineList:
        case PrGpuTopology_LineStrip: return MTLPrimitiveTopologyClassLine;
        case PrGpuTopology_PointList: return MTLPrimitiveTopologyClassPoint;
        default: return MTLPrimitiveTopologyClassTriangle;
    }
}

// The stages of the work that makes accesses.
MTLStages stagesOf(PrGpuAccessFlags access) {
    MTLStages stages = 0;
    if (access & (PrGpuAccessFlag_IndexRead | PrGpuAccessFlag_IndirectRead | PrGpuAccessFlag_VertexRead
                  | PrGpuAccessFlag_VertexWrite))
        stages |= MTLStageVertex;
    if (access & (PrGpuAccessFlag_IndirectRead | PrGpuAccessFlag_ComputeRead | PrGpuAccessFlag_ComputeWrite))
        stages |= MTLStageDispatch;
    if (access & (PrGpuAccessFlag_FragmentRead | PrGpuAccessFlag_FragmentWrite | PrGpuAccessFlag_ColorAttachment
                  | PrGpuAccessFlag_DepthStencilRead | PrGpuAccessFlag_DepthStencilWrite))
        stages |= MTLStageFragment;
    if (access & (PrGpuAccessFlag_CopyRead | PrGpuAccessFlag_CopyWrite))
        stages |= MTLStageBlit;
    return stages;
}

// ---------------------------------------------------------------------------------------------------------------------
// Slots, blocks and retirement

void setHeapEntry(id<MTLBuffer> heap, uint32_t slot, uint64_t value) {
    static_cast<uint64_t*>(heap.contents)[slot] = value;
}

// Puts an object into a new resource slot, adds it to the residency set and writes its heap entry. Returns zero if no
// slot is left. The caller holds the mutex.
uint32_t addResourceLocked(MetalDevice* d, MetalSlot kind, id object, uint64_t entry) {
    uint32_t slot = 0;
    if (d->freeResources.count != 0) {
        slot = d->freeResources.last();
        d->freeResources.pop();
    } else if (d->nextResource < kMaxResources) {
        slot = d->nextResource++;
    }
    if (slot == 0)
        return 0;
    MetalResource& r = d->resources[slot];
    r = MetalResource{};
    r.kind = kind;
    r.object = [object retain];
    [d->residency addAllocation:object];
    d->residencyDirty = true;
    setHeapEntry(d->resourceHeap, slot, entry);
    return slot;
}

uint32_t addResource(MetalDevice* d, MetalSlot kind, id object, uint64_t entry) {
    prMutexLock(&d->mutex);
    uint32_t slot = addResourceLocked(d, kind, object, entry);
    prMutexUnlock(&d->mutex);
    return slot;
}

// Frees a resource slot and what it holds. The caller holds the mutex.
void freeResource(MetalDevice* d, uint32_t slot) {
    MetalResource& r = d->resources[slot];
    if (r.object != nil) {
        [d->residency removeAllocation:r.object];
        d->residencyDirty = true;
        [r.object release];
    }
    r = MetalResource{};
    setHeapEntry(d->resourceHeap, slot, 0);
    d->freeResources.push(slot);
}

void freePipeline(MetalDevice* d, uint32_t id) {
    MetalPipeline& p = d->pipelines[id];
    [p.state release];
    [p.depthStencil release];
    p = MetalPipeline{};
    d->freePipelines.push(id);
}

// Frees what the command buffers that are done held: retired objects, and the blocks, allocators and tables of their
// recording. The caller holds the mutex.
void collect(MetalDevice* d) {
    uint64_t done = d->done.load();
    uint32_t kept = 0;
    for (uint32_t i = 0; i < d->inFlight.count; ++i) {
        const MetalInFlight& f = d->inFlight.data[i];
        if (f.fence == 0 || f.fence > done)
            d->inFlight.data[kept++] = f;
    }
    d->inFlight.count = kept;
    uint64_t oldest = kept != 0 ? d->inFlight.data[0].serial : d->serial;
    kept = 0;
    for (uint32_t i = 0; i < d->retired.count; ++i) {
        const MetalRetired& r = d->retired.data[i];
        if (r.serial > oldest) {
            d->retired.data[kept++] = r;
        } else if (r.kind == MetalRetiredKind_Resource) {
            freeResource(d, r.slot);
        } else {
            freePipeline(d, r.slot);
        }
    }
    d->retired.count = kept;
    kept = 0;
    for (uint32_t i = 0; i < d->held.count; ++i) {
        MetalHeld& h = d->held.data[i];
        if (h.fence > done) {
            d->held.data[kept++] = h;
            continue;
        }
        switch (h.kind) {
            case MetalHeldKind_StreamBlock:
                h.block.used = 0;
                d->streamBlocks.push(h.block);
                break;
            case MetalHeldKind_StagingBlock:
                h.block.used = 0;
                d->stagingBlocks.push(h.block);
                break;
            case MetalHeldKind_Allocator:
                [h.object reset];
                d->allocators.push(h.object);
                break;
            default:
                d->tables.push(h.object);
                break;
        }
    }
    d->held.count = kept;
}

// Retires an object, which is freed once the command buffers that began before now are done.
void retire(MetalDevice* d, MetalRetiredKind kind, uint32_t slot) {
    prMutexLock(&d->mutex);
    d->retired.push(MetalRetired{d->serial, kind, slot});
    collect(d);
    prMutexUnlock(&d->mutex);
}

void commitResidency(MetalDevice* d) {
    if (d->residencyDirty) {
        [d->residency commit];
        d->residencyDirty = false;
    }
}

// Returns a block of shared memory of at least `size` bytes, from the free blocks or new, or one without a buffer if
// the device has no memory or slot for it. A block larger than `blockSize` is made for its size. The caller holds the
// mutex.
MetalBlock takeBlock(MetalDevice* d, Array<MetalBlock>& free, uint64_t blockSize, uint64_t size) {
    for (uint32_t i = 0; i < free.count; ++i) {
        if (free.data[i].buffer.length >= size) {
            MetalBlock block = free.data[i];
            free.erase(i);
            return block;
        }
    }
    uint64_t length = size > blockSize ? alignUp(size, kStreamAlignment) : blockSize;
    id<MTLBuffer> buffer = [d->device newBufferWithLength:length
                                                  options:MTLResourceStorageModeShared
                                                          | MTLResourceCPUCacheModeWriteCombined
                                                          | MTLResourceHazardTrackingModeUntracked];
    if (buffer == nil)
        return MetalBlock{};
    uint32_t slot = addResourceLocked(d, MetalSlot_Buffer, buffer, buffer.gpuAddress);
    [buffer release];
    if (slot == 0)
        return MetalBlock{};
    d->resources[slot].buffer = PrGpuBufferDesc{.size = length, .memory = PrGpuMemory_Upload};
    return MetalBlock{buffer, slot, 0};
}

id<MTLBuffer> bufferOf(const MetalDevice* d, uint32_t slot) {
    PR_ASSERT(slot < kMaxResources && d->resources[slot].kind == MetalSlot_Buffer);
    return d->resources[slot].object;
}

id<MTLTexture> textureOf(const MetalDevice* d, uint32_t slot) {
    PR_ASSERT(slot < kMaxResources && d->resources[slot].kind == MetalSlot_Texture);
    return d->resources[slot].object;
}

// Commits a command buffer, which reaches `fence` once it is done and the functions that wait for it have run. Takes
// the copied `dones`. The caller holds the mutex.
void commit(MetalDevice* d, id<MTL4CommandBuffer> buffer, PrGpuFence fence, MetalDone* PR_NULLABLE dones,
            uint32_t doneCount) {
    MTL4CommitOptions* options = [[MTL4CommitOptions new] autorelease];
    MetalDevice* owner = d;
    [options addFeedbackHandler:^(id<MTL4CommitFeedback> feedback) {
        if (feedback.error != nil) {
            NSLog(@"lib/gpu: Metal reported an error from a command buffer: %@", feedback.error);
            owner->lost.store(true);
        }
        double seconds = feedback.error == nil ? feedback.GPUEndTime - feedback.GPUStartTime : 0;
        for (uint32_t i = 0; i < doneCount; ++i)
            dones[i].done(dones[i].context, seconds);
        prMemoryFree(dones);
        prMutexLock(&owner->doneMutex);
        if (owner->done.load() < fence)
            owner->done.store(fence);
        prConditionBroadcast(&owner->doneSignal);
        prMutexUnlock(&owner->doneMutex);
    }];
    id<MTL4CommandBuffer> list[1] = {buffer};
    [d->queue commit:list count:1 options:options];
}

// ---------------------------------------------------------------------------------------------------------------------
// Uploads

// Opens the batch of uploads if it is not open. It waits for the work before it, which may read what it writes. The
// caller holds the mutex.
void openUploads(MetalDevice* d) {
    if (d->uploads != nil)
        return;
    if (d->allocators.count != 0) {
        d->uploadAllocator = d->allocators.last();
        d->allocators.pop();
    } else {
        d->uploadAllocator = [d->device newCommandAllocator];
    }
    d->uploads = [d->device newCommandBuffer];
    [d->uploads beginCommandBufferWithAllocator:d->uploadAllocator];
    d->uploads.label = @"uploads";
    d->uploadEncoder = [[d->uploads computeCommandEncoder] retain];
    [d->uploadEncoder barrierAfterQueueStages:kAllStages
                                 beforeStages:MTLStageBlit
                            visibilityOptions:MTL4VisibilityOptionDevice];
    d->uploadSerial = d->serial++;
    d->inFlight.push(MetalInFlight{d->uploadSerial, 0});
}

// Commits the open batch of uploads, ahead of whatever is submitted after it. Its copies into buffers are disjoint, so
// they go at once. The caller holds the mutex.
void submitUploads(MetalDevice* d) {
    if (d->uploads == nil)
        return;
    const Array<GpuSharedUploadCopy>& copies = d->uploadBatch.copies;
    for (uint32_t i = 0; i < copies.count; ++i) {
        const GpuSharedUploadCopy& c = copies.data[i];
        if (c.size != 0)
            [d->uploadEncoder copyFromBuffer:d->uploadBlocks.data[c.block].buffer
                                sourceOffset:c.source
                                    toBuffer:bufferOf(d, c.buffer)
                           destinationOffset:c.target
                                        size:c.size];
    }
    [d->uploadEncoder barrierAfterStages:MTLStageBlit
                       beforeQueueStages:kAllStages
                       visibilityOptions:MTL4VisibilityOptionDevice];
    [d->uploadEncoder endEncoding];
    [d->uploadEncoder release];
    d->uploadEncoder = nil;
    [d->uploads endCommandBuffer];
    commitResidency(d);
    PrGpuFence fence = ++d->submitted;
    commit(d, d->uploads, fence, nullptr, 0);
    for (uint32_t i = 0; i < d->inFlight.count; ++i) {
        if (d->inFlight.data[i].serial == d->uploadSerial)
            d->inFlight.data[i].fence = fence;
    }
    for (uint32_t i = 0; i < d->uploadBlocks.count; ++i)
        d->held.push(MetalHeld{fence, MetalHeldKind_StagingBlock, d->uploadBlocks.data[i], nil});
    d->held.push(MetalHeld{fence, MetalHeldKind_Allocator, {}, d->uploadAllocator});
    d->uploadBlocks.clear();
    gpuSharedUploadBatchClear(&d->uploadBatch);
    [d->uploads release];
    d->uploads = nil;
    d->uploadAllocator = nil;
}

// Returns room for `size` bytes in the batch's staging, as the block's index in the batch and the offset in it. Returns
// false if the device has no memory for it. The caller holds the mutex.
bool stage(MetalDevice* d, uint64_t size, uint32_t* block, uint64_t* offset) {
    MetalBlock* last = d->uploadBlocks.count != 0 ? &d->uploadBlocks.last() : nullptr;
    uint64_t at = last != nullptr ? alignUp(last->used, 256) : 0;
    if (last == nullptr || at + size > last->buffer.length) {
        if (d->uploadBlocks.count >= kStagingFlushBlocks)
            submitUploads(d);
        collect(d);
        MetalBlock added = takeBlock(d, d->stagingBlocks, kStagingBlock, size);
        if (added.buffer == nil)
            return false;
        d->uploadBlocks.push(added);
        last = &d->uploadBlocks.last();
        at = 0;
    }
    openUploads(d);
    last->used = at + size;
    *block = d->uploadBlocks.count - 1;
    *offset = at;
    return true;
}

void bufferUpload(PrGpuDevice* device, PrGpuBuffer buffer, uint64_t offset, const void* data, uint64_t size) {
    MetalDevice* d = metalOf(device);
    @autoreleasepool {
        prMutexLock(&d->mutex);
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        while (size != 0) {
            uint64_t n = size < kStagingBlock ? size : kStagingBlock;
            uint32_t block = 0;
            uint64_t at = 0;
            if (!stage(d, n, &block, &at))
                break;
            memCopy(static_cast<uint8_t*>(d->uploadBlocks.data[block].buffer.contents) + at, bytes, n);
            gpuSharedUploadAddCopy(&d->uploadBatch, GpuSharedUploadCopy{buffer.id, block, offset, at, n});
            bytes += n;
            offset += n;
            size -= n;
        }
        prMutexUnlock(&d->mutex);
    }
}

void textureUpload(PrGpuDevice* device, PrGpuTexture texture, const PrGpuTextureRegion* region, const void* data,
                   uint32_t rowBytes, uint32_t layerBytes) {
    MetalDevice* d = metalOf(device);
    @autoreleasepool {
        id<MTLTexture> t = textureOf(d, texture.id);
        const PrGpuTextureDesc& desc = d->resources[texture.id].texture;
        PrGpuFormatBlock b = prGpuFormatBlock(desc.format);
        uint32_t rows = (region->extent.height + b.height - 1) / b.height;
        uint32_t row = (region->extent.width + b.width - 1) / b.width * b.bytes;
        uint32_t slices = region->extent.depth;
        uint64_t sliceBytes = uint64_t(row) * rows;
        prMutexLock(&d->mutex);
        // Staging holds the texels with rows packed. An upload larger than a block is staged in a block of its size.
        uint32_t block = 0;
        uint64_t at = 0;
        if (!stage(d, sliceBytes * slices, &block, &at)) {
            prMutexUnlock(&d->mutex);
            return;
        }
        id<MTLBuffer> staging = d->uploadBlocks.data[block].buffer;
        uint8_t* to = static_cast<uint8_t*>(staging.contents) + at;
        const uint8_t* from = static_cast<const uint8_t*>(data);
        for (uint32_t s = 0; s < slices; ++s) {
            for (uint32_t y = 0; y < rows; ++y)
                memCopy(to + s * sliceBytes + uint64_t(y) * row,
                        from + uint64_t(s) * layerBytes + uint64_t(y) * rowBytes, row);
        }
        if (gpuSharedUploadNeedsTextureBarrier(&d->uploadBatch, texture.id))
            [d->uploadEncoder barrierAfterEncoderStages:MTLStageBlit
                                    beforeEncoderStages:MTLStageBlit
                                      visibilityOptions:MTL4VisibilityOptionDevice];
        bool is3d = desc.kind == PrGpuTextureKind_3d;
        uint32_t images = is3d ? 1 : slices;
        for (uint32_t i = 0; i < images; ++i) {
            [d->uploadEncoder copyFromBuffer:staging
                                sourceOffset:at + i * sliceBytes
                           sourceBytesPerRow:row
                         sourceBytesPerImage:sliceBytes
                                  sourceSize:MTLSizeMake(region->extent.width, region->extent.height,
                                                         is3d ? slices : 1)
                                   toTexture:t
                            destinationSlice:is3d ? 0 : region->firstLayer + i
                            destinationLevel:region->level
                           destinationOrigin:MTLOriginMake(region->x, region->y, is3d ? region->z : 0)];
        }
        prMutexUnlock(&d->mutex);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// The device

bool isLost(const PrGpuDevice* device) {
    return metalOf(device)->lost.load();
}

// Apple silicon's GPU is never removed, so the device only reports that it is lost and refuses new work.
void simulateLoss(PrGpuDevice* device) {
    metalOf(device)->lost.store(true);
}

bool isComplete(const PrGpuDevice* device, PrGpuFence fence) {
    const MetalDevice* d = metalOf(device);
    return d->lost.load() || d->done.load() >= fence;
}

void wait(PrGpuDevice* device, PrGpuFence fence) {
    MetalDevice* d = metalOf(device);
    prMutexLock(&d->doneMutex);
    while (d->done.load() < fence)
        prConditionWait(&d->doneSignal, &d->doneMutex);
    prMutexUnlock(&d->doneMutex);
    prMutexLock(&d->mutex);
    collect(d);
    prMutexUnlock(&d->mutex);
}

void waitIdle(PrGpuDevice* device) {
    MetalDevice* d = metalOf(device);
    prMutexLock(&d->mutex);
    submitUploads(d);
    PrGpuFence fence = d->submitted;
    prMutexUnlock(&d->mutex);
    wait(device, fence);
}

bool supportsFormat(const PrGpuDevice* device, PrGpuFormat format, PrGpuTextureUsageFlags usage) {
    const MetalDevice* d = metalOf(device);
    if (formatOf(format) == MTLPixelFormatInvalid)
        return false;
    bool bc = format >= PrGpuFormat_Bc1 && format <= PrGpuFormat_Bc7Srgb;
    bool compressed = format >= PrGpuFormat_Bc1;
    if (bc && !d->device.supportsBCTextureCompression)
        return false;
    if (compressed && (usage & ~PrGpuTextureUsageFlag_Sampled))
        return false;
    bool depth = prGpuFormatIsDepthStencil(format);
    if ((usage & PrGpuTextureUsageFlag_DepthStencil) && !depth)
        return false;
    if ((usage & (PrGpuTextureUsageFlag_RenderTarget | PrGpuTextureUsageFlag_Storage)) && depth)
        return false;
    return true;
}

void destroyDevice(PrGpuDevice* device) {
    MetalDevice* d = metalOf(device);
    @autoreleasepool {
        prMutexLock(&d->mutex);
        collect(d);
        for (uint32_t i = 0; i < d->held.count; ++i)
            [d->held.data[i].object release];
        for (uint32_t i = 1; i < d->nextResource; ++i)
            [d->resources[i].object release];
        for (uint32_t i = 1; i < d->nextSampler; ++i)
            [d->samplers[i] release];
        for (uint32_t i = 1; i < d->nextPipeline; ++i) {
            [d->pipelines[i].state release];
            [d->pipelines[i].depthStencil release];
        }
        for (uint32_t i = 0; i < d->allocators.count; ++i)
            [d->allocators.data[i] release];
        for (uint32_t i = 0; i < d->tables.count; ++i)
            [d->tables.data[i] release];
        prMutexUnlock(&d->mutex);
        [d->queue removeResidencySet:d->residency];
        prMemoryFree(d->resources);
        prMemoryFree(d->samplers);
        prMemoryFree(d->pipelines);
        d->freeResources.reset();
        d->freePipelines.reset();
        d->inFlight.reset();
        d->retired.reset();
        d->held.reset();
        d->allocators.reset();
        d->tables.reset();
        d->streamBlocks.reset();
        d->stagingBlocks.reset();
        d->uploadBlocks.reset();
        gpuSharedUploadBatchReset(&d->uploadBatch);
        [d->resourceHeap release];
        [d->samplerHeap release];
        [d->residency release];
        [d->compiler release];
        [d->queue release];
        dispatch_release(d->feedback);
        [d->device release];
        prMutexReset(&d->mutex);
        prConditionReset(&d->doneSignal);
        prMutexReset(&d->doneMutex);
    }
    prMemoryFree(d);
}

// ---------------------------------------------------------------------------------------------------------------------
// Buffers

PrStatus bufferCreate(PrGpuDevice* device, const PrGpuBufferDesc* desc, PrGpuBuffer* out) {
    MetalDevice* d = metalOf(device);
    @autoreleasepool {
        MTLResourceOptions options = MTLResourceHazardTrackingModeUntracked;
        options |= desc->memory == PrGpuMemory_Device ? MTLResourceStorageModePrivate : MTLResourceStorageModeShared;
        if (desc->memory == PrGpuMemory_Upload)
            options |= MTLResourceCPUCacheModeWriteCombined;
        // Metal zeroes a new buffer's memory.
        id<MTLBuffer> buffer = [d->device newBufferWithLength:alignUp(desc->size, 16) options:options];
        if (buffer == nil)
            return PrStatus_Limit;
        if (desc->name.size != 0)
            buffer.label = stringOf(desc->name);
        uint32_t slot = addResource(d, MetalSlot_Buffer, buffer, buffer.gpuAddress);
        [buffer release];
        if (slot == 0)
            return PrStatus_Limit;
        d->resources[slot].buffer = *desc;
        d->resources[slot].buffer.name = PrStringView{};
        out->id = slot;
        return PrStatus_Ok;
    }
}

void bufferDestroy(PrGpuDevice* device, PrGpuBuffer buffer) {
    retire(metalOf(device), MetalRetiredKind_Resource, buffer.id);
}

PrGpuBufferDesc bufferDesc(const PrGpuDevice* device, PrGpuBuffer buffer) {
    return metalOf(device)->resources[buffer.id].buffer;
}

// Metal reaches a buffer by its address, for reading and writing alike.
uint32_t bufferStorageHandle(const PrGpuDevice*, PrGpuBuffer buffer) {
    return buffer.id;
}

void* bufferMap(const PrGpuDevice* device, PrGpuBuffer buffer) {
    return bufferOf(metalOf(device), buffer.id).contents;
}

// ---------------------------------------------------------------------------------------------------------------------
// Textures

MTLTextureUsage textureUsageOf(PrGpuTextureUsageFlags usage) {
    // No texture asks for MTLTextureUsagePixelFormatView, which would keep the GPU from compressing it. A view keeps
    // its texture's pixel format or swaps sRGB for linear, and neither needs it.
    MTLTextureUsage u = 0;
    if (usage & PrGpuTextureUsageFlag_Sampled)
        u |= MTLTextureUsageShaderRead;
    if (usage & PrGpuTextureUsageFlag_Storage)
        u |= MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    if (usage & (PrGpuTextureUsageFlag_RenderTarget | PrGpuTextureUsageFlag_DepthStencil))
        u |= MTLTextureUsageRenderTarget;
    return u;
}

PrStatus textureCreate(PrGpuDevice* device, const PrGpuTextureDesc* desc, PrGpuTexture* out) {
    MetalDevice* d = metalOf(device);
    @autoreleasepool {
        MTLPixelFormat format = formatOf(desc->format);
        if (format == MTLPixelFormatInvalid)
            return PrStatus_NotSupported;
        MTLTextureDescriptor* td = [[MTLTextureDescriptor new] autorelease];
        td.textureType = textureTypeOf(desc->kind, desc->samples);
        td.pixelFormat = format;
        td.width = desc->extent.width;
        td.height = desc->extent.height;
        bool cube = desc->kind == PrGpuTextureKind_Cube || desc->kind == PrGpuTextureKind_CubeArray;
        if (desc->kind == PrGpuTextureKind_3d)
            td.depth = desc->extent.depth;
        else
            td.arrayLength = cube ? desc->extent.depth / 6 : desc->extent.depth;
        td.mipmapLevelCount = desc->levels;
        td.sampleCount = desc->samples;
        td.storageMode = MTLStorageModePrivate;
        td.hazardTrackingMode = MTLHazardTrackingModeUntracked;
        td.usage = textureUsageOf(desc->usage);
        id<MTLTexture> texture = [d->device newTextureWithDescriptor:td];
        if (texture == nil)
            return PrStatus_Limit;
        if (desc->name.size != 0)
            texture.label = stringOf(desc->name);
        uint32_t slot = addResource(d, MetalSlot_Texture, texture, texture.gpuResourceID._impl);
        [texture release];
        if (slot == 0)
            return PrStatus_Limit;
        d->resources[slot].texture = *desc;
        d->resources[slot].texture.name = PrStringView{};
        out->id = slot;
        return PrStatus_Ok;
    }
}

PrStatus textureCreateView(PrGpuDevice* device, PrGpuTexture texture, PrGpuTextureKind kind,
                           PrGpuSubresources subresources, PrGpuTexture* out) {
    MetalDevice* d = metalOf(device);
    @autoreleasepool {
        id<MTLTexture> t = textureOf(d, texture.id);
        PrGpuTextureDesc desc = d->resources[texture.id].texture;
        uint8_t levels =
            subresources.levels != 0 ? subresources.levels : uint8_t(desc.levels - subresources.firstLevel);
        uint32_t layerCount = desc.kind == PrGpuTextureKind_3d ? 1 : desc.extent.depth;
        uint16_t layers = subresources.layers != 0 ? subresources.layers
                                                   : uint16_t(layerCount - subresources.firstLayer);
        id<MTLTexture> view = [t newTextureViewWithPixelFormat:t.pixelFormat
                                                   textureType:textureTypeOf(kind, desc.samples)
                                                        levels:NSMakeRange(subresources.firstLevel, levels)
                                                        slices:NSMakeRange(subresources.firstLayer, layers)];
        if (view == nil)
            return PrStatus_Limit;
        uint32_t slot = addResource(d, MetalSlot_Texture, view, view.gpuResourceID._impl);
        [view release];
        if (slot == 0)
            return PrStatus_Limit;
        PrGpuTextureDesc& v = d->resources[slot].texture;
        v = desc;
        v.name = PrStringView{};
        v.kind = kind;
        v.levels = levels;
        uint32_t width = desc.extent.width >> subresources.firstLevel;
        uint32_t height = desc.extent.height >> subresources.firstLevel;
        v.extent.width = width != 0 ? width : 1;
        v.extent.height = height != 0 ? height : 1;
        v.extent.depth = desc.kind == PrGpuTextureKind_3d ? desc.extent.depth : layers;
        out->id = slot;
        return PrStatus_Ok;
    }
}

void textureDestroy(PrGpuDevice* device, PrGpuTexture texture) {
    retire(metalOf(device), MetalRetiredKind_Resource, texture.id);
}

PrGpuTextureDesc textureDesc(const PrGpuDevice* device, PrGpuTexture texture) {
    return metalOf(device)->resources[texture.id].texture;
}

// Metal reaches a texture by its resource id, for sampling and for reading and writing alike.
uint32_t textureStorageHandle(const PrGpuDevice*, PrGpuTexture texture) {
    return texture.id;
}

// ---------------------------------------------------------------------------------------------------------------------
// Samplers

PrStatus samplerCreate(PrGpuDevice* device, const PrGpuSamplerDesc* desc, PrGpuSampler* out) {
    MetalDevice* d = metalOf(device);
    @autoreleasepool {
        MTLSamplerDescriptor* sd = [[MTLSamplerDescriptor new] autorelease];
        sd.magFilter = desc->magnify == PrGpuFilter_Linear ? MTLSamplerMinMagFilterLinear
                                                           : MTLSamplerMinMagFilterNearest;
        sd.minFilter = desc->minify == PrGpuFilter_Linear ? MTLSamplerMinMagFilterLinear
                                                          : MTLSamplerMinMagFilterNearest;
        sd.mipFilter = desc->mip == PrGpuFilter_Linear ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest;
        sd.sAddressMode = addressOf(desc->address[0]);
        sd.tAddressMode = addressOf(desc->address[1]);
        sd.rAddressMode = addressOf(desc->address[2]);
        sd.borderColor = MTLSamplerBorderColorTransparentBlack;
        sd.maxAnisotropy = desc->maxAnisotropy > 1 ? desc->maxAnisotropy : 1;
        sd.lodMinClamp = desc->minLod;
        sd.lodMaxClamp = desc->maxLod;
        sd.lodBias = desc->lodBias;
        sd.compareFunction = compareOf(desc->compare);
        sd.supportArgumentBuffers = YES;
        id<MTLSamplerState> sampler = [d->device newSamplerStateWithDescriptor:sd];
        if (sampler == nil)
            return PrStatus_Limit;
        prMutexLock(&d->mutex);
        uint32_t slot = d->nextSampler < kMaxSamplers ? d->nextSampler++ : 0;
        if (slot != 0) {
            d->samplers[slot] = sampler;
            setHeapEntry(d->samplerHeap, slot, sampler.gpuResourceID._impl);
        }
        prMutexUnlock(&d->mutex);
        if (slot == 0) {
            [sampler release];
            return PrStatus_Limit;
        }
        out->id = slot;
        return PrStatus_Ok;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Shader libraries and pipelines

PrStatus shaderLibraryCreate(PrGpuDevice* device, PrGpuShaderEntry* entries, uint32_t entryCount,
                             PrGpuShaderLibrary* PR_NULLABLE* PR_NONNULL out) {
    MetalDevice* d = metalOf(device);
    @autoreleasepool {
        if (entryCount == 0)
            return PrStatus_InvalidData;
        // Every entry point shares the one metallib, which the library's bytes hold until the library is destroyed.
        dispatch_data_t data = dispatch_data_create(entries[0].code, entries[0].codeSize, nullptr,
                                                    ^{});
        NSError* error = nil;
        id<MTLLibrary> library = [d->device newLibraryWithData:data error:&error];
        dispatch_release(data);
        if (library == nil) {
            NSLog(@"lib/gpu: Metal took no shader library: %@", error);
            return PrStatus_InvalidData;
        }
        MetalLibrary* m = allocZeroed<MetalLibrary>();
        m->library = library;
        *out = m;
        return PrStatus_Ok;
    }
}

void shaderLibraryDestroy(PrGpuDevice*, PrGpuShaderLibrary* library) {
    MetalLibrary* m = static_cast<MetalLibrary*>(library);
    [m->library release];
    prMemoryFree(m);
}

MTL4LibraryFunctionDescriptor* functionOf(const PrGpuShaderLibrary* library, const PrGpuShaderEntry* entry) {
    MTL4LibraryFunctionDescriptor* f = [[MTL4LibraryFunctionDescriptor new] autorelease];
    f.name = stringOf(entry->name);
    f.library = static_cast<const MetalLibrary*>(library)->library;
    return f;
}

// Puts a pipeline into a new id, or returns zero if none is left.
uint32_t addPipeline(MetalDevice* d, const MetalPipeline& pipeline) {
    prMutexLock(&d->mutex);
    uint32_t id = 0;
    if (d->freePipelines.count != 0) {
        id = d->freePipelines.last();
        d->freePipelines.pop();
    } else if (d->nextPipeline < kMaxPipelines) {
        id = d->nextPipeline++;
    }
    if (id != 0)
        d->pipelines[id] = pipeline;
    prMutexUnlock(&d->mutex);
    return id;
}

PrStatus graphicsPipelineCreate(PrGpuDevice* device, const PrGpuGraphicsPipelineDesc* desc,
                                const PrGpuShaderEntry* vertex, const PrGpuShaderEntry* PR_NULLABLE fragment,
                                PrGpuPipeline* out) {
    MetalDevice* d = metalOf(device);
    @autoreleasepool {
        MTL4RenderPipelineDescriptor* pd = [[MTL4RenderPipelineDescriptor new] autorelease];
        pd.vertexFunctionDescriptor = functionOf(desc->library, vertex);
        if (fragment != nullptr)
            pd.fragmentFunctionDescriptor = functionOf(desc->library, fragment);
        pd.rasterSampleCount = desc->formats.samples;
        pd.inputPrimitiveTopology = topologyClassOf(desc->topology);
        if (desc->name.size != 0)
            pd.label = stringOf(desc->name);
        for (uint8_t i = 0; i < desc->formats.colorCount; ++i) {
            MTL4RenderPipelineColorAttachmentDescriptor* a = pd.colorAttachments[i];
            const PrGpuBlend& blend = desc->blend[i];
            a.pixelFormat = formatOf(desc->formats.color[i]);
            a.blendingState = blend.enabled ? MTL4BlendStateEnabled : MTL4BlendStateDisabled;
            a.sourceRGBBlendFactor = blendFactorOf(blend.colorSource, false);
            a.destinationRGBBlendFactor = blendFactorOf(blend.colorTarget, false);
            a.rgbBlendOperation = blendOpOf(blend.colorOp);
            a.sourceAlphaBlendFactor = blendFactorOf(blend.alphaSource, true);
            a.destinationAlphaBlendFactor = blendFactorOf(blend.alphaTarget, true);
            a.alphaBlendOperation = blendOpOf(blend.alphaOp);
            PrGpuColorWriteFlags write = blend.write != 0 ? blend.write : PrGpuColorWriteFlag_All;
            a.writeMask = ((write & PrGpuColorWriteFlag_Red) ? MTLColorWriteMaskRed : 0)
                          | ((write & PrGpuColorWriteFlag_Green) ? MTLColorWriteMaskGreen : 0)
                          | ((write & PrGpuColorWriteFlag_Blue) ? MTLColorWriteMaskBlue : 0)
                          | ((write & PrGpuColorWriteFlag_Alpha) ? MTLColorWriteMaskAlpha : 0);
        }
        NSError* error = nil;
        id<MTLRenderPipelineState> state = [d->compiler newRenderPipelineStateWithDescriptor:pd
                                                                         compilerTaskOptions:nil
                                                                                       error:&error];
        if (state == nil) {
            NSLog(@"lib/gpu: Metal made no pipeline for %@: %@", pd.vertexFunctionDescriptor, error);
            return PrStatus_NotSupported;
        }
        MTLDepthStencilDescriptor* dd = [[MTLDepthStencilDescriptor new] autorelease];
        dd.depthCompareFunction = desc->depthTest ? compareOf(desc->depthCompare) : MTLCompareFunctionAlways;
        dd.depthWriteEnabled = desc->depthWrite;
        if (desc->stencilTest) {
            MTLStencilDescriptor* faces[2] = {[[MTLStencilDescriptor new] autorelease],
                                              [[MTLStencilDescriptor new] autorelease]};
            const PrGpuStencilFace* from[2] = {&desc->stencilFront, &desc->stencilBack};
            for (int f = 0; f < 2; ++f) {
                faces[f].stencilFailureOperation = stencilOpOf(from[f]->fail);
                faces[f].depthFailureOperation = stencilOpOf(from[f]->depthFail);
                faces[f].depthStencilPassOperation = stencilOpOf(from[f]->pass);
                faces[f].stencilCompareFunction = compareOf(from[f]->compare);
                faces[f].readMask = desc->stencilReadMask;
                faces[f].writeMask = desc->stencilWriteMask;
            }
            dd.frontFaceStencil = faces[0];
            dd.backFaceStencil = faces[1];
        }
        MetalPipeline p{};
        p.state = state;
        p.depthStencil = [d->device newDepthStencilStateWithDescriptor:dd];
        p.primitive = primitiveOf(desc->topology);
        p.cull = desc->cull == PrGpuCull_Front ? MTLCullModeFront
                 : desc->cull == PrGpuCull_Back ? MTLCullModeBack
                                                : MTLCullModeNone;
        p.winding = desc->frontClockwise ? MTLWindingClockwise : MTLWindingCounterClockwise;
        p.depthBias = desc->depthBias;
        p.depthBiasSlope = desc->depthBiasSlope;
        p.depthBiasClamp = desc->depthBiasClamp;
        uint32_t id = addPipeline(d, p);
        if (id == 0) {
            [p.state release];
            [p.depthStencil release];
            return PrStatus_Limit;
        }
        out->id = id;
        return PrStatus_Ok;
    }
}

PrStatus computePipelineCreate(PrGpuDevice* device, const PrGpuShaderLibrary* library, const PrGpuShaderEntry* compute,
                               PrStringView name, PrGpuPipeline* out) {
    MetalDevice* d = metalOf(device);
    @autoreleasepool {
        MTL4ComputePipelineDescriptor* pd = [[MTL4ComputePipelineDescriptor new] autorelease];
        pd.computeFunctionDescriptor = functionOf(library, compute);
        if (name.size != 0)
            pd.label = stringOf(name);
        NSError* error = nil;
        id<MTLComputePipelineState> state = [d->compiler newComputePipelineStateWithDescriptor:pd
                                                                           compilerTaskOptions:nil
                                                                                         error:&error];
        if (state == nil) {
            NSLog(@"lib/gpu: Metal made no compute pipeline: %@", error);
            return PrStatus_NotSupported;
        }
        MetalPipeline p{};
        p.compute = true;
        p.state = state;
        p.groupSize = MTLSizeMake(compute->groupSize.width, compute->groupSize.height, compute->groupSize.depth);
        uint32_t id = addPipeline(d, p);
        if (id == 0) {
            [state release];
            return PrStatus_Limit;
        }
        out->id = id;
        return PrStatus_Ok;
    }
}

void pipelineDestroy(PrGpuDevice* device, PrGpuPipeline pipeline) {
    retire(metalOf(device), MetalRetiredKind_Pipeline, pipeline.id);
}

PrGpuExtent pipelineGroupSize(const PrGpuDevice* device, PrGpuPipeline pipeline) {
    MTLSize size = metalOf(device)->pipelines[pipeline.id].groupSize;
    return PrGpuExtent{uint32_t(size.width), uint32_t(size.height), uint32_t(size.depth)};
}

// ---------------------------------------------------------------------------------------------------------------------
// Command buffers

MetalCommands* commandsOf(PrGpuCommands* commands) {
    return static_cast<MetalCommands*>(commands);
}

// Marks the state of the host's pass that the device changed. The state of the device's own encoders ends with them.
void changed(MetalPass* pass, PrGpuHostStateFlags flags) {
    if (pass->host)
        commandsOf(pass->commands)->changed |= flags;
}

// Ends the last encoder, which stays open for the barriers after it until another encoder begins.
void endTail(MetalCommands* c) {
    if (c->compute != nil) {
        [c->compute endEncoding];
        [c->compute release];
        c->compute = nil;
    }
    if (c->tail != nil) {
        [c->tail endEncoding];
        [c->tail release];
        c->tail = nil;
    }
}

// Returns the encoder of dispatches and copies, begun after the last barrier.
id<MTL4ComputeCommandEncoder> computeEncoder(MetalCommands* c) {
    if (c->compute == nil) {
        endTail(c);
        c->compute = [[c->buffer computeCommandEncoder] retain];
        [c->compute setArgumentTable:c->table];
    }
    return c->compute;
}

bool commandsBegin(PrGpuDevice* device, const PrGpuCommandsOptions* options,
                   PrGpuCommands* PR_NULLABLE* PR_NONNULL out) {
    MetalDevice* d = metalOf(device);
    @autoreleasepool {
        MetalCommands* c = allocZeroed<MetalCommands>();
        c->device = device;
        prMutexLock(&d->mutex);
        collect(d);
        c->serial = d->serial++;
        d->inFlight.push(MetalInFlight{c->serial, 0});
        if (d->tables.count != 0) {
            c->table = d->tables.last();
            d->tables.pop();
        }
        if (options->native == nullptr && d->allocators.count != 0) {
            c->allocator = d->allocators.last();
            d->allocators.pop();
        }
        prMutexUnlock(&d->mutex);
        if (c->table == nil) {
            MTL4ArgumentTableDescriptor* td = [[MTL4ArgumentTableDescriptor new] autorelease];
            td.maxBufferBindCount = MetalBinding_Count;
            c->table = [d->device newArgumentTableWithDescriptor:td error:nil];
        }
        [c->table setAddress:d->resourceHeap.gpuAddress atIndex:MetalBinding_Resources];
        [c->table setAddress:d->samplerHeap.gpuAddress atIndex:MetalBinding_Samplers];
        if (options->native != nullptr) {
            c->host = true;
            c->buffer = [static_cast<id<MTL4CommandBuffer>>(options->native) retain];
        } else {
            if (c->allocator == nil)
                c->allocator = [d->device newCommandAllocator];
            c->buffer = [d->device newCommandBuffer];
            [c->buffer beginCommandBufferWithAllocator:c->allocator];
        }
        *out = c;
        return true;
    }
}

// Allocates `size` bytes of the stream, at kStreamAlignment, in a block that the command buffer holds.
void* stream(MetalDevice* d, MetalCommands* c, uint64_t size, uint32_t* slot, uint64_t* offset) {
    MetalBlock* last = c->blocks.count != 0 ? &c->blocks.last() : nullptr;
    uint64_t at = last != nullptr ? alignUp(last->used, kStreamAlignment) : 0;
    if (last == nullptr || at + size > last->buffer.length) {
        prMutexLock(&d->mutex);
        MetalBlock block = takeBlock(d, d->streamBlocks, kStreamBlock, size);
        prMutexUnlock(&d->mutex);
        if (block.buffer == nil) {
            // Out of memory loses the device, and the caller writes into memory that the GPU never reads.
            d->lost.store(true);
            void* scratch = prMemoryAlloc(size);
            c->scratch.push(scratch);
            *slot = 0;
            *offset = 0;
            return scratch;
        }
        c->blocks.push(block);
        last = &c->blocks.last();
        at = 0;
    }
    last->used = at + size;
    *slot = last->slot;
    *offset = at;
    return static_cast<uint8_t*>(last->buffer.contents) + at;
}

void* commandsStream(PrGpuDevice* device, PrGpuCommands* commands, uint64_t size, PrGpuBuffer* buffer,
                     uint64_t* offset) {
    uint32_t slot = 0;
    void* bytes = stream(metalOf(device), commandsOf(commands), size, &slot, offset);
    buffer->id = slot;
    return bytes;
}

// Copies root data into the stream, and points the argument table's binding at it.
void setRoot(PrGpuDevice* device, PrGpuCommands* commands, const void* data, uint32_t size, uint32_t binding) {
    MetalDevice* d = metalOf(device);
    MetalCommands* c = commandsOf(commands);
    uint32_t slot = 0;
    uint64_t offset = 0;
    void* bytes = stream(d, c, size != 0 ? size : 16, &slot, &offset);
    memCopy(bytes, data, size);
    if (slot != 0)
        [c->table setAddress:static_cast<id<MTLBuffer>>(d->resources[slot].object).gpuAddress + offset
                     atIndex:binding];
}

void commandsSetPush(PrGpuDevice* device, PrGpuCommands* commands, const void* data, uint32_t size) {
    setRoot(device, commands, data, size, MetalBinding_Push);
}

void commandsSetUniforms(PrGpuDevice* device, PrGpuCommands* commands, const void* data, uint32_t size) {
    setRoot(device, commands, data, size, MetalBinding_Uniforms);
}

// Sets a graphics pipeline's state on a render encoder.
void setRenderPipeline(const MetalPipeline& p, id<MTL4RenderCommandEncoder> encoder) {
    [encoder setRenderPipelineState:p.state];
    [encoder setDepthStencilState:p.depthStencil];
    [encoder setCullMode:p.cull];
    [encoder setFrontFacingWinding:p.winding];
    [encoder setDepthBias:p.depthBias slopeScale:p.depthBiasSlope clamp:p.depthBiasClamp];
}

void commandsSetPipeline(PrGpuDevice* device, PrGpuCommands* commands, PrGpuPipeline pipeline) {
    MetalCommands* c = commandsOf(commands);
    c->pipeline = pipeline.id;
    const MetalPipeline& p = metalOf(device)->pipelines[pipeline.id];
    if (MetalPass* pass = c->pass) {
        PR_ASSERT(!p.compute);
        setRenderPipeline(p, pass->encoder);
        changed(pass, PrGpuHostStateFlag_Pipeline);
    }
}

void commandsDispatch(PrGpuDevice* device, PrGpuCommands* commands, PrGpuExtent groups) {
    MetalCommands* c = commandsOf(commands);
    const MetalPipeline& p = metalOf(device)->pipelines[c->pipeline];
    PR_ASSERT(p.compute);
    id<MTL4ComputeCommandEncoder> e = computeEncoder(c);
    [e setComputePipelineState:p.state];
    [e dispatchThreadgroups:MTLSizeMake(groups.width, groups.height, groups.depth) threadsPerThreadgroup:p.groupSize];
}

void commandsDispatchIndirect(PrGpuDevice* device, PrGpuCommands* commands, PrGpuBuffer buffer, uint64_t offset) {
    MetalDevice* d = metalOf(device);
    MetalCommands* c = commandsOf(commands);
    const MetalPipeline& p = d->pipelines[c->pipeline];
    id<MTL4ComputeCommandEncoder> e = computeEncoder(c);
    [e setComputePipelineState:p.state];
    [e dispatchThreadgroupsWithIndirectBuffer:bufferOf(d, buffer.id).gpuAddress + offset
                        threadsPerThreadgroup:p.groupSize];
}

void commandsCopyBuffer(PrGpuDevice* device, PrGpuCommands* commands, PrGpuBuffer source, uint64_t sourceOffset,
                        PrGpuBuffer target, uint64_t targetOffset, uint64_t size) {
    MetalDevice* d = metalOf(device);
    [computeEncoder(commandsOf(commands)) copyFromBuffer:bufferOf(d, source.id)
                                            sourceOffset:sourceOffset
                                                toBuffer:bufferOf(d, target.id)
                                       destinationOffset:targetOffset
                                                    size:size];
}

void commandsCopyTexture(PrGpuDevice* device, PrGpuCommands* commands, PrGpuTexture source,
                         const PrGpuTextureRegion* sourceRegion, PrGpuTexture target,
                         const PrGpuTextureRegion* targetRegion) {
    MetalDevice* d = metalOf(device);
    id<MTL4ComputeCommandEncoder> e = computeEncoder(commandsOf(commands));
    bool is3d = d->resources[source.id].texture.kind == PrGpuTextureKind_3d;
    uint32_t images = is3d ? 1 : sourceRegion->extent.depth;
    for (uint32_t i = 0; i < images; ++i) {
        [e copyFromTexture:textureOf(d, source.id)
                    sourceSlice:is3d ? 0 : sourceRegion->firstLayer + i
                    sourceLevel:sourceRegion->level
                   sourceOrigin:MTLOriginMake(sourceRegion->x, sourceRegion->y, is3d ? sourceRegion->z : 0)
                     sourceSize:MTLSizeMake(sourceRegion->extent.width, sourceRegion->extent.height,
                                            is3d ? sourceRegion->extent.depth : 1)
                      toTexture:textureOf(d, target.id)
               destinationSlice:is3d ? 0 : targetRegion->firstLayer + i
               destinationLevel:targetRegion->level
              destinationOrigin:MTLOriginMake(targetRegion->x, targetRegion->y, is3d ? targetRegion->z : 0)];
    }
}

void commandsCopyBufferToTexture(PrGpuDevice* device, PrGpuCommands* commands, PrGpuBuffer source,
                                 uint64_t sourceOffset, uint32_t rowBytes, uint32_t layerBytes, PrGpuTexture target,
                                 const PrGpuTextureRegion* targetRegion) {
    MetalDevice* d = metalOf(device);
    id<MTL4ComputeCommandEncoder> e = computeEncoder(commandsOf(commands));
    bool is3d = d->resources[target.id].texture.kind == PrGpuTextureKind_3d;
    uint32_t images = is3d ? 1 : targetRegion->extent.depth;
    uint64_t imageBytes = layerBytes != 0 ? layerBytes : uint64_t(rowBytes) * targetRegion->extent.height;
    for (uint32_t i = 0; i < images; ++i) {
        [e copyFromBuffer:bufferOf(d, source.id)
                   sourceOffset:sourceOffset + i * imageBytes
              sourceBytesPerRow:rowBytes
            sourceBytesPerImage:imageBytes
                     sourceSize:MTLSizeMake(targetRegion->extent.width, targetRegion->extent.height,
                                            is3d ? targetRegion->extent.depth : 1)
                      toTexture:textureOf(d, target.id)
               destinationSlice:is3d ? 0 : targetRegion->firstLayer + i
               destinationLevel:targetRegion->level
              destinationOrigin:MTLOriginMake(targetRegion->x, targetRegion->y, is3d ? targetRegion->z : 0)];
    }
}

void commandsCopyTextureToBuffer(PrGpuDevice* device, PrGpuCommands* commands, PrGpuTexture source,
                                 const PrGpuTextureRegion* sourceRegion, PrGpuBuffer target, uint64_t targetOffset,
                                 uint32_t rowBytes, uint32_t layerBytes) {
    MetalDevice* d = metalOf(device);
    id<MTL4ComputeCommandEncoder> e = computeEncoder(commandsOf(commands));
    bool is3d = d->resources[source.id].texture.kind == PrGpuTextureKind_3d;
    uint32_t images = is3d ? 1 : sourceRegion->extent.depth;
    uint64_t imageBytes = layerBytes != 0 ? layerBytes : uint64_t(rowBytes) * sourceRegion->extent.height;
    for (uint32_t i = 0; i < images; ++i) {
        [e copyFromTexture:textureOf(d, source.id)
                         sourceSlice:is3d ? 0 : sourceRegion->firstLayer + i
                         sourceLevel:sourceRegion->level
                        sourceOrigin:MTLOriginMake(sourceRegion->x, sourceRegion->y, is3d ? sourceRegion->z : 0)
                          sourceSize:MTLSizeMake(sourceRegion->extent.width, sourceRegion->extent.height,
                                                 is3d ? sourceRegion->extent.depth : 1)
                            toBuffer:bufferOf(d, target.id)
                   destinationOffset:targetOffset + i * imageBytes
              destinationBytesPerRow:rowBytes
            destinationBytesPerImage:imageBytes];
    }
}

void commandsOnDone(PrGpuDevice*, PrGpuCommands* commands, PrGpuDoneFn done, void* PR_NULLABLE context) {
    commandsOf(commands)->dones.push(MetalDone{done, context});
}

void commandsPushGroup(PrGpuDevice*, PrGpuCommands* commands, PrStringView name) {
    @autoreleasepool {
        [commandsOf(commands)->buffer pushDebugGroup:stringOf(name)];
    }
}

void commandsPopGroup(PrGpuDevice*, PrGpuCommands* commands) {
    [commandsOf(commands)->buffer popDebugGroup];
}

// Holds the stages of every later encoder that `after` names until the stages that `before` names of every earlier
// encoder are done.
void barrier(MetalCommands* c, PrGpuAccessFlags before, PrGpuAccessFlags after) {
    MTLStages wait = stagesOf(before);
    MTLStages hold = stagesOf(after);
    if (wait == 0 || hold == 0)
        return;
    if (c->compute != nil) {
        c->tail = c->compute;
        c->compute = nil;
    }
    if (c->tail == nil)
        c->tail = [[c->buffer computeCommandEncoder] retain];
    [c->tail barrierAfterStages:wait beforeQueueStages:hold visibilityOptions:MTL4VisibilityOptionDevice];
}

void commandsBufferBarrier(PrGpuDevice*, PrGpuCommands* commands, PrGpuBuffer, PrGpuAccessFlags before,
                           PrGpuAccessFlags after) {
    barrier(commandsOf(commands), before, after);
}

void commandsTextureBarrier(PrGpuDevice*, PrGpuCommands* commands, PrGpuTexture, PrGpuSubresources,
                            PrGpuAccessFlags before, PrGpuAccessFlags after) {
    barrier(commandsOf(commands), before, after);
}

void commandsGlobalBarrier(PrGpuDevice*, PrGpuCommands* commands, PrGpuAccessFlags before, PrGpuAccessFlags after) {
    barrier(commandsOf(commands), before, after);
}

void commandsGenerateMipmaps(PrGpuDevice* device, PrGpuCommands* commands, PrGpuTexture texture,
                             PrGpuAccessFlags before) {
    MetalCommands* c = commandsOf(commands);
    barrier(c, before, PrGpuAccessFlag_CopyRead | PrGpuAccessFlag_CopyWrite);
    [computeEncoder(c) generateMipmapsForTexture:textureOf(metalOf(device), texture.id)];
    barrier(c, PrGpuAccessFlag_CopyRead | PrGpuAccessFlag_CopyWrite, PrGpuAccessFlag_ShaderRead);
}

PrGpuHostStateFlags hostStateSync(PrGpuDevice*, PrGpuCommands* commands) {
    MetalCommands* c = commandsOf(commands);
    @autoreleasepool {
        if (c->pass == nullptr)
            endTail(c);
    }
    PrGpuHostStateFlags changed = c->changed;
    c->changed = 0;
    return changed;
}

PrGpuFence commandsSubmit(PrGpuDevice* device, PrGpuCommands* commands) {
    MetalDevice* d = metalOf(device);
    MetalCommands* c = commandsOf(commands);
    @autoreleasepool {
        endTail(c);
        if (!c->host)
            [c->buffer endCommandBuffer];
        // The functions to call once the work is done, with its time.
        uint32_t doneCount = c->dones.count;
        MetalDone* dones = nullptr;
        if (doneCount != 0) {
            dones = static_cast<MetalDone*>(prMemoryAlloc(sizeof(MetalDone) * doneCount));
            memCopy(dones, c->dones.data, sizeof(MetalDone) * doneCount);
        }
        prMutexLock(&d->mutex);
        submitUploads(d);
        commitResidency(d);
        for (uint32_t i = 0; i < c->drawables.count; ++i)
            [d->queue waitForDrawable:c->drawables.data[i]];
        PrGpuFence fence = ++d->submitted;
        commit(d, c->buffer, fence, dones, doneCount);
        for (uint32_t i = 0; i < c->drawables.count; ++i) {
            [d->queue signalDrawable:c->drawables.data[i]];
            [c->drawables.data[i] present];
            [c->drawables.data[i] release];
        }
        for (uint32_t i = 0; i < d->inFlight.count; ++i) {
            if (d->inFlight.data[i].serial == c->serial)
                d->inFlight.data[i].fence = fence;
        }
        for (uint32_t i = 0; i < c->blocks.count; ++i)
            d->held.push(MetalHeld{fence, MetalHeldKind_StreamBlock, c->blocks.data[i], nil});
        if (c->allocator != nil)
            d->held.push(MetalHeld{fence, MetalHeldKind_Allocator, {}, c->allocator});
        d->held.push(MetalHeld{fence, MetalHeldKind_Table, {}, c->table});
        prMutexUnlock(&d->mutex);
        [c->buffer release];
        for (uint32_t i = 0; i < c->scratch.count; ++i)
            prMemoryFree(c->scratch.data[i]);
        c->scratch.reset();
        c->blocks.reset();
        c->drawables.reset();
        c->dones.reset();
        prMemoryFree(c);
        return fence;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Passes

PrGpuPass* passBegin(PrGpuDevice* device, PrGpuCommands* commands, const PrGpuPassDesc* desc) {
    MetalDevice* d = metalOf(device);
    MetalCommands* c = commandsOf(commands);
    @autoreleasepool {
        MetalPass* pass = allocZeroed<MetalPass>();
        pass->commands = commands;
        if (desc->host != nullptr) {
            pass->host = true;
            pass->formats = desc->hostFormats;
            pass->extent = desc->hostExtent;
            pass->encoder = [static_cast<id<MTL4RenderCommandEncoder>>(desc->host) retain];
            c->changed |= PrGpuHostStateFlag_Bindings;
        } else {
            MTL4RenderPassDescriptor* rp = [[MTL4RenderPassDescriptor new] autorelease];
            pass->formats.samples = 1;
            for (uint8_t i = 0; i < desc->colorCount; ++i) {
                const PrGpuAttachment& a = desc->color[i];
                MTLRenderPassColorAttachmentDescriptor* ca = rp.colorAttachments[i];
                ca.texture = textureOf(d, a.texture.id);
                ca.level = a.level;
                ca.slice = a.layer;
                ca.loadAction = a.load == PrGpuLoad_Clear  ? MTLLoadActionClear
                                : a.load == PrGpuLoad_Keep ? MTLLoadActionLoad
                                                           : MTLLoadActionDontCare;
                const PrColor& clear = desc->clearColor[i];
                ca.clearColor = MTLClearColorMake(double(clear.r), double(clear.g), double(clear.b), double(clear.a));
                if (a.store == PrGpuStore_Resolve) {
                    ca.resolveTexture = textureOf(d, a.resolve.id);
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
            if (desc->depthStencil.texture.id != 0) {
                const PrGpuAttachment& a = desc->depthStencil;
                const PrGpuTextureDesc& t = d->resources[a.texture.id].texture;
                MTLRenderPassDepthAttachmentDescriptor* da = rp.depthAttachment;
                da.texture = textureOf(d, a.texture.id);
                da.level = a.level;
                da.slice = a.layer;
                da.loadAction = a.load == PrGpuLoad_Clear  ? MTLLoadActionClear
                                : a.load == PrGpuLoad_Keep ? MTLLoadActionLoad
                                                           : MTLLoadActionDontCare;
                da.storeAction = a.store == PrGpuStore_Keep ? MTLStoreActionStore : MTLStoreActionDontCare;
                da.clearDepth = double(desc->clearDepth);
                if (t.format == PrGpuFormat_D32FS8) {
                    MTLRenderPassStencilAttachmentDescriptor* sa = rp.stencilAttachment;
                    sa.texture = da.texture;
                    sa.level = a.level;
                    sa.slice = a.layer;
                    sa.loadAction = desc->stencilLoad == PrGpuLoad_Clear  ? MTLLoadActionClear
                                    : desc->stencilLoad == PrGpuLoad_Keep ? MTLLoadActionLoad
                                                                          : MTLLoadActionDontCare;
                    sa.storeAction = desc->stencilStore == PrGpuStore_Keep ? MTLStoreActionStore
                                                                           : MTLStoreActionDontCare;
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
            endTail(c);
            pass->encoder = [[c->buffer renderCommandEncoderWithDescriptor:rp] retain];
            if (desc->name.size != 0)
                pass->encoder.label = stringOf(desc->name);
        }
        [pass->encoder setArgumentTable:c->table atStages:MTLRenderStageVertex | MTLRenderStageFragment];
        if (!pass->host) {
            [pass->encoder setViewport:MTLViewport{0, 0, double(pass->extent.x), double(pass->extent.y), 0, 1}];
        }
        c->pass = pass;
        c->pipeline = 0;
        return pass;
    }
}

void passEnd(PrGpuDevice*, PrGpuPass* handle) {
    MetalPass* pass = static_cast<MetalPass*>(handle);
    MetalCommands* c = commandsOf(pass->commands);
    c->pass = nullptr;
    if (pass->host) {
        [pass->encoder release];
    } else {
        // The pass's encoder stays open for the barriers after it.
        c->tail = pass->encoder;
    }
    prMemoryFree(pass);
}

void passSetViewport(PrGpuDevice*, PrGpuPass* handle, PrRect viewport, float minDepth, float maxDepth) {
    MetalPass* pass = static_cast<MetalPass*>(handle);
    [pass->encoder setViewport:MTLViewport{double(viewport.left), double(viewport.top),
                                           double(viewport.right - viewport.left),
                                           double(viewport.bottom - viewport.top), double(minDepth), double(maxDepth)}];
    changed(pass, PrGpuHostStateFlag_Viewport);
}

void passSetScissor(PrGpuDevice*, PrGpuPass* handle, PrIRect scissor) {
    MetalPass* pass = static_cast<MetalPass*>(handle);
    // Metal requires the scissor within the attachments.
    int32_t left = scissor.left > 0 ? scissor.left : 0;
    int32_t top = scissor.top > 0 ? scissor.top : 0;
    int32_t right = scissor.right < int32_t(pass->extent.x) ? scissor.right : int32_t(pass->extent.x);
    int32_t bottom = scissor.bottom < int32_t(pass->extent.y) ? scissor.bottom : int32_t(pass->extent.y);
    if (right < left)
        right = left;
    if (bottom < top)
        bottom = top;
    [pass->encoder setScissorRect:MTLScissorRect{NSUInteger(left), NSUInteger(top), NSUInteger(right - left),
                                                 NSUInteger(bottom - top)}];
    changed(pass, PrGpuHostStateFlag_Scissor);
}

void passSetBlendConstant(PrGpuDevice*, PrGpuPass* handle, PrColor constant) {
    MetalPass* pass = static_cast<MetalPass*>(handle);
    [pass->encoder setBlendColorRed:constant.r green:constant.g blue:constant.b alpha:constant.a];
    changed(pass, PrGpuHostStateFlag_BlendConstant);
}

void passSetStencilReference(PrGpuDevice*, PrGpuPass* handle, uint8_t reference) {
    MetalPass* pass = static_cast<MetalPass*>(handle);
    [pass->encoder setStencilReferenceValue:reference];
    changed(pass, PrGpuHostStateFlag_StencilReference);
}

void passSetIndexBuffer(PrGpuDevice*, PrGpuPass* handle, PrGpuBuffer buffer, uint64_t offset, bool wide) {
    MetalPass* pass = static_cast<MetalPass*>(handle);
    pass->indexBuffer = buffer;
    pass->indexOffset = offset;
    pass->indexType = wide ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16;
}

const MetalPipeline& passPipeline(PrGpuDevice* device, MetalPass* pass) {
    return metalOf(device)->pipelines[commandsOf(pass->commands)->pipeline];
}

void passDraw(PrGpuDevice* device, PrGpuPass* handle, uint32_t vertexCount, uint32_t instanceCount,
              uint32_t firstVertex, uint32_t firstInstance) {
    MetalPass* pass = static_cast<MetalPass*>(handle);
    [pass->encoder drawPrimitives:passPipeline(device, pass).primitive
                      vertexStart:firstVertex
                      vertexCount:vertexCount
                    instanceCount:instanceCount
                     baseInstance:firstInstance];
}

void passDrawIndexed(PrGpuDevice* device, PrGpuPass* handle, uint32_t indexCount, uint32_t instanceCount,
                     uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) {
    MetalDevice* d = metalOf(device);
    MetalPass* pass = static_cast<MetalPass*>(handle);
    id<MTLBuffer> indices = bufferOf(d, pass->indexBuffer.id);
    uint64_t at = pass->indexOffset + uint64_t(firstIndex) * (pass->indexType == MTLIndexTypeUInt32 ? 4 : 2);
    [pass->encoder drawIndexedPrimitives:passPipeline(device, pass).primitive
                              indexCount:indexCount
                               indexType:pass->indexType
                             indexBuffer:indices.gpuAddress + at
                       indexBufferLength:indices.length - at
                           instanceCount:instanceCount
                              baseVertex:vertexOffset
                            baseInstance:firstInstance];
}

void passDrawIndirect(PrGpuDevice* device, PrGpuPass* handle, PrGpuBuffer buffer, uint64_t offset, uint32_t count,
                      uint32_t stride) {
    MetalDevice* d = metalOf(device);
    MetalPass* pass = static_cast<MetalPass*>(handle);
    uint64_t address = bufferOf(d, buffer.id).gpuAddress + offset;
    for (uint32_t i = 0; i < count; ++i)
        [pass->encoder drawPrimitives:passPipeline(device, pass).primitive indirectBuffer:address + i * stride];
}

void passDrawIndexedIndirect(PrGpuDevice* device, PrGpuPass* handle, PrGpuBuffer buffer, uint64_t offset,
                             uint32_t count, uint32_t stride) {
    MetalDevice* d = metalOf(device);
    MetalPass* pass = static_cast<MetalPass*>(handle);
    id<MTLBuffer> indices = bufferOf(d, pass->indexBuffer.id);
    uint64_t address = bufferOf(d, buffer.id).gpuAddress + offset;
    for (uint32_t i = 0; i < count; ++i)
        [pass->encoder drawIndexedPrimitives:passPipeline(device, pass).primitive
                                   indexType:pass->indexType
                                 indexBuffer:indices.gpuAddress + pass->indexOffset
                           indexBufferLength:indices.length - pass->indexOffset
                              indirectBuffer:address + i * stride];
}

// ---------------------------------------------------------------------------------------------------------------------
// Swap chains

PrStatus swapChainCreate(PrGpuDevice* device, const PrAppSurface* surface, const PrGpuSwapChainDesc* desc,
                         PrGpuSwapChain* PR_NULLABLE* PR_NONNULL out) {
    MetalDevice* d = metalOf(device);
    @autoreleasepool {
        if (surface->kind != PrAppSurfaceKind_NsView)
            return PrStatus_NotSupported;
        NSView* view = (NSView*)surface->handle;
        // A view that backs itself with a Metal layer keeps that layer, so that AppKit asks the view to display as it
        // resizes. Any other view hosts a new Metal layer.
        CAMetalLayer* layer = [view.layer isKindOfClass:CAMetalLayer.class] ? (CAMetalLayer*)view.layer
                                                                            : [CAMetalLayer layer];
        layer.device = d->device;
        // HDR presents extended sRGB, encoded, in half floats, which the layer shows past SDR white.
        bool hdr = desc->intent == PrGpuSwapChainIntent_Hdr;
        PrGpuSwapChainInfo info = hdr ? PrGpuSwapChainInfo{PrGpuFormat_Rgba16F, PrColorSpace_Srgb}
                                      : PrGpuSwapChainInfo{PrGpuFormat_Bgra8, PrColorSpace_Srgb};
        layer.pixelFormat = formatOf(info.format);
        CGColorSpaceRef space = CGColorSpaceCreateWithName(hdr ? kCGColorSpaceExtendedSRGB : kCGColorSpaceSRGB);
        layer.colorspace = space;
        CGColorSpaceRelease(space);
        layer.wantsExtendedDynamicRangeContent = hdr ? YES : NO;
        // The layer is readable, so that a frame can be copied or read back.
        layer.framebufferOnly = NO;
        layer.contentsScale = view.window != nil ? view.window.backingScaleFactor : 1;
        layer.presentsWithTransaction = (desc->flags & PrGpuSwapChainFlag_Threaded) == 0;
        if (view.layer != layer) {
            view.layer = layer;
            view.wantsLayer = YES;
        }
        // AppKit sets the gravity of a layer that backs a view from the view's placement, and that of a layer that a
        // view hosts is the swap chain's to set. A gravity's top is the layer's greatest y, which is the window's
        // bottom where the layer's geometry is flipped.
        bool stretch = desc->anchor == PrGpuSwapChainAnchor_Stretch;
        view.layerContentsPlacement = stretch ? NSViewLayerContentsPlacementScaleAxesIndependently
                                              : NSViewLayerContentsPlacementTopLeft;
        layer.contentsGravity = stretch                     ? kCAGravityResize
                                : [layer contentsAreFlipped] ? kCAGravityBottomLeft
                                                             : kCAGravityTopLeft;
        [d->queue addResidencySet:layer.residencySet];
        MetalSwapChain* sc = allocZeroed<MetalSwapChain>();
        sc->device = device;
        sc->info = info;
        sc->layer = [layer retain];
        sc->view = view;
        sc->transactional = layer.presentsWithTransaction;
        *out = sc;
        return PrStatus_Ok;
    }
}

// Lets go of the acquired drawable's texture, whose slot stays until the work that drew into it is done.
void releaseDrawable(MetalDevice* d, MetalSwapChain* sc) {
    if (sc->texture.id != 0)
        retire(d, MetalRetiredKind_Resource, sc->texture.id);
    sc->texture = PrGpuTexture{};
    [sc->drawable release];
    sc->drawable = nil;
}

void swapChainDestroy(PrGpuDevice* device, PrGpuSwapChain* handle) {
    MetalDevice* d = metalOf(device);
    MetalSwapChain* sc = static_cast<MetalSwapChain*>(handle);
    waitIdle(device);
    releaseDrawable(d, sc);
    [d->queue removeResidencySet:sc->layer.residencySet];
    [sc->layer release];
    prMemoryFree(sc);
}

// A layer waits for a drawable only when asked for one, at its extent, which the frame gives.
void swapChainWait(PrGpuDevice*, PrGpuSwapChain*) {}

PrStatus swapChainAcquire(PrGpuDevice* device, PrGpuSwapChain* handle, PrUVec2 extent, PrGpuTexture* out) {
    MetalDevice* d = metalOf(device);
    MetalSwapChain* sc = static_cast<MetalSwapChain*>(handle);
    @autoreleasepool {
        releaseDrawable(d, sc);
        // On macOS 27, the compositor skips each present with the transaction of a drawable that an MTL4 queue
        // signalled, except during a live resize. A frame presents with the transaction only there, which is where it
        // keeps the drawable in step with the window's size.
        if (sc->transactional && sc->layer.presentsWithTransaction != sc->view.inLiveResize)
            sc->layer.presentsWithTransaction = sc->view.inLiveResize;
        CGSize size = CGSizeMake(extent.x, extent.y);
        if (!CGSizeEqualToSize(sc->layer.drawableSize, size))
            sc->layer.drawableSize = size;
        id<CAMetalDrawable> drawable = [sc->layer nextDrawable];
        if (drawable == nil)
            return PrStatus_InvalidState;
        id<MTLTexture> texture = drawable.texture;
        uint32_t slot = addResource(d, MetalSlot_Texture, texture, texture.gpuResourceID._impl);
        if (slot == 0)
            return PrStatus_Limit;
        // The layer's residency set holds the drawable's texture, so the device's does not.
        prMutexLock(&d->mutex);
        [d->residency removeAllocation:texture];
        prMutexUnlock(&d->mutex);
        d->resources[slot].texture = PrGpuTextureDesc{.format = sc->info.format,
                                                      .usage = PrGpuTextureUsageFlag_RenderTarget
                                                               | PrGpuTextureUsageFlag_Sampled,
                                                      .levels = 1,
                                                      .samples = 1,
                                                      .extent = {uint32_t(texture.width), uint32_t(texture.height), 1}};
        sc->drawable = [drawable retain];
        sc->texture = PrGpuTexture{slot};
        *out = sc->texture;
        return PrStatus_Ok;
    }
}

void swapChainPresent(PrGpuDevice* device, PrGpuSwapChain* handle, PrGpuCommands* commands) {
    MetalSwapChain* sc = static_cast<MetalSwapChain*>(handle);
    if (sc->drawable == nil)
        return;
    commandsOf(commands)->drawables.push([sc->drawable retain]);
    releaseDrawable(metalOf(device), sc);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrGpuMetalOptions prGpuMetalOptionsDefault(void) {
    return PrGpuMetalOptions{};
}

PrStatus prGpuMetalCreate(const PrGpuMetalOptions* PR_NULLABLE options, PrGpuDevice* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    PrGpuMetalOptions o = options != nullptr ? *options : prGpuMetalOptionsDefault();
    @autoreleasepool {
        id<MTLDevice> device = o.device != nullptr ? [static_cast<id<MTLDevice>>(o.device) retain]
                                                   : MTLCreateSystemDefaultDevice();
        if (device == nil)
            return PrStatus_NotSupported;
        if (![device supportsFamily:MTLGPUFamilyMetal4]) {
            [device release];
            return PrStatus_NotSupported;
        }
        MetalDevice* d = allocZeroed<MetalDevice>();
        d->device = device;
        d->feedback = dispatch_queue_create("lib/gpu feedback", DISPATCH_QUEUE_SERIAL);
        if (o.queue != nullptr) {
            d->queue = [static_cast<id<MTL4CommandQueue>>(o.queue) retain];
        } else {
            // The descriptor releases its queue, though its property does not say that it retains it.
            MTL4CommandQueueDescriptor* qd = [[MTL4CommandQueueDescriptor new] autorelease];
            dispatch_retain(d->feedback);
            qd.feedbackQueue = d->feedback;
            d->queue = [device newMTL4CommandQueueWithDescriptor:qd error:nil];
        }
        d->compiler = [device newCompilerWithDescriptor:[[MTL4CompilerDescriptor new] autorelease] error:nil];
        prMutexInit(&d->doneMutex);
        prConditionInit(&d->doneSignal);
        MTLResidencySetDescriptor* rd = [[MTLResidencySetDescriptor new] autorelease];
        rd.initialCapacity = 256;
        d->residency = [device newResidencySetWithDescriptor:rd error:nil];
        d->resourceHeap = [device newBufferWithLength:kMaxResources * sizeof(uint64_t)
                                              options:MTLResourceStorageModeShared
                                                      | MTLResourceHazardTrackingModeUntracked];
        d->samplerHeap = [device newBufferWithLength:kMaxSamplers * sizeof(uint64_t)
                                             options:MTLResourceStorageModeShared
                                                     | MTLResourceHazardTrackingModeUntracked];
        [d->residency addAllocation:d->resourceHeap];
        [d->residency addAllocation:d->samplerHeap];
        [d->residency commit];
        [d->queue addResidencySet:d->residency];
        prMutexInit(&d->mutex);
        d->resources = static_cast<MetalResource*>(prMemoryAllocZeroed(sizeof(MetalResource) * kMaxResources));
        d->samplers = static_cast<id<MTLSamplerState>*>(prMemoryAllocZeroed(sizeof(id) * kMaxSamplers));
        d->pipelines = static_cast<MetalPipeline*>(prMemoryAllocZeroed(sizeof(MetalPipeline) * kMaxPipelines));
        d->nextResource = 1;
        d->nextSampler = 1;
        d->nextPipeline = 1;
        d->serial = 1;

        d->destroy = destroyDevice;
        d->isLost = isLost;
        d->simulateLoss = simulateLoss;
        d->isComplete = isComplete;
        d->wait = wait;
        d->waitIdle = waitIdle;
        d->supportsFormat = supportsFormat;
        d->bufferCreate = bufferCreate;
        d->bufferDestroy = bufferDestroy;
        d->bufferDesc = bufferDesc;
        d->bufferStorageHandle = bufferStorageHandle;
        d->bufferMap = bufferMap;
        d->bufferUpload = bufferUpload;
        d->textureCreate = textureCreate;
        d->textureCreateView = textureCreateView;
        d->textureDestroy = textureDestroy;
        d->textureDesc = textureDesc;
        d->textureStorageHandle = textureStorageHandle;
        d->textureUpload = textureUpload;
        d->samplerCreate = samplerCreate;
        d->shaderLibraryCreate = shaderLibraryCreate;
        d->shaderLibraryDestroy = shaderLibraryDestroy;
        d->graphicsPipelineCreate = graphicsPipelineCreate;
        d->computePipelineCreate = computePipelineCreate;
        d->pipelineDestroy = pipelineDestroy;
        d->pipelineGroupSize = pipelineGroupSize;
        d->commandsBegin = commandsBegin;
        d->commandsSubmit = commandsSubmit;
        d->commandsStream = commandsStream;
        d->commandsSetPush = commandsSetPush;
        d->commandsSetUniforms = commandsSetUniforms;
        d->commandsSetPipeline = commandsSetPipeline;
        d->commandsDispatch = commandsDispatch;
        d->commandsDispatchIndirect = commandsDispatchIndirect;
        d->commandsCopyBuffer = commandsCopyBuffer;
        d->commandsCopyTexture = commandsCopyTexture;
        d->commandsGenerateMipmaps = commandsGenerateMipmaps;
        d->commandsCopyBufferToTexture = commandsCopyBufferToTexture;
        d->commandsCopyTextureToBuffer = commandsCopyTextureToBuffer;
        d->commandsOnDone = commandsOnDone;
        d->commandsPushGroup = commandsPushGroup;
        d->commandsPopGroup = commandsPopGroup;
        d->commandsBufferBarrier = commandsBufferBarrier;
        d->commandsTextureBarrier = commandsTextureBarrier;
        d->commandsGlobalBarrier = commandsGlobalBarrier;
        d->hostStateSync = hostStateSync;
        d->passBegin = passBegin;
        d->passEnd = passEnd;
        d->passSetViewport = passSetViewport;
        d->passSetScissor = passSetScissor;
        d->passSetBlendConstant = passSetBlendConstant;
        d->passSetStencilReference = passSetStencilReference;
        d->passSetIndexBuffer = passSetIndexBuffer;
        d->passDraw = passDraw;
        d->passDrawIndexed = passDrawIndexed;
        d->passDrawIndirect = passDrawIndirect;
        d->passDrawIndexedIndirect = passDrawIndexedIndirect;
        d->swapChainCreate = swapChainCreate;
        d->swapChainDestroy = swapChainDestroy;
        d->swapChainWait = swapChainWait;
        d->swapChainAcquire = swapChainAcquire;
        d->swapChainPresent = swapChainPresent;
        d->shaderTarget = PrGpuShaderTarget_Metal;
        // The name's UTF-8 lives no longer than the autorelease pool, so the device keeps a copy.
        if (![device.name getCString:d->adapter maxLength:sizeof(d->adapter) encoding:NSUTF8StringEncoding])
            memCopy(d->adapter, "Metal", sizeof("Metal"));
        d->info.adapter = PrStringView{d->adapter, strlen(d->adapter)};
        d->info.backend = "Metal";
        // Framebuffer fetch on Apple's GPUs reads in raster order.
        d->info.features = PrGpuFeatureFlag_Timestamps | PrGpuFeatureFlag_TextureCompressionAstc
                           | PrGpuFeatureFlag_OrderedTargetReads
                           | (device.supportsBCTextureCompression ? PrGpuFeatureFlag_TextureCompressionBc : 0);
        d->info.maxTextureSize = 16384;
        d->info.maxResourceHandles = kMaxResources - 1;
        d->info.maxSamplerHandles = kMaxSamplers - 1;
        *out = d;
        return PrStatus_Ok;
    }
}

void* prGpuMetalDevice(const PrGpuDevice* device) {
    return metalOf(device)->device;
}

void* prGpuMetalQueue(const PrGpuDevice* device) {
    return metalOf(device)->queue;
}

void* prGpuMetalResidencySet(const PrGpuDevice* device) {
    return metalOf(device)->residency;
}

void* prGpuMetalTexture(const PrGpuDevice* device, PrGpuTexture texture) {
    return textureOf(metalOf(device), texture.id);
}

PrStatus prGpuMetalTextureWrap(PrGpuDevice* device, void* texture, PrGpuTextureUsageFlags usage, PrGpuTexture* out) {
    *out = PrGpuTexture{};
    MetalDevice* d = metalOf(device);
    id<MTLTexture> t = static_cast<id<MTLTexture>>(texture);
    uint32_t slot = addResource(d, MetalSlot_Texture, t, t.gpuResourceID._impl);
    if (slot == 0)
        return PrStatus_Limit;
    d->resources[slot].texture = PrGpuTextureDesc{.format = gpuFormatOf(t.pixelFormat),
                                                  .usage = usage,
                                                  .levels = uint8_t(t.mipmapLevelCount),
                                                  .samples = uint8_t(t.sampleCount),
                                                  .extent = {uint32_t(t.width), uint32_t(t.height),
                                                             uint32_t(t.arrayLength)}};
    out->id = slot;
    return PrStatus_Ok;
}
