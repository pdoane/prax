// The Direct3D 12 GPU backend, on shader model 6.6's descriptor heaps and enhanced barriers.
//
// A handle is a slot of the device's shader-visible heap of resource descriptors, or of its heap of samplers, which
// shaders index directly (pr/gpu.slang). A buffer's slot holds a raw view of it, and a texture's a shader resource
// view. A resource that shaders write takes a second slot for its unordered access view, which is its storage handle.
// Every pipeline shares one root signature: push data as 32 root constants at b0, uniforms as a root constant buffer at
// b1, and the handle of the target's copy as one root constant at b2. Root data is set on the command list where a draw
// or a dispatch takes it, so that a set of push data serves both.
//
// The device tracks no layouts. A barrier maps the accesses before and after it to enhanced barriers' sync, access and,
// for a texture, layout. A texture begins in the shader resource layout, which is what ShaderRead maps to.
//
// A destroyed object is kept until every command buffer that began before its destruction is done, since any of them
// may use it. A thread of the device's own waits for each submission's fence in order, then runs the functions that
// wait for it and raises the device's count of fences done, so that a fence is reached only after them.
//
// Uploads are copied in a command list of the device's own, executed just before the next submission, or sooner once
// its staging fills a few blocks. D3D12 completes the work of each ExecuteCommandLists before the next one begins on a
// queue, so the uploads wait for the work before them, and the work after them waits for the uploads.
//
// A pipeline that reads its target draws after a copy of the target, over the draw's scissor, into a texture of the
// command buffer's, which the shader reads at its pixel. Fragments of one draw read what the draws before it left.
//
// COM objects are held as raw pointers and released by hand, because the structures that hold them run no constructors.

#include "pr/gpu_d3d12.h"

#include "pr/align.h"
#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/atomic.h"
#include "pr/gpu_backend.h"
#include "pr/integer.h"
#include "pr/mem.h"
#include "pr/mutex.h"
#include "pr/raw_array.h"
#include "pr/search.h"
#include "pr/thread.h"
#include "pr/windows_string.h"

#include "gpu/backends/shared/src/gpu_shared_device.h"
#include "gpu/backends/shared/src/gpu_shared_upload.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <stdio.h>

namespace pr_impl {

namespace {

// The functions of the device's objects, which their creation installs.
extern const PrGpuCommands kCommandsFunctions;
extern const PrGpuPass kPassFunctions;
extern const PrGpuSwapChain kSwapChainFunctions;
extern const PrGpuShaderLibrary kLibraryFunctions;

// The handles that a device gives out, of resources and of samplers, and the pipelines that it holds. Slot zero of each
// is never used, so that zero is none. D3D12 allows no more than 2048 samplers in a shader-visible heap.
constexpr uint32_t kMaxResources = 1u << 16;
constexpr uint32_t kMaxSamplers = 2048;
constexpr uint32_t kMaxPipelines = 4096;

// The root signature's parameters, as pr/gpu.slang declares their registers.
enum {
    D3d12Root_Push,         // b0
    D3d12Root_Uniforms,     // b1
    D3d12Root_TargetCopy,   // b2
};
typedef uint32_t D3d12Root;

// The root data that the graphics or the compute bind point has yet to take.
enum {
    D3d12DirtyFlag_GraphicsPush = 1u << 0,
    D3d12DirtyFlag_ComputePush = 1u << 1,
    D3d12DirtyFlag_GraphicsUniforms = 1u << 2,
    D3d12DirtyFlag_ComputeUniforms = 1u << 3,
    D3d12DirtyFlag_All = 0xf,
};
typedef uint8_t D3d12DirtyFlags;

// The stream's blocks, and the alignment of its allocations, which root constant buffers require.
constexpr uint64_t kStreamBlock = uint64_t(1) << 20;
constexpr uint64_t kStreamAlignment = 256;

// The staging of uploads: blocks of upload memory, and the number that a batch fills before it is executed without
// waiting for a submission. A texture's texels start at a multiple of kPlacementAlignment, with rows kPitchAlignment
// apart.
constexpr uint64_t kStagingBlock = uint64_t(4) << 20;
constexpr uint32_t kStagingFlushBlocks = 4;
constexpr uint64_t kPlacementAlignment = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
constexpr uint32_t kPitchAlignment = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;

// The command buffers in flight that time their work. A command buffer beyond them reports zero seconds.
constexpr uint32_t kTimestampPairs = 1024;

// The buffers of a swap chain. Three let a frame record while one shows and one waits to.
constexpr uint32_t kSwapChainBuffers = 3;

// The longest that a wait for a fence goes without checking whether the device was lost, in milliseconds. D3D12
// completes every fence when it removes a device, so the check guards against a fence that removal leaves behind.
constexpr DWORD kLostCheckMilliseconds = 1000;

// The size of the zeroed buffer that a command buffer's uniforms read until it sets its own.
constexpr uint64_t kZeroUniformBytes = kPrGpuMaxUniformBytes;

template <typename T>
void release(T* PR_NULLABLE object) {
    if (object != nullptr)
        object->Release();
}

// What a resource slot holds.
enum {
    D3d12Slot_Free,
    D3d12Slot_Buffer,
    D3d12Slot_Texture,
    D3d12Slot_Storage,      // the unordered access view of the resource in another slot
};
typedef uint8_t D3d12Slot;

// A buffer, a texture or a view. A view covers the subresources of its resource from `firstLevel` and `firstLayer`,
// which subresource indices count from.
struct D3d12Resource {
    D3d12Slot kind;
    union {
        PrGpuBufferDesc buffer;
        PrGpuTextureDesc texture;
    };
    ID3D12Resource* PR_NULLABLE resource;   // held; null for a storage slot
    uint32_t storage;                       // the slot of the unordered access view, or zero
    uint8_t* PR_NULLABLE mapped;            // the bytes of an Upload or Readback buffer
    D3D12_GPU_VIRTUAL_ADDRESS address;      // where a buffer starts, for root and index buffer views

    // The texture's resource: its format, which is typeless where views read it in more than one, and its levels and
    // layers.
    DXGI_FORMAT format;
    uint8_t resourceLevels;
    uint16_t resourceLayers;
    uint8_t firstLevel;
    uint16_t firstLayer;
};

struct D3d12Pipeline {
    ID3D12PipelineState* PR_NULLABLE state; // held
    bool compute;
    bool readsTarget;
    D3D_PRIMITIVE_TOPOLOGY topology;
    PrGpuExtent groupSize;
};

// A batch of uploads, and the buffers that it wrote.
struct D3d12UploadBatch : GpuSharedUploadBatch {
    RawArray<uint32_t> written;
};

// Empties the batch, keeping its storage.
void uploadBatchClear(D3d12UploadBatch* batch) {
    gpuSharedUploadBatchClear(batch);
    batch->written.clear();
}

// Frees the batch's storage.
void uploadBatchReset(D3d12UploadBatch* batch) {
    gpuSharedUploadBatchReset(batch);
    batch->written.reset();
}

// An object that waits to be freed until every command buffer that began before `serial` is done.
enum {
    D3d12RetiredKind_Resource,
    D3d12RetiredKind_Pipeline,
};
typedef uint8_t D3d12RetiredKind;

struct D3d12Retired {
    uint64_t serial;
    D3d12RetiredKind kind;
    uint32_t slot;
};

// A block of upload memory that the stream or staging writes into, with a resource slot so that shaders can read it.
struct D3d12Block {
    ID3D12Resource* PR_NULLABLE buffer;     // the slot's resource
    uint8_t* PR_NULLABLE mapped;
    uint64_t length;
    uint32_t slot;
    uint64_t used;
};

// A command allocator and the command list that records with it.
struct D3d12List {
    ID3D12CommandAllocator* PR_NULLABLE allocator;  // held
    ID3D12GraphicsCommandList7* PR_NULLABLE list;   // held
};

// The descriptor heaps, not visible to shaders, that a command buffer writes the views of a pass's attachments into.
// D3D12 reads the views as the pass sets them, so the heaps are free again once the command buffer is submitted.
struct D3d12AttachmentHeaps {
    ID3D12DescriptorHeap* PR_NULLABLE color;    // kPrGpuMaxColorAttachments render target views, held
    ID3D12DescriptorHeap* PR_NULLABLE depth;    // one depth stencil view, held
};

// What a command buffer held, which is free again once its fence is reached.
enum {
    D3d12HeldKind_StreamBlock,
    D3d12HeldKind_StagingBlock,
    D3d12HeldKind_List,
    D3d12HeldKind_Timestamp,
};
typedef uint8_t D3d12HeldKind;

struct D3d12Held {
    PrGpuFence fence;
    D3d12HeldKind kind;
    D3d12Block block;
    D3d12List list;
    uint32_t timestamp;
};

// A command buffer that is begun or in flight, and the fence that it completes at once it is submitted.
struct D3d12InFlight {
    uint64_t serial;
    PrGpuFence fence;                       // zero until submitted
};

// A function that a command buffer calls once it is done.
struct D3d12Done {
    PrGpuDoneFn done;
    void* PR_NULLABLE context;
};

// A submission that the completion thread waits for: its fence, the functions to call, and the timestamps that time it,
// or kTimestampPairs.
struct D3d12Submission {
    PrGpuFence fence;
    D3d12Done* PR_NULLABLE dones;
    uint32_t doneCount;
    uint32_t timestamp;
};

struct D3d12Library : GpuSharedLibrary {};

struct D3d12Pass;
struct D3d12SwapChain;

struct D3d12Commands : GpuSharedCommands {
    uint64_t serial;                        // the order in which command buffers began
    D3d12Pass* PR_NULLABLE pass;            // the open pass
    D3d12List list;                         // a null allocator for the application's command list
    D3d12AttachmentHeaps attachments;
    uint32_t pipeline;                         // the pipeline of dispatches and draws
    RawArray<D3d12Block> blocks;               // the blocks of the stream
    RawArray<D3d12SwapChain*> presents;        // the swap chains that present once the command buffer is executed
    RawArray<D3d12Done> dones;
    RawArray<void*> scratch;                // what the stream gave out once the device had no memory left
    uint32_t timestamp;                     // the slot of the timestamps, or kTimestampPairs

    // The root data, as set, and the bind points that have yet to take it.
    uint8_t push[kPrGpuMaxPushBytes];
    uint32_t pushWords;
    D3D12_GPU_VIRTUAL_ADDRESS uniforms;
    D3d12DirtyFlags dirty;
    PrGpuNativeStateFlags changed;            // the state that the device changed since the last sync

    // The texture that a pipeline that reads its target reads a copy of the target from, or zero.
    PrGpuTexture targetCopy;
};

struct D3d12Pass : GpuSharedPass {
    PrGpuAttachment color[kPrGpuMaxColorAttachments];
    PrGpuAttachment depthStencil;
    PrIRect scissor;                        // within the attachments
    bool named;                             // true if the pass opened a group of commands for debuggers
};

// DXGI presents a swap chain's whole buffers. Anchored at the top left, unscaled, the buffers are at least as large as
// the frames, which draw at their top left, and the window cuts them off at its edges. The buffers only grow, so that a
// resize within them waits for no work and allocates nothing. Stretched, the buffers are the frame's extent, which DXGI
// scales to the window, and each resize resizes them.
struct D3d12SwapChain : GpuSharedSwapChain {
    IDXGISwapChain3* swapChain;             // held
    HWND window;
    bool hdr;                               // PrGpuSwapChainIntent_Hdr
    HMONITOR PR_NULLABLE monitor;           // the display that the format was chosen for
    IDXGIFactory6* displays;                // held; reads the displays' modes, and is made again when they change
    HANDLE PR_NULLABLE waitable;
    bool waited;                            // the swap chain was waited on for the next frame
    bool stretch;                           // PrGpuSwapChainAnchor_Stretch
    DXGI_FORMAT format;
    uint32_t buffers[kSwapChainBuffers];    // the resource slots of the buffers, by buffer
    PrUVec2 capacity;                       // the buffers' extent
    PrGpuTexture current;                   // the buffer of the acquired frame
};

struct D3d12Device : GpuSharedDevice {
    ID3D12Device10* device;                 // held
    ID3D12CommandQueue* queue;              // held
    IDXGIFactory6* PR_NULLABLE factory;     // held
    ID3D12RootSignature* rootSignature;     // held
    ID3D12DescriptorHeap* resourceHeap;     // held
    ID3D12DescriptorHeap* samplerHeap;      // held
    uint32_t resourceIncrement;
    uint32_t samplerIncrement;
    uint32_t colorIncrement;
    ID3D12CommandSignature* drawSignature;          // held
    ID3D12CommandSignature* drawIndexedSignature;   // held
    ID3D12CommandSignature* dispatchSignature;      // held
    ID3D12Resource* zeros;                          // the uniforms of a command buffer that sets none, held
    ID3D12QueryHeap* PR_NULLABLE timestamps;        // held
    ID3D12Resource* PR_NULLABLE timestampReadback;  // held
    const uint64_t* PR_NULLABLE timestampTicks;
    double ticksPerSecond;
    ID3D12Fence* fence;                     // held
    Atomic<bool> lost;
    char adapter[128];                      // the adapter's name, in UTF-8

    // The completion thread, and what it waits for. The done mutex guards the submissions and `stopping`, and raising
    // `done`.
    PrThread completion;
    Atomic<uint64_t> done;                  // the fence of the last submission that is done
    PrMutex doneMutex;
    PrCondition doneSignal;                 // broadcast when `done` rises
    PrCondition pendingSignal;              // signaled when a submission is added
    RawArray<D3d12Submission> pending;      // in the order of their fences
    bool stopping;

    // The mutex guards everything below. The tables have fixed capacities, so that recording reads them without it.
    PrMutex mutex;
    D3d12Resource* resources;               // by slot
    D3d12Pipeline* pipelines;               // by id
    RawArray<uint32_t> freeResources;
    uint32_t nextResource;
    uint32_t nextSampler;
    RawArray<uint32_t> freePipelines;
    uint32_t nextPipeline;
    RawArray<uint32_t> freeTimestamps;

    PrGpuFence submitted;                   // the last fence that a submission returned
    uint64_t serial;                        // the serial of the next command buffer
    RawArray<D3d12InFlight> inFlight;       // in the order of their submission
    RawArray<D3d12Retired> retired;
    RawArray<D3d12Held> held;
    RawArray<D3d12List> lists;                      // free, with their allocators reset
    RawArray<D3d12AttachmentHeaps> attachmentHeaps; // free
    RawArray<D3d12Block> streamBlocks;              // free
    RawArray<D3d12Block> stagingBlocks;             // free

    // The open batch of uploads: its command list, the staging blocks that it reads, and its copies.
    D3d12List uploads;                      // a null list while no batch is open
    uint64_t uploadSerial;
    RawArray<D3d12Block> uploadBlocks;
    D3d12UploadBatch uploadBatch;
};

D3d12Device* d3d12Of(PrGpuDevice* device) {
    return static_cast<D3d12Device*>(device);
}

const D3d12Device* d3d12Of(const PrGpuDevice* device) {
    return static_cast<const D3d12Device*>(device);
}

D3d12Commands* commandsOf(PrGpuCommands* commands) {
    return static_cast<D3d12Commands*>(commands);
}

// Names an object for debuggers and captures.
void nameObject(ID3D12Object* object, PrStringView name) {
    if (name.size == 0)
        return;
    wchar_t* wide = prWindowsWidenCopy(name, nullptr);
    if (wide != nullptr)
        object->SetName(wide);
    prMemoryFree(wide);
}

// ---------------------------------------------------------------------------------------------------------------------
// Formats and states

DXGI_FORMAT formatOf(PrGpuFormat format) {
    switch (format) {
        case PrGpuFormat_R8: return DXGI_FORMAT_R8_UNORM;
        case PrGpuFormat_Rg8: return DXGI_FORMAT_R8G8_UNORM;
        case PrGpuFormat_Rgba8: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case PrGpuFormat_Rgba8Srgb: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case PrGpuFormat_Bgra8: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case PrGpuFormat_Bgra8Srgb: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        case PrGpuFormat_Rgb10A2: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case PrGpuFormat_Rg11B10F: return DXGI_FORMAT_R11G11B10_FLOAT;
        case PrGpuFormat_R16F: return DXGI_FORMAT_R16_FLOAT;
        case PrGpuFormat_Rg16F: return DXGI_FORMAT_R16G16_FLOAT;
        case PrGpuFormat_Rgba16F: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case PrGpuFormat_R32F: return DXGI_FORMAT_R32_FLOAT;
        case PrGpuFormat_Rg32F: return DXGI_FORMAT_R32G32_FLOAT;
        case PrGpuFormat_Rgba32F: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case PrGpuFormat_R8Uint: return DXGI_FORMAT_R8_UINT;
        case PrGpuFormat_R16Uint: return DXGI_FORMAT_R16_UINT;
        case PrGpuFormat_R32Uint: return DXGI_FORMAT_R32_UINT;
        case PrGpuFormat_Rg32Uint: return DXGI_FORMAT_R32G32_UINT;
        case PrGpuFormat_Rgba32Uint: return DXGI_FORMAT_R32G32B32A32_UINT;
        case PrGpuFormat_D16: return DXGI_FORMAT_D16_UNORM;
        case PrGpuFormat_D32F: return DXGI_FORMAT_D32_FLOAT;
        case PrGpuFormat_D32FS8: return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        case PrGpuFormat_Bc1: return DXGI_FORMAT_BC1_UNORM;
        case PrGpuFormat_Bc1Srgb: return DXGI_FORMAT_BC1_UNORM_SRGB;
        case PrGpuFormat_Bc3: return DXGI_FORMAT_BC3_UNORM;
        case PrGpuFormat_Bc3Srgb: return DXGI_FORMAT_BC3_UNORM_SRGB;
        case PrGpuFormat_Bc4: return DXGI_FORMAT_BC4_UNORM;
        case PrGpuFormat_Bc5: return DXGI_FORMAT_BC5_UNORM;
        case PrGpuFormat_Bc6HUfloat: return DXGI_FORMAT_BC6H_UF16;
        case PrGpuFormat_Bc7: return DXGI_FORMAT_BC7_UNORM;
        case PrGpuFormat_Bc7Srgb: return DXGI_FORMAT_BC7_UNORM_SRGB;
        default: return DXGI_FORMAT_UNKNOWN;
    }
}

PrGpuFormat gpuFormatOf(DXGI_FORMAT format) {
    for (uint8_t f = 1; f < PrGpuFormat_Count; ++f) {
        if (formatOf(PrGpuFormat(f)) == format)
            return PrGpuFormat(f);
    }
    return PrGpuFormat_None;
}

bool isSrgb(PrGpuFormat format) {
    return format == PrGpuFormat_Rgba8Srgb || format == PrGpuFormat_Bgra8Srgb || format == PrGpuFormat_Bc1Srgb ||
        format == PrGpuFormat_Bc3Srgb || format == PrGpuFormat_Bc7Srgb || format == PrGpuFormat_Astc4x4Srgb;
}

// The format of a texture's resource: typeless for depth, which shader resource views read in a color format, and for
// sRGB that shaders write, whose unordered access views take the linear format.
DXGI_FORMAT resourceFormatOf(PrGpuFormat format, PrGpuTextureUsageFlags usage) {
    switch (format) {
        case PrGpuFormat_D16: return DXGI_FORMAT_R16_TYPELESS;
        case PrGpuFormat_D32F: return DXGI_FORMAT_R32_TYPELESS;
        case PrGpuFormat_D32FS8: return DXGI_FORMAT_R32G8X24_TYPELESS;
        case PrGpuFormat_Rgba8Srgb:
            return (usage & PrGpuTextureUsageFlag_Storage) ? DXGI_FORMAT_R8G8B8A8_TYPELESS : formatOf(format);
        case PrGpuFormat_Bgra8Srgb:
            return (usage & PrGpuTextureUsageFlag_Storage) ? DXGI_FORMAT_B8G8R8A8_TYPELESS : formatOf(format);
        default: return formatOf(format);
    }
}

// The format in which shaders read a texture.
DXGI_FORMAT shaderFormatOf(PrGpuFormat format) {
    switch (format) {
        case PrGpuFormat_D16: return DXGI_FORMAT_R16_UNORM;
        case PrGpuFormat_D32F: return DXGI_FORMAT_R32_FLOAT;
        case PrGpuFormat_D32FS8: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        default: return formatOf(format);
    }
}

// The format in which shaders write a texture, which no sRGB format is.
DXGI_FORMAT storageFormatOf(PrGpuFormat format) {
    switch (format) {
        case PrGpuFormat_Rgba8Srgb: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case PrGpuFormat_Bgra8Srgb: return DXGI_FORMAT_B8G8R8A8_UNORM;
        default: return formatOf(format);
    }
}

// The format of a texture's texels in a buffer that a copy reads or writes: the resource's, or its depth plane's.
DXGI_FORMAT copyFormatOf(const D3d12Resource& r) {
    return r.texture.format == PrGpuFormat_D32FS8 ? DXGI_FORMAT_R32_TYPELESS : r.format;
}

D3D12_COMPARISON_FUNC compareOf(PrGpuCompare compare) {
    switch (compare) {
        case PrGpuCompare_Never: return D3D12_COMPARISON_FUNC_NEVER;
        case PrGpuCompare_Less: return D3D12_COMPARISON_FUNC_LESS;
        case PrGpuCompare_Equal: return D3D12_COMPARISON_FUNC_EQUAL;
        case PrGpuCompare_LessEqual: return D3D12_COMPARISON_FUNC_LESS_EQUAL;
        case PrGpuCompare_Greater: return D3D12_COMPARISON_FUNC_GREATER;
        case PrGpuCompare_NotEqual: return D3D12_COMPARISON_FUNC_NOT_EQUAL;
        case PrGpuCompare_GreaterEqual: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        default: return D3D12_COMPARISON_FUNC_ALWAYS;
    }
}

// The weight of a blend. D3D12 takes no color weight for alpha, so an alpha blend takes the alpha of a color weight.
D3D12_BLEND blendFactorOf(PrGpuBlendFactor factor, bool alpha) {
    switch (factor) {
        case PrGpuBlendFactor_Zero: return D3D12_BLEND_ZERO;
        case PrGpuBlendFactor_Source: return alpha ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_SRC_COLOR;
        case PrGpuBlendFactor_OneMinusSource: return alpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_INV_SRC_COLOR;
        case PrGpuBlendFactor_SourceAlpha: return D3D12_BLEND_SRC_ALPHA;
        case PrGpuBlendFactor_OneMinusSourceAlpha: return D3D12_BLEND_INV_SRC_ALPHA;
        case PrGpuBlendFactor_Target: return alpha ? D3D12_BLEND_DEST_ALPHA : D3D12_BLEND_DEST_COLOR;
        case PrGpuBlendFactor_OneMinusTarget: return alpha ? D3D12_BLEND_INV_DEST_ALPHA : D3D12_BLEND_INV_DEST_COLOR;
        case PrGpuBlendFactor_TargetAlpha: return D3D12_BLEND_DEST_ALPHA;
        case PrGpuBlendFactor_OneMinusTargetAlpha: return D3D12_BLEND_INV_DEST_ALPHA;
        case PrGpuBlendFactor_SourceAlphaSaturated: return D3D12_BLEND_SRC_ALPHA_SAT;
        case PrGpuBlendFactor_Constant: return D3D12_BLEND_BLEND_FACTOR;
        case PrGpuBlendFactor_OneMinusConstant: return D3D12_BLEND_INV_BLEND_FACTOR;
        case PrGpuBlendFactor_Source1: return alpha ? D3D12_BLEND_SRC1_ALPHA : D3D12_BLEND_SRC1_COLOR;
        case PrGpuBlendFactor_OneMinusSource1: return alpha ? D3D12_BLEND_INV_SRC1_ALPHA : D3D12_BLEND_INV_SRC1_COLOR;
        default: return D3D12_BLEND_ONE;
    }
}

D3D12_BLEND_OP blendOpOf(PrGpuBlendOp op) {
    switch (op) {
        case PrGpuBlendOp_Subtract: return D3D12_BLEND_OP_SUBTRACT;
        case PrGpuBlendOp_ReverseSubtract: return D3D12_BLEND_OP_REV_SUBTRACT;
        case PrGpuBlendOp_Min: return D3D12_BLEND_OP_MIN;
        case PrGpuBlendOp_Max: return D3D12_BLEND_OP_MAX;
        default: return D3D12_BLEND_OP_ADD;
    }
}

D3D12_STENCIL_OP stencilOpOf(PrGpuStencilOp op) {
    switch (op) {
        case PrGpuStencilOp_Zero: return D3D12_STENCIL_OP_ZERO;
        case PrGpuStencilOp_Replace: return D3D12_STENCIL_OP_REPLACE;
        case PrGpuStencilOp_IncrementClamp: return D3D12_STENCIL_OP_INCR_SAT;
        case PrGpuStencilOp_DecrementClamp: return D3D12_STENCIL_OP_DECR_SAT;
        case PrGpuStencilOp_Invert: return D3D12_STENCIL_OP_INVERT;
        case PrGpuStencilOp_IncrementWrap: return D3D12_STENCIL_OP_INCR;
        case PrGpuStencilOp_DecrementWrap: return D3D12_STENCIL_OP_DECR;
        default: return D3D12_STENCIL_OP_KEEP;
    }
}

D3D12_TEXTURE_ADDRESS_MODE addressOf(PrGpuAddress address) {
    switch (address) {
        case PrGpuAddress_Repeat: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        case PrGpuAddress_Mirror: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
        case PrGpuAddress_Border: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        default: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    }
}

D3D_PRIMITIVE_TOPOLOGY topologyOf(PrGpuTopology topology) {
    switch (topology) {
        case PrGpuTopology_TriangleStrip: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
        case PrGpuTopology_LineList: return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
        case PrGpuTopology_LineStrip: return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
        case PrGpuTopology_PointList: return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
        default: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    }
}

D3D12_PRIMITIVE_TOPOLOGY_TYPE topologyTypeOf(PrGpuTopology topology) {
    switch (topology) {
        case PrGpuTopology_LineList:
        case PrGpuTopology_LineStrip: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        case PrGpuTopology_PointList: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
        default: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    }
}

// The stages of the work that makes accesses.
D3D12_BARRIER_SYNC syncOf(PrGpuAccessFlags access) {
    D3D12_BARRIER_SYNC sync = D3D12_BARRIER_SYNC_NONE;
    if (access & PrGpuAccessFlag_IndexRead)
        sync |= D3D12_BARRIER_SYNC_INDEX_INPUT;
    if (access & PrGpuAccessFlag_IndirectRead)
        sync |= D3D12_BARRIER_SYNC_EXECUTE_INDIRECT;
    if (access & (PrGpuAccessFlag_VertexRead | PrGpuAccessFlag_VertexWrite))
        sync |= D3D12_BARRIER_SYNC_VERTEX_SHADING;
    if (access & (PrGpuAccessFlag_FragmentRead | PrGpuAccessFlag_FragmentWrite))
        sync |= D3D12_BARRIER_SYNC_PIXEL_SHADING;
    if (access & (PrGpuAccessFlag_ComputeRead | PrGpuAccessFlag_ComputeWrite))
        sync |= D3D12_BARRIER_SYNC_COMPUTE_SHADING;
    if (access & (PrGpuAccessFlag_CopyRead | PrGpuAccessFlag_CopyWrite))
        sync |= D3D12_BARRIER_SYNC_COPY;
    if (access & PrGpuAccessFlag_ColorAttachment)
        sync |= D3D12_BARRIER_SYNC_RENDER_TARGET;
    if (access & (PrGpuAccessFlag_DepthStencilRead | PrGpuAccessFlag_DepthStencilWrite))
        sync |= D3D12_BARRIER_SYNC_DEPTH_STENCIL;
    return sync;
}

// The memory accesses, or NO_ACCESS for none.
D3D12_BARRIER_ACCESS accessOf(PrGpuAccessFlags access) {
    D3D12_BARRIER_ACCESS a = D3D12_BARRIER_ACCESS_COMMON;
    if (access & PrGpuAccessFlag_IndexRead)
        a |= D3D12_BARRIER_ACCESS_INDEX_BUFFER;
    if (access & PrGpuAccessFlag_IndirectRead)
        a |= D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT;
    if (access & PrGpuAccessFlag_ShaderRead)
        a |= D3D12_BARRIER_ACCESS_SHADER_RESOURCE;
    if (access & PrGpuAccessFlag_ShaderWrite)
        a |= D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
    if (access & PrGpuAccessFlag_CopyRead)
        a |= D3D12_BARRIER_ACCESS_COPY_SOURCE;
    if (access & PrGpuAccessFlag_CopyWrite)
        a |= D3D12_BARRIER_ACCESS_COPY_DEST;
    if (access & PrGpuAccessFlag_ColorAttachment)
        a |= D3D12_BARRIER_ACCESS_RENDER_TARGET;
    if (access & PrGpuAccessFlag_DepthStencilRead)
        a |= D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ;
    if (access & PrGpuAccessFlag_DepthStencilWrite)
        a |= D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE;
    return a == D3D12_BARRIER_ACCESS_COMMON ? D3D12_BARRIER_ACCESS_NO_ACCESS : a;
}

// The layout of a texture in its accesses. A texture that shaders write is read through its unordered access view.
D3D12_BARRIER_LAYOUT layoutOf(PrGpuAccessFlags access) {
    if (access & PrGpuAccessFlag_Discard)
        return D3D12_BARRIER_LAYOUT_UNDEFINED;
    if (access & PrGpuAccessFlag_Present)
        return D3D12_BARRIER_LAYOUT_PRESENT;
    if (access & PrGpuAccessFlag_ColorAttachment)
        return D3D12_BARRIER_LAYOUT_RENDER_TARGET;
    if (access & PrGpuAccessFlag_DepthStencilWrite)
        return D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE;
    if (access & PrGpuAccessFlag_ShaderWrite)
        return D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS;
    if (access & PrGpuAccessFlag_CopyWrite)
        return D3D12_BARRIER_LAYOUT_COPY_DEST;
    if (access & PrGpuAccessFlag_DepthStencilRead)
        return D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ;
    bool shader = (access & PrGpuAccessFlag_ShaderRead) != 0;
    bool copy = (access & PrGpuAccessFlag_CopyRead) != 0;
    if (shader && copy)
        return D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_GENERIC_READ;
    if (copy)
        return D3D12_BARRIER_LAYOUT_COPY_SOURCE;
    return D3D12_BARRIER_LAYOUT_SHADER_RESOURCE;
}

// The accesses of a texture in `layout`, which leaves out those that the layout does not allow.
D3D12_BARRIER_ACCESS textureAccessOf(PrGpuAccessFlags access, D3D12_BARRIER_LAYOUT layout) {
    switch (layout) {
        case D3D12_BARRIER_LAYOUT_UNDEFINED:
        case D3D12_BARRIER_LAYOUT_PRESENT: return D3D12_BARRIER_ACCESS_NO_ACCESS;
        case D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS: return D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
        case D3D12_BARRIER_LAYOUT_RENDER_TARGET: return D3D12_BARRIER_ACCESS_RENDER_TARGET;
        case D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE:
            return D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE | D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ;
        case D3D12_BARRIER_LAYOUT_COPY_DEST: return D3D12_BARRIER_ACCESS_COPY_DEST;
        default: return accessOf(access & ~(PrGpuAccessFlag_IndexRead | PrGpuAccessFlag_IndirectRead));
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Slots, blocks and retirement

D3D12_CPU_DESCRIPTOR_HANDLE resourceDescriptor(const D3d12Device* d, uint32_t slot) {
    D3D12_CPU_DESCRIPTOR_HANDLE handle = d->resourceHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += size_t(slot) * d->resourceIncrement;
    return handle;
}

D3D12_CPU_DESCRIPTOR_HANDLE samplerDescriptor(const D3d12Device* d, uint32_t slot) {
    D3D12_CPU_DESCRIPTOR_HANDLE handle = d->samplerHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += size_t(slot) * d->samplerIncrement;
    return handle;
}

// Takes a free resource slot, or returns zero if none is left. The caller holds the mutex.
uint32_t takeSlotLocked(D3d12Device* d) {
    uint32_t slot = 0;
    if (d->freeResources.count != 0) {
        slot = d->freeResources.last();
        d->freeResources.pop();
    } else if (d->nextResource < kMaxResources) {
        slot = d->nextResource++;
    }
    if (slot != 0)
        d->resources[slot] = D3d12Resource{};
    return slot;
}

uint32_t takeSlot(D3d12Device* d) {
    prMutexLock(&d->mutex);
    uint32_t slot = takeSlotLocked(d);
    prMutexUnlock(&d->mutex);
    return slot;
}

// Frees a slot that was taken and never filled.
void returnSlot(D3d12Device* d, uint32_t slot) {
    prMutexLock(&d->mutex);
    d->resources[slot] = D3d12Resource{};
    d->freeResources.push(slot);
    prMutexUnlock(&d->mutex);
}

// Frees a resource slot, its storage slot and what they hold. The caller holds the mutex.
void freeResource(D3d12Device* d, uint32_t slot) {
    D3d12Resource& r = d->resources[slot];
    if (r.storage != 0) {
        d->resources[r.storage] = D3d12Resource{};
        d->freeResources.push(r.storage);
    }
    if (r.mapped != nullptr)
        r.resource->Unmap(0, nullptr);
    release(r.resource);
    r = D3d12Resource{};
    d->freeResources.push(slot);
}

void freePipeline(D3d12Device* d, uint32_t id) {
    D3d12Pipeline& p = d->pipelines[id];
    release(p.state);
    p = D3d12Pipeline{};
    d->freePipelines.push(id);
}

// Frees what the command buffers that are done held: retired objects, and the blocks, lists and timestamps of their
// recording. The caller holds the mutex.
void collect(D3d12Device* d) {
    uint64_t done = d->done.load();
    uint32_t kept = 0;
    for (uint32_t i = 0; i < d->inFlight.count; ++i) {
        const D3d12InFlight& f = d->inFlight.data[i];
        if (f.fence == 0 || f.fence > done)
            d->inFlight.data[kept++] = f;
    }
    d->inFlight.count = kept;
    uint64_t oldest = kept != 0 ? d->inFlight.data[0].serial : d->serial;
    kept = 0;
    for (uint32_t i = 0; i < d->retired.count; ++i) {
        const D3d12Retired& r = d->retired.data[i];
        if (r.serial > oldest) {
            d->retired.data[kept++] = r;
        } else if (r.kind == D3d12RetiredKind_Resource) {
            freeResource(d, r.slot);
        } else {
            freePipeline(d, r.slot);
        }
    }
    d->retired.count = kept;
    kept = 0;
    for (uint32_t i = 0; i < d->held.count; ++i) {
        D3d12Held& h = d->held.data[i];
        if (h.fence > done) {
            d->held.data[kept++] = h;
            continue;
        }
        switch (h.kind) {
            case D3d12HeldKind_StreamBlock:
                h.block.used = 0;
                d->streamBlocks.push(h.block);
                break;
            case D3d12HeldKind_StagingBlock:
                h.block.used = 0;
                d->stagingBlocks.push(h.block);
                break;
            case D3d12HeldKind_List:
                h.list.allocator->Reset();
                d->lists.push(h.list);
                break;
            default:
                d->freeTimestamps.push(h.timestamp);
                break;
        }
    }
    d->held.count = kept;
}

// Retires an object, which is freed once the command buffers that began before now are done.
void retire(D3d12Device* d, D3d12RetiredKind kind, uint32_t slot) {
    prMutexLock(&d->mutex);
    d->retired.push(D3d12Retired{d->serial, kind, slot});
    collect(d);
    prMutexUnlock(&d->mutex);
}

// Creates a committed buffer in `heap`. Returns null if the device has no memory for it.
ID3D12Resource* PR_NULLABLE createBuffer(D3d12Device* d, uint64_t size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags) {
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = heap;
    D3D12_RESOURCE_DESC1 desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    ID3D12Resource* buffer = nullptr;
    if (FAILED(d->device->CreateCommittedResource3(&properties, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&buffer))))
        return nullptr;
    return buffer;
}

// Writes a raw view of the buffer's `size` bytes, which shaders read as a ByteAddressBuffer, to `slot`, or an unordered
// access view of them if `storage`.
void writeBufferView(D3d12Device* d, ID3D12Resource* buffer, uint64_t size, uint32_t slot, bool storage) {
    if (storage) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC v{};
        v.Format = DXGI_FORMAT_R32_TYPELESS;
        v.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        v.Buffer.NumElements = UINT(size / 4);
        v.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        d->device->CreateUnorderedAccessView(buffer, nullptr, &v, resourceDescriptor(d, slot));
        return;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC v{};
    v.Format = DXGI_FORMAT_R32_TYPELESS;
    v.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    v.Buffer.NumElements = UINT(size / 4);
    v.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    d->device->CreateShaderResourceView(buffer, &v, resourceDescriptor(d, slot));
}

// Returns a block of upload memory of at least `size` bytes, from the free blocks or new, or one without a buffer if
// the device has no memory or slot for it. A block larger than `blockSize` is made for its size. The caller holds the
// mutex.
D3d12Block takeBlock(D3d12Device* d, RawArray<D3d12Block>& free, uint64_t blockSize, uint64_t size) {
    for (uint32_t i = 0; i < free.count; ++i) {
        if (free.data[i].length >= size) {
            D3d12Block block = free.data[i];
            free.erase(i);
            return block;
        }
    }
    uint64_t length = size > blockSize ? alignUp(size, kStreamAlignment) : blockSize;
    ID3D12Resource* buffer = createBuffer(d, length, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE);
    if (buffer == nullptr)
        return D3d12Block{};
    void* mapped = nullptr;
    D3D12_RANGE none{0, 0};
    uint32_t slot = SUCCEEDED(buffer->Map(0, &none, &mapped)) ? takeSlotLocked(d) : 0;
    if (slot == 0) {
        buffer->Release();
        return D3d12Block{};
    }
    D3d12Resource& r = d->resources[slot];
    r.kind = D3d12Slot_Buffer;
    r.buffer = PrGpuBufferDesc{.size = length, .memory = PrGpuMemory_Upload};
    r.resource = buffer;
    r.mapped = static_cast<uint8_t*>(mapped);
    r.address = buffer->GetGPUVirtualAddress();
    writeBufferView(d, buffer, length, slot, false);
    return D3d12Block{buffer, r.mapped, length, slot, 0};
}

ID3D12Resource* resourceOf(const D3d12Device* d, uint32_t slot) {
    PR_ASSERT(slot < kMaxResources && d->resources[slot].resource != nullptr);
    return d->resources[slot].resource;
}

const D3d12Resource& textureOf(const D3d12Device* d, uint32_t slot) {
    PR_ASSERT(slot < kMaxResources && d->resources[slot].kind == D3d12Slot_Texture);
    return d->resources[slot];
}

// Returns a list and its allocator, open, from the free lists or new. Returns a null list if the device cannot make
// them. The caller holds the mutex.
D3d12List takeList(D3d12Device* d) {
    D3d12List l{};
    if (d->lists.count != 0) {
        l = d->lists.last();
        d->lists.pop();
    } else {
        if (FAILED(d->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&l.allocator))))
            return D3d12List{};
        if (FAILED(d->device->CreateCommandList1(0, D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_FLAG_NONE,
                IID_PPV_ARGS(&l.list)))) {
            l.allocator->Release();
            return D3d12List{};
        }
    }
    l.list->Reset(l.allocator, nullptr);
    return l;
}

// Hands a submission to the completion thread. The caller holds the mutex, which orders the submissions by fence.
void addSubmission(D3d12Device* d, const D3d12Submission& submission) {
    prMutexLock(&d->doneMutex);
    d->pending.push(submission);
    prConditionSignal(&d->pendingSignal);
    prMutexUnlock(&d->doneMutex);
}

// Executes a closed list on the queue, and returns the fence that it completes at. The caller holds the mutex.
PrGpuFence execute(D3d12Device* d, ID3D12GraphicsCommandList7* list) {
    ID3D12CommandList* lists[1] = {list};
    d->queue->ExecuteCommandLists(1, lists);
    PrGpuFence fence = ++d->submitted;
    d->queue->Signal(d->fence, fence);
    return fence;
}

// ---------------------------------------------------------------------------------------------------------------------
// The device

// Returns true if the device is lost, and notes it.
bool isLostNow(const D3d12Device* d) {
    if (d->lost.load())
        return true;
    if (d->device->GetDeviceRemovedReason() == S_OK)
        return false;
    const_cast<D3d12Device*>(d)->lost.store(true);
    return true;
}

bool isLost(const PrGpuDevice* device) {
    return isLostNow(d3d12Of(device));
}

// Removes the device through ID3D12Device5, as D3D12 removes a device after a hang, so that the loss takes the path
// that a real one takes.
void simulateLoss(PrGpuDevice* device) {
    D3d12Device* d = d3d12Of(device);
    d->device->RemoveDevice();
    d->lost.store(true);
}

bool isComplete(const PrGpuDevice* device, PrGpuFence fence) {
    const D3d12Device* d = d3d12Of(device);
    return isLostNow(d) || d->done.load() >= fence;
}

void wait(PrGpuDevice* device, PrGpuFence fence) {
    D3d12Device* d = d3d12Of(device);
    prMutexLock(&d->doneMutex);
    while (d->done.load() < fence)
        prConditionWait(&d->doneSignal, &d->doneMutex);
    prMutexUnlock(&d->doneMutex);
    prMutexLock(&d->mutex);
    collect(d);
    prMutexUnlock(&d->mutex);
}

void submitUploads(D3d12Device* d);

void waitIdle(PrGpuDevice* device) {
    D3d12Device* d = d3d12Of(device);
    prMutexLock(&d->mutex);
    submitUploads(d);
    PrGpuFence fence = d->submitted;
    prMutexUnlock(&d->mutex);
    wait(device, fence);
}

// Waits for each submission's fence in order, then calls its functions with the seconds that its timestamps measured,
// and raises the count of fences done.
void serveCompletions(void* PR_NULLABLE context) {
    D3d12Device* d = static_cast<D3d12Device*>(context);
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    prMutexLock(&d->doneMutex);
    for (;;) {
        while (d->pending.count == 0 && !d->stopping)
            prConditionWait(&d->pendingSignal, &d->doneMutex);
        if (d->pending.count == 0)
            break;
        D3d12Submission s = d->pending.data[0];
        d->pending.erase(0);
        prMutexUnlock(&d->doneMutex);
        while (d->fence->GetCompletedValue() < s.fence && !isLostNow(d)) {
            if (FAILED(d->fence->SetEventOnCompletion(s.fence, event)))
                break;
            WaitForSingleObject(event, kLostCheckMilliseconds);
        }
        double seconds = 0;
        if (!isLostNow(d) && s.timestamp != kTimestampPairs && d->ticksPerSecond > 0) {
            const uint64_t* ticks = d->timestampTicks + size_t(s.timestamp) * 2;
            seconds = ticks[1] > ticks[0] ? double(ticks[1] - ticks[0]) / d->ticksPerSecond : 0;
        }
        for (uint32_t i = 0; i < s.doneCount; ++i)
            s.dones[i].done(s.dones[i].context, seconds);
        prMemoryFree(s.dones);
        prMutexLock(&d->doneMutex);
        if (d->done.load() < s.fence)
            d->done.store(s.fence);
        prConditionBroadcast(&d->doneSignal);
    }
    prMutexUnlock(&d->doneMutex);
    CloseHandle(event);
}

bool supportsFormat(const PrGpuDevice* device, PrGpuFormat format, PrGpuTextureUsageFlags usage) {
    const D3d12Device* d = d3d12Of(device);
    if (formatOf(format) == DXGI_FORMAT_UNKNOWN)
        return false;
    bool compressed = format >= PrGpuFormat_Bc1;
    if (compressed && (usage & ~PrGpuTextureUsageFlag_Sampled))
        return false;
    bool depth = prGpuFormatIsDepthStencil(format);
    if ((usage & PrGpuTextureUsageFlag_DepthStencil) && !depth)
        return false;
    if ((usage & (PrGpuTextureUsageFlag_RenderTarget | PrGpuTextureUsageFlag_Storage)) && depth)
        return false;
    D3D12_FEATURE_DATA_FORMAT_SUPPORT shader{shaderFormatOf(format)};
    D3D12_FEATURE_DATA_FORMAT_SUPPORT target{formatOf(format)};
    D3D12_FEATURE_DATA_FORMAT_SUPPORT storage{storageFormatOf(format)};
    if (FAILED(d->device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &shader, sizeof(shader))) ||
        FAILED(d->device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &target, sizeof(target))) ||
        FAILED(d->device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &storage, sizeof(storage))))
    return false;
    if ((usage & PrGpuTextureUsageFlag_Sampled) && !(shader.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_LOAD))
        return false;
    if ((usage & PrGpuTextureUsageFlag_RenderTarget) && !(target.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET))
        return false;
    if ((usage & PrGpuTextureUsageFlag_DepthStencil) && !(target.Support1 & D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL))
        return false;
    if ((usage & PrGpuTextureUsageFlag_Storage) &&
            !(storage.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW))
        return false;
    return true;
}

void destroyDevice(PrGpuDevice* device) {
    D3d12Device* d = d3d12Of(device);
    gpuSharedDeviceReset(d);
    prMutexLock(&d->doneMutex);
    d->stopping = true;
    prConditionSignal(&d->pendingSignal);
    prMutexUnlock(&d->doneMutex);
    prThreadJoin(&d->completion);
    prMutexLock(&d->mutex);
    collect(d);
    for (uint32_t i = 0; i < d->held.count; ++i) {
        if (d->held.data[i].kind == D3d12HeldKind_List) {
            release(d->held.data[i].list.allocator);
            release(d->held.data[i].list.list);
        }
    }
    for (uint32_t i = 1; i < d->nextResource; ++i) {
        D3d12Resource& r = d->resources[i];
        if (r.mapped != nullptr)
            r.resource->Unmap(0, nullptr);
        release(r.resource);
    }
    for (uint32_t i = 1; i < d->nextPipeline; ++i)
        release(d->pipelines[i].state);
    for (uint32_t i = 0; i < d->lists.count; ++i) {
        release(d->lists.data[i].allocator);
        release(d->lists.data[i].list);
    }
    for (uint32_t i = 0; i < d->attachmentHeaps.count; ++i) {
        release(d->attachmentHeaps.data[i].color);
        release(d->attachmentHeaps.data[i].depth);
    }
    prMutexUnlock(&d->mutex);
    prMemoryFree(d->resources);
    prMemoryFree(d->pipelines);
    d->freeResources.reset();
    d->freePipelines.reset();
    d->freeTimestamps.reset();
    d->inFlight.reset();
    d->retired.reset();
    d->held.reset();
    d->lists.reset();
    d->attachmentHeaps.reset();
    d->streamBlocks.reset();
    d->stagingBlocks.reset();
    d->uploadBlocks.reset();
    d->pending.reset();
    uploadBatchReset(&d->uploadBatch);
    release(d->drawSignature);
    release(d->drawIndexedSignature);
    release(d->dispatchSignature);
    release(d->zeros);
    release(d->timestamps);
    release(d->timestampReadback);
    release(d->resourceHeap);
    release(d->samplerHeap);
    release(d->rootSignature);
    release(d->fence);
    release(d->factory);
    release(d->queue);
    release(d->device);
    prMutexReset(&d->mutex);
    prConditionReset(&d->pendingSignal);
    prConditionReset(&d->doneSignal);
    prMutexReset(&d->doneMutex);
    prMemoryFree(d);
}

// ---------------------------------------------------------------------------------------------------------------------
// Uploads

// Opens the batch of uploads if it is not open. Returns false if the device cannot make its list. The caller holds the
// mutex.
bool openUploads(D3d12Device* d) {
    if (d->uploads.list != nullptr)
        return true;
    d->uploads = takeList(d);
    if (d->uploads.list == nullptr)
        return false;
    d->uploadSerial = d->serial++;
    d->inFlight.push(D3d12InFlight{d->uploadSerial, 0});
    return true;
}

// Records a barrier on a texture's subresources.
void textureBarrier(ID3D12GraphicsCommandList7* list, const D3d12Resource& r, D3D12_BARRIER_SUBRESOURCE_RANGE range,
        PrGpuAccessFlags before, PrGpuAccessFlags after) {
    D3D12_TEXTURE_BARRIER b{};
    b.LayoutBefore = layoutOf(before);
    b.LayoutAfter = layoutOf(after);
    b.AccessBefore = textureAccessOf(before, b.LayoutBefore);
    b.AccessAfter = textureAccessOf(after, b.LayoutAfter);
    // A discard waits for the accesses that it names. One that names none waits for all the work before it, because
    // D3D12 requires a wait for work earlier in the command list that touched the texture.
    b.SyncBefore = syncOf(before);
    if (before & PrGpuAccessFlag_Discard) {
        if (b.SyncBefore == D3D12_BARRIER_SYNC_NONE)
            b.SyncBefore = D3D12_BARRIER_SYNC_ALL;
    } else if (b.AccessBefore == D3D12_BARRIER_ACCESS_NO_ACCESS) {
        b.SyncBefore = D3D12_BARRIER_SYNC_NONE;
    }
    b.SyncAfter = b.AccessAfter == D3D12_BARRIER_ACCESS_NO_ACCESS ? D3D12_BARRIER_SYNC_NONE : syncOf(after);
    b.pResource = r.resource;
    b.Subresources = range;
    if ((before & PrGpuAccessFlag_Discard) && (b.LayoutAfter == D3D12_BARRIER_LAYOUT_RENDER_TARGET ||
            b.LayoutAfter == D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE))
        b.Flags = D3D12_TEXTURE_BARRIER_FLAG_DISCARD;
    D3D12_BARRIER_GROUP group{};
    group.Type = D3D12_BARRIER_TYPE_TEXTURE;
    group.NumBarriers = 1;
    group.pTextureBarriers = &b;
    list->Barrier(1, &group);
}

// The subresources of a texture or view that `s` names.
D3D12_BARRIER_SUBRESOURCE_RANGE rangeOf(const D3d12Resource& r, PrGpuSubresources s) {
    bool is3d = r.texture.kind == PrGpuTextureKind_3d;
    uint32_t levels = s.levels != 0 ? s.levels : r.texture.levels - s.firstLevel;
    uint32_t layers = is3d ? 1 : (s.layers != 0 ? s.layers : r.texture.extent.depth - s.firstLayer);
    D3D12_BARRIER_SUBRESOURCE_RANGE range{};
    range.IndexOrFirstMipLevel = r.firstLevel + s.firstLevel;
    range.NumMipLevels = levels;
    range.FirstArraySlice = is3d ? 0 : r.firstLayer + s.firstLayer;
    range.NumArraySlices = layers;
    range.FirstPlane = 0;
    range.NumPlanes = r.texture.format == PrGpuFormat_D32FS8 ? 2 : 1;
    return range;
}

// Records a barrier on all memory.
void globalBarrier(ID3D12GraphicsCommandList7* list, PrGpuAccessFlags before, PrGpuAccessFlags after) {
    D3D12_GLOBAL_BARRIER b{};
    b.AccessBefore = accessOf(before);
    b.AccessAfter = accessOf(after);
    b.SyncBefore = b.AccessBefore == D3D12_BARRIER_ACCESS_NO_ACCESS ? D3D12_BARRIER_SYNC_NONE : syncOf(before);
    b.SyncAfter = b.AccessAfter == D3D12_BARRIER_ACCESS_NO_ACCESS ? D3D12_BARRIER_SYNC_NONE : syncOf(after);
    if (b.SyncBefore == D3D12_BARRIER_SYNC_NONE && b.SyncAfter == D3D12_BARRIER_SYNC_NONE)
        return;
    D3D12_BARRIER_GROUP group{};
    group.Type = D3D12_BARRIER_TYPE_GLOBAL;
    group.NumBarriers = 1;
    group.pGlobalBarriers = &b;
    list->Barrier(1, &group);
}

// Executes the open batch of uploads, ahead of whatever is submitted after it. Its copies into buffers are disjoint, so
// they go at once. The caller holds the mutex.
void submitUploads(D3d12Device* d) {
    if (d->uploads.list == nullptr)
        return;
    ID3D12GraphicsCommandList7* list = d->uploads.list;
    const RawArray<GpuSharedUploadCopy>& copies = d->uploadBatch.copies;
    for (uint32_t i = 0; i < copies.count; ++i) {
        const GpuSharedUploadCopy& c = copies.data[i];
        if (c.size != 0)
            list->CopyBufferRegion(resourceOf(d, c.buffer), c.target, d->uploadBlocks.data[c.block].buffer, c.source, c.size);
    }
    for (uint32_t i = 0; i < d->uploadBatch.written.count; ++i) {
        const D3d12Resource& r = d->resources[d->uploadBatch.written.data[i]];
        textureBarrier(list, r, rangeOf(r, PrGpuSubresources{}), PrGpuAccessFlag_CopyWrite, PrGpuAccessFlag_ShaderRead);
    }
    list->Close();
    PrGpuFence fence = execute(d, list);
    addSubmission(d, D3d12Submission{fence, nullptr, 0, kTimestampPairs});
    for (uint32_t i = 0; i < d->inFlight.count; ++i) {
        if (d->inFlight.data[i].serial == d->uploadSerial)
            d->inFlight.data[i].fence = fence;
    }
    for (uint32_t i = 0; i < d->uploadBlocks.count; ++i)
        d->held.push(D3d12Held{fence, D3d12HeldKind_StagingBlock, d->uploadBlocks.data[i], {}, 0});
    d->held.push(D3d12Held{fence, D3d12HeldKind_List, {}, d->uploads, 0});
    d->uploadBlocks.clear();
    uploadBatchClear(&d->uploadBatch);
    d->uploads = D3d12List{};
}

// Returns room for `size` bytes at `alignment` in the batch's staging, as the block's index in the batch and the offset
// in it. Returns false if the device has no memory for it. The caller holds the mutex.
bool stage(D3d12Device* d, uint64_t size, uint64_t alignment, uint32_t* block, uint64_t* offset) {
    D3d12Block* last = d->uploadBlocks.count != 0 ? &d->uploadBlocks.last() : nullptr;
    uint64_t at = last != nullptr ? alignUp(last->used, alignment) : 0;
    if (last == nullptr || at + size > last->length) {
        if (d->uploadBlocks.count >= kStagingFlushBlocks)
            submitUploads(d);
        collect(d);
        D3d12Block added = takeBlock(d, d->stagingBlocks, kStagingBlock, size);
        if (added.buffer == nullptr)
            return false;
        d->uploadBlocks.push(added);
        last = &d->uploadBlocks.last();
        at = 0;
    }
    if (!openUploads(d))
        return false;
    last->used = at + size;
    *block = d->uploadBlocks.count - 1;
    *offset = at;
    return true;
}

void bufferUpload(PrGpuDevice* device, PrGpuBuffer buffer, uint64_t offset, const void* data, uint64_t size) {
    D3d12Device* d = d3d12Of(device);
    prMutexLock(&d->mutex);
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    while (size != 0) {
        uint64_t n = size < kStagingBlock ? size : kStagingBlock;
        uint32_t block = 0;
        uint64_t at = 0;
        if (!stage(d, n, 16, &block, &at))
            break;
        prMemCopy(d->uploadBlocks.data[block].mapped + at, bytes, n);
        gpuSharedUploadAddCopy(&d->uploadBatch, GpuSharedUploadCopy{buffer.id, block, offset, at, n});
        bytes += n;
        offset += n;
        size -= n;
    }
    prMutexUnlock(&d->mutex);
}

// Returns the index of a subresource of a texture or view.
UINT subresourceOf(const D3d12Resource& r, uint32_t level, uint32_t layer) {
    uint32_t mip = r.firstLevel + level;
    uint32_t slice = r.texture.kind == PrGpuTextureKind_3d ? 0 : r.firstLayer + layer;
    return UINT(mip + slice * r.resourceLevels);
}

void textureUpload(PrGpuDevice* device, PrGpuTexture texture, const PrGpuTextureRegion* region, const void* data,
        uint32_t rowBytes, uint32_t layerBytes) {
    D3d12Device* d = d3d12Of(device);
    const D3d12Resource& r = textureOf(d, texture.id);
    PrGpuFormatBlock b = prGpuFormatBlock(r.texture.format);
    uint32_t rows = (region->extent.height + b.height - 1) / b.height;
    uint32_t row = (region->extent.width + b.width - 1) / b.width * b.bytes;
    uint32_t pitch = alignUp(row, kPitchAlignment);
    uint32_t slices = region->extent.depth;
    uint64_t sliceBytes = alignUp(uint64_t(pitch) * rows, kPlacementAlignment);
    prMutexLock(&d->mutex);
    // Staging holds the texels with rows at D3D12's pitch. An upload larger than a block is staged in a block of its
    // size.
    uint32_t block = 0;
    uint64_t at = 0;
    if (!stage(d, sliceBytes * slices, kPlacementAlignment, &block, &at)) {
        prMutexUnlock(&d->mutex);
        return;
    }
    const D3d12Block& staging = d->uploadBlocks.data[block];
    uint8_t* to = staging.mapped + at;
    const uint8_t* from = static_cast<const uint8_t*>(data);
    for (uint32_t s = 0; s < slices; ++s) {
        for (uint32_t y = 0; y < rows; ++y) {
            uint8_t* rowTo = to + s * sliceBytes + uint64_t(y) * pitch;
            const uint8_t* rowFrom = from + uint64_t(s) * layerBytes + uint64_t(y) * rowBytes;
            prMemCopy(rowTo, rowFrom, row);
        }
    }
    ID3D12GraphicsCommandList7* list = d->uploads.list;
    if (d->uploadBatch.written.find(texture.id) == kArrayMissing) {
        d->uploadBatch.written.push(texture.id);
        textureBarrier(list, r, rangeOf(r, PrGpuSubresources{}), PrGpuAccessFlag_ShaderRead, PrGpuAccessFlag_CopyWrite);
    }
    if (gpuSharedUploadNeedsTextureBarrier(&d->uploadBatch, texture.id))
        globalBarrier(list, PrGpuAccessFlag_CopyWrite, PrGpuAccessFlag_CopyWrite);
    bool is3d = r.texture.kind == PrGpuTextureKind_3d;
    uint32_t images = is3d ? 1 : slices;
    for (uint32_t i = 0; i < images; ++i) {
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = staging.buffer;
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint.Offset = at + i * sliceBytes;
        source.PlacedFootprint.Footprint.Format = copyFormatOf(r);
        source.PlacedFootprint.Footprint.Width = alignUp(region->extent.width, uint32_t(b.width));
        source.PlacedFootprint.Footprint.Height = alignUp(region->extent.height, uint32_t(b.height));
        source.PlacedFootprint.Footprint.Depth = is3d ? slices : 1;
        source.PlacedFootprint.Footprint.RowPitch = pitch;
        D3D12_TEXTURE_COPY_LOCATION target{};
        target.pResource = r.resource;
        target.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        target.SubresourceIndex = subresourceOf(r, region->level, is3d ? 0 : region->firstLayer + i);
        list->CopyTextureRegion(&target, region->x, region->y, is3d ? region->z : 0, &source, nullptr);
    }
    prMutexUnlock(&d->mutex);
}

// ---------------------------------------------------------------------------------------------------------------------
// Buffers

PrStatus bufferCreate(PrGpuDevice* device, const PrGpuBufferDesc* desc, PrGpuBuffer* out) {
    D3d12Device* d = d3d12Of(device);
    D3D12_HEAP_TYPE heap = D3D12_HEAP_TYPE_DEFAULT;
    if (desc->memory == PrGpuMemory_Upload)
        heap = D3D12_HEAP_TYPE_UPLOAD;
    else if (desc->memory == PrGpuMemory_Readback)
        heap = D3D12_HEAP_TYPE_READBACK;
    bool storage = (desc->usage & PrGpuBufferUsageFlag_Storage) != 0;
    uint64_t size = alignUp(desc->size, 16);
    // D3D12 zeroes a new committed resource's memory.
    D3D12_RESOURCE_FLAGS flags = storage ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ID3D12Resource* buffer = createBuffer(d, size, heap, flags);
    if (buffer == nullptr)
        return PrStatus_Limit;
    nameObject(buffer, desc->name);
    void* mapped = nullptr;
    if (heap != D3D12_HEAP_TYPE_DEFAULT && FAILED(buffer->Map(0, nullptr, &mapped))) {
        buffer->Release();
        return PrStatus_Limit;
    }
    prMutexLock(&d->mutex);
    uint32_t slot = takeSlotLocked(d);
    uint32_t storageSlot = slot != 0 && storage ? takeSlotLocked(d) : 0;
    if (slot != 0 && storage && storageSlot == 0) {
        d->freeResources.push(slot);
        slot = 0;
    }
    prMutexUnlock(&d->mutex);
    if (slot == 0) {
        buffer->Release();
        return PrStatus_Limit;
    }
    D3d12Resource& r = d->resources[slot];
    r.kind = D3d12Slot_Buffer;
    r.buffer = *desc;
    r.buffer.name = PrStringView{};
    r.resource = buffer;
    r.storage = storageSlot;
    r.mapped = static_cast<uint8_t*>(mapped);
    r.address = buffer->GetGPUVirtualAddress();
    writeBufferView(d, buffer, size, slot, false);
    if (storageSlot != 0) {
        d->resources[storageSlot].kind = D3d12Slot_Storage;
        writeBufferView(d, buffer, size, storageSlot, true);
    }
    out->id = slot;
    return PrStatus_Ok;
}

void bufferDestroy(PrGpuDevice* device, PrGpuBuffer buffer) {
    retire(d3d12Of(device), D3d12RetiredKind_Resource, buffer.id);
}

PrGpuBufferDesc bufferDesc(const PrGpuDevice* device, PrGpuBuffer buffer) {
    return d3d12Of(device)->resources[buffer.id].buffer;
}

// Shaders write a buffer through its unordered access view, in a slot of its own.
uint32_t bufferStorageHandle(const PrGpuDevice* device, PrGpuBuffer buffer) {
    return d3d12Of(device)->resources[buffer.id].storage;
}

void* bufferMap(const PrGpuDevice* device, PrGpuBuffer buffer) {
    const D3d12Resource& r = d3d12Of(device)->resources[buffer.id];
    PR_ASSERT(r.kind == D3d12Slot_Buffer && r.mapped != nullptr);
    return r.mapped;
}

// ---------------------------------------------------------------------------------------------------------------------
// Textures

// Writes the shader resource view of a texture or view to its slot, and the unordered access view of its first level to
// its storage slot, if it has one.
void writeTextureViews(D3d12Device* d, uint32_t slot) {
    const D3d12Resource& r = d->resources[slot];
    const PrGpuTextureDesc& t = r.texture;
    bool array = r.resourceLayers > 1 || r.firstLayer != 0;
    D3D12_SHADER_RESOURCE_VIEW_DESC s{};
    s.Format = shaderFormatOf(t.format);
    s.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    switch (t.kind) {
        case PrGpuTextureKind_3d:
            s.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
            s.Texture3D.MostDetailedMip = r.firstLevel;
            s.Texture3D.MipLevels = t.levels;
            break;
        case PrGpuTextureKind_Cube:
        case PrGpuTextureKind_CubeArray:
            if (t.kind == PrGpuTextureKind_Cube && r.firstLayer == 0) {
                s.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
                s.TextureCube.MostDetailedMip = r.firstLevel;
                s.TextureCube.MipLevels = t.levels;
            } else {
                s.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
                s.TextureCubeArray.MostDetailedMip = r.firstLevel;
                s.TextureCubeArray.MipLevels = t.levels;
                s.TextureCubeArray.First2DArrayFace = r.firstLayer;
                s.TextureCubeArray.NumCubes = t.extent.depth / 6;
            }
            break;
        default:
            if (t.samples > 1) {
                if (array || t.kind == PrGpuTextureKind_2dArray) {
                    s.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
                    s.Texture2DMSArray.FirstArraySlice = r.firstLayer;
                    s.Texture2DMSArray.ArraySize = t.extent.depth;
                } else {
                    s.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
                }
            } else if (array || t.kind == PrGpuTextureKind_2dArray) {
                s.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                s.Texture2DArray.MostDetailedMip = r.firstLevel;
                s.Texture2DArray.MipLevels = t.levels;
                s.Texture2DArray.FirstArraySlice = r.firstLayer;
                s.Texture2DArray.ArraySize = t.extent.depth;
            } else {
                s.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                s.Texture2D.MostDetailedMip = r.firstLevel;
                s.Texture2D.MipLevels = t.levels;
            }
            break;
    }
    d->device->CreateShaderResourceView(r.resource, &s, resourceDescriptor(d, slot));
    if (r.storage == 0)
        return;
    D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
    u.Format = storageFormatOf(t.format);
    if (t.kind == PrGpuTextureKind_3d) {
        u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        u.Texture3D.MipSlice = r.firstLevel;
        u.Texture3D.WSize = UINT(-1);
    } else if (array || t.kind != PrGpuTextureKind_2d) {
        u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
        u.Texture2DArray.MipSlice = r.firstLevel;
        u.Texture2DArray.FirstArraySlice = r.firstLayer;
        u.Texture2DArray.ArraySize = t.extent.depth;
    } else {
        u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        u.Texture2D.MipSlice = r.firstLevel;
    }
    d->device->CreateUnorderedAccessView(r.resource, nullptr, &u, resourceDescriptor(d, r.storage));
}

// Puts a texture's resource into new slots, as `desc` describes it, and writes its views. Returns zero, and releases
// nothing, if no slot is left.
uint32_t addTexture(D3d12Device* d, ID3D12Resource* resource, const PrGpuTextureDesc& desc, DXGI_FORMAT format,
        uint8_t resourceLevels, uint16_t resourceLayers, uint8_t firstLevel, uint16_t firstLayer) {
    bool storage = (desc.usage & PrGpuTextureUsageFlag_Storage) != 0;
    prMutexLock(&d->mutex);
    uint32_t slot = takeSlotLocked(d);
    uint32_t storageSlot = slot != 0 && storage ? takeSlotLocked(d) : 0;
    if (slot != 0 && storage && storageSlot == 0) {
        d->freeResources.push(slot);
        slot = 0;
    }
    prMutexUnlock(&d->mutex);
    if (slot == 0)
        return 0;
    D3d12Resource& r = d->resources[slot];
    r.kind = D3d12Slot_Texture;
    r.texture = desc;
    r.texture.name = PrStringView{};
    r.resource = resource;
    r.storage = storageSlot;
    r.format = format;
    r.resourceLevels = resourceLevels;
    r.resourceLayers = resourceLayers;
    r.firstLevel = firstLevel;
    r.firstLayer = firstLayer;
    if (storageSlot != 0)
        d->resources[storageSlot].kind = D3d12Slot_Storage;
    writeTextureViews(d, slot);
    return slot;
}

PrStatus textureCreate(PrGpuDevice* device, const PrGpuTextureDesc* desc, PrGpuTexture* out) {
    D3d12Device* d = d3d12Of(device);
    DXGI_FORMAT format = resourceFormatOf(desc->format, desc->usage);
    if (format == DXGI_FORMAT_UNKNOWN)
        return PrStatus_NotSupported;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC1 rd{};
    rd.Dimension =
        desc->kind == PrGpuTextureKind_3d ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = desc->extent.width;
    rd.Height = desc->extent.height;
    rd.DepthOrArraySize = UINT16(desc->extent.depth);
    rd.MipLevels = desc->levels;
    rd.Format = format;
    rd.SampleDesc.Count = desc->samples;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    if (desc->usage & PrGpuTextureUsageFlag_RenderTarget)
        rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (desc->usage & PrGpuTextureUsageFlag_DepthStencil)
        rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    if (desc->usage & PrGpuTextureUsageFlag_Storage)
        rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource* resource = nullptr;
    if (FAILED(d->device->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_BARRIER_LAYOUT_SHADER_RESOURCE, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&resource))))
        return PrStatus_Limit;
    nameObject(resource, desc->name);
    uint16_t layers = desc->kind == PrGpuTextureKind_3d ? 1 : uint16_t(desc->extent.depth);
    uint32_t slot = addTexture(d, resource, *desc, format, desc->levels, layers, 0, 0);
    if (slot == 0) {
        resource->Release();
        return PrStatus_Limit;
    }
    out->id = slot;
    return PrStatus_Ok;
}

PrStatus textureCreateView(PrGpuDevice* device, PrGpuTexture texture, PrGpuTextureKind kind,
        PrGpuSubresources subresources, PrGpuTexture* out) {
    D3d12Device* d = d3d12Of(device);
    const D3d12Resource& t = textureOf(d, texture.id);
    PrGpuTextureDesc desc = t.texture;
    uint8_t levels = subresources.levels != 0 ? subresources.levels : uint8_t(desc.levels - subresources.firstLevel);
    uint32_t layerCount = desc.kind == PrGpuTextureKind_3d ? 1 : desc.extent.depth;
    uint16_t layers = subresources.layers != 0 ? subresources.layers : uint16_t(layerCount - subresources.firstLayer);
    PrGpuTextureDesc v = desc;
    v.kind = kind;
    v.levels = levels;
    uint32_t width = desc.extent.width >> subresources.firstLevel;
    uint32_t height = desc.extent.height >> subresources.firstLevel;
    v.extent.width = width != 0 ? width : 1;
    v.extent.height = height != 0 ? height : 1;
    v.extent.depth = desc.kind == PrGpuTextureKind_3d ? desc.extent.depth : layers;
    t.resource->AddRef();
    uint32_t slot = addTexture(d, t.resource, v, t.format, t.resourceLevels, t.resourceLayers,
        uint8_t(t.firstLevel + subresources.firstLevel), uint16_t(t.firstLayer + subresources.firstLayer));
    if (slot == 0) {
        t.resource->Release();
        return PrStatus_Limit;
    }
    out->id = slot;
    return PrStatus_Ok;
}

void textureDestroy(PrGpuDevice* device, PrGpuTexture texture) {
    gpuSharedTextureForget(d3d12Of(device), texture);
    retire(d3d12Of(device), D3d12RetiredKind_Resource, texture.id);
}

PrGpuTextureDesc textureDesc(const PrGpuDevice* device, PrGpuTexture texture) {
    return d3d12Of(device)->resources[texture.id].texture;
}

// Shaders read and write a texture through its unordered access view, in a slot of its own.
uint32_t textureStorageHandle(const PrGpuDevice* device, PrGpuTexture texture) {
    return d3d12Of(device)->resources[texture.id].storage;
}

// ---------------------------------------------------------------------------------------------------------------------
// Samplers

PrStatus samplerCreate(PrGpuDevice* device, const PrGpuSamplerDesc* desc, PrGpuSampler* out) {
    D3d12Device* d = d3d12Of(device);
    D3D12_SAMPLER_DESC s{};
    bool compare = desc->compare != PrGpuCompare_Never;
    D3D12_FILTER_REDUCTION_TYPE reduction =
        compare ? D3D12_FILTER_REDUCTION_TYPE_COMPARISON : D3D12_FILTER_REDUCTION_TYPE_STANDARD;
    if (desc->maxAnisotropy > 1) {
        s.Filter = D3D12_ENCODE_ANISOTROPIC_FILTER(reduction);
        s.MaxAnisotropy = desc->maxAnisotropy;
    } else {
        D3D12_FILTER_TYPE minify =
            desc->minify == PrGpuFilter_Linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
        D3D12_FILTER_TYPE magnify =
            desc->magnify == PrGpuFilter_Linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
        D3D12_FILTER_TYPE mip = desc->mip == PrGpuFilter_Linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
        s.Filter = D3D12_ENCODE_BASIC_FILTER(minify, magnify, mip, reduction);
        s.MaxAnisotropy = 1;
    }
    s.AddressU = addressOf(desc->address[0]);
    s.AddressV = addressOf(desc->address[1]);
    s.AddressW = addressOf(desc->address[2]);
    s.MipLODBias = desc->lodBias;
    s.ComparisonFunc = compare ? compareOf(desc->compare) : D3D12_COMPARISON_FUNC_NEVER;
    s.MinLOD = desc->minLod;
    s.MaxLOD = desc->maxLod;
    prMutexLock(&d->mutex);
    uint32_t slot = d->nextSampler < kMaxSamplers ? d->nextSampler++ : 0;
    prMutexUnlock(&d->mutex);
    if (slot == 0)
        return PrStatus_Limit;
    d->device->CreateSampler(&s, samplerDescriptor(d, slot));
    out->id = slot;
    return PrStatus_Ok;
}

// ---------------------------------------------------------------------------------------------------------------------
// Shader libraries and pipelines

// The entries hold each entry point's DXIL, which a pipeline compiles from as it is made.
PrStatus shaderLibraryCreate(PrGpuDevice* device, PrGpuShaderEntry* entries, uint32_t entryCount,
        PrGpuShaderLibrary* PR_NULLABLE* PR_NONNULL out) {
    if (entryCount == 0)
        return PrStatus_InvalidData;
    D3d12Library* library = allocZeroed<D3d12Library>();
    static_cast<PrGpuShaderLibrary&>(*library) = kLibraryFunctions;
    library->device = device;
    library->entryCount = entryCount;
    library->entries = entries;
    *out = library;
    return PrStatus_Ok;
}

void shaderLibraryDestroy(PrGpuShaderLibrary* library) {
    D3d12Library* l = static_cast<D3d12Library*>(library);
    prMemoryFree(l->entries);
    prMemoryFree(l);
}

// Puts a pipeline into a new id, or returns zero if none is left.
uint32_t addPipeline(D3d12Device* d, const D3d12Pipeline& pipeline) {
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

D3D12_SHADER_BYTECODE bytecodeOf(const PrGpuShaderEntry* entry) {
    return D3D12_SHADER_BYTECODE{entry->code, SIZE_T(entry->codeSize)};
}

D3D12_DEPTH_STENCILOP_DESC stencilFaceOf(const PrGpuStencilFace& face) {
    return D3D12_DEPTH_STENCILOP_DESC{stencilOpOf(face.fail), stencilOpOf(face.depthFail), stencilOpOf(face.pass),
        compareOf(face.compare)};
}

PrStatus graphicsPipelineCreate(PrGpuDevice* device, const PrGpuGraphicsPipelineDesc* desc,
        const PrGpuShaderEntry* vertex, const PrGpuShaderEntry* PR_NULLABLE fragment, PrGpuPipeline* out) {
    D3d12Device* d = d3d12Of(device);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
    p.pRootSignature = d->rootSignature;
    p.VS = bytecodeOf(vertex);
    if (fragment != nullptr)
        p.PS = bytecodeOf(fragment);
    p.BlendState.IndependentBlendEnable = TRUE;
    for (uint8_t i = 0; i < desc->formats.colorCount; ++i) {
        const PrGpuBlend& blend = desc->blend[i];
        D3D12_RENDER_TARGET_BLEND_DESC& a = p.BlendState.RenderTarget[i];
        a.BlendEnable = blend.enabled ? TRUE : FALSE;
        a.SrcBlend = blendFactorOf(blend.colorSource, false);
        a.DestBlend = blendFactorOf(blend.colorTarget, false);
        a.BlendOp = blendOpOf(blend.colorOp);
        a.SrcBlendAlpha = blendFactorOf(blend.alphaSource, true);
        a.DestBlendAlpha = blendFactorOf(blend.alphaTarget, true);
        a.BlendOpAlpha = blendOpOf(blend.alphaOp);
        a.LogicOp = D3D12_LOGIC_OP_NOOP;
        PrGpuColorWriteFlags write = blend.write != 0 ? blend.write : PrGpuColorWriteFlag_All;
        a.RenderTargetWriteMask = UINT8(((write & PrGpuColorWriteFlag_Red) ? D3D12_COLOR_WRITE_ENABLE_RED : 0) |
            ((write & PrGpuColorWriteFlag_Green) ? D3D12_COLOR_WRITE_ENABLE_GREEN : 0) |
            ((write & PrGpuColorWriteFlag_Blue) ? D3D12_COLOR_WRITE_ENABLE_BLUE : 0) |
            ((write & PrGpuColorWriteFlag_Alpha) ? D3D12_COLOR_WRITE_ENABLE_ALPHA : 0));
        p.RTVFormats[i] = formatOf(desc->formats.color[i]);
    }
    p.NumRenderTargets = desc->formats.colorCount;
    p.SampleMask = UINT_MAX;
    p.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    if (desc->cull == PrGpuCull_Front)
        p.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
    else if (desc->cull == PrGpuCull_Back)
        p.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    else
        p.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    // D3D12 winds triangles in the target, with y down, as the description does.
    p.RasterizerState.FrontCounterClockwise = desc->frontClockwise ? FALSE : TRUE;
    p.RasterizerState.DepthBias = INT(desc->depthBias);
    p.RasterizerState.DepthBiasClamp = desc->depthBiasClamp;
    p.RasterizerState.SlopeScaledDepthBias = desc->depthBiasSlope;
    p.RasterizerState.DepthClipEnable = TRUE;
    bool depth = desc->formats.depthStencil != PrGpuFormat_None;
    p.DepthStencilState.DepthEnable = depth && (desc->depthTest || desc->depthWrite) ? TRUE : FALSE;
    p.DepthStencilState.DepthWriteMask = desc->depthWrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    p.DepthStencilState.DepthFunc = desc->depthTest ? compareOf(desc->depthCompare) : D3D12_COMPARISON_FUNC_ALWAYS;
    p.DepthStencilState.StencilEnable = depth && desc->stencilTest ? TRUE : FALSE;
    p.DepthStencilState.StencilReadMask = desc->stencilReadMask;
    p.DepthStencilState.StencilWriteMask = desc->stencilWriteMask;
    p.DepthStencilState.FrontFace = stencilFaceOf(desc->stencilFront);
    p.DepthStencilState.BackFace = stencilFaceOf(desc->stencilBack);
    p.DSVFormat = depth ? formatOf(desc->formats.depthStencil) : DXGI_FORMAT_UNKNOWN;
    p.PrimitiveTopologyType = topologyTypeOf(desc->topology);
    p.SampleDesc.Count = desc->formats.samples;
    ID3D12PipelineState* state = nullptr;
    HRESULT made = d->device->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&state));
    if (FAILED(made)) {
        fprintf(stderr, "lib/gpu: D3D12 made no pipeline for %.*s (0x%08lx)\n", int(vertex->name.size),
            vertex->name.data, long(made));
        return PrStatus_NotSupported;
    }
    nameObject(state, desc->name);
    D3d12Pipeline pipeline{};
    pipeline.state = state;
    pipeline.readsTarget = desc->readsTarget;
    pipeline.topology = topologyOf(desc->topology);
    uint32_t id = addPipeline(d, pipeline);
    if (id == 0) {
        state->Release();
        return PrStatus_Limit;
    }
    out->id = id;
    return PrStatus_Ok;
}

PrStatus computePipelineCreate(PrGpuDevice* device, const PrGpuShaderLibrary*, const PrGpuShaderEntry* compute,
        PrStringView name, PrGpuPipeline* out) {
    D3d12Device* d = d3d12Of(device);
    D3D12_COMPUTE_PIPELINE_STATE_DESC p{};
    p.pRootSignature = d->rootSignature;
    p.CS = bytecodeOf(compute);
    ID3D12PipelineState* state = nullptr;
    HRESULT made = d->device->CreateComputePipelineState(&p, IID_PPV_ARGS(&state));
    if (FAILED(made)) {
        fprintf(stderr, "lib/gpu: D3D12 made no compute pipeline for %.*s (0x%08lx)\n", int(compute->name.size),
            compute->name.data, long(made));
        return PrStatus_NotSupported;
    }
    nameObject(state, name);
    D3d12Pipeline pipeline{};
    pipeline.state = state;
    pipeline.compute = true;
    pipeline.groupSize = compute->groupSize;
    uint32_t id = addPipeline(d, pipeline);
    if (id == 0) {
        state->Release();
        return PrStatus_Limit;
    }
    out->id = id;
    return PrStatus_Ok;
}

void pipelineDestroy(PrGpuDevice* device, PrGpuPipeline pipeline) {
    retire(d3d12Of(device), D3d12RetiredKind_Pipeline, pipeline.id);
}

PrGpuExtent pipelineGroupSize(const PrGpuDevice* device, PrGpuPipeline pipeline) {
    return d3d12Of(device)->pipelines[pipeline.id].groupSize;
}

// ---------------------------------------------------------------------------------------------------------------------
// Command buffers

// Binds the device's heaps and root signature on a list, and sets its root data to what nothing has set: zeros.
void bindRoot(D3d12Device* d, D3d12Commands* c) {
    ID3D12GraphicsCommandList7* list = c->list.list;
    ID3D12DescriptorHeap* heaps[2] = {d->resourceHeap, d->samplerHeap};
    list->SetDescriptorHeaps(2, heaps);
    list->SetGraphicsRootSignature(d->rootSignature);
    list->SetComputeRootSignature(d->rootSignature);
    list->SetGraphicsRoot32BitConstant(D3d12Root_TargetCopy, 0, 0);
    list->SetComputeRoot32BitConstant(D3d12Root_TargetCopy, 0, 0);
    c->dirty = D3d12DirtyFlag_All;
    c->changed |= PrGpuNativeStateFlag_Bindings;
}

// Returns attachment heaps from the free ones, or new, or null heaps if the device cannot make them. The caller holds
// the mutex.
D3d12AttachmentHeaps takeAttachmentHeaps(D3d12Device* d) {
    if (d->attachmentHeaps.count != 0) {
        D3d12AttachmentHeaps heaps = d->attachmentHeaps.last();
        d->attachmentHeaps.pop();
        return heaps;
    }
    D3d12AttachmentHeaps heaps{};
    D3D12_DESCRIPTOR_HEAP_DESC color{
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kPrGpuMaxColorAttachments,
        D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0,
    };
    D3D12_DESCRIPTOR_HEAP_DESC depth{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    if (FAILED(d->device->CreateDescriptorHeap(&color, IID_PPV_ARGS(&heaps.color))) ||
            FAILED(d->device->CreateDescriptorHeap(&depth, IID_PPV_ARGS(&heaps.depth)))) {
        release(heaps.color);
        return D3d12AttachmentHeaps{};
    }
    return heaps;
}

bool commandsBegin(PrGpuDevice* device, const PrGpuCommandsOptions* options, PrGpuCommands* PR_NULLABLE* PR_NONNULL out) {
    D3d12Device* d = d3d12Of(device);
    D3d12List list{};
    if (options->native != nullptr) {
        ID3D12GraphicsCommandList7* native = static_cast<ID3D12GraphicsCommandList7*>(options->native);
        native->AddRef();
        list.list = native;
    }
    prMutexLock(&d->mutex);
    collect(d);
    if (list.list == nullptr)
        list = takeList(d);
    D3d12AttachmentHeaps heaps = list.list != nullptr ? takeAttachmentHeaps(d) : D3d12AttachmentHeaps{};
    if (list.list == nullptr || heaps.color == nullptr) {
        if (list.allocator != nullptr)
            d->lists.push(list);
        else
            release(list.list);
        prMutexUnlock(&d->mutex);
        return false;
    }
    D3d12Commands* c = allocZeroed<D3d12Commands>();
    static_cast<PrGpuCommands&>(*c) = kCommandsFunctions;
    c->device = device;
    c->native = options->native != nullptr;
    c->list = list;
    c->attachments = heaps;
    c->serial = d->serial++;
    d->inFlight.push(D3d12InFlight{c->serial, 0});
    c->timestamp = kTimestampPairs;
    if (!c->native && d->freeTimestamps.count != 0) {
        c->timestamp = d->freeTimestamps.last();
        d->freeTimestamps.pop();
    }
    prMutexUnlock(&d->mutex);
    c->pushWords = kPrGpuMaxPushBytes / 4;
    c->uniforms = d->zeros->GetGPUVirtualAddress();
    bindRoot(d, c);
    if (c->timestamp != kTimestampPairs)
        c->list.list->EndQuery(d->timestamps, D3D12_QUERY_TYPE_TIMESTAMP, c->timestamp * 2);
    *out = c;
    return true;
}

// Allocates `size` bytes of the stream, at kStreamAlignment, in a block that the command buffer holds.
void* stream(D3d12Device* d, D3d12Commands* c, uint64_t size, uint32_t* slot, uint64_t* offset) {
    D3d12Block* last = c->blocks.count != 0 ? &c->blocks.last() : nullptr;
    uint64_t at = last != nullptr ? alignUp(last->used, kStreamAlignment) : 0;
    if (last == nullptr || at + size > last->length) {
        prMutexLock(&d->mutex);
        D3d12Block block = takeBlock(d, d->streamBlocks, kStreamBlock, size);
        prMutexUnlock(&d->mutex);
        if (block.buffer == nullptr) {
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
    return last->mapped + at;
}

void* commandsStream(PrGpuCommands* commands, uint64_t size, PrGpuBuffer* buffer, uint64_t* offset) {
    PrGpuDevice* device = static_cast<GpuSharedCommands*>(commands)->device;
    uint32_t slot = 0;
    void* bytes = stream(d3d12Of(device), commandsOf(commands), size, &slot, offset);
    buffer->id = slot;
    return bytes;
}

void commandsSetPush(PrGpuCommands* commands, const void* data, uint32_t size) {
    D3d12Commands* c = commandsOf(commands);
    prMemCopy(c->push, data, size);
    uint32_t words = (size + 3) / 4;
    if (words * 4 > size)
        prMemZero(c->push + size, words * 4 - size);
    c->pushWords = words;
    c->dirty |= D3d12DirtyFlag_GraphicsPush | D3d12DirtyFlag_ComputePush;
}

void commandsSetUniforms(PrGpuCommands* commands, const void* data, uint32_t size) {
    PrGpuDevice* device = static_cast<GpuSharedCommands*>(commands)->device;
    D3d12Device* d = d3d12Of(device);
    D3d12Commands* c = commandsOf(commands);
    uint32_t slot = 0;
    uint64_t offset = 0;
    void* bytes = stream(d, c, size != 0 ? size : 16, &slot, &offset);
    prMemCopy(bytes, data, size);
    if (slot == 0)
        return;
    c->uniforms = d->resources[slot].address + offset;
    c->dirty |= D3d12DirtyFlag_GraphicsUniforms | D3d12DirtyFlag_ComputeUniforms;
}

// Sets the root data that the graphics bind point has yet to take, before a draw.
void flushGraphicsRoot(D3d12Commands* c) {
    ID3D12GraphicsCommandList7* list = c->list.list;
    if (c->dirty & D3d12DirtyFlag_GraphicsPush) {
        if (c->pushWords != 0)
            list->SetGraphicsRoot32BitConstants(D3d12Root_Push, c->pushWords, c->push, 0);
        c->changed |= PrGpuNativeStateFlag_Push;
    }
    if (c->dirty & D3d12DirtyFlag_GraphicsUniforms)
        list->SetGraphicsRootConstantBufferView(D3d12Root_Uniforms, c->uniforms);
    c->dirty &= ~(D3d12DirtyFlag_GraphicsPush | D3d12DirtyFlag_GraphicsUniforms);
}

// Sets the root data that the compute bind point has yet to take, before a dispatch.
void flushComputeRoot(D3d12Commands* c) {
    ID3D12GraphicsCommandList7* list = c->list.list;
    if ((c->dirty & D3d12DirtyFlag_ComputePush) && c->pushWords != 0) {
        list->SetComputeRoot32BitConstants(D3d12Root_Push, c->pushWords, c->push, 0);
        c->changed |= PrGpuNativeStateFlag_Push;
    }
    if (c->dirty & D3d12DirtyFlag_ComputeUniforms)
        list->SetComputeRootConstantBufferView(D3d12Root_Uniforms, c->uniforms);
    c->dirty &= ~(D3d12DirtyFlag_ComputePush | D3d12DirtyFlag_ComputeUniforms);
}

void commandsSetPipeline(PrGpuCommands* commands, PrGpuPipeline pipeline) {
    PrGpuDevice* device = static_cast<GpuSharedCommands*>(commands)->device;
    D3d12Commands* c = commandsOf(commands);
    c->pipeline = pipeline.id;
    const D3d12Pipeline& p = d3d12Of(device)->pipelines[pipeline.id];
    if (D3d12Pass* pass = c->pass) {
        PR_ASSERT(!p.compute);
        c->list.list->SetPipelineState(p.state);
        c->list.list->IASetPrimitiveTopology(p.topology);
        c->changed |= PrGpuNativeStateFlag_Pipeline;
    }
}

void commandsDispatch(PrGpuCommands* commands, PrGpuExtent groups) {
    gpuSharedCheckOutsidePass(commands);
    PrGpuDevice* device = static_cast<GpuSharedCommands*>(commands)->device;
    D3d12Commands* c = commandsOf(commands);
    const D3d12Pipeline& p = d3d12Of(device)->pipelines[c->pipeline];
    PR_ASSERT(p.compute);
    c->list.list->SetPipelineState(p.state);
    c->changed |= PrGpuNativeStateFlag_Pipeline;
    flushComputeRoot(c);
    c->list.list->Dispatch(groups.width, groups.height, groups.depth);
}

void commandsDispatchIndirect(PrGpuCommands* commands, PrGpuBuffer buffer, uint64_t offset) {
    gpuSharedCheckOutsidePass(commands);
    PrGpuDevice* device = static_cast<GpuSharedCommands*>(commands)->device;
    D3d12Device* d = d3d12Of(device);
    D3d12Commands* c = commandsOf(commands);
    const D3d12Pipeline& p = d->pipelines[c->pipeline];
    PR_ASSERT(p.compute);
    c->list.list->SetPipelineState(p.state);
    c->changed |= PrGpuNativeStateFlag_Pipeline;
    flushComputeRoot(c);
    c->list.list->ExecuteIndirect(d->dispatchSignature, 1, resourceOf(d, buffer.id), offset, nullptr, 0);
}

void commandsCopyBuffer(PrGpuCommands* commands, PrGpuBuffer source, uint64_t sourceOffset,
        PrGpuBuffer target, uint64_t targetOffset, uint64_t size) {
    gpuSharedCheckOutsidePass(commands);
    PrGpuDevice* device = static_cast<GpuSharedCommands*>(commands)->device;
    D3d12Device* d = d3d12Of(device);
    commandsOf(commands)->list.list->CopyBufferRegion(resourceOf(d, target.id), targetOffset,
        resourceOf(d, source.id), sourceOffset, size);
}

// The location of a subresource of a texture, for a copy.
D3D12_TEXTURE_COPY_LOCATION subresourceLocation(const D3d12Resource& r, uint32_t level, uint32_t layer) {
    D3D12_TEXTURE_COPY_LOCATION l{};
    l.pResource = r.resource;
    l.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    l.SubresourceIndex = subresourceOf(r, level, layer);
    return l;
}

// The location of a texture's texels in a buffer at `offset`, with rows `rowBytes` apart, for a copy of `extent`.
D3D12_TEXTURE_COPY_LOCATION footprintLocation(const D3d12Resource& texture, ID3D12Resource* buffer, uint64_t offset,
        uint32_t rowBytes, PrGpuExtent extent) {
    PrGpuFormatBlock b = prGpuFormatBlock(texture.texture.format);
    D3D12_TEXTURE_COPY_LOCATION l{};
    l.pResource = buffer;
    l.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    l.PlacedFootprint.Offset = offset;
    l.PlacedFootprint.Footprint.Format = copyFormatOf(texture);
    l.PlacedFootprint.Footprint.Width = alignUp(extent.width, uint32_t(b.width));
    l.PlacedFootprint.Footprint.Height = alignUp(extent.height, uint32_t(b.height));
    l.PlacedFootprint.Footprint.Depth = extent.depth;
    l.PlacedFootprint.Footprint.RowPitch = rowBytes;
    return l;
}

// The box of a region within one subresource.
D3D12_BOX boxOf(const PrGpuTextureRegion* region, bool is3d) {
    uint32_t z = is3d ? region->z : 0;
    uint32_t depth = is3d ? region->extent.depth : 1;
    return D3D12_BOX{region->x, region->y, z, region->x + region->extent.width, region->y + region->extent.height, z + depth};
}

void commandsCopyTexture(PrGpuCommands* commands, PrGpuTexture source,
        const PrGpuTextureRegion* sourceRegion, PrGpuTexture target, const PrGpuTextureRegion* targetRegion) {
    gpuSharedCheckOutsidePass(commands);
    PrGpuDevice* device = static_cast<GpuSharedCommands*>(commands)->device;
    D3d12Device* d = d3d12Of(device);
    const D3d12Resource& from = textureOf(d, source.id);
    const D3d12Resource& to = textureOf(d, target.id);
    bool is3d = from.texture.kind == PrGpuTextureKind_3d;
    uint32_t images = is3d ? 1 : sourceRegion->extent.depth;
    D3D12_BOX box = boxOf(sourceRegion, is3d);
    for (uint32_t i = 0; i < images; ++i) {
        D3D12_TEXTURE_COPY_LOCATION s = subresourceLocation(from, sourceRegion->level, sourceRegion->firstLayer + i);
        D3D12_TEXTURE_COPY_LOCATION t = subresourceLocation(to, targetRegion->level, targetRegion->firstLayer + i);
        commandsOf(commands)->list.list->CopyTextureRegion(&t, targetRegion->x, targetRegion->y,
            is3d ? targetRegion->z : 0, &s, &box);
    }
}

void commandsCopyBufferToTexture(PrGpuCommands* commands, PrGpuBuffer source,
        uint64_t sourceOffset, uint32_t rowBytes, uint32_t layerBytes, PrGpuTexture target,
        const PrGpuTextureRegion* targetRegion) {
    gpuSharedCheckOutsidePass(commands);
    PrGpuDevice* device = static_cast<GpuSharedCommands*>(commands)->device;
    D3d12Device* d = d3d12Of(device);
    const D3d12Resource& to = textureOf(d, target.id);
    bool is3d = to.texture.kind == PrGpuTextureKind_3d;
    uint32_t images = is3d ? 1 : targetRegion->extent.depth;
    uint64_t imageBytes = layerBytes != 0 ? layerBytes : uint64_t(rowBytes) * targetRegion->extent.height;
    PrGpuExtent extent{targetRegion->extent.width, targetRegion->extent.height, is3d ? targetRegion->extent.depth : 1};
    for (uint32_t i = 0; i < images; ++i) {
        D3D12_TEXTURE_COPY_LOCATION s = footprintLocation(to, resourceOf(d, source.id), sourceOffset + i * imageBytes,
            rowBytes, extent);
        D3D12_TEXTURE_COPY_LOCATION t = subresourceLocation(to, targetRegion->level, targetRegion->firstLayer + i);
        commandsOf(commands)->list.list->CopyTextureRegion(&t, targetRegion->x, targetRegion->y,
            is3d ? targetRegion->z : 0, &s, nullptr);
    }
}

void commandsCopyTextureToBuffer(PrGpuCommands* commands, PrGpuTexture source,
        const PrGpuTextureRegion* sourceRegion, PrGpuBuffer target, uint64_t targetOffset, uint32_t rowBytes,
        uint32_t layerBytes) {
    gpuSharedCheckOutsidePass(commands);
    PrGpuDevice* device = static_cast<GpuSharedCommands*>(commands)->device;
    D3d12Device* d = d3d12Of(device);
    const D3d12Resource& from = textureOf(d, source.id);
    bool is3d = from.texture.kind == PrGpuTextureKind_3d;
    uint32_t images = is3d ? 1 : sourceRegion->extent.depth;
    uint64_t imageBytes = layerBytes != 0 ? layerBytes : uint64_t(rowBytes) * sourceRegion->extent.height;
    PrGpuExtent extent{sourceRegion->extent.width, sourceRegion->extent.height, is3d ? sourceRegion->extent.depth : 1};
    D3D12_BOX box = boxOf(sourceRegion, is3d);
    for (uint32_t i = 0; i < images; ++i) {
        D3D12_TEXTURE_COPY_LOCATION s = subresourceLocation(from, sourceRegion->level, sourceRegion->firstLayer + i);
        D3D12_TEXTURE_COPY_LOCATION t = footprintLocation(from, resourceOf(d, target.id),
            targetOffset + i * imageBytes, rowBytes, extent);
        commandsOf(commands)->list.list->CopyTextureRegion(&t, 0, 0, 0, &s, &box);
    }
}

void commandsOnDone(PrGpuCommands* commands, PrGpuDoneFn done, void* PR_NULLABLE context) {
    commandsOf(commands)->dones.push(D3d12Done{done, context});
}

// PIX reads an event's name as a string of 8-bit characters where its metadata is 1.
void commandsPushGroup(PrGpuCommands* commands, PrStringView name) {
    char text[256];
    size_t size = name.size < sizeof(text) - 1 ? name.size : sizeof(text) - 1;
    prMemCopy(text, name.data, size);
    text[size] = 0;
    commandsOf(commands)->list.list->BeginEvent(1, text, UINT(size + 1));
}

void commandsPopGroup(PrGpuCommands* commands) {
    commandsOf(commands)->list.list->EndEvent();
}

void commandsBufferBarrier(PrGpuCommands* commands, PrGpuBuffer buffer, PrGpuAccessFlags before, PrGpuAccessFlags after) {
    gpuSharedCheckOutsidePass(commands);
    PrGpuDevice* device = static_cast<GpuSharedCommands*>(commands)->device;
    // Only a buffer that shaders write takes unordered accesses.
    const D3d12Resource& r = d3d12Of(device)->resources[buffer.id];
    if (!(r.buffer.usage & PrGpuBufferUsageFlag_Storage)) {
        before &= ~PrGpuAccessFlag_ShaderWrite;
        after &= ~PrGpuAccessFlag_ShaderWrite;
    }
    D3D12_BUFFER_BARRIER b{};
    b.AccessBefore = accessOf(before);
    b.AccessAfter = accessOf(after);
    b.SyncBefore = b.AccessBefore == D3D12_BARRIER_ACCESS_NO_ACCESS ? D3D12_BARRIER_SYNC_NONE : syncOf(before);
    b.SyncAfter = b.AccessAfter == D3D12_BARRIER_ACCESS_NO_ACCESS ? D3D12_BARRIER_SYNC_NONE : syncOf(after);
    if (b.SyncBefore == D3D12_BARRIER_SYNC_NONE && b.SyncAfter == D3D12_BARRIER_SYNC_NONE)
        return;
    b.pResource = r.resource;
    b.Offset = 0;
    b.Size = UINT64_MAX;
    D3D12_BARRIER_GROUP group{};
    group.Type = D3D12_BARRIER_TYPE_BUFFER;
    group.NumBarriers = 1;
    group.pBufferBarriers = &b;
    commandsOf(commands)->list.list->Barrier(1, &group);
}

void commandsTextureBarrier(PrGpuCommands* commands, PrGpuTexture texture,
        PrGpuSubresources subresources, PrGpuAccessFlags before, PrGpuAccessFlags after) {
    gpuSharedCheckOutsidePass(commands);
    PrGpuDevice* device = static_cast<GpuSharedCommands*>(commands)->device;
    const D3d12Resource& r = textureOf(d3d12Of(device), texture.id);
    textureBarrier(commandsOf(commands)->list.list, r, rangeOf(r, subresources), before, after);
}

void commandsGlobalBarrier(PrGpuCommands* commands, PrGpuAccessFlags before, PrGpuAccessFlags after) {
    gpuSharedCheckOutsidePass(commands);
    globalBarrier(commandsOf(commands)->list.list, before, after);
}

PrGpuFence commandsSubmit(PrGpuCommands* commands) {
    PrGpuDevice* device = static_cast<GpuSharedCommands*>(commands)->device;
    D3d12Device* d = d3d12Of(device);
    D3d12Commands* c = commandsOf(commands);
    gpuSharedSubmitted(c);
    ID3D12GraphicsCommandList7* list = c->list.list;
    if (c->timestamp != kTimestampPairs) {
        list->EndQuery(d->timestamps, D3D12_QUERY_TYPE_TIMESTAMP, c->timestamp * 2 + 1);
        list->ResolveQueryData(d->timestamps, D3D12_QUERY_TYPE_TIMESTAMP, c->timestamp * 2, 2, d->timestampReadback,
            uint64_t(c->timestamp) * 16);
    }
    if (!c->native)
        list->Close();
    // The functions to call once the work is done, with its time.
    uint32_t doneCount = c->dones.count;
    D3d12Done* dones = nullptr;
    if (doneCount != 0) {
        dones = static_cast<D3d12Done*>(prMemoryAlloc(sizeof(D3d12Done) * doneCount));
        prMemCopy(dones, c->dones.data, sizeof(D3d12Done) * doneCount);
    }
    prMutexLock(&d->mutex);
    submitUploads(d);
    PrGpuFence fence = execute(d, list);
    addSubmission(d, D3d12Submission{fence, dones, doneCount, c->timestamp});
    for (uint32_t i = 0; i < d->inFlight.count; ++i) {
        if (d->inFlight.data[i].serial == c->serial)
            d->inFlight.data[i].fence = fence;
    }
    for (uint32_t i = 0; i < c->blocks.count; ++i)
        d->held.push(D3d12Held{fence, D3d12HeldKind_StreamBlock, c->blocks.data[i], {}, 0});
    if (c->native)
        release(c->list.list);
    else
        d->held.push(D3d12Held{fence, D3d12HeldKind_List, {}, c->list, 0});
    if (c->timestamp != kTimestampPairs)
        d->held.push(D3d12Held{fence, D3d12HeldKind_Timestamp, {}, {}, c->timestamp});
    d->attachmentHeaps.push(c->attachments);
    if (c->targetCopy.id != 0)
        d->retired.push(D3d12Retired{d->serial, D3d12RetiredKind_Resource, c->targetCopy.id});
    prMutexUnlock(&d->mutex);
    // DXGI presents a frame once the work executed before it on the queue is done.
    for (uint32_t i = 0; i < c->presents.count; ++i) {
        HRESULT presented = c->presents.data[i]->swapChain->Present(1, 0);
        if (presented == DXGI_ERROR_DEVICE_REMOVED || presented == DXGI_ERROR_DEVICE_RESET)
            d->lost.store(true);
    }
    for (uint32_t i = 0; i < c->scratch.count; ++i)
        prMemoryFree(c->scratch.data[i]);
    c->scratch.reset();
    c->blocks.reset();
    c->presents.reset();
    c->dones.reset();
    prMemoryFree(c);
    return fence;
}

// ---------------------------------------------------------------------------------------------------------------------
// Passes

// The extent of a level of a texture or view.
PrUVec2 levelExtent(const D3d12Resource& r, uint32_t level) {
    uint32_t width = r.texture.extent.width >> level;
    uint32_t height = r.texture.extent.height >> level;
    return PrUVec2{width != 0 ? width : 1, height != 0 ? height : 1};
}

// Writes the render target view of an attachment to `at`.
void writeColorView(D3d12Device* d, const PrGpuAttachment& a, D3D12_CPU_DESCRIPTOR_HANDLE at) {
    const D3d12Resource& r = textureOf(d, a.texture.id);
    uint32_t mip = r.firstLevel + a.level;
    uint32_t slice = r.firstLayer + a.layer;
    D3D12_RENDER_TARGET_VIEW_DESC v{};
    v.Format = formatOf(r.texture.format);
    if (r.texture.kind == PrGpuTextureKind_3d) {
        v.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
        v.Texture3D.MipSlice = mip;
        v.Texture3D.FirstWSlice = a.layer;
        v.Texture3D.WSize = 1;
    } else if (r.texture.samples > 1) {
        if (r.resourceLayers > 1) {
            v.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY;
            v.Texture2DMSArray.FirstArraySlice = slice;
            v.Texture2DMSArray.ArraySize = 1;
        } else {
            v.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
        }
    } else if (r.resourceLayers > 1) {
        v.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        v.Texture2DArray.MipSlice = mip;
        v.Texture2DArray.FirstArraySlice = slice;
        v.Texture2DArray.ArraySize = 1;
    } else {
        v.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        v.Texture2D.MipSlice = mip;
    }
    d->device->CreateRenderTargetView(r.resource, &v, at);
}

// Writes the depth stencil view of an attachment to `at`.
void writeDepthView(D3d12Device* d, const PrGpuAttachment& a, D3D12_CPU_DESCRIPTOR_HANDLE at) {
    const D3d12Resource& r = textureOf(d, a.texture.id);
    uint32_t mip = r.firstLevel + a.level;
    uint32_t slice = r.firstLayer + a.layer;
    D3D12_DEPTH_STENCIL_VIEW_DESC v{};
    v.Format = formatOf(r.texture.format);
    if (r.texture.samples > 1) {
        if (r.resourceLayers > 1) {
            v.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY;
            v.Texture2DMSArray.FirstArraySlice = slice;
            v.Texture2DMSArray.ArraySize = 1;
        } else {
            v.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
        }
    } else if (r.resourceLayers > 1) {
        v.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        v.Texture2DArray.MipSlice = mip;
        v.Texture2DArray.FirstArraySlice = slice;
        v.Texture2DArray.ArraySize = 1;
    } else {
        v.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        v.Texture2D.MipSlice = mip;
    }
    d->device->CreateDepthStencilView(r.resource, &v, at);
}

// Sets the viewport and scissor that cover the pass's attachments.
void coverAttachments(D3d12Pass* pass, ID3D12GraphicsCommandList7* list) {
    D3D12_VIEWPORT viewport{0, 0, float(pass->extent.x), float(pass->extent.y), 0, 1};
    list->RSSetViewports(1, &viewport);
    D3D12_RECT scissor{0, 0, LONG(pass->extent.x), LONG(pass->extent.y)};
    list->RSSetScissorRects(1, &scissor);
    pass->scissor = PrIRect{0, 0, int32_t(pass->extent.x), int32_t(pass->extent.y)};
    commandsOf(pass->commands)->changed |= PrGpuNativeStateFlag_Viewport | PrGpuNativeStateFlag_Scissor;
}

PrGpuPass* passBegin(PrGpuCommands* commands, const PrGpuPassDesc* desc) {
    PrGpuDevice* device = static_cast<GpuSharedCommands*>(commands)->device;
    D3d12Device* d = d3d12Of(device);
    D3d12Commands* c = commandsOf(commands);
    gpuSharedPassBegan(c);
    ID3D12GraphicsCommandList7* list = c->list.list;
    D3d12Pass* pass = allocZeroed<D3d12Pass>();
    static_cast<PrGpuPass&>(*pass) = kPassFunctions;
    pass->commands = c;
    if (desc->native != nullptr) {
        // The application may have bound another root signature since the command buffer began.
        pass->native = true;
        pass->formats = desc->nativeFormats;
        pass->extent = desc->nativeExtent;
        pass->scissor = PrIRect{0, 0, int32_t(pass->extent.x), int32_t(pass->extent.y)};
        list->SetGraphicsRootSignature(d->rootSignature);
        list->SetGraphicsRoot32BitConstant(D3d12Root_TargetCopy, 0, 0);
        c->dirty |= D3d12DirtyFlag_GraphicsPush | D3d12DirtyFlag_GraphicsUniforms;
        c->changed |= PrGpuNativeStateFlag_Bindings;
    } else {
        pass->formats.samples = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE colors[kPrGpuMaxColorAttachments]{};
        D3D12_CPU_DESCRIPTOR_HANDLE first = c->attachments.color->GetCPUDescriptorHandleForHeapStart();
        for (uint8_t i = 0; i < desc->colorCount; ++i) {
            const PrGpuAttachment& a = desc->color[i];
            const D3d12Resource& t = textureOf(d, a.texture.id);
            colors[i] = first;
            colors[i].ptr += size_t(i) * d->colorIncrement;
            writeColorView(d, a, colors[i]);
            pass->color[i] = a;
            pass->formats.color[pass->formats.colorCount++] = t.texture.format;
            pass->formats.samples = t.texture.samples;
            pass->extent = levelExtent(t, a.level);
        }
        D3D12_CPU_DESCRIPTOR_HANDLE depth = c->attachments.depth->GetCPUDescriptorHandleForHeapStart();
        bool hasDepth = desc->depthStencil.texture.id != 0;
        if (hasDepth) {
            const PrGpuAttachment& a = desc->depthStencil;
            const D3d12Resource& t = textureOf(d, a.texture.id);
            writeDepthView(d, a, depth);
            pass->depthStencil = a;
            pass->formats.depthStencil = t.texture.format;
            pass->formats.samples = t.texture.samples;
            if (desc->colorCount == 0)
                pass->extent = levelExtent(t, a.level);
        }
        list->OMSetRenderTargets(desc->colorCount, colors, FALSE, hasDepth ? &depth : nullptr);
        for (uint8_t i = 0; i < desc->colorCount; ++i) {
            if (desc->color[i].load == PrGpuLoad_Clear) {
                const PrColor& clear = desc->clearColor[i];
                const float rgba[4] = {clear.r, clear.g, clear.b, clear.a};
                list->ClearRenderTargetView(colors[i], rgba, 0, nullptr);
            }
        }
        if (hasDepth) {
            D3D12_CLEAR_FLAGS clears{};
            if (desc->depthStencil.load == PrGpuLoad_Clear)
                clears |= D3D12_CLEAR_FLAG_DEPTH;
            if (pass->formats.depthStencil == PrGpuFormat_D32FS8 && desc->stencilLoad == PrGpuLoad_Clear)
                clears |= D3D12_CLEAR_FLAG_STENCIL;
            if (clears != 0)
                list->ClearDepthStencilView(depth, clears, desc->clearDepth, desc->clearStencil, 0, nullptr);
        }
        coverAttachments(pass, list);
        pass->named = desc->name.size != 0;
        if (pass->named)
            commandsPushGroup(commands, desc->name);
    }
    c->pass = pass;
    c->pipeline = 0;
    return pass;
}

// Resolves the samples of a color attachment into its resolve target, both of which are in the render target layout
// before and after.
void resolve(D3d12Device* d, ID3D12GraphicsCommandList7* list, const PrGpuAttachment& a) {
    const D3d12Resource& from = textureOf(d, a.texture.id);
    const D3d12Resource& to = textureOf(d, a.resolve.id);
    D3D12_BARRIER_SUBRESOURCE_RANGE fromRange = rangeOf(from, PrGpuSubresources{a.level, 1, a.layer, 1});
    D3D12_BARRIER_SUBRESOURCE_RANGE toRange = rangeOf(to, PrGpuSubresources{a.level, 1, a.layer, 1});
    D3D12_TEXTURE_BARRIER b[2]{};
    for (int i = 0; i < 2; ++i) {
        b[i].SyncBefore = D3D12_BARRIER_SYNC_RENDER_TARGET;
        b[i].SyncAfter = D3D12_BARRIER_SYNC_RESOLVE;
        b[i].AccessBefore = D3D12_BARRIER_ACCESS_RENDER_TARGET;
        b[i].LayoutBefore = D3D12_BARRIER_LAYOUT_RENDER_TARGET;
    }
    b[0].AccessAfter = D3D12_BARRIER_ACCESS_RESOLVE_SOURCE;
    b[0].LayoutAfter = D3D12_BARRIER_LAYOUT_RESOLVE_SOURCE;
    b[0].pResource = from.resource;
    b[0].Subresources = fromRange;
    b[1].AccessAfter = D3D12_BARRIER_ACCESS_RESOLVE_DEST;
    b[1].LayoutAfter = D3D12_BARRIER_LAYOUT_RESOLVE_DEST;
    b[1].pResource = to.resource;
    b[1].Subresources = toRange;
    D3D12_BARRIER_GROUP group{};
    group.Type = D3D12_BARRIER_TYPE_TEXTURE;
    group.NumBarriers = 2;
    group.pTextureBarriers = b;
    list->Barrier(1, &group);
    list->ResolveSubresource(to.resource, subresourceOf(to, a.level, a.layer), from.resource,
        subresourceOf(from, a.level, a.layer), formatOf(from.texture.format));
    for (int i = 0; i < 2; ++i) {
        b[i].SyncBefore = D3D12_BARRIER_SYNC_RESOLVE;
        b[i].SyncAfter = D3D12_BARRIER_SYNC_RENDER_TARGET;
        b[i].AccessBefore = b[i].AccessAfter;
        b[i].LayoutBefore = b[i].LayoutAfter;
        b[i].AccessAfter = D3D12_BARRIER_ACCESS_RENDER_TARGET;
        b[i].LayoutAfter = D3D12_BARRIER_LAYOUT_RENDER_TARGET;
    }
    list->Barrier(1, &group);
}

void passEnd(PrGpuPass* handle) {
    PrGpuDevice* device = static_cast<GpuSharedPass*>(handle)->commands->device;
    D3d12Device* d = d3d12Of(device);
    D3d12Pass* pass = static_cast<D3d12Pass*>(handle);
    D3d12Commands* c = commandsOf(pass->commands);
    gpuSharedPassEnded(c);
    c->pass = nullptr;
    if (!pass->native) {
        for (uint8_t i = 0; i < pass->formats.colorCount; ++i) {
            if (pass->color[i].store == PrGpuStore_Resolve)
                resolve(d, c->list.list, pass->color[i]);
        }
        if (pass->named)
            commandsPopGroup(c);
    }
    prMemoryFree(pass);
}

// Hands the pass's command list to a native draw, then binds the device's heaps and root signature again, covers a pass
// of the device's own with the viewport and the scissor, and leaves the pass's pipeline and root data unset. An
// application's pass reports every state as changed, since the native draw may have changed any of it.
void passNativeDraw(PrGpuPass* handle, PrGpuNativeKind, PrGpuNativeDrawFn draw, void* PR_NULLABLE context) {
    PrGpuDevice* device = static_cast<GpuSharedPass*>(handle)->commands->device;
    D3d12Device* d = d3d12Of(device);
    D3d12Pass* pass = static_cast<D3d12Pass*>(handle);
    D3d12Commands* c = commandsOf(pass->commands);
    PrGpuD3d12NativePass native{.commandList = c->list.list};
    draw(context, &native);
    bindRoot(d, c);
    if (!pass->native)
        coverAttachments(pass, c->list.list);
    c->pipeline = 0;
    c->changed |= PrGpuNativeStateFlags(~0u);
}

// A texture's native object is its ID3D12Resource, which a view shares with its texture.
bool textureNative(PrGpuDevice* device, PrGpuTexture texture, PrGpuNativeKind, void* out) {
    *static_cast<void**>(out) = textureOf(d3d12Of(device), texture.id).resource;
    return true;
}

PrGpuNativeStateFlags nativeStateSync(PrGpuCommands* commands) {
    D3d12Commands* c = commandsOf(commands);
    PrGpuNativeStateFlags changed = c->changed;
    c->changed = 0;
    return changed;
}

void passSetViewport(PrGpuPass* handle, PrRect viewport, float minDepth, float maxDepth) {
    D3d12Pass* pass = static_cast<D3d12Pass*>(handle);
    D3D12_VIEWPORT v{
        viewport.left, viewport.top, viewport.right - viewport.left, viewport.bottom - viewport.top,
        minDepth, maxDepth,
    };
    commandsOf(pass->commands)->list.list->RSSetViewports(1, &v);
    commandsOf(pass->commands)->changed |= PrGpuNativeStateFlag_Viewport;
}

void passSetScissor(PrGpuPass* handle, PrIRect scissor) {
    D3d12Pass* pass = static_cast<D3d12Pass*>(handle);
    int32_t left = scissor.left > 0 ? scissor.left : 0;
    int32_t top = scissor.top > 0 ? scissor.top : 0;
    int32_t right = scissor.right < int32_t(pass->extent.x) ? scissor.right : int32_t(pass->extent.x);
    int32_t bottom = scissor.bottom < int32_t(pass->extent.y) ? scissor.bottom : int32_t(pass->extent.y);
    if (right < left)
        right = left;
    if (bottom < top)
        bottom = top;
    pass->scissor = PrIRect{left, top, right, bottom};
    D3D12_RECT r{left, top, right, bottom};
    commandsOf(pass->commands)->list.list->RSSetScissorRects(1, &r);
    commandsOf(pass->commands)->changed |= PrGpuNativeStateFlag_Scissor;
}

void passSetBlendConstant(PrGpuPass* handle, PrColor constant) {
    D3d12Pass* pass = static_cast<D3d12Pass*>(handle);
    const float rgba[4] = {constant.r, constant.g, constant.b, constant.a};
    commandsOf(pass->commands)->list.list->OMSetBlendFactor(rgba);
    commandsOf(pass->commands)->changed |= PrGpuNativeStateFlag_BlendConstant;
}

void passSetStencilReference(PrGpuPass* handle, uint8_t reference) {
    D3d12Pass* pass = static_cast<D3d12Pass*>(handle);
    commandsOf(pass->commands)->list.list->OMSetStencilRef(reference);
    commandsOf(pass->commands)->changed |= PrGpuNativeStateFlag_StencilReference;
}

void passSetIndexBuffer(PrGpuPass* handle, PrGpuBuffer buffer, uint64_t offset, bool wide) {
    PrGpuDevice* device = static_cast<GpuSharedPass*>(handle)->commands->device;
    D3d12Device* d = d3d12Of(device);
    D3d12Pass* pass = static_cast<D3d12Pass*>(handle);
    const D3d12Resource& r = d->resources[buffer.id];
    D3D12_INDEX_BUFFER_VIEW view{
        r.address + offset, UINT(alignUp(r.buffer.size, 16) - offset),
        wide ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT,
    };
    commandsOf(pass->commands)->list.list->IASetIndexBuffer(&view);
    commandsOf(pass->commands)->changed |= PrGpuNativeStateFlag_IndexBuffer;
}

// Copies the pass's first color attachment, over the scissor, into the command buffer's copy of it, which the draw's
// shader reads as the target, and points the root data at the copy. Returns false if the device cannot make the copy.
bool copyTarget(D3d12Device* d, D3d12Commands* c, D3d12Pass* pass) {
    ID3D12GraphicsCommandList7* list = c->list.list;
    const PrGpuAttachment& a = pass->color[0];
    const D3d12Resource& target = textureOf(d, a.texture.id);
    PrUVec2 extent = pass->extent;
    if (c->targetCopy.id != 0) {
        const PrGpuTextureDesc& copy = d->resources[c->targetCopy.id].texture;
        if (copy.format != target.texture.format || copy.extent.width < extent.x || copy.extent.height < extent.y) {
            retire(d, D3d12RetiredKind_Resource, c->targetCopy.id);
            c->targetCopy = PrGpuTexture{};
        }
    }
    if (c->targetCopy.id == 0) {
        PrGpuTextureDesc desc = prGpuTextureDesc2d(target.texture.format, extent, PrGpuTextureUsageFlag_Sampled);
        desc.name = "target copy";
        if (textureCreate(c->device, &desc, &c->targetCopy) != PrStatus_Ok)
            return false;
    }
    const D3d12Resource& copy = d->resources[c->targetCopy.id];
    PrIRect s = pass->scissor;
    if (s.right <= s.left || s.bottom <= s.top)
        return true;
    D3D12_BARRIER_SUBRESOURCE_RANGE targetRange = rangeOf(target, PrGpuSubresources{a.level, 1, a.layer, 1});
    D3D12_BARRIER_SUBRESOURCE_RANGE copyRange = rangeOf(copy, PrGpuSubresources{0, 1, 0, 1});
    textureBarrier(list, target, targetRange, PrGpuAccessFlag_ColorAttachment, PrGpuAccessFlag_CopyRead);
    textureBarrier(list, copy, copyRange, PrGpuAccessFlag_FragmentRead, PrGpuAccessFlag_CopyWrite);
    D3D12_TEXTURE_COPY_LOCATION from = subresourceLocation(target, a.level, a.layer);
    D3D12_TEXTURE_COPY_LOCATION to = subresourceLocation(copy, 0, 0);
    D3D12_BOX box{UINT(s.left), UINT(s.top), 0, UINT(s.right), UINT(s.bottom), 1};
    list->CopyTextureRegion(&to, UINT(s.left), UINT(s.top), 0, &from, &box);
    textureBarrier(list, target, targetRange, PrGpuAccessFlag_CopyRead, PrGpuAccessFlag_ColorAttachment);
    textureBarrier(list, copy, copyRange, PrGpuAccessFlag_CopyWrite, PrGpuAccessFlag_FragmentRead);
    list->SetGraphicsRoot32BitConstant(D3d12Root_TargetCopy, c->targetCopy.id, 0);
    return true;
}

// Prepares a draw: its root data, and the copy of the target that a pipeline that reads the target reads.
const D3d12Pipeline& prepareDraw(D3d12Device* d, D3d12Pass* pass) {
    D3d12Commands* c = commandsOf(pass->commands);
    const D3d12Pipeline& p = d->pipelines[c->pipeline];
    flushGraphicsRoot(c);
    if (p.readsTarget) {
        PR_ASSERT(!pass->native && pass->formats.colorCount != 0 && pass->formats.samples == 1);
        copyTarget(d, c, pass);
    }
    return p;
}

void passDraw(PrGpuPass* handle, uint32_t vertexCount, uint32_t instanceCount,
        uint32_t firstVertex, uint32_t firstInstance) {
    PrGpuDevice* device = static_cast<GpuSharedPass*>(handle)->commands->device;
    D3d12Pass* pass = static_cast<D3d12Pass*>(handle);
    prepareDraw(d3d12Of(device), pass);
    commandsOf(pass->commands)->list.list->DrawInstanced(vertexCount, instanceCount, firstVertex, firstInstance);
}

void passDrawIndexed(PrGpuPass* handle, uint32_t indexCount, uint32_t instanceCount,
        uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) {
    PrGpuDevice* device = static_cast<GpuSharedPass*>(handle)->commands->device;
    D3d12Pass* pass = static_cast<D3d12Pass*>(handle);
    prepareDraw(d3d12Of(device), pass);
    commandsOf(pass->commands)->list.list->DrawIndexedInstanced(indexCount, instanceCount, firstIndex, vertexOffset,
        firstInstance);
}

// Records indirect draws through `signature`, which reads arguments of `argumentBytes`: all at once if they are packed,
// and one at a time otherwise.
void drawIndirect(D3d12Device* d, D3d12Pass* pass, ID3D12CommandSignature* signature, uint32_t argumentBytes,
        PrGpuBuffer buffer, uint64_t offset, uint32_t count, uint32_t stride) {
    prepareDraw(d, pass);
    ID3D12GraphicsCommandList7* list = commandsOf(pass->commands)->list.list;
    ID3D12Resource* arguments = resourceOf(d, buffer.id);
    if (stride == argumentBytes || count == 1) {
        list->ExecuteIndirect(signature, count, arguments, offset, nullptr, 0);
        return;
    }
    for (uint32_t i = 0; i < count; ++i)
        list->ExecuteIndirect(signature, 1, arguments, offset + uint64_t(i) * stride, nullptr, 0);
}

void passDrawIndirect(PrGpuPass* handle, PrGpuBuffer buffer, uint64_t offset, uint32_t count, uint32_t stride) {
    PrGpuDevice* device = static_cast<GpuSharedPass*>(handle)->commands->device;
    D3d12Device* d = d3d12Of(device);
    drawIndirect(d, static_cast<D3d12Pass*>(handle), d->drawSignature, sizeof(PrGpuDrawIndirect), buffer, offset, count, stride);
}

void passDrawIndexedIndirect(PrGpuPass* handle, PrGpuBuffer buffer, uint64_t offset, uint32_t count, uint32_t stride) {
    PrGpuDevice* device = static_cast<GpuSharedPass*>(handle)->commands->device;
    D3d12Device* d = d3d12Of(device);
    drawIndirect(d, static_cast<D3d12Pass*>(handle), d->drawIndexedSignature, sizeof(PrGpuDrawIndexedIndirect),
        buffer, offset, count, stride);
}

// ---------------------------------------------------------------------------------------------------------------------
// Swap chains

// Takes the swap chain's buffers from DXGI into resource slots. Returns false if the device has no slot for one.
bool takeBuffers(D3d12Device* d, D3d12SwapChain* sc) {
    bool taken = true;
    for (uint32_t i = 0; i < kSwapChainBuffers; ++i) {
        ID3D12Resource* buffer = nullptr;
        sc->buffers[i] = 0;
        if (FAILED(sc->swapChain->GetBuffer(i, IID_PPV_ARGS(&buffer)))) {
            taken = false;
            continue;
        }
        PrGpuTextureDesc desc = prGpuTextureDesc2d(sc->info.format, sc->capacity,
            PrGpuTextureUsageFlag_RenderTarget | PrGpuTextureUsageFlag_Sampled);
        sc->buffers[i] = addTexture(d, buffer, desc, sc->format, 1, 1, 0, 0);
        if (sc->buffers[i] == 0) {
            buffer->Release();
            taken = false;
        }
    }
    return taken;
}

// Frees the slots of the swap chain's buffers, whose work is done, and the references that they hold.
void releaseBuffers(D3d12Device* d, D3d12SwapChain* sc) {
    prMutexLock(&d->mutex);
    for (uint32_t& slot : sc->buffers) {
        if (slot != 0)
            freeResource(d, slot);
        slot = 0;
    }
    prMutexUnlock(&d->mutex);
    sc->current = PrGpuTexture{};
}

// Returns true if the display that shows most of the window is in HDR mode, in which DXGI composes in HDR10.
bool displayTakesHdr10(IDXGIFactory6* factory, HWND window) {
    HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    bool hdr10 = false;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; !hdr10 && factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        IDXGIOutput* output = nullptr;
        for (UINT k = 0; !hdr10 && adapter->EnumOutputs(k, &output) != DXGI_ERROR_NOT_FOUND; ++k) {
            IDXGIOutput6* output6 = nullptr;
            DXGI_OUTPUT_DESC1 desc{};
            if (SUCCEEDED(output->QueryInterface(IID_PPV_ARGS(&output6))) && SUCCEEDED(output6->GetDesc1(&desc)))
                hdr10 = desc.Monitor == monitor && desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
            release(output6);
            release(output);
        }
        release(adapter);
    }
    return hdr10;
}

// Returns true if the swap chain presents its buffers in the color space.
bool presents(IDXGISwapChain3* swapChain, DXGI_COLOR_SPACE_TYPE space) {
    UINT support = 0;
    return SUCCEEDED(swapChain->CheckColorSpaceSupport(space, &support)) &&
        (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) != 0;
}

// Returns the format and the color space that a swap chain's intent wants for the window's display. HDR presents HDR10
// to a display in HDR mode, which then shows it without conversion, or scRGB there where the swap chain cannot present
// HDR10. Elsewhere Windows composes in sRGB, and HDR presents 8-bit sRGB as SDR does, since nothing more would show.
// DXGI presents half floats only in scRGB.
PrGpuSwapChainInfo infoFor(D3d12SwapChain* sc) {
    const PrGpuSwapChainInfo kSdr{PrGpuFormat_Bgra8, PrColorSpace_Srgb};
    if (!sc->hdr || !displayTakesHdr10(sc->displays, sc->window))
        return kSdr;
    if (presents(sc->swapChain, DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020))
        return PrGpuSwapChainInfo{PrGpuFormat_Rgb10A2, PrColorSpace_Rec2100Pq};
    return PrGpuSwapChainInfo{PrGpuFormat_Rgba16F, PrColorSpace_SrgbLinear};
}

// Makes the swap chain's buffers `info`'s format at `width` by `height`, and presents them in its color space. Every
// reference to the buffers is released first, as resizing them needs. Returns false if DXGI refuses the format.
bool takeInfo(D3d12SwapChain* sc, PrGpuSwapChainInfo info, UINT width, UINT height) {
    DXGI_FORMAT format = formatOf(info.format);
    if (FAILED(sc->swapChain->ResizeBuffers(kSwapChainBuffers, width, height, format,
            DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)))
        return false;
    DXGI_COLOR_SPACE_TYPE space = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    if (info.space == PrColorSpace_Rec2100Pq)
        space = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    else if (info.space == PrColorSpace_SrgbLinear)
        space = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
    sc->swapChain->SetColorSpace1(space);
    sc->format = format;
    sc->info = info;
    return true;
}

// Returns what an HDR swap chain wants now, if the window moved to another display or the displays' modes changed since
// it chose, as when HDR is turned on or off; or else what it has.
PrGpuSwapChainInfo infoNow(D3d12SwapChain* sc) {
    if (!sc->hdr)
        return sc->info;
    HMONITOR monitor = MonitorFromWindow(sc->window, MONITOR_DEFAULTTONEAREST);
    bool current = sc->displays->IsCurrent();
    if (monitor == sc->monitor && current)
        return sc->info;
    if (!current) {
        IDXGIFactory6* displays = nullptr;
        if (SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&displays)))) {
            release(sc->displays);
            sc->displays = displays;
        }
    }
    sc->monitor = monitor;
    return infoFor(sc);
}

// The extent of the monitor that shows most of the window, in pixels.
PrUVec2 monitorExtent(HWND window) {
    MONITORINFO info{};
    info.cbSize = sizeof info;
    if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &info))
        return PrUVec2{};
    return PrUVec2{
        uint32_t(info.rcMonitor.right - info.rcMonitor.left),
        uint32_t(info.rcMonitor.bottom - info.rcMonitor.top),
    };
}

PrStatus swapChainCreate(PrGpuDevice* device, const PrAppSurface* surface, const PrGpuSwapChainDesc* desc,
        PrGpuSwapChain* PR_NULLABLE* PR_NONNULL out) {
    D3d12Device* d = d3d12Of(device);
    if (surface->kind != PrAppSurfaceKind_Hwnd || d->factory == nullptr)
        return PrStatus_NotSupported;
    HWND window = static_cast<HWND>(surface->handle);
    // The swap chain begins in 8-bit sRGB, and takes the format that its intent wants for the window's display below.
    const PrGpuSwapChainInfo info{PrGpuFormat_Bgra8, PrColorSpace_Srgb};
    RECT client{};
    GetClientRect(window, &client);
    DXGI_SWAP_CHAIN_DESC1 s{};
    s.Width = client.right > client.left ? UINT(client.right - client.left) : 8;
    s.Height = client.bottom > client.top ? UINT(client.bottom - client.top) : 8;
    s.Format = formatOf(info.format);
    s.SampleDesc.Count = 1;
    // The buffers are readable, so that a frame can be copied or read back.
    s.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT;
    s.BufferCount = kSwapChainBuffers;
    s.Scaling = desc->anchor == PrGpuSwapChainAnchor_Stretch ? DXGI_SCALING_STRETCH : DXGI_SCALING_NONE;
    s.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    s.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    s.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    IDXGISwapChain1* chain1 = nullptr;
    if (FAILED(d->factory->CreateSwapChainForHwnd(d->queue, window, &s, nullptr, nullptr, &chain1)))
        return PrStatus_NotSupported;
    d->factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
    IDXGISwapChain3* native = nullptr;
    chain1->QueryInterface(IID_PPV_ARGS(&native));
    chain1->Release();
    if (native == nullptr)
        return PrStatus_NotSupported;
    native->SetMaximumFrameLatency(2);
    D3d12SwapChain* sc = allocZeroed<D3d12SwapChain>();
    static_cast<PrGpuSwapChain&>(*sc) = kSwapChainFunctions;
    sc->device = device;
    sc->info = info;
    sc->swapChain = native;
    sc->window = window;
    sc->hdr = desc->intent == PrGpuSwapChainIntent_Hdr;
    d->factory->AddRef();
    sc->displays = d->factory;
    if (sc->hdr) {
        sc->monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
        PrGpuSwapChainInfo chosen = infoFor(sc);
        if ((chosen.format != info.format || chosen.space != info.space) && !takeInfo(sc, chosen, s.Width, s.Height)) {
            release(sc->displays);
            release(native);
            prMemoryFree(sc);
            return PrStatus_NotSupported;
        }
    }
    sc->stretch = desc->anchor == PrGpuSwapChainAnchor_Stretch;
    sc->waitable = native->GetFrameLatencyWaitableObject();
    sc->format = s.Format;
    sc->capacity = PrUVec2{s.Width, s.Height};
    if (!takeBuffers(d, sc)) {
        releaseBuffers(d, sc);
        if (sc->waitable != nullptr)
            CloseHandle(sc->waitable);
        release(sc->displays);
        release(native);
        prMemoryFree(sc);
        return PrStatus_Limit;
    }
    *out = sc;
    return PrStatus_Ok;
}

void swapChainDestroy(PrGpuSwapChain* handle) {
    PrGpuDevice* device = static_cast<GpuSharedSwapChain*>(handle)->device;
    D3d12Device* d = d3d12Of(device);
    D3d12SwapChain* sc = static_cast<D3d12SwapChain*>(handle);
    // DXGI lets a window have one swap chain at a time, so the swap chain is gone before this returns, and the window
    // can take another.
    waitIdle(device);
    releaseBuffers(d, sc);
    if (sc->waitable != nullptr)
        CloseHandle(sc->waitable);
    release(sc->displays);
    release(sc->swapChain);
    prMemoryFree(sc);
}

// The waitable object counts the frames that the swap chain can take, and each wait takes one, so a frame waits once.
void swapChainWait(PrGpuSwapChain* handle) {
    D3d12SwapChain* sc = static_cast<D3d12SwapChain*>(handle);
    if (sc->waited)
        return;
    if (sc->waitable != nullptr)
        WaitForSingleObjectEx(sc->waitable, 1000, TRUE);
    sc->waited = true;
}

PrStatus swapChainAcquire(PrGpuSwapChain* handle, PrUVec2 extent, PrGpuTexture* out) {
    PrGpuDevice* device = static_cast<GpuSharedSwapChain*>(handle)->device;
    D3d12Device* d = d3d12Of(device);
    D3d12SwapChain* sc = static_cast<D3d12SwapChain*>(handle);
    swapChainWait(sc);
    sc->waited = false;
    // Stretched buffers are the frame's extent. Anchored buffers grow, to the extent of the window's monitor at least,
    // so that a window dragged larger resizes them once.
    PrUVec2 capacity = extent;
    if (!sc->stretch) {
        capacity = sc->capacity;
        if (extent.x > capacity.x || extent.y > capacity.y) {
            capacity = monitorExtent(sc->window);
            capacity.x = capacity.x > extent.x ? capacity.x : extent.x;
            capacity.y = capacity.y > extent.y ? capacity.y : extent.y;
        }
    }
    // The format follows the window's display, and changes as the buffers' extent does.
    PrGpuSwapChainInfo info = infoNow(sc);
    bool reformat = info.format != sc->info.format || info.space != sc->info.space;
    if (capacity.x != sc->capacity.x || capacity.y != sc->capacity.y || sc->buffers[0] == 0 || reformat) {
        // Resizing the buffers needs every reference to them released, and the work that uses them done.
        waitIdle(device);
        releaseBuffers(d, sc);
        bool resized = takeInfo(sc, reformat ? info : sc->info, capacity.x, capacity.y);
        if (resized)
            sc->capacity = capacity;
        bool taken = takeBuffers(d, sc);
        if (!resized || !taken)
            return PrStatus_InvalidState;
    }
    uint32_t slot = sc->buffers[sc->swapChain->GetCurrentBackBufferIndex()];
    d->resources[slot].texture.extent = PrGpuExtent{extent.x, extent.y, 1};
    sc->current = PrGpuTexture{slot};
    *out = sc->current;
    return PrStatus_Ok;
}

void swapChainPresent(PrGpuSwapChain* handle, PrGpuCommands* commands) {
    D3d12SwapChain* sc = static_cast<D3d12SwapChain*>(handle);
    if (sc->current.id == 0)
        return;
    commandsOf(commands)->presents.push(sc);
    sc->current = PrGpuTexture{};
}

// ---------------------------------------------------------------------------------------------------------------------
// Creation

// Writes the debug layer's errors and warnings to the standard error, except the warning for a clear of a texture made
// without an optimized clear value, which no texture has.
void CALLBACK reportMessage(D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID id,
        LPCSTR description, void*) {
    if (severity <= D3D12_MESSAGE_SEVERITY_WARNING && id != D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE &&
            id != D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE)
        fprintf(stderr, "d3d12: %s\n", description);
}

// Returns true if a device has what the backend needs: shader model 6.6's descriptor heaps, which resource binding tier
// 3 indexes without bounds, and enhanced barriers.
bool hasFeatures(ID3D12Device* device) {
    D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_6};
    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    D3D12_FEATURE_DATA_D3D12_OPTIONS12 options12{};
    return SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model))) &&
        model.HighestShaderModel >= D3D_SHADER_MODEL_6_6 &&
        SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options))) &&
        options.ResourceBindingTier >= D3D12_RESOURCE_BINDING_TIER_3 &&
        SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &options12, sizeof(options12))) &&
        options12.EnhancedBarriersSupported;
}

// Creates a device on the first adapter, in order of performance, that has what the backend needs, or returns null.
ID3D12Device10* PR_NULLABLE createDevice(IDXGIFactory6* factory) {
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
            IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        ID3D12Device10* device = nullptr;
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 &&
                SUCCEEDED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device))) &&
                !hasFeatures(device)) {
            device->Release();
            device = nullptr;
        }
        adapter->Release();
        if (device != nullptr)
            return device;
    }
    return nullptr;
}

// Creates the root signature that every pipeline shares, which pr/gpu.slang's registers name.
ID3D12RootSignature* PR_NULLABLE createRootSignature(ID3D12Device* device) {
    D3D12_ROOT_PARAMETER1 parameters[3]{};
    parameters[D3d12Root_Push].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[D3d12Root_Push].Constants = D3D12_ROOT_CONSTANTS{0, 0, kPrGpuMaxPushBytes / 4};
    parameters[D3d12Root_Uniforms].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[D3d12Root_Uniforms].Descriptor = D3D12_ROOT_DESCRIPTOR1{1, 0, D3D12_ROOT_DESCRIPTOR_FLAG_NONE};
    parameters[D3d12Root_TargetCopy].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[D3d12Root_TargetCopy].Constants = D3D12_ROOT_CONSTANTS{2, 0, 1};
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
    desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    desc.Desc_1_1.NumParameters = 3;
    desc.Desc_1_1.pParameters = parameters;
    desc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
        D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;
    ID3DBlob* blob = nullptr;
    ID3DBlob* error = nullptr;
    ID3D12RootSignature* signature = nullptr;
    if (SUCCEEDED(D3D12SerializeVersionedRootSignature(&desc, &blob, &error)))
        device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&signature));
    release(blob);
    release(error);
    return signature;
}

// Creates the signature of indirect draws or dispatches that read arguments of one kind.
ID3D12CommandSignature* PR_NULLABLE createSignature(ID3D12Device* device, D3D12_INDIRECT_ARGUMENT_TYPE type, uint32_t bytes) {
    D3D12_INDIRECT_ARGUMENT_DESC argument{};
    argument.Type = type;
    D3D12_COMMAND_SIGNATURE_DESC desc{bytes, 1, &argument, 0};
    ID3D12CommandSignature* signature = nullptr;
    device->CreateCommandSignature(&desc, nullptr, IID_PPV_ARGS(&signature));
    return signature;
}

// Creates what the device holds beside its device and queue: its heaps, signatures, timestamps and fence. Returns false
// if the device cannot make one of them.
bool createObjects(D3d12Device* d) {
    ID3D12Device10* device = d->device;
    D3D12_DESCRIPTOR_HEAP_DESC resources{
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kMaxResources,
        D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0,
    };
    D3D12_DESCRIPTOR_HEAP_DESC samplers{
        D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kMaxSamplers,
        D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0,
    };
    if (FAILED(device->CreateDescriptorHeap(&resources, IID_PPV_ARGS(&d->resourceHeap))) ||
        FAILED(device->CreateDescriptorHeap(&samplers, IID_PPV_ARGS(&d->samplerHeap))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&d->fence))))
    return false;
    d->resourceIncrement = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    d->samplerIncrement = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    d->colorIncrement = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    d->rootSignature = createRootSignature(device);
    d->drawSignature = createSignature(device, D3D12_INDIRECT_ARGUMENT_TYPE_DRAW, sizeof(PrGpuDrawIndirect));
    d->drawIndexedSignature = createSignature(device, D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED,
        sizeof(PrGpuDrawIndexedIndirect));
    d->dispatchSignature = createSignature(device, D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH,
        sizeof(PrGpuDispatchIndirect));
    d->zeros = createBuffer(d, kZeroUniformBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE);
    if (d->rootSignature == nullptr || d->drawSignature == nullptr || d->drawIndexedSignature == nullptr ||
            d->dispatchSignature == nullptr || d->zeros == nullptr)
        return false;
    // The timestamps that each timed command buffer writes, at its start and its end, and the memory that they resolve
    // into.
    UINT64 frequency = 0;
    if (SUCCEEDED(d->queue->GetTimestampFrequency(&frequency)))
        d->ticksPerSecond = double(frequency);
    D3D12_QUERY_HEAP_DESC queries{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kTimestampPairs * 2, 0};
    if (SUCCEEDED(device->CreateQueryHeap(&queries, IID_PPV_ARGS(&d->timestamps)))) {
        d->timestampReadback = createBuffer(d, uint64_t(kTimestampPairs) * 16, D3D12_HEAP_TYPE_READBACK,
            D3D12_RESOURCE_FLAG_NONE);
        void* mapped = nullptr;
        if (d->timestampReadback != nullptr && SUCCEEDED(d->timestampReadback->Map(0, nullptr, &mapped))) {
            d->timestampTicks = static_cast<const uint64_t*>(mapped);
            for (uint32_t i = kTimestampPairs; i-- > 0;)
                d->freeTimestamps.push(i);
        }
    }
    return true;
}

// Names the device for its info, from its adapter's name.
void nameAdapter(D3d12Device* d) {
    LUID luid = d->device->GetAdapterLuid();
    IDXGIAdapter1* adapter = nullptr;
    DXGI_ADAPTER_DESC1 desc{};
    if (d->factory != nullptr && SUCCEEDED(d->factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter))))
        adapter->GetDesc1(&desc);
    release(adapter);
    uint32_t units = uint32_t(lstrlenW(desc.Description));
    uint32_t capacity = uint32_t(sizeof(d->adapter) - 1);
    uint32_t size = prWindowsNarrow(desc.Description, units, d->adapter, capacity);
    if (size == 0 || size > capacity)
        prMemCopy(d->adapter, "Direct3D 12", sizeof("Direct3D 12"));
    else
        d->adapter[size] = 0;
}

// The functions of the device's objects.
constexpr PrGpuCommands kCommandsFunctions = {
    .deviceFn = gpuSharedCommandsDevice,
    .submitFn = commandsSubmit,
    .streamFn = commandsStream,
    .setPushFn = commandsSetPush,
    .setUniformsFn = commandsSetUniforms,
    .setPipelineFn = commandsSetPipeline,
    .dispatchFn = commandsDispatch,
    .dispatchIndirectFn = commandsDispatchIndirect,
    .copyBufferFn = commandsCopyBuffer,
    .copyTextureFn = commandsCopyTexture,
    .copyBufferToTextureFn = commandsCopyBufferToTexture,
    .copyTextureToBufferFn = commandsCopyTextureToBuffer,
    .generateMipmapsFn = gpuSharedGenerateMipmaps,
    .onDoneFn = commandsOnDone,
    .pushGroupFn = commandsPushGroup,
    .popGroupFn = commandsPopGroup,
    .bufferBarrierFn = commandsBufferBarrier,
    .textureBarrierFn = commandsTextureBarrier,
    .globalBarrierFn = commandsGlobalBarrier,
    .nativeStateSyncFn = nativeStateSync,
    .passBeginFn = passBegin,
};

constexpr PrGpuPass kPassFunctions = {
    .commandsFn = gpuSharedPassCommands,
    .formatsFn = gpuSharedPassFormats,
    .extentFn = gpuSharedPassExtent,
    .endFn = passEnd,
    .nativeDrawFn = passNativeDraw,
    .setViewportFn = passSetViewport,
    .setScissorFn = passSetScissor,
    .setBlendConstantFn = passSetBlendConstant,
    .setStencilReferenceFn = passSetStencilReference,
    .setIndexBufferFn = passSetIndexBuffer,
    .drawFn = passDraw,
    .drawIndexedFn = passDrawIndexed,
    .drawIndirectFn = passDrawIndirect,
    .drawIndexedIndirectFn = passDrawIndexedIndirect,
};

constexpr PrGpuSwapChain kSwapChainFunctions = {
    .deviceFn = gpuSharedSwapChainDevice,
    .infoFn = gpuSharedSwapChainInfo,
    .destroyFn = swapChainDestroy,
    .waitFn = swapChainWait,
    .acquireFn = swapChainAcquire,
    .presentFn = swapChainPresent,
};

constexpr PrGpuShaderLibrary kLibraryFunctions = {
    .destroyFn = shaderLibraryDestroy,
    .entriesFn = gpuSharedLibraryEntries,
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrGpuD3d12Options prGpuD3d12OptionsDefault(void) {
    PrGpuD3d12Options options{};
    char value[2] = {};
    if (GetEnvironmentVariableA("PR_D3D12_DEBUG", value, sizeof(value)) == 1)
        options.validation = value[0] != '0';
    return options;
}

PrStatus prGpuD3d12Create(const PrGpuD3d12Options* PR_NULLABLE options, PrGpuDevice* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    PrGpuD3d12Options o = options != nullptr ? *options : prGpuD3d12OptionsDefault();
    if (o.validation && o.device == nullptr) {
        ID3D12Debug* debug = nullptr;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
            debug->EnableDebugLayer();
        release(debug);
    }
    IDXGIFactory6* factory = nullptr;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))))
        factory = nullptr;
    ID3D12Device10* device = nullptr;
    if (o.device != nullptr) {
        static_cast<ID3D12Device*>(o.device)->QueryInterface(IID_PPV_ARGS(&device));
        if (device != nullptr && !hasFeatures(device)) {
            device->Release();
            device = nullptr;
        }
    } else if (factory != nullptr) {
        device = createDevice(factory);
    }
    if (device == nullptr) {
        release(factory);
        return PrStatus_NotSupported;
    }
    if (o.validation && o.device == nullptr) {
        ID3D12InfoQueue1* messages = nullptr;
        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&messages)))) {
            DWORD cookie = 0;
            messages->RegisterMessageCallback(reportMessage, D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &cookie);
        }
        release(messages);
    }
    ID3D12CommandQueue* queue = static_cast<ID3D12CommandQueue*>(o.queue);
    if (queue != nullptr) {
        queue->AddRef();
    } else {
        D3D12_COMMAND_QUEUE_DESC desc{D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
        if (FAILED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)))) {
            device->Release();
            release(factory);
            return PrStatus_NotSupported;
        }
    }
    D3d12Device* d = allocZeroed<D3d12Device>();
    gpuSharedDeviceInit(d);
    d->device = device;
    d->queue = queue;
    d->factory = factory;
    prMutexInit(&d->mutex);
    prMutexInit(&d->doneMutex);
    prConditionInit(&d->doneSignal);
    prConditionInit(&d->pendingSignal);
    d->resources = static_cast<D3d12Resource*>(prMemoryAllocZeroed(sizeof(D3d12Resource) * kMaxResources));
    d->pipelines = static_cast<D3d12Pipeline*>(prMemoryAllocZeroed(sizeof(D3d12Pipeline) * kMaxPipelines));
    d->nextResource = 1;
    d->nextSampler = 1;
    d->nextPipeline = 1;
    d->serial = 1;
    bool made = createObjects(d);
    prThreadStart(&d->completion, serveCompletions, d);
    if (!made) {
        destroyDevice(d);
        return PrStatus_NotSupported;
    }
    nameAdapter(d);

    d->destroyFn = destroyDevice;
    d->isLostFn = isLost;
    d->simulateLossFn = simulateLoss;
    d->isCompleteFn = isComplete;
    d->waitFn = wait;
    d->waitIdleFn = waitIdle;
    d->supportsFormatFn = supportsFormat;
    d->bufferCreateFn = bufferCreate;
    d->bufferDestroyFn = bufferDestroy;
    d->bufferDescFn = bufferDesc;
    d->bufferStorageHandleFn = bufferStorageHandle;
    d->bufferMapFn = bufferMap;
    d->bufferUploadFn = bufferUpload;
    d->textureCreateFn = textureCreate;
    d->textureCreateViewFn = textureCreateView;
    d->textureDestroyFn = textureDestroy;
    d->textureDescFn = textureDesc;
    d->textureStorageHandleFn = textureStorageHandle;
    d->textureUploadFn = textureUpload;
    d->samplerCreate = samplerCreate;
    d->shaderLibraryCreateFn = shaderLibraryCreate;
    d->graphicsPipelineCreateFn = graphicsPipelineCreate;
    d->computePipelineCreateFn = computePipelineCreate;
    d->pipelineDestroyFn = pipelineDestroy;
    d->pipelineGroupSizeFn = pipelineGroupSize;
    d->commandsBeginFn = commandsBegin;
    d->textureNativeFn = textureNative;
    d->swapChainCreateFn = swapChainCreate;
    d->shaderTarget = PrGpuShaderTarget_Dxil;
    d->info.adapter = PrStringView{d->adapter, strlen(d->adapter)};
    d->info.backend = "Direct3D 12";
    d->nativeKinds = 1u << PrGpuNativeKind_D3d12;
    d->info.features = PrGpuFeatureFlag_TextureCompressionBc |
        (d->timestampTicks != nullptr ? PrGpuFeatureFlag_Timestamps : 0);
    d->info.maxTextureSize = D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;
    d->info.maxResourceHandles = kMaxResources - 1;
    d->info.maxSamplerHandles = kMaxSamplers - 1;
    *out = d;
    return PrStatus_Ok;
}

void* prGpuD3d12Device(const PrGpuDevice* device) {
    return d3d12Of(device)->device;
}

void* prGpuD3d12Queue(const PrGpuDevice* device) {
    return d3d12Of(device)->queue;
}

PrStatus prGpuD3d12TextureWrap(PrGpuDevice* device, void* resource, PrGpuTextureUsageFlags usage, PrGpuTexture* out) {
    *out = PrGpuTexture{};
    D3d12Device* d = d3d12Of(device);
    ID3D12Resource* r = static_cast<ID3D12Resource*>(resource);
    D3D12_RESOURCE_DESC rd = r->GetDesc();
    PrGpuFormat format = gpuFormatOf(rd.Format);
    if (format == PrGpuFormat_None)
        return PrStatus_NotSupported;
    bool is3d = rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    PrGpuTextureKind kind = PrGpuTextureKind_2d;
    if (is3d)
        kind = PrGpuTextureKind_3d;
    else if (rd.DepthOrArraySize > 1)
        kind = PrGpuTextureKind_2dArray;
    PrGpuTextureDesc desc{
        .kind = kind,
        .format = format,
        .usage = usage,
        .levels = uint8_t(rd.MipLevels),
        .samples = uint8_t(rd.SampleDesc.Count),
        .extent = {uint32_t(rd.Width), uint32_t(rd.Height), uint32_t(rd.DepthOrArraySize)},
    };
    r->AddRef();
    uint32_t slot = addTexture(d, r, desc, rd.Format, uint8_t(rd.MipLevels),
        is3d ? uint16_t(1) : uint16_t(rd.DepthOrArraySize), 0, 0);
    if (slot == 0) {
        r->Release();
        return PrStatus_Limit;
    }
    out->id = slot;
    return PrStatus_Ok;
}
