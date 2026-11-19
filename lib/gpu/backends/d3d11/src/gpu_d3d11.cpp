// The Direct3D 11 GPU backend, on Direct3D 11.1 at feature level 11_0, with slots and no bindless.
//
// A command buffer records its commands into memory of its own, on any thread. Its submission replays them on the
// immediate context, which the context mutex guards. The replay binds each draw's and dispatch's slots, root data and
// pipeline as it reaches them. Root data goes into a ring of constant buffer memory, which the replay fills once for
// the whole command buffer before it records any work, and binds at offsets. A fence that the immediate context signals
// after each replay tells a thread of the device's own that the work is done.
//
// Memory that the CPU writes or reads is the CPU's own. An Upload buffer and a block of the stream hold their bytes in
// memory of the CPU's, beside a buffer of the device's that a submission fills from it before its work. A Readback
// buffer holds a staging buffer, which the device's thread copies into the CPU's memory once the work that wrote it is
// done. Uploads write the device's buffers and textures through the immediate context, which orders them before the
// work submitted after them.
//
// A destroyed object is kept until every command buffer that began before its destruction is done, since any of them
// may use it.
//
// COM objects are held as raw pointers and released by hand, because the structures that hold them run no constructors.

#include "pr/gpu_d3d11.h"

#include "pr/align.h"
#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/atomic.h"
#include "pr/gpu_backend.h"
#include "pr/mem.h"
#include "pr/min_max.h"
#include "pr/mutex.h"
#include "pr/raw_array.h"
#include "pr/thread.h"
#include "pr/windows_string.h"

#include "gpu/backends/shared/src/gpu_shared_device.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_4.h>
#include <dcomp.h>
#include <dxgi1_6.h>
#include <stdio.h>

namespace pr_impl {

namespace {

// The functions of the device's objects, which their creation installs.
extern const PrGpuCommands kCommandsFunctions;
extern const PrGpuPass kPassFunctions;
extern const PrGpuSwapChain kSwapChainFunctions;
extern const PrGpuShaderLibrary kLibraryFunctions;

// The number of slots of each kind, and the first register of the kinds that share a register type with another, as
// pr/gpu.slang places them: Texture slots after the Buffer slots at t, and RwBuffer slots after the RwTexture slots at
// u.
constexpr uint32_t kBufferSlots = 4;
constexpr uint32_t kRwBufferSlots = 4;
constexpr uint32_t kTextureSlots = 8;
constexpr uint32_t kRwTextureSlots = 4;
constexpr uint32_t kTextureRegister = kBufferSlots;
constexpr uint32_t kRwBufferRegister = kRwTextureSlots;
constexpr uint32_t kShaderResources = kBufferSlots + kTextureSlots;
constexpr uint32_t kUnorderedAccesses = kRwTextureSlots + kRwBufferSlots;

// The bytes of a stream block. A larger allocation takes a block of its own, which is freed once its work is done.
constexpr uint64_t kStreamBlock = uint64_t(1) << 20;

// The ring of root data: its first size, and the alignment of each binding of it, which constant buffer offsets take in
// units of 16 constants of 16 bytes.
constexpr uint32_t kRootRingBytes = 1u << 20;
constexpr uint32_t kRootAlignment = 256;

// The buffers of a swap chain. Three let a frame record while one shows and one waits to.
constexpr uint32_t kSwapChainBuffers = 3;

// The longest that a wait for a fence goes without checking whether the device was lost, in milliseconds.
constexpr DWORD kLostCheckMilliseconds = 1000;

template <typename T>
void release(T* PR_NULLABLE object) {
    if (object != nullptr)
        object->Release();
}

// What a resource id holds.
enum {
    D3d11Kind_Free,
    D3d11Kind_Buffer,
    D3d11Kind_Texture,
};
typedef uint8_t D3d11Kind;

// The raw views of a buffer from an offset, which slots bind. A buffer makes them the first time that a slot binds it
// at the offset.
struct D3d11BufferView {
    uint64_t offset;
    ID3D11ShaderResourceView* PR_NULLABLE read;         // held
    ID3D11UnorderedAccessView* PR_NULLABLE write;       // held
};

// The view of a level and a layer of a texture as a pass's attachment, made the first time that a pass takes it.
struct D3d11AttachmentView {
    uint8_t level;
    uint16_t layer;
    ID3D11View* view;                                   // held: a render target or a depth stencil view
};

// A buffer, a texture or a view of a texture. A view covers the subresources of its resource from `firstLevel` and
// `firstLayer`, which subresource indices count from.
struct D3d11Resource {
    D3d11Kind kind;
    union {
        PrGpuBufferDesc buffer;
        PrGpuTextureDesc texture;
    };
    ID3D11Resource* PR_NULLABLE resource;               // held
    uint64_t size;                                      // a buffer's bytes, a multiple of 16
    uint8_t* PR_NULLABLE memory;                        // the CPU's bytes of an Upload or Readback buffer or a block
    bool stream;                                        // a block of the stream
    RawArray<D3d11BufferView> bufferViews;

    // A texture's resource: its format, which is typeless where views read it in more than one, its levels and layers,
    // and its views.
    DXGI_FORMAT format;
    uint8_t resourceLevels;
    uint16_t resourceLayers;
    uint8_t firstLevel;
    uint16_t firstLayer;
    ID3D11ShaderResourceView* PR_NULLABLE read;         // held; null without PrGpuTextureUsageFlag_Sampled
    ID3D11UnorderedAccessView* PR_NULLABLE write;       // held; null without PrGpuTextureUsageFlag_Storage
    RawArray<D3d11AttachmentView> attachmentViews;
};

struct D3d11Pipeline {
    bool compute;
    ID3D11VertexShader* PR_NULLABLE vertex;             // held
    ID3D11PixelShader* PR_NULLABLE fragment;            // held
    ID3D11ComputeShader* PR_NULLABLE kernel;            // held
    ID3D11InputLayout* PR_NULLABLE layout;              // held
    ID3D11BlendState* PR_NULLABLE blend;                // held
    ID3D11RasterizerState* PR_NULLABLE raster;          // held
    ID3D11DepthStencilState* PR_NULLABLE depth;         // held
    D3D11_PRIMITIVE_TOPOLOGY topology;
    PrGpuExtent groupSize;

    // The number of vertex buffers that the pipeline reads, and their strides. Direct3D 11 takes the strides with the
    // buffers, not with the pipeline.
    uint8_t vertexBufferCount;
    UINT vertexStrides[kPrGpuMaxVertexBuffers]; // by vertex buffer
};

// An object that waits to be freed until every command buffer that began before `serial` is done.
enum {
    D3d11RetiredKind_Resource,
    D3d11RetiredKind_Pipeline,
};
typedef uint8_t D3d11RetiredKind;

struct D3d11Retired {
    uint64_t serial;
    D3d11RetiredKind kind;
    uint32_t id;
};

// A command buffer that is begun or in flight, and the fence that it completes at once it is submitted.
struct D3d11InFlight {
    uint64_t serial;
    PrGpuFence fence;                                   // zero until submitted
};

// A block of the stream that a command buffer fills, and the bytes of it that it used.
struct D3d11Block {
    uint32_t id;
    uint64_t used;
};

// A block of the stream that submitted work reads, which is free again once its fence is reached.
struct D3d11Held {
    PrGpuFence fence;
    uint32_t id;
};

// A function that a command buffer calls once it is done.
struct D3d11Done {
    PrGpuDoneFn done;
    void* PR_NULLABLE context;
};

// The queries that time a submission: its start and end on the GPU's clock, within a disjoint query that gives the
// clock's frequency.
struct D3d11Clock {
    ID3D11Query* disjoint;                              // held
    ID3D11Query* start;                                 // held
    ID3D11Query* end;                                   // held
};

// A copy into a Readback buffer that the device's thread finishes once the work is done: the staging buffer's bytes, or
// the texels of a staging texture of `images` subresources, each `rows` rows of `rowSize` bytes, which land `rowBytes`
// and `imageBytes` apart at `offset` into the buffer.
struct D3d11Readback {
    uint32_t buffer;
    ID3D11Texture2D* PR_NULLABLE staging;               // held; null for a copy from a buffer
    ID3D11Texture3D* PR_NULLABLE staging3d;             // held
    uint64_t offset;
    uint32_t rowBytes;
    uint64_t imageBytes;
    uint32_t images;
    uint32_t rows;
    uint32_t rowSize;
    uint32_t depth;                                     // the slices of a 3D staging texture
};

// A submission that the device's thread waits for.
struct D3d11Submission {
    PrGpuFence fence;
    D3d11Done* PR_NULLABLE dones;
    uint32_t doneCount;
    D3d11Readback* PR_NULLABLE readbacks;
    uint32_t readbackCount;
    D3d11Clock clock;                                   // null queries for a submission that is not timed
};

struct D3d11Library : GpuSharedLibrary {};

struct D3d11Pass;
struct D3d11SwapChain;

struct D3d11Commands : GpuSharedCommands {
    uint64_t serial;                                    // the order in which command buffers began
    RawArray<uint8_t> ops;                              // the recording
    D3d11Pass* PR_NULLABLE pass;                        // the open pass
    RawArray<D3d11Block> blocks;                        // the blocks of the stream
    RawArray<D3d11SwapChain*> presents;                 // the swap chains that present once the work is replayed
    RawArray<D3d11Done> dones;
    RawArray<void*> scratch;                            // what the stream gave out once the device had no memory left
};

struct D3d11Pass : GpuSharedPass {};

// DXGI presents a swap chain's whole buffers. Anchored at the top left, unscaled, the buffers are at least as large as
// the frames, which draw at their top left, and the window cuts them off at its edges. The buffers only grow, so that a
// resize within them waits for no work and allocates nothing. Stretched, the buffers are the frame's extent, which DXGI
// scales to the window, and each resize resizes them. Direct3D 11 shows the buffer to draw into as buffer 0.
struct D3d11SwapChain : GpuSharedSwapChain {
    IDXGISwapChain3* swapChain;                         // held
    HWND window;
    bool hdr;                                           // PrGpuSwapChainIntent_Hdr
    HMONITOR PR_NULLABLE monitor;                       // the display that the format was chosen for
    IDXGIFactory6* displays;                            // held; made again as the displays' modes change
    HANDLE PR_NULLABLE waitable;
    bool waited;                                        // the swap chain was waited on for the next frame
    bool stretch;                                       // PrGpuSwapChainAnchor_Stretch
    DXGI_FORMAT format;
    uint32_t buffer;                                    // the resource id of buffer 0
    PrUVec2 capacity;                                   // the buffers' extent
    PrGpuTexture current;                               // the buffer of the acquired frame

    // A transparent swap chain presents premultiplied alpha, which only a swap chain for composition can. A visual of
    // DirectComposition shows it over the window's client area, unscaled at the top left. Each is held, or null.
    IDCompositionDevice* PR_NULLABLE composition;
    IDCompositionTarget* PR_NULLABLE target;
    IDCompositionVisual* PR_NULLABLE visual;
};

struct D3d11Device : GpuSharedDevice {
    PrGpuD3d11Options options;
    ID3D11Device5* device;                              // held
    IDXGIFactory6* PR_NULLABLE factory;                 // held
    ID3D11Fence* fence;                                 // held
    ID3D11InfoQueue* PR_NULLABLE messages;              // held; the debug layer's, with validation
    Atomic<bool> lost;
    char adapter[128];                                  // the adapter's name, in UTF-8

    // The context mutex guards the immediate context and everything that replays read and write: the root ring, the
    // views that buffers and textures make as they are bound, and the queries.
    PrMutex contextMutex;
    ID3D11DeviceContext4* context;                      // held
    ID3DUserDefinedAnnotation* PR_NULLABLE annotation;  // held
    ID3D11Buffer* root;                                 // the ring of root data, held
    uint32_t rootSize;
    uint32_t rootUsed;
    ID3D11Buffer* zeros;                                // the root data of a command buffer that sets none, held
    RawArray<D3d11Clock> clocks;                        // free

    // The completion thread, and what it waits for. The done mutex guards the submissions and `stopping`, and raising
    // `done`.
    PrThread completion;
    Atomic<uint64_t> done;                              // the fence of the last submission that is done
    PrMutex doneMutex;
    PrCondition doneSignal;                             // broadcast when `done` rises
    PrCondition pendingSignal;                          // signaled when a submission is added
    RawArray<D3d11Submission> pending;                  // in the order of their fences
    bool stopping;

    // The mutex guards everything below, except the tables' references. The tables never move, so that recording reads
    // them without it.
    PrMutex mutex;
    GpuSharedTable<D3d11Resource> resources;
    GpuSharedTable<D3d11Pipeline> pipelines;
    GpuSharedTable<ID3D11SamplerState*> samplers;       // held
    RawArray<uint32_t> uploadBuffers;                   // the ids of the Upload buffers
    RawArray<uint32_t> streamBlocks;                    // the ids of the free blocks of kStreamBlock bytes
    RawArray<D3d11Held> held;

    PrGpuFence submitted;                               // the last fence that a submission returned
    uint64_t serial;                                    // the serial of the next command buffer
    RawArray<D3d11InFlight> inFlight;                   // in the order of their submission
    RawArray<D3d11Retired> retired;
};

D3d11Device* d3d11Of(PrGpuDevice* device) {
    return static_cast<D3d11Device*>(device);
}

const D3d11Device* d3d11Of(const PrGpuDevice* device) {
    return static_cast<const D3d11Device*>(device);
}

D3d11Commands* commandsOf(PrGpuCommands* commands) {
    return static_cast<D3d11Commands*>(commands);
}

D3d11Device* deviceOfCommands(PrGpuCommands* commands) {
    return d3d11Of(static_cast<GpuSharedCommands*>(commands)->device);
}

// Names an object for debuggers and captures.
void nameObject(ID3D11DeviceChild* object, PrStringView name) {
    if (name.size != 0)
        object->SetPrivateData(WKPDID_D3DDebugObjectName, UINT(name.size), name.data);
}

// ---------------------------------------------------------------------------------------------------------------------
// Formats and states

DXGI_FORMAT vertexFormatOf(PrGpuVertexFormat format) {
    switch (format) {
        case PrGpuVertexFormat_Float: return DXGI_FORMAT_R32_FLOAT;
        case PrGpuVertexFormat_Float2: return DXGI_FORMAT_R32G32_FLOAT;
        case PrGpuVertexFormat_Float3: return DXGI_FORMAT_R32G32B32_FLOAT;
        case PrGpuVertexFormat_Float4: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case PrGpuVertexFormat_Half2: return DXGI_FORMAT_R16G16_FLOAT;
        case PrGpuVertexFormat_Half4: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case PrGpuVertexFormat_Uint: return DXGI_FORMAT_R32_UINT;
        case PrGpuVertexFormat_Uint2: return DXGI_FORMAT_R32G32_UINT;
        case PrGpuVertexFormat_Uint3: return DXGI_FORMAT_R32G32B32_UINT;
        case PrGpuVertexFormat_Uint4: return DXGI_FORMAT_R32G32B32A32_UINT;
        case PrGpuVertexFormat_Sint: return DXGI_FORMAT_R32_SINT;
        case PrGpuVertexFormat_Sint2: return DXGI_FORMAT_R32G32_SINT;
        case PrGpuVertexFormat_Sint3: return DXGI_FORMAT_R32G32B32_SINT;
        case PrGpuVertexFormat_Sint4: return DXGI_FORMAT_R32G32B32A32_SINT;
        case PrGpuVertexFormat_Unorm8x4: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case PrGpuVertexFormat_Snorm8x4: return DXGI_FORMAT_R8G8B8A8_SNORM;
        case PrGpuVertexFormat_Uint8x4: return DXGI_FORMAT_R8G8B8A8_UINT;
        case PrGpuVertexFormat_Unorm16x2: return DXGI_FORMAT_R16G16_UNORM;
        case PrGpuVertexFormat_Unorm16x4: return DXGI_FORMAT_R16G16B16A16_UNORM;
        case PrGpuVertexFormat_Snorm16x2: return DXGI_FORMAT_R16G16_SNORM;
        case PrGpuVertexFormat_Snorm16x4: return DXGI_FORMAT_R16G16B16A16_SNORM;
        default: return DXGI_FORMAT_UNKNOWN;
    }
}

DXGI_FORMAT formatOf(PrGpuFormat format) {
    switch (prGpuFormatStored(format)) {
        case PrGpuFormat_R8: return DXGI_FORMAT_R8_UNORM;
        case PrGpuFormat_Rg8: return DXGI_FORMAT_R8G8_UNORM;
        case PrGpuFormat_R16: return DXGI_FORMAT_R16_UNORM;
        case PrGpuFormat_Rg16: return DXGI_FORMAT_R16G16_UNORM;
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
        case PrGpuFormat_Bc2: return DXGI_FORMAT_BC2_UNORM;
        case PrGpuFormat_Bc2Srgb: return DXGI_FORMAT_BC2_UNORM_SRGB;
        case PrGpuFormat_Bc3: return DXGI_FORMAT_BC3_UNORM;
        case PrGpuFormat_Bc3Srgb: return DXGI_FORMAT_BC3_UNORM_SRGB;
        case PrGpuFormat_Bc4: return DXGI_FORMAT_BC4_UNORM;
        case PrGpuFormat_Bc4Snorm: return DXGI_FORMAT_BC4_SNORM;
        case PrGpuFormat_Bc5: return DXGI_FORMAT_BC5_UNORM;
        case PrGpuFormat_Bc5Snorm: return DXGI_FORMAT_BC5_SNORM;
        case PrGpuFormat_Bc6HUfloat: return DXGI_FORMAT_BC6H_UF16;
        case PrGpuFormat_Bc6HSfloat: return DXGI_FORMAT_BC6H_SF16;
        case PrGpuFormat_Bc7: return DXGI_FORMAT_BC7_UNORM;
        case PrGpuFormat_Bc7Srgb: return DXGI_FORMAT_BC7_UNORM_SRGB;
        default: return DXGI_FORMAT_UNKNOWN;
    }
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

D3D11_COMPARISON_FUNC compareOf(PrGpuCompare compare) {
    switch (compare) {
        case PrGpuCompare_Never: return D3D11_COMPARISON_NEVER;
        case PrGpuCompare_Less: return D3D11_COMPARISON_LESS;
        case PrGpuCompare_Equal: return D3D11_COMPARISON_EQUAL;
        case PrGpuCompare_LessEqual: return D3D11_COMPARISON_LESS_EQUAL;
        case PrGpuCompare_Greater: return D3D11_COMPARISON_GREATER;
        case PrGpuCompare_NotEqual: return D3D11_COMPARISON_NOT_EQUAL;
        case PrGpuCompare_GreaterEqual: return D3D11_COMPARISON_GREATER_EQUAL;
        default: return D3D11_COMPARISON_ALWAYS;
    }
}

// The weight of a blend. Direct3D 11 takes no color weight for alpha, so an alpha blend takes the alpha of a color
// weight.
D3D11_BLEND blendFactorOf(PrGpuBlendFactor factor, bool alpha) {
    switch (factor) {
        case PrGpuBlendFactor_Zero: return D3D11_BLEND_ZERO;
        case PrGpuBlendFactor_Source: return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
        case PrGpuBlendFactor_OneMinusSource: return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
        case PrGpuBlendFactor_SourceAlpha: return D3D11_BLEND_SRC_ALPHA;
        case PrGpuBlendFactor_OneMinusSourceAlpha: return D3D11_BLEND_INV_SRC_ALPHA;
        case PrGpuBlendFactor_Target: return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
        case PrGpuBlendFactor_OneMinusTarget: return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
        case PrGpuBlendFactor_TargetAlpha: return D3D11_BLEND_DEST_ALPHA;
        case PrGpuBlendFactor_OneMinusTargetAlpha: return D3D11_BLEND_INV_DEST_ALPHA;
        case PrGpuBlendFactor_SourceAlphaSaturated: return D3D11_BLEND_SRC_ALPHA_SAT;
        case PrGpuBlendFactor_Constant: return D3D11_BLEND_BLEND_FACTOR;
        case PrGpuBlendFactor_OneMinusConstant: return D3D11_BLEND_INV_BLEND_FACTOR;
        case PrGpuBlendFactor_Source1: return alpha ? D3D11_BLEND_SRC1_ALPHA : D3D11_BLEND_SRC1_COLOR;
        case PrGpuBlendFactor_OneMinusSource1: return alpha ? D3D11_BLEND_INV_SRC1_ALPHA : D3D11_BLEND_INV_SRC1_COLOR;
        default: return D3D11_BLEND_ONE;
    }
}

D3D11_BLEND_OP blendOpOf(PrGpuBlendOp op) {
    switch (op) {
        case PrGpuBlendOp_Subtract: return D3D11_BLEND_OP_SUBTRACT;
        case PrGpuBlendOp_ReverseSubtract: return D3D11_BLEND_OP_REV_SUBTRACT;
        case PrGpuBlendOp_Min: return D3D11_BLEND_OP_MIN;
        case PrGpuBlendOp_Max: return D3D11_BLEND_OP_MAX;
        default: return D3D11_BLEND_OP_ADD;
    }
}

D3D11_STENCIL_OP stencilOpOf(PrGpuStencilOp op) {
    switch (op) {
        case PrGpuStencilOp_Zero: return D3D11_STENCIL_OP_ZERO;
        case PrGpuStencilOp_Replace: return D3D11_STENCIL_OP_REPLACE;
        case PrGpuStencilOp_IncrementClamp: return D3D11_STENCIL_OP_INCR_SAT;
        case PrGpuStencilOp_DecrementClamp: return D3D11_STENCIL_OP_DECR_SAT;
        case PrGpuStencilOp_Invert: return D3D11_STENCIL_OP_INVERT;
        case PrGpuStencilOp_IncrementWrap: return D3D11_STENCIL_OP_INCR;
        case PrGpuStencilOp_DecrementWrap: return D3D11_STENCIL_OP_DECR;
        default: return D3D11_STENCIL_OP_KEEP;
    }
}

D3D11_TEXTURE_ADDRESS_MODE addressOf(PrGpuAddress address) {
    switch (address) {
        case PrGpuAddress_Repeat: return D3D11_TEXTURE_ADDRESS_WRAP;
        case PrGpuAddress_Mirror: return D3D11_TEXTURE_ADDRESS_MIRROR;
        case PrGpuAddress_Border: return D3D11_TEXTURE_ADDRESS_BORDER;
        default: return D3D11_TEXTURE_ADDRESS_CLAMP;
    }
}

D3D11_PRIMITIVE_TOPOLOGY topologyOf(PrGpuTopology topology) {
    switch (topology) {
        case PrGpuTopology_TriangleStrip: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
        case PrGpuTopology_LineList: return D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
        case PrGpuTopology_LineStrip: return D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
        case PrGpuTopology_PointList: return D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
        default: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Ids and retirement

// Takes a free resource id, or returns zero if none is left. The caller holds the mutex.
uint32_t takeIdLocked(D3d11Device* d) {
    return d->resources.take();
}

// Releases what a resource holds. The caller holds the mutex.
void releaseResource(D3d11Resource& r) {
    for (uint32_t i = 0; i < r.bufferViews.count; ++i) {
        release(r.bufferViews.data[i].read);
        release(r.bufferViews.data[i].write);
    }
    r.bufferViews.reset();
    for (uint32_t i = 0; i < r.attachmentViews.count; ++i)
        release(r.attachmentViews.data[i].view);
    r.attachmentViews.reset();
    release(r.read);
    release(r.write);
    release(r.resource);
    prMemoryFree(r.memory);
    r = D3d11Resource{};
}

// Frees a resource id and what it holds. The caller holds the mutex.
void freeResource(D3d11Device* d, uint32_t id) {
    if (d->resources[id].kind == D3d11Kind_Buffer && d->resources[id].buffer.memory == PrGpuMemory_Upload &&
            !d->resources[id].stream) {
        uint32_t at = d->uploadBuffers.find(id);
        if (at != kArrayMissing)
            d->uploadBuffers.erase(at);
    }
    releaseResource(d->resources[id]);
    d->resources.free(id);
}

void releasePipeline(D3d11Pipeline& p) {
    release(p.vertex);
    release(p.fragment);
    release(p.kernel);
    release(p.layout);
    release(p.blend);
    release(p.raster);
    release(p.depth);
    p = D3d11Pipeline{};
}

// Frees what the command buffers that are done held: retired objects, and the blocks of their stream. The caller holds
// the mutex.
void collect(D3d11Device* d) {
    uint64_t done = d->done.load();
    uint32_t kept = 0;
    for (uint32_t i = 0; i < d->inFlight.count; ++i) {
        const D3d11InFlight& f = d->inFlight.data[i];
        if (f.fence == 0 || f.fence > done)
            d->inFlight.data[kept++] = f;
    }
    d->inFlight.count = kept;
    uint64_t oldest = kept != 0 ? d->inFlight.data[0].serial : d->serial;
    kept = 0;
    for (uint32_t i = 0; i < d->retired.count; ++i) {
        const D3d11Retired& r = d->retired.data[i];
        if (r.serial > oldest) {
            d->retired.data[kept++] = r;
        } else if (r.kind == D3d11RetiredKind_Resource) {
            freeResource(d, r.id);
        } else {
            releasePipeline(d->pipelines[r.id]);
            d->pipelines.free(r.id);
        }
    }
    d->retired.count = kept;
    kept = 0;
    for (uint32_t i = 0; i < d->held.count; ++i) {
        const D3d11Held& h = d->held.data[i];
        if (h.fence > done)
            d->held.data[kept++] = h;
        else if (d->resources[h.id].size == kStreamBlock)
            d->streamBlocks.push(h.id);
        else
            freeResource(d, h.id);
    }
    d->held.count = kept;
}

// Retires an object, which is freed once the command buffers that began before now are done.
void retire(D3d11Device* d, D3d11RetiredKind kind, uint32_t id) {
    prMutexLock(&d->mutex);
    d->retired.push(D3d11Retired{d->serial, kind, id});
    collect(d);
    prMutexUnlock(&d->mutex);
}

// ---------------------------------------------------------------------------------------------------------------------
// The device

// Returns true if the device is lost, and notes it.
bool isLostNow(const D3d11Device* d) {
    if (d->lost.load())
        return true;
    if (d->device->GetDeviceRemovedReason() == S_OK)
        return false;
    const_cast<D3d11Device*>(d)->lost.store(true);
    return true;
}

bool removed(const GpuSharedDevice* device) {
    return isLostNow(static_cast<const D3d11Device*>(device));
}

// Direct3D 11 has no way to remove a device, so a simulated loss only marks the device lost.
void simulateLoss(PrGpuDevice* device) {
    d3d11Of(device)->lost.store(true);
}

bool isComplete(const PrGpuDevice* device, PrGpuFence fence) {
    const D3d11Device* d = d3d11Of(device);
    return isLostNow(d) || d->done.load() >= fence;
}

void wait(PrGpuDevice* device, PrGpuFence fence) {
    D3d11Device* d = d3d11Of(device);
    prMutexLock(&d->doneMutex);
    while (d->done.load() < fence)
        prConditionWait(&d->doneSignal, &d->doneMutex);
    prMutexUnlock(&d->doneMutex);
    prMutexLock(&d->mutex);
    collect(d);
    prMutexUnlock(&d->mutex);
}

void waitIdle(PrGpuDevice* device) {
    D3d11Device* d = d3d11Of(device);
    prMutexLock(&d->mutex);
    PrGpuFence fence = d->submitted;
    prMutexUnlock(&d->mutex);
    wait(device, fence);
}

// Copies the staging memory of a submission's reads into their Readback buffers. The caller holds the context mutex.
void finishReadback(D3d11Device* d, const D3d11Readback& r) {
    uint8_t* memory = d->resources[r.buffer].memory;
    if (r.staging == nullptr && r.staging3d == nullptr) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        ID3D11Resource* staging = d->resources[r.buffer].resource;
        if (SUCCEEDED(d->context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
            prMemCopy(memory, mapped.pData, size_t(d->resources[r.buffer].size));
            d->context->Unmap(staging, 0);
        }
        return;
    }
    ID3D11Resource* staging = r.staging != nullptr ? static_cast<ID3D11Resource*>(r.staging) : r.staging3d;
    for (uint32_t image = 0; image < r.images; ++image) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(d->context->Map(staging, image, D3D11_MAP_READ, 0, &mapped)))
            continue;
        for (uint32_t z = 0; z < r.depth; ++z) {
            for (uint32_t y = 0; y < r.rows; ++y) {
                const uint8_t* from = static_cast<const uint8_t*>(mapped.pData) + size_t(z) * mapped.DepthPitch +
                    size_t(y) * mapped.RowPitch;
                uint64_t at = r.offset + (image + z) * r.imageBytes + uint64_t(y) * r.rowBytes;
                prMemCopy(memory + at, from, r.rowSize);
            }
        }
        d->context->Unmap(staging, image);
    }
}

// Waits for each submission's fence in order, finishes its reads, then calls its functions with the seconds that its
// queries measured, and raises the count of fences done.
void serveCompletions(void* PR_NULLABLE context) {
    D3d11Device* d = static_cast<D3d11Device*>(context);
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    prMutexLock(&d->doneMutex);
    for (;;) {
        while (d->pending.count == 0 && !d->stopping)
            prConditionWait(&d->pendingSignal, &d->doneMutex);
        if (d->pending.count == 0)
            break;
        D3d11Submission s = d->pending.data[0];
        d->pending.erase(0);
        prMutexUnlock(&d->doneMutex);
        while (d->fence->GetCompletedValue() < s.fence && !isLostNow(d)) {
            if (FAILED(d->fence->SetEventOnCompletion(s.fence, event)))
                break;
            WaitForSingleObject(event, kLostCheckMilliseconds);
        }
        double seconds = 0;
        prMutexLock(&d->contextMutex);
        bool lost = isLostNow(d);
        for (uint32_t i = 0; i < s.readbackCount; ++i) {
            if (!lost)
                finishReadback(d, s.readbacks[i]);
            release(s.readbacks[i].staging);
            release(s.readbacks[i].staging3d);
        }
        if (s.clock.disjoint != nullptr) {
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT frequency{};
            UINT64 start = 0;
            UINT64 end = 0;
            // The fence is reached, so the queries hold their data, unless the device was lost.
            bool read = !lost && d->context->GetData(s.clock.disjoint, &frequency, sizeof(frequency), 0) == S_OK &&
                d->context->GetData(s.clock.start, &start, sizeof(start), 0) == S_OK &&
                d->context->GetData(s.clock.end, &end, sizeof(end), 0) == S_OK;
            if (read && !frequency.Disjoint && frequency.Frequency != 0 && end > start)
                seconds = double(end - start) / double(frequency.Frequency);
            d->clocks.push(s.clock);
        }
        prMutexUnlock(&d->contextMutex);
        for (uint32_t i = 0; i < s.doneCount; ++i)
            s.dones[i].done(s.dones[i].context, seconds);
        prMemoryFree(s.dones);
        prMemoryFree(s.readbacks);
        prMutexLock(&d->doneMutex);
        if (d->done.load() < s.fence)
            d->done.store(s.fence);
        prConditionBroadcast(&d->doneSignal);
    }
    prMutexUnlock(&d->doneMutex);
    CloseHandle(event);
}

bool supportsFormat(const PrGpuDevice* device, PrGpuFormat format, PrGpuTextureUsageFlags usage) {
    const D3d11Device* d = d3d11Of(device);
    if (formatOf(format) == DXGI_FORMAT_UNKNOWN)
        return false;
    bool compressed = prGpuFormatIsCompressed(format);
    if (compressed && (usage & ~PrGpuTextureUsageFlag_Sampled))
        return false;
    bool depth = prGpuFormatIsDepthStencil(format);
    if ((usage & PrGpuTextureUsageFlag_DepthStencil) && !depth)
        return false;
    if ((usage & (PrGpuTextureUsageFlag_RenderTarget | PrGpuTextureUsageFlag_Storage)) && depth)
        return false;
    UINT shader = 0;
    UINT target = 0;
    UINT storage = 0;
    if (FAILED(d->device->CheckFormatSupport(shaderFormatOf(format), &shader)) ||
            FAILED(d->device->CheckFormatSupport(formatOf(format), &target)))
        return false;
    d->device->CheckFormatSupport(storageFormatOf(format), &storage);
    if ((usage & PrGpuTextureUsageFlag_Sampled) && !(shader & D3D11_FORMAT_SUPPORT_SHADER_LOAD))
        return false;
    if ((usage & PrGpuTextureUsageFlag_RenderTarget) && !(target & D3D11_FORMAT_SUPPORT_RENDER_TARGET))
        return false;
    if ((usage & PrGpuTextureUsageFlag_DepthStencil) && !(target & D3D11_FORMAT_SUPPORT_DEPTH_STENCIL))
        return false;
    if ((usage & PrGpuTextureUsageFlag_Storage) && !(storage & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW))
        return false;
    return true;
}

// Frees the device. A device whose initialization failed has no completion thread and no work.
void destroyDevice(GpuSharedDevice* device) {
    D3d11Device* d = static_cast<D3d11Device*>(device);
    gpuSharedDeviceReset(d);
    if (gpuSharedDeviceIsReady(d)) {
        prMutexLock(&d->doneMutex);
        d->stopping = true;
        prConditionSignal(&d->pendingSignal);
        prMutexUnlock(&d->doneMutex);
        prThreadJoin(&d->completion);
    }
    prMutexLock(&d->mutex);
    for (uint32_t i = d->resources.firstId; i < d->resources.nextId; ++i)
        releaseResource(d->resources[i]);
    for (uint32_t i = d->pipelines.firstId; i < d->pipelines.nextId; ++i)
        releasePipeline(d->pipelines[i]);
    for (uint32_t i = d->samplers.firstId; i < d->samplers.nextId; ++i)
        release(d->samplers[i]);
    prMutexUnlock(&d->mutex);
    for (uint32_t i = 0; i < d->clocks.count; ++i) {
        release(d->clocks.data[i].disjoint);
        release(d->clocks.data[i].start);
        release(d->clocks.data[i].end);
    }
    d->resources.reset();
    d->pipelines.reset();
    d->samplers.reset();
    d->uploadBuffers.reset();
    d->streamBlocks.reset();
    d->held.reset();
    d->inFlight.reset();
    d->retired.reset();
    d->pending.reset();
    d->clocks.reset();
    release(d->root);
    release(d->zeros);
    release(d->annotation);
    release(d->messages);
    release(d->fence);
    release(d->context);
    release(d->factory);
    release(d->device);
    prMutexReset(&d->mutex);
    prMutexReset(&d->contextMutex);
    prConditionReset(&d->pendingSignal);
    prConditionReset(&d->doneSignal);
    prMutexReset(&d->doneMutex);
    prMemoryFree(d);
}

// ---------------------------------------------------------------------------------------------------------------------
// Buffers

// Creates a buffer of the device's of `size` bytes that every use reads: shaders through raw views, vertices, indices
// and indirect arguments, and shaders write if `storage`. It holds zeros.
ID3D11Buffer* PR_NULLABLE createDeviceBuffer(D3d11Device* d, uint64_t size, bool storage) {
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = UINT(size);
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER;
    if (storage)
        desc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
    desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS | D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
    void* zeros = prMemoryAllocZeroed(size_t(size));
    D3D11_SUBRESOURCE_DATA initial{zeros, 0, 0};
    ID3D11Buffer* buffer = nullptr;
    if (FAILED(d->device->CreateBuffer(&desc, &initial, &buffer)))
        buffer = nullptr;
    prMemoryFree(zeros);
    return buffer;
}

// Puts a buffer into a new id. Returns zero, and releases nothing, if no id is left.
uint32_t addBuffer(D3d11Device* d, ID3D11Buffer* buffer, const PrGpuBufferDesc& desc, uint64_t size, uint8_t* memory,
        bool stream) {
    prMutexLock(&d->mutex);
    uint32_t id = takeIdLocked(d);
    if (id != 0) {
        D3d11Resource& r = d->resources[id];
        r.kind = D3d11Kind_Buffer;
        r.buffer = desc;
        r.buffer.name = PrStringView{};
        r.resource = buffer;
        r.size = size;
        r.memory = memory;
        r.stream = stream;
        if (desc.memory == PrGpuMemory_Upload && !stream)
            d->uploadBuffers.push(id);
    }
    prMutexUnlock(&d->mutex);
    return id;
}

PrStatus bufferCreate(PrGpuDevice* device, const PrGpuBufferDesc* desc, PrGpuBuffer* out) {
    D3d11Device* d = d3d11Of(device);
    uint64_t size = alignUp(desc->size, 16);
    ID3D11Buffer* buffer = nullptr;
    uint8_t* memory = nullptr;
    if (desc->memory == PrGpuMemory_Readback) {
        D3D11_BUFFER_DESC staging{};
        staging.ByteWidth = UINT(size);
        staging.Usage = D3D11_USAGE_STAGING;
        staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(d->device->CreateBuffer(&staging, nullptr, &buffer)))
            return PrStatus_Limit;
    } else {
        buffer = createDeviceBuffer(d, size, (desc->usage & PrGpuBufferUsageFlag_Storage) != 0);
        if (buffer == nullptr)
            return PrStatus_Limit;
    }
    if (desc->memory != PrGpuMemory_Device)
        memory = static_cast<uint8_t*>(prMemoryAllocZeroed(size_t(size)));
    nameObject(buffer, desc->name);
    uint32_t id = addBuffer(d, buffer, *desc, size, memory, false);
    if (id == 0) {
        buffer->Release();
        prMemoryFree(memory);
        return PrStatus_Limit;
    }
    out->id = id;
    return PrStatus_Ok;
}

void bufferRetain(PrGpuDevice* device, PrGpuBuffer buffer) {
    d3d11Of(device)->resources.retain(buffer.id);
}

void bufferRelease(PrGpuDevice* device, PrGpuBuffer buffer) {
    if (!d3d11Of(device)->resources.release(buffer.id))
        return;
    retire(d3d11Of(device), D3d11RetiredKind_Resource, buffer.id);
}

PrGpuBufferDesc bufferDesc(const PrGpuDevice* device, PrGpuBuffer buffer) {
    return d3d11Of(device)->resources[buffer.id].buffer;
}

// Direct3D 11 has no bindless, so no handle reaches a buffer.
uint32_t bufferStorageHandle(const PrGpuDevice*, PrGpuBuffer) {
    return 0;
}

void* bufferMap(const PrGpuDevice* device, PrGpuBuffer buffer) {
    const D3d11Resource& r = d3d11Of(device)->resources[buffer.id];
    PR_ASSERT(r.kind == D3d11Kind_Buffer && r.memory != nullptr);
    return r.memory;
}

// Writes the device's buffer through the immediate context, which orders the write before the work submitted after it.
// A buffer that the CPU reads or writes keeps its bytes in its own memory as well.
void bufferUpload(PrGpuDevice* device, PrGpuBuffer buffer, uint64_t offset, const void* data, uint64_t size) {
    D3d11Device* d = d3d11Of(device);
    D3d11Resource& r = d->resources[buffer.id];
    if (r.memory != nullptr)
        prMemCopy(r.memory + offset, data, size_t(size));
    if (r.buffer.memory == PrGpuMemory_Readback)
        return;
    D3D11_BOX box{UINT(offset), 0, 0, UINT(offset + size), 1, 1};
    prMutexLock(&d->contextMutex);
    d->context->UpdateSubresource(r.resource, 0, &box, data, 0, 0);
    prMutexUnlock(&d->contextMutex);
}

// Returns the raw views of a buffer from `offset`, made the first time that they are asked for. The caller holds the
// context mutex.
const D3d11BufferView& bufferViewOf(D3d11Device* d, uint32_t id, uint64_t offset) {
    D3d11Resource& r = d->resources[id];
    for (uint32_t i = 0; i < r.bufferViews.count; ++i) {
        if (r.bufferViews.data[i].offset == offset)
            return r.bufferViews.data[i];
    }
    D3d11BufferView view{.offset = offset};
    UINT first = UINT(offset / 4);
    UINT count = UINT((r.size - offset) / 4);
    D3D11_SHADER_RESOURCE_VIEW_DESC read{};
    read.Format = DXGI_FORMAT_R32_TYPELESS;
    read.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    read.BufferEx = D3D11_BUFFEREX_SRV{first, count, D3D11_BUFFEREX_SRV_FLAG_RAW};
    d->device->CreateShaderResourceView(r.resource, &read, &view.read);
    if (r.buffer.usage & PrGpuBufferUsageFlag_Storage) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC write{};
        write.Format = DXGI_FORMAT_R32_TYPELESS;
        write.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        write.Buffer = D3D11_BUFFER_UAV{first, count, D3D11_BUFFER_UAV_FLAG_RAW};
        d->device->CreateUnorderedAccessView(r.resource, &write, &view.write);
    }
    r.bufferViews.push(view);
    return r.bufferViews.last();
}

// ---------------------------------------------------------------------------------------------------------------------
// Textures

// Returns the shader resource view of a texture or view.
D3D11_SHADER_RESOURCE_VIEW_DESC textureViewOf(const D3d11Resource& r) {
    const PrGpuTextureDesc& t = r.texture;
    bool array = r.resourceLayers > 1 || r.firstLayer != 0;
    D3D11_SHADER_RESOURCE_VIEW_DESC s{};
    s.Format = shaderFormatOf(t.format);
    switch (t.kind) {
        case PrGpuTextureKind_3d:
            s.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
            s.Texture3D.MostDetailedMip = r.firstLevel;
            s.Texture3D.MipLevels = t.levels;
            break;
        case PrGpuTextureKind_Cube:
        case PrGpuTextureKind_CubeArray:
            if (t.kind == PrGpuTextureKind_Cube && r.firstLayer == 0) {
                s.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
                s.TextureCube.MostDetailedMip = r.firstLevel;
                s.TextureCube.MipLevels = t.levels;
            } else {
                s.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;
                s.TextureCubeArray.MostDetailedMip = r.firstLevel;
                s.TextureCubeArray.MipLevels = t.levels;
                s.TextureCubeArray.First2DArrayFace = r.firstLayer;
                s.TextureCubeArray.NumCubes = t.extent.depth / 6;
            }
            break;
        default:
            if (t.samples > 1) {
                if (array || t.kind == PrGpuTextureKind_2dArray) {
                    s.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY;
                    s.Texture2DMSArray.FirstArraySlice = r.firstLayer;
                    s.Texture2DMSArray.ArraySize = t.extent.depth;
                } else {
                    s.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
                }
            } else if (array || t.kind == PrGpuTextureKind_2dArray) {
                s.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
                s.Texture2DArray.MostDetailedMip = r.firstLevel;
                s.Texture2DArray.MipLevels = t.levels;
                s.Texture2DArray.FirstArraySlice = r.firstLayer;
                s.Texture2DArray.ArraySize = t.extent.depth;
            } else {
                s.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                s.Texture2D.MostDetailedMip = r.firstLevel;
                s.Texture2D.MipLevels = t.levels;
            }
            break;
    }
    return s;
}

// Returns the unordered access view of the first level of a texture or view.
D3D11_UNORDERED_ACCESS_VIEW_DESC storageViewOf(const D3d11Resource& r) {
    const PrGpuTextureDesc& t = r.texture;
    bool array = r.resourceLayers > 1 || r.firstLayer != 0;
    D3D11_UNORDERED_ACCESS_VIEW_DESC u{};
    u.Format = storageFormatOf(t.format);
    if (t.kind == PrGpuTextureKind_3d) {
        u.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
        u.Texture3D.MipSlice = r.firstLevel;
        u.Texture3D.WSize = UINT(-1);
    } else if (array || t.kind != PrGpuTextureKind_2d) {
        u.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
        u.Texture2DArray.MipSlice = r.firstLevel;
        u.Texture2DArray.FirstArraySlice = r.firstLayer;
        u.Texture2DArray.ArraySize = t.extent.depth;
    } else {
        u.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        u.Texture2D.MipSlice = r.firstLevel;
    }
    return u;
}

// Puts a texture's resource into a new id, as `desc` describes it, and makes its views. Returns zero, and releases
// nothing, if no id is left or a view cannot be made.
uint32_t addTexture(D3d11Device* d, ID3D11Resource* resource, const PrGpuTextureDesc& desc, DXGI_FORMAT format,
        uint8_t resourceLevels, uint16_t resourceLayers, uint8_t firstLevel, uint16_t firstLayer) {
    D3d11Resource r{};
    r.kind = D3d11Kind_Texture;
    r.texture = desc;
    r.texture.name = PrStringView{};
    r.resource = resource;
    r.format = format;
    r.resourceLevels = resourceLevels;
    r.resourceLayers = resourceLayers;
    r.firstLevel = firstLevel;
    r.firstLayer = firstLayer;
    if (desc.usage & PrGpuTextureUsageFlag_Sampled) {
        D3D11_SHADER_RESOURCE_VIEW_DESC s = textureViewOf(r);
        if (FAILED(d->device->CreateShaderResourceView(resource, &s, &r.read)))
            return 0;
    }
    if (desc.usage & PrGpuTextureUsageFlag_Storage) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC u = storageViewOf(r);
        if (FAILED(d->device->CreateUnorderedAccessView(resource, &u, &r.write))) {
            release(r.read);
            return 0;
        }
    }
    prMutexLock(&d->mutex);
    uint32_t id = takeIdLocked(d);
    if (id != 0)
        d->resources[id] = r;
    prMutexUnlock(&d->mutex);
    if (id == 0) {
        release(r.read);
        release(r.write);
    }
    return id;
}

PrStatus textureCreate(PrGpuDevice* device, const PrGpuTextureDesc* desc, PrGpuTexture* out) {
    D3d11Device* d = d3d11Of(device);
    DXGI_FORMAT format = resourceFormatOf(desc->format, desc->usage);
    if (format == DXGI_FORMAT_UNKNOWN)
        return PrStatus_NotSupported;
    UINT bind = 0;
    if (desc->usage & PrGpuTextureUsageFlag_Sampled)
        bind |= D3D11_BIND_SHADER_RESOURCE;
    if (desc->usage & PrGpuTextureUsageFlag_RenderTarget)
        bind |= D3D11_BIND_RENDER_TARGET;
    if (desc->usage & PrGpuTextureUsageFlag_DepthStencil)
        bind |= D3D11_BIND_DEPTH_STENCIL;
    if (desc->usage & PrGpuTextureUsageFlag_Storage)
        bind |= D3D11_BIND_UNORDERED_ACCESS;
    ID3D11Resource* resource = nullptr;
    if (desc->kind == PrGpuTextureKind_3d) {
        D3D11_TEXTURE3D_DESC t{};
        t.Width = desc->extent.width;
        t.Height = desc->extent.height;
        t.Depth = desc->extent.depth;
        t.MipLevels = desc->levels;
        t.Format = format;
        t.Usage = D3D11_USAGE_DEFAULT;
        t.BindFlags = bind;
        ID3D11Texture3D* texture = nullptr;
        if (FAILED(d->device->CreateTexture3D(&t, nullptr, &texture)))
            return PrStatus_Limit;
        resource = texture;
    } else {
        D3D11_TEXTURE2D_DESC t{};
        t.Width = desc->extent.width;
        t.Height = desc->extent.height;
        t.MipLevels = desc->levels;
        t.ArraySize = desc->extent.depth;
        t.Format = format;
        t.SampleDesc.Count = desc->samples;
        t.Usage = D3D11_USAGE_DEFAULT;
        t.BindFlags = bind;
        if (desc->kind == PrGpuTextureKind_Cube || desc->kind == PrGpuTextureKind_CubeArray)
            t.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
        ID3D11Texture2D* texture = nullptr;
        if (FAILED(d->device->CreateTexture2D(&t, nullptr, &texture)))
            return PrStatus_Limit;
        resource = texture;
    }
    nameObject(resource, desc->name);
    uint16_t layers = desc->kind == PrGpuTextureKind_3d ? 1 : uint16_t(desc->extent.depth);
    uint32_t id = addTexture(d, resource, *desc, format, desc->levels, layers, 0, 0);
    if (id == 0) {
        resource->Release();
        return PrStatus_Limit;
    }
    out->id = id;
    return PrStatus_Ok;
}

PrStatus textureCreateView(PrGpuDevice* device, PrGpuTexture texture, PrGpuTextureKind kind,
        PrGpuSubresources subresources, PrGpuTexture* out) {
    D3d11Device* d = d3d11Of(device);
    const D3d11Resource& t = d->resources[texture.id];
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
    uint32_t id = addTexture(d, t.resource, v, t.format, t.resourceLevels, t.resourceLayers,
        uint8_t(t.firstLevel + subresources.firstLevel), uint16_t(t.firstLayer + subresources.firstLayer));
    if (id == 0) {
        t.resource->Release();
        return PrStatus_Limit;
    }
    out->id = id;
    return PrStatus_Ok;
}

void textureRetain(PrGpuDevice* device, PrGpuTexture texture) {
    d3d11Of(device)->resources.retain(texture.id);
}

void textureRelease(PrGpuDevice* device, PrGpuTexture texture) {
    if (!d3d11Of(device)->resources.release(texture.id))
        return;
    gpuSharedTextureForget(d3d11Of(device), texture);
    retire(d3d11Of(device), D3d11RetiredKind_Resource, texture.id);
}

PrGpuTextureDesc textureDesc(const PrGpuDevice* device, PrGpuTexture texture) {
    return d3d11Of(device)->resources[texture.id].texture;
}

// Direct3D 11 has no bindless, so no handle reaches a texture.
uint32_t textureStorageHandle(const PrGpuDevice*, PrGpuTexture) {
    return 0;
}

// The index of a subresource of a texture or view.
UINT subresourceOf(const D3d11Resource& r, uint32_t level, uint32_t layer) {
    uint32_t mip = r.firstLevel + level;
    uint32_t slice = r.texture.kind == PrGpuTextureKind_3d ? 0 : r.firstLayer + layer;
    return D3D11CalcSubresource(mip, slice, r.resourceLevels);
}

// Returns the box of `region` in a texture of `format`, rounded out to whole blocks. Direct3D 11 requires the box of a
// compressed format to cover whole blocks, even at a mip level that ends inside a block.
D3D11_BOX boxOf(PrGpuFormat format, const PrGpuTextureRegion& region, bool is3d) {
    PrGpuFormatBlock b = prGpuFormatBlock(format);
    uint32_t width = alignUp(region.extent.width, uint32_t(b.width));
    uint32_t height = alignUp(region.extent.height, uint32_t(b.height));
    return D3D11_BOX{
        region.x, region.y, is3d ? region.z : 0, region.x + width, region.y + height,
        is3d ? region.z + region.extent.depth : 1,
    };
}

// Writes texels into a texture through the immediate context, which orders the write before the work submitted after
// it. The caller holds the context mutex.
void writeTexels(D3d11Device* d, const D3d11Resource& r, const PrGpuTextureRegion* region, const uint8_t* data,
        uint32_t rowBytes, uint32_t layerBytes) {
    bool is3d = r.texture.kind == PrGpuTextureKind_3d;
    PrGpuFormatBlock b = prGpuFormatBlock(r.texture.format);
    uint32_t rows = (region->extent.height + b.height - 1) / b.height;
    uint64_t imageBytes = layerBytes != 0 ? layerBytes : uint64_t(rowBytes) * rows;
    uint32_t images = is3d ? 1 : region->extent.depth;
    D3D11_BOX box = boxOf(r.texture.format, *region, is3d);
    for (uint32_t i = 0; i < images; ++i) {
        UINT subresource = subresourceOf(r, region->level, is3d ? 0 : region->firstLayer + i);
        d->context->UpdateSubresource(r.resource, subresource, &box, data + i * imageBytes, rowBytes, UINT(imageBytes));
    }
}

void textureUpload(PrGpuDevice* device, PrGpuTexture texture, const PrGpuTextureRegion* region, const void* data,
        uint32_t rowBytes, uint32_t layerBytes) {
    D3d11Device* d = d3d11Of(device);
    prMutexLock(&d->contextMutex);
    writeTexels(d, d->resources[texture.id], region, static_cast<const uint8_t*>(data), rowBytes, layerBytes);
    prMutexUnlock(&d->contextMutex);
}

// ---------------------------------------------------------------------------------------------------------------------
// Samplers

PrStatus samplerCreate(PrGpuDevice* device, const PrGpuSamplerDesc* desc, PrGpuSampler* out) {
    D3d11Device* d = d3d11Of(device);
    D3D11_SAMPLER_DESC s{};
    bool compare = desc->compare != PrGpuCompare_Never;
    D3D11_FILTER_REDUCTION_TYPE reduction =
        compare ? D3D11_FILTER_REDUCTION_TYPE_COMPARISON : D3D11_FILTER_REDUCTION_TYPE_STANDARD;
    if (desc->maxAnisotropy > 1) {
        s.Filter = D3D11_ENCODE_ANISOTROPIC_FILTER(reduction);
        s.MaxAnisotropy = desc->maxAnisotropy;
    } else {
        D3D11_FILTER_TYPE minify =
            desc->minify == PrGpuFilter_Linear ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT;
        D3D11_FILTER_TYPE magnify =
            desc->magnify == PrGpuFilter_Linear ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT;
        D3D11_FILTER_TYPE mip = desc->mip == PrGpuFilter_Linear ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT;
        s.Filter = D3D11_ENCODE_BASIC_FILTER(minify, magnify, mip, reduction);
        s.MaxAnisotropy = 1;
    }
    s.AddressU = addressOf(desc->address[0]);
    s.AddressV = addressOf(desc->address[1]);
    s.AddressW = addressOf(desc->address[2]);
    s.MipLODBias = desc->lodBias;
    s.ComparisonFunc = compare ? compareOf(desc->compare) : D3D11_COMPARISON_NEVER;
    s.MinLOD = desc->minLod;
    s.MaxLOD = desc->maxLod;
    ID3D11SamplerState* sampler = nullptr;
    if (FAILED(d->device->CreateSamplerState(&s, &sampler)))
        return PrStatus_Limit;
    prMutexLock(&d->mutex);
    const uint32_t id = d->samplers.take();
    if (id != 0)
        d->samplers[id] = sampler;
    prMutexUnlock(&d->mutex);
    if (id == 0) {
        sampler->Release();
        return PrStatus_Limit;
    }
    out->id = id;
    return PrStatus_Ok;
}

// ---------------------------------------------------------------------------------------------------------------------
// Shader libraries and pipelines

// The entries hold each entry point's DXBC, which a pipeline makes its shaders from.
PrStatus shaderLibraryCreate(PrGpuDevice* device, PrGpuShaderEntry* entries, uint32_t entryCount,
        PrGpuShaderLibrary* PR_NULLABLE* PR_NONNULL out) {
    if (entryCount == 0)
        return PrStatus_InvalidData;
    D3d11Library* library = allocZeroed<D3d11Library>();
    static_cast<PrGpuShaderLibrary&>(*library) = kLibraryFunctions;
    library->device = device;
    library->entryCount = entryCount;
    library->entries = entries;
    *out = library;
    return PrStatus_Ok;
}

void shaderLibraryDestroy(PrGpuShaderLibrary* library) {
    D3d11Library* l = static_cast<D3d11Library*>(library);
    prMemoryFree(l->entries);
    prMemoryFree(l);
}

// Puts a pipeline into a new id, or returns zero if none is left.
uint32_t addPipeline(D3d11Device* d, const D3d11Pipeline& pipeline) {
    prMutexLock(&d->mutex);
    const uint32_t id = d->pipelines.take();
    if (id != 0)
        d->pipelines[id] = pipeline;
    prMutexUnlock(&d->mutex);
    return id;
}

D3D11_DEPTH_STENCILOP_DESC stencilFaceOf(const PrGpuStencilFace& face) {
    return D3D11_DEPTH_STENCILOP_DESC{
        stencilOpOf(face.fail), stencilOpOf(face.depthFail), stencilOpOf(face.pass), compareOf(face.compare),
    };
}

// Reports a pipeline that Direct3D 11 refuses, for the entry point that it was made from.
PrStatus refused(const PrGpuShaderEntry* entry, const char* what, HRESULT result) {
    fprintf(stderr, "lib/gpu: D3D11 made no %s for %.*s (0x%08lx)\n", what, int(entry->name.size), entry->name.data,
        long(result));
    return PrStatus_NotSupported;
}

PrStatus graphicsPipelineCreate(PrGpuDevice* device, const PrGpuGraphicsPipelineDesc* desc,
        const PrGpuShaderEntry* vertex, const PrGpuShaderEntry* PR_NULLABLE fragment, PrGpuPipeline* out) {
    D3d11Device* d = d3d11Of(device);
    // An entry point that reaches a resource by handle has no code.
    if (vertex->codeSize == 0 || (fragment != nullptr && fragment->codeSize == 0))
        return PrStatus_NotSupported;
    D3d11Pipeline p{};
    HRESULT made = d->device->CreateVertexShader(vertex->code, SIZE_T(vertex->codeSize), nullptr, &p.vertex);
    if (FAILED(made))
        return refused(vertex, "vertex shader", made);
    if (fragment != nullptr) {
        made = d->device->CreatePixelShader(fragment->code, SIZE_T(fragment->codeSize), nullptr, &p.fragment);
        if (FAILED(made)) {
            releasePipeline(p);
            return refused(fragment, "pixel shader", made);
        }
    }
    D3D11_BLEND_DESC blend{};
    blend.IndependentBlendEnable = TRUE;
    for (uint8_t i = 0; i < desc->formats.colorCount; ++i) {
        const PrGpuBlend& b = desc->blend[i];
        D3D11_RENDER_TARGET_BLEND_DESC& a = blend.RenderTarget[i];
        a.BlendEnable = b.enabled ? TRUE : FALSE;
        a.SrcBlend = blendFactorOf(b.colorSource, false);
        a.DestBlend = blendFactorOf(b.colorTarget, false);
        a.BlendOp = blendOpOf(b.colorOp);
        a.SrcBlendAlpha = blendFactorOf(b.alphaSource, true);
        a.DestBlendAlpha = blendFactorOf(b.alphaTarget, true);
        a.BlendOpAlpha = blendOpOf(b.alphaOp);
        PrGpuColorWriteFlags write = b.write != 0 ? b.write : PrGpuColorWriteFlag_All;
        a.RenderTargetWriteMask = UINT8(((write & PrGpuColorWriteFlag_Red) ? D3D11_COLOR_WRITE_ENABLE_RED : 0) |
            ((write & PrGpuColorWriteFlag_Green) ? D3D11_COLOR_WRITE_ENABLE_GREEN : 0) |
            ((write & PrGpuColorWriteFlag_Blue) ? D3D11_COLOR_WRITE_ENABLE_BLUE : 0) |
            ((write & PrGpuColorWriteFlag_Alpha) ? D3D11_COLOR_WRITE_ENABLE_ALPHA : 0));
    }
    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID;
    if (desc->cull == PrGpuCull_Front)
        raster.CullMode = D3D11_CULL_FRONT;
    else if (desc->cull == PrGpuCull_Back)
        raster.CullMode = D3D11_CULL_BACK;
    else
        raster.CullMode = D3D11_CULL_NONE;
    // Direct3D 11 winds triangles in the target, with y down, as the description does.
    raster.FrontCounterClockwise = desc->frontClockwise ? FALSE : TRUE;
    raster.DepthBias = INT(desc->depthBias);
    raster.DepthBiasClamp = desc->depthBiasClamp;
    raster.SlopeScaledDepthBias = desc->depthBiasSlope;
    raster.DepthClipEnable = TRUE;
    raster.ScissorEnable = TRUE;
    raster.MultisampleEnable = desc->formats.samples > 1 ? TRUE : FALSE;
    bool depth = desc->formats.depthStencil != PrGpuFormat_None;
    D3D11_DEPTH_STENCIL_DESC stencil{};
    stencil.DepthEnable = depth && (desc->depthTest || desc->depthWrite) ? TRUE : FALSE;
    stencil.DepthWriteMask = desc->depthWrite ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    stencil.DepthFunc = desc->depthTest ? compareOf(desc->depthCompare) : D3D11_COMPARISON_ALWAYS;
    stencil.StencilEnable = depth && desc->stencilTest ? TRUE : FALSE;
    stencil.StencilReadMask = desc->stencilReadMask;
    stencil.StencilWriteMask = desc->stencilWriteMask;
    stencil.FrontFace = stencilFaceOf(desc->stencilFront);
    stencil.BackFace = stencilFaceOf(desc->stencilBack);
    if (FAILED(d->device->CreateBlendState(&blend, &p.blend)) ||
            FAILED(d->device->CreateRasterizerState(&raster, &p.raster)) ||
            FAILED(d->device->CreateDepthStencilState(&stencil, &p.depth))) {
        releasePipeline(p);
        return PrStatus_NotSupported;
    }
    // The vertex shader reads attribute k with the semantic ATTRIBUTEk (pr/gpu.slang's PR_GPU_ATTRIBUTE).
    const PrGpuVertexLayout& layout = desc->vertexLayout;
    if (layout.attributeCount != 0) {
        D3D11_INPUT_ELEMENT_DESC elements[kPrGpuMaxVertexAttributes]{};
        for (uint32_t i = 0; i < layout.attributeCount; ++i) {
            const PrGpuVertexAttribute& a = layout.attributes[i];
            elements[i] = D3D11_INPUT_ELEMENT_DESC{
                .SemanticName = "ATTRIBUTE",
                .SemanticIndex = i,
                .Format = vertexFormatOf(a.format),
                .InputSlot = a.buffer,
                .AlignedByteOffset = a.offset,
                .InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA,
            };
            if (layout.buffers[a.buffer].step == PrGpuVertexStep_Instance) {
                elements[i].InputSlotClass = D3D11_INPUT_PER_INSTANCE_DATA;
                elements[i].InstanceDataStepRate = 1;
            }
        }
        made = d->device->CreateInputLayout(elements, layout.attributeCount, vertex->code, SIZE_T(vertex->codeSize),
            &p.layout);
        if (FAILED(made)) {
            releasePipeline(p);
            return refused(vertex, "input layout", made);
        }
    }
    p.topology = topologyOf(desc->topology);
    p.vertexBufferCount = layout.bufferCount;
    for (uint32_t i = 0; i < layout.bufferCount; ++i)
        p.vertexStrides[i] = layout.buffers[i].stride;
    nameObject(p.vertex, desc->name);
    uint32_t id = addPipeline(d, p);
    if (id == 0) {
        releasePipeline(p);
        return PrStatus_Limit;
    }
    out->id = id;
    return PrStatus_Ok;
}

PrStatus computePipelineCreate(PrGpuDevice* device, const PrGpuShaderLibrary*, const PrGpuShaderEntry* compute,
        PrStringView name, PrGpuPipeline* out) {
    D3d11Device* d = d3d11Of(device);
    if (compute->codeSize == 0)
        return PrStatus_NotSupported;
    D3d11Pipeline p{};
    p.compute = true;
    p.groupSize = compute->groupSize;
    HRESULT made = d->device->CreateComputeShader(compute->code, SIZE_T(compute->codeSize), nullptr, &p.kernel);
    if (FAILED(made))
        return refused(compute, "compute shader", made);
    nameObject(p.kernel, name);
    uint32_t id = addPipeline(d, p);
    if (id == 0) {
        releasePipeline(p);
        return PrStatus_Limit;
    }
    out->id = id;
    return PrStatus_Ok;
}

void pipelineRetain(PrGpuDevice* device, PrGpuPipeline pipeline) {
    d3d11Of(device)->pipelines.retain(pipeline.id);
}

void pipelineRelease(PrGpuDevice* device, PrGpuPipeline pipeline) {
    if (!d3d11Of(device)->pipelines.release(pipeline.id))
        return;
    retire(d3d11Of(device), D3d11RetiredKind_Pipeline, pipeline.id);
}

PrGpuExtent pipelineGroupSize(const PrGpuDevice* device, PrGpuPipeline pipeline) {
    return d3d11Of(device)->pipelines[pipeline.id].groupSize;
}

// ---------------------------------------------------------------------------------------------------------------------
// Recording

// The commands of a recording. Each is a header and the operation's payload, which follows it.
enum {
    D3d11Op_Pipeline,
    D3d11Op_Push,
    D3d11Op_Uniforms,
    D3d11Op_Buffer,
    D3d11Op_RwBuffer,
    D3d11Op_Texture,
    D3d11Op_RwTexture,
    D3d11Op_Dispatch,
    D3d11Op_DispatchIndirect,
    D3d11Op_CopyBuffer,
    D3d11Op_CopyTexture,
    D3d11Op_CopyBufferToTexture,
    D3d11Op_CopyTextureToBuffer,
    D3d11Op_PushGroup,
    D3d11Op_PopGroup,
    D3d11Op_PassBegin,
    D3d11Op_PassEnd,
    D3d11Op_Viewport,
    D3d11Op_Scissor,
    D3d11Op_BlendConstant,
    D3d11Op_StencilReference,
    D3d11Op_IndexBuffer,
    D3d11Op_VertexBuffer,
    D3d11Op_Draw,
    D3d11Op_DrawIndexed,
    D3d11Op_DrawIndirect,
    D3d11Op_DrawIndexedIndirect,
};
typedef uint32_t D3d11Op;

// A command's header. Its payload is `bytes` long, and the next command follows it at a multiple of 8 bytes.
struct D3d11OpHeader {
    D3d11Op op;
    uint32_t bytes;
};

// The bytes from a command's header to the next command's.
uint32_t opStride(const D3d11OpHeader& header) {
    return uint32_t(sizeof(header)) + alignUp(header.bytes, 8u);
}

// The payloads of the commands that take more than one value.
struct D3d11SlotOp {
    uint32_t slot;
    uint32_t id;                                        // the resource's id

    // The offset into a buffer at which a Buffer or RwBuffer slot binds it, or the id of a Texture slot's sampler.
    uint64_t offset;
};

struct D3d11CopyBufferOp {
    uint32_t source;
    uint32_t target;
    uint64_t sourceOffset;
    uint64_t targetOffset;
    uint64_t size;
};

struct D3d11CopyTextureOp {
    uint32_t source;
    uint32_t target;
    PrGpuTextureRegion sourceRegion;
    PrGpuTextureRegion targetRegion;
};

struct D3d11CopyBufferTextureOp {
    uint32_t buffer;
    uint32_t texture;
    uint64_t offset;
    uint32_t rowBytes;
    uint32_t layerBytes;
    PrGpuTextureRegion region;
};

struct D3d11ViewportOp {
    PrRect viewport;
    float minDepth;
    float maxDepth;
};

struct D3d11VertexBufferOp {
    uint32_t slot;
    uint32_t buffer;
    uint64_t offset;
};

struct D3d11IndexBufferOp {
    uint32_t buffer;
    uint32_t wide;
    uint64_t offset;
};

struct D3d11DrawOp {
    uint32_t count;
    uint32_t instanceCount;
    uint32_t first;
    int32_t vertexOffset;
    uint32_t firstInstance;
};

struct D3d11IndirectOp {
    uint32_t buffer;
    uint32_t count;
    uint64_t offset;
    uint32_t stride;
};

// Appends a command and its payload.
void record(D3d11Commands* c, D3d11Op op, const void* PR_NULLABLE payload, uint32_t size) {
    D3d11OpHeader header{op, size};
    uint32_t at = c->ops.count;
    c->ops.resize(at + opStride(header));
    prMemCopy(c->ops.data + at, &header, sizeof(header));
    if (size != 0)
        prMemCopy(c->ops.data + at + sizeof(header), payload, size);
}

template <typename T>
void record(D3d11Commands* c, D3d11Op op, const T& payload) {
    record(c, op, &payload, sizeof(payload));
}

bool commandsBegin(PrGpuDevice* device, const PrGpuCommandsOptions* options, PrGpuCommands* PR_NULLABLE* PR_NONNULL out) {
    // Direct3D 11 has no native command buffers.
    if (options->native != nullptr)
        return false;
    D3d11Device* d = d3d11Of(device);
    D3d11Commands* c = allocZeroed<D3d11Commands>();
    static_cast<PrGpuCommands&>(*c) = kCommandsFunctions;
    c->device = device;
    prMutexLock(&d->mutex);
    collect(d);
    c->serial = d->serial++;
    d->inFlight.push(D3d11InFlight{c->serial, 0});
    prMutexUnlock(&d->mutex);
    *out = c;
    return true;
}

// Allocates `size` bytes of the stream at `alignment`, in a block that the command buffer holds.
void* commandsStream(PrGpuCommands* commands, uint64_t size, PrGpuBufferUsageFlags usage, PrGpuBuffer* buffer, uint64_t* offset) {
    D3d11Device* d = deviceOfCommands(commands);
    D3d11Commands* c = commandsOf(commands);
    uint64_t alignment = gpuSharedStreamAlignment(d, usage);
    D3d11Block* last = c->blocks.count != 0 ? &c->blocks.last() : nullptr;
    uint64_t at = last != nullptr ? alignUp(last->used, alignment) : 0;
    if (last == nullptr || at + size > d->resources[last->id].size) {
        uint32_t id = 0;
        prMutexLock(&d->mutex);
        if (size <= kStreamBlock && d->streamBlocks.count != 0) {
            id = d->streamBlocks.last();
            d->streamBlocks.pop();
        }
        prMutexUnlock(&d->mutex);
        if (id == 0) {
            uint64_t bytes = size > kStreamBlock ? alignUp(size, uint64_t(256)) : kStreamBlock;
            ID3D11Buffer* made = createDeviceBuffer(d, bytes, false);
            uint8_t* memory = made != nullptr ? static_cast<uint8_t*>(prMemoryAllocZeroed(size_t(bytes))) : nullptr;
            PrGpuBufferDesc desc{.size = bytes, .usage = PrGpuBufferUsageFlag_All, .memory = PrGpuMemory_Upload};
            id = made != nullptr ? addBuffer(d, made, desc, bytes, memory, true) : 0;
            if (id == 0) {
                release(made);
                prMemoryFree(memory);
                // Out of memory loses the device, and the caller writes into memory that the GPU never reads.
                d->lost.store(true);
                void* scratch = prMemoryAlloc(size_t(size != 0 ? size : 1));
                c->scratch.push(scratch);
                buffer->id = 0;
                *offset = 0;
                return scratch;
            }
        }
        c->blocks.push(D3d11Block{id, 0});
        last = &c->blocks.last();
        at = 0;
    }
    last->used = at + size;
    buffer->id = last->id;
    *offset = at;
    return d->resources[last->id].memory + at;
}

void commandsSetPush(PrGpuCommands* commands, const void* data, uint32_t size) {
    record(commandsOf(commands), D3d11Op_Push, data, size);
}

void commandsSetUniforms(PrGpuCommands* commands, const void* data, uint32_t size) {
    record(commandsOf(commands), D3d11Op_Uniforms, data, size);
}

void commandsSetPipeline(PrGpuCommands* commands, PrGpuPipeline pipeline) {
    record(commandsOf(commands), D3d11Op_Pipeline, pipeline.id);
}

void commandsSetBuffer(PrGpuCommands* commands, uint32_t slot, PrGpuBuffer buffer, uint64_t offset) {
    record(commandsOf(commands), D3d11Op_Buffer, D3d11SlotOp{slot, buffer.id, offset});
}

void commandsSetRwBuffer(PrGpuCommands* commands, uint32_t slot, PrGpuBuffer buffer, uint64_t offset) {
    record(commandsOf(commands), D3d11Op_RwBuffer, D3d11SlotOp{slot, buffer.id, offset});
}

void commandsSetTexture(PrGpuCommands* commands, uint32_t slot, PrGpuTexture texture, PrGpuSampler sampler) {
    record(commandsOf(commands), D3d11Op_Texture, D3d11SlotOp{slot, texture.id, sampler.id});
}

void commandsSetRwTexture(PrGpuCommands* commands, uint32_t slot, PrGpuTexture texture) {
    record(commandsOf(commands), D3d11Op_RwTexture, D3d11SlotOp{slot, texture.id, 0});
}

void commandsDispatch(PrGpuCommands* commands, PrGpuExtent groups) {
    gpuSharedCheckOutsidePass(commands);
    record(commandsOf(commands), D3d11Op_Dispatch, groups);
}

void commandsDispatchIndirect(PrGpuCommands* commands, PrGpuBuffer buffer, uint64_t offset) {
    gpuSharedCheckOutsidePass(commands);
    record(commandsOf(commands), D3d11Op_DispatchIndirect, D3d11IndirectOp{buffer.id, 1, offset, 0});
}

void commandsCopyBuffer(PrGpuCommands* commands, PrGpuBuffer source, uint64_t sourceOffset, PrGpuBuffer target,
        uint64_t targetOffset, uint64_t size) {
    gpuSharedCheckOutsidePass(commands);
    record(commandsOf(commands), D3d11Op_CopyBuffer, D3d11CopyBufferOp{source.id, target.id, sourceOffset, targetOffset, size});
}

void commandsCopyTexture(PrGpuCommands* commands, PrGpuTexture source, const PrGpuTextureRegion* sourceRegion,
        PrGpuTexture target, const PrGpuTextureRegion* targetRegion) {
    gpuSharedCheckOutsidePass(commands);
    record(commandsOf(commands), D3d11Op_CopyTexture,
        D3d11CopyTextureOp{source.id, target.id, *sourceRegion, *targetRegion});
}

void commandsCopyBufferToTexture(PrGpuCommands* commands, PrGpuBuffer source, uint64_t sourceOffset, uint32_t rowBytes,
        uint32_t layerBytes, PrGpuTexture target, const PrGpuTextureRegion* targetRegion) {
    gpuSharedCheckOutsidePass(commands);
    // Direct3D 11 copies into a texture from the CPU's memory, which an Upload buffer or the stream holds.
    PR_ASSERT(deviceOfCommands(commands)->resources[source.id].memory != nullptr);
    record(commandsOf(commands), D3d11Op_CopyBufferToTexture,
        D3d11CopyBufferTextureOp{source.id, target.id, sourceOffset, rowBytes, layerBytes, *targetRegion});
}

void commandsCopyTextureToBuffer(PrGpuCommands* commands, PrGpuTexture source, const PrGpuTextureRegion* sourceRegion,
        PrGpuBuffer target, uint64_t targetOffset, uint32_t rowBytes, uint32_t layerBytes) {
    gpuSharedCheckOutsidePass(commands);
    // Direct3D 11 copies out of a texture into the CPU's memory, which a Readback buffer holds.
    PR_ASSERT(deviceOfCommands(commands)->resources[target.id].buffer.memory == PrGpuMemory_Readback);
    record(commandsOf(commands), D3d11Op_CopyTextureToBuffer,
        D3d11CopyBufferTextureOp{target.id, source.id, targetOffset, rowBytes, layerBytes, *sourceRegion});
}

void commandsOnDone(PrGpuCommands* commands, PrGpuDoneFn done, void* PR_NULLABLE context) {
    commandsOf(commands)->dones.push(D3d11Done{done, context});
}

void commandsPushGroup(PrGpuCommands* commands, PrStringView name) {
    record(commandsOf(commands), D3d11Op_PushGroup, name.data, uint32_t(name.size));
}

void commandsPopGroup(PrGpuCommands* commands) {
    record(commandsOf(commands), D3d11Op_PopGroup, nullptr, 0);
}

// Direct3D 11 orders the accesses of its work itself.
void commandsBufferBarrier(PrGpuCommands* commands, PrGpuBuffer, PrGpuAccessFlags, PrGpuAccessFlags) {
    gpuSharedCheckOutsidePass(commands);
}

void commandsTextureBarrier(PrGpuCommands* commands, PrGpuTexture, PrGpuSubresources, PrGpuAccessFlags,
        PrGpuAccessFlags) {
    gpuSharedCheckOutsidePass(commands);
}

void commandsGlobalBarrier(PrGpuCommands* commands, PrGpuAccessFlags, PrGpuAccessFlags) {
    gpuSharedCheckOutsidePass(commands);
}

// The device hands out no native objects, so nothing else changes its state.
PrGpuNativeStateFlags nativeStateSync(PrGpuCommands*) {
    return 0;
}

// The extent of a level of a texture or view.
PrUVec2 levelExtent(const D3d11Resource& r, uint32_t level) {
    uint32_t width = r.texture.extent.width >> level;
    uint32_t height = r.texture.extent.height >> level;
    return PrUVec2{width != 0 ? width : 1, height != 0 ? height : 1};
}

PrGpuPass* passBegin(PrGpuCommands* commands, const PrGpuPassDesc* desc) {
    // Direct3D 11 has no native passes.
    PR_ASSERT(desc->native == nullptr);
    D3d11Device* d = deviceOfCommands(commands);
    D3d11Commands* c = commandsOf(commands);
    gpuSharedPassBegan(c);
    D3d11Pass* pass = allocZeroed<D3d11Pass>();
    static_cast<PrGpuPass&>(*pass) = kPassFunctions;
    pass->commands = c;
    pass->formats.samples = 1;
    for (uint8_t i = 0; i < desc->colorCount; ++i) {
        const D3d11Resource& t = d->resources[desc->color[i].texture.id];
        pass->formats.color[pass->formats.colorCount++] = t.texture.format;
        pass->formats.samples = t.texture.samples;
        pass->extent = levelExtent(t, desc->color[i].level);
    }
    if (desc->depthStencil.texture.id != 0) {
        const D3d11Resource& t = d->resources[desc->depthStencil.texture.id];
        pass->formats.depthStencil = t.texture.format;
        pass->formats.samples = t.texture.samples;
        if (desc->colorCount == 0)
            pass->extent = levelExtent(t, desc->depthStencil.level);
    }
    PrGpuPassDesc copy = *desc;
    copy.name = PrStringView{};
    record(c, D3d11Op_PassBegin, copy);
    if (desc->name.size != 0)
        commandsPushGroup(commands, desc->name);
    c->pass = pass;
    return pass;
}

void passEnd(PrGpuPass* handle) {
    D3d11Pass* pass = static_cast<D3d11Pass*>(handle);
    D3d11Commands* c = commandsOf(pass->commands);
    gpuSharedPassEnded(c);
    record(c, D3d11Op_PassEnd, nullptr, 0);
    c->pass = nullptr;
    prMemoryFree(pass);
}

D3d11Commands* commandsOfPass(PrGpuPass* pass) {
    return commandsOf(static_cast<GpuSharedPass*>(pass)->commands);
}

void passSetViewport(PrGpuPass* pass, PrRect viewport, float minDepth, float maxDepth) {
    record(commandsOfPass(pass), D3d11Op_Viewport, D3d11ViewportOp{viewport, minDepth, maxDepth});
}

void passSetScissor(PrGpuPass* pass, PrIRect scissor) {
    record(commandsOfPass(pass), D3d11Op_Scissor, scissor);
}

void passSetBlendConstant(PrGpuPass* pass, PrColor constant) {
    record(commandsOfPass(pass), D3d11Op_BlendConstant, constant);
}

void passSetStencilReference(PrGpuPass* pass, uint8_t reference) {
    record(commandsOfPass(pass), D3d11Op_StencilReference, uint32_t(reference));
}

void passSetIndexBuffer(PrGpuPass* pass, PrGpuBuffer buffer, uint64_t offset, bool wide) {
    record(commandsOfPass(pass), D3d11Op_IndexBuffer, D3d11IndexBufferOp{buffer.id, wide ? 1u : 0u, offset});
}

void passSetVertexBuffer(PrGpuPass* pass, uint32_t slot, PrGpuBuffer buffer, uint64_t offset) {
    record(commandsOfPass(pass), D3d11Op_VertexBuffer, D3d11VertexBufferOp{slot, buffer.id, offset});
}

void passDraw(PrGpuPass* pass, uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex,
        uint32_t firstInstance) {
    record(commandsOfPass(pass), D3d11Op_Draw, D3d11DrawOp{vertexCount, instanceCount, firstVertex, 0, firstInstance});
}

void passDrawIndexed(PrGpuPass* pass, uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex,
        int32_t vertexOffset, uint32_t firstInstance) {
    record(commandsOfPass(pass), D3d11Op_DrawIndexed,
        D3d11DrawOp{indexCount, instanceCount, firstIndex, vertexOffset, firstInstance});
}

void passDrawIndirect(PrGpuPass* pass, PrGpuBuffer buffer, uint64_t offset, uint32_t count, uint32_t stride) {
    record(commandsOfPass(pass), D3d11Op_DrawIndirect, D3d11IndirectOp{buffer.id, count, offset, stride});
}

void passDrawIndexedIndirect(PrGpuPass* pass, PrGpuBuffer buffer, uint64_t offset, uint32_t count, uint32_t stride) {
    record(commandsOfPass(pass), D3d11Op_DrawIndexedIndirect, D3d11IndirectOp{buffer.id, count, offset, stride});
}

// ---------------------------------------------------------------------------------------------------------------------
// Replay

// Reads a command's payload.
template <typename T>
T payloadOf(const uint8_t* bytes) {
    T value;
    prMemCopy(&value, bytes, sizeof(T));
    return value;
}

// Where a binding of root data lies in the ring, in constants of 16 bytes.
struct D3d11RootBinding {
    ID3D11Buffer* buffer;
    UINT first;
    UINT count;
};

// What a replay has bound, and what the next draw or dispatch binds again.
struct D3d11Replay {
    D3d11Device* d;
    D3d11Commands* c;
    RawArray<D3d11RootBinding> roots;                   // the ring's bindings, in the order of the root commands
    uint32_t nextRoot;
    RawArray<D3d11Readback> readbacks;

    uint32_t pipeline;
    D3d11RootBinding push;
    D3d11RootBinding uniforms;
    D3d11SlotOp buffers[kBufferSlots];
    D3d11SlotOp rwBuffers[kRwBufferSlots];
    D3d11SlotOp textures[kTextureSlots];
    D3d11SlotOp rwTextures[kRwTextureSlots];
    bool graphicsDirty;
    bool computeDirty;
    bool computeWrites;                                 // the compute stage has unordered access views bound

    // The pass that is open: its attachments, and the state that its draws take.
    bool inPass;
    uint8_t colorCount;
    PrGpuAttachment color[kPrGpuMaxColorAttachments];
    float blendConstant[4];
    UINT stencilReference;
    bool pipelineDirty;
    D3d11VertexBufferOp vertexBuffers[kPrGpuMaxVertexBuffers];
    bool vertexBuffersDirty;
    uint32_t groups;                                    // the groups of commands that are open
};

// Writes the root data of the recording into the ring, and notes where each lies, before the replay records work that
// reads it. The ring grows to hold one command buffer's root data.
void writeRoots(D3d11Replay& r) {
    D3d11Device* d = r.d;
    const RawArray<uint8_t>& ops = r.c->ops;
    uint32_t bytes = 0;
    for (uint32_t at = 0; at < ops.count;) {
        D3d11OpHeader h = payloadOf<D3d11OpHeader>(ops.data + at);
        at += opStride(h);
        if (h.op == D3d11Op_Push || h.op == D3d11Op_Uniforms)
            bytes += alignUp(h.bytes != 0 ? h.bytes : 16u, kRootAlignment);
    }
    if (bytes == 0)
        return;
    D3D11_MAP map = D3D11_MAP_WRITE_NO_OVERWRITE;
    if (d->rootUsed + bytes > d->rootSize) {
        if (bytes > d->rootSize) {
            uint32_t size = d->rootSize;
            while (size < bytes)
                size *= 2;
            D3D11_BUFFER_DESC desc{size, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
            ID3D11Buffer* grown = nullptr;
            if (FAILED(d->device->CreateBuffer(&desc, nullptr, &grown))) {
                d->lost.store(true);
                return;
            }
            release(d->root);
            d->root = grown;
            d->rootSize = size;
        }
        // Discarding renames the ring's memory, so the work before still reads what it was given.
        map = D3D11_MAP_WRITE_DISCARD;
        d->rootUsed = 0;
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(d->context->Map(d->root, 0, map, 0, &mapped))) {
        d->lost.store(true);
        return;
    }
    for (uint32_t at = 0; at < ops.count;) {
        D3d11OpHeader h = payloadOf<D3d11OpHeader>(ops.data + at);
        const uint8_t* payload = ops.data + at + sizeof(h);
        at += opStride(h);
        if (h.op != D3d11Op_Push && h.op != D3d11Op_Uniforms)
            continue;
        uint32_t size = alignUp(h.bytes != 0 ? h.bytes : 16u, kRootAlignment);
        uint8_t* to = static_cast<uint8_t*>(mapped.pData) + d->rootUsed;
        prMemCopy(to, payload, h.bytes);
        prMemZero(to + h.bytes, size - h.bytes);
        r.roots.push(D3d11RootBinding{d->root, d->rootUsed / 16, size / 16});
        d->rootUsed += size;
    }
    d->context->Unmap(d->root, 0);
}

// Returns the buffer of the device's that a buffer's id names.
ID3D11Buffer* PR_NULLABLE bufferOf(D3d11Device* d, uint32_t id) {
    return static_cast<ID3D11Buffer*>(d->resources[id].resource);
}

// Gathers the slots' views of the shader stages and their unordered access views.
void slotViews(D3d11Replay& r, ID3D11ShaderResourceView* PR_NULLABLE* reads, ID3D11SamplerState* PR_NULLABLE* samplers,
        ID3D11UnorderedAccessView* PR_NULLABLE* writes) {
    D3d11Device* d = r.d;
    for (uint32_t i = 0; i < kBufferSlots; ++i)
        reads[i] = r.buffers[i].id != 0 ? bufferViewOf(d, r.buffers[i].id, r.buffers[i].offset).read : nullptr;
    for (uint32_t i = 0; i < kTextureSlots; ++i) {
        reads[kTextureRegister + i] = r.textures[i].id != 0 ? d->resources[r.textures[i].id].read : nullptr;
        samplers[i] = r.textures[i].offset != 0 ? d->samplers[r.textures[i].offset] : nullptr;
    }
    for (uint32_t i = 0; i < kRwTextureSlots; ++i)
        writes[i] = r.rwTextures[i].id != 0 ? d->resources[r.rwTextures[i].id].write : nullptr;
    for (uint32_t i = 0; i < kRwBufferSlots; ++i) {
        const D3d11SlotOp& s = r.rwBuffers[i];
        writes[kRwBufferRegister + i] = s.id != 0 ? bufferViewOf(d, s.id, s.offset).write : nullptr;
    }
}

// Unbinds the shader resource views of every stage, so that a view that the work after them writes is not also bound
// for reading, which Direct3D 11 refuses.
void unbindReads(D3d11Replay& r) {
    ID3D11ShaderResourceView* none[kShaderResources]{};
    r.d->context->VSSetShaderResources(0, kShaderResources, none);
    r.d->context->PSSetShaderResources(0, kShaderResources, none);
    r.d->context->CSSetShaderResources(0, kShaderResources, none);
    r.graphicsDirty = true;
    r.computeDirty = true;
}

// Unbinds the compute stage's unordered access views, so that what a dispatch wrote can be read.
void unbindComputeWrites(D3d11Replay& r) {
    if (!r.computeWrites)
        return;
    ID3D11UnorderedAccessView* none[kUnorderedAccesses]{};
    r.d->context->CSSetUnorderedAccessViews(0, kUnorderedAccesses, none, nullptr);
    r.computeWrites = false;
}

// Binds what a draw takes: the pipeline, the root data, the slots and the vertex buffers.
void prepareDraw(D3d11Replay& r) {
    D3d11Device* d = r.d;
    ID3D11DeviceContext4* context = d->context;
    const D3d11Pipeline& p = d->pipelines[r.pipeline];
    unbindComputeWrites(r);
    if (r.pipelineDirty) {
        context->IASetInputLayout(p.layout);
        context->IASetPrimitiveTopology(p.topology);
        context->VSSetShader(p.vertex, nullptr, 0);
        context->PSSetShader(p.fragment, nullptr, 0);
        context->OMSetBlendState(p.blend, r.blendConstant, 0xffffffff);
        context->RSSetState(p.raster);
        context->OMSetDepthStencilState(p.depth, r.stencilReference);
        r.pipelineDirty = false;
        r.vertexBuffersDirty = true;
    }
    if (r.graphicsDirty) {
        ID3D11Buffer* roots[2] = {r.push.buffer, r.uniforms.buffer};
        UINT firsts[2] = {r.push.first, r.uniforms.first};
        UINT counts[2] = {r.push.count, r.uniforms.count};
        context->VSSetConstantBuffers1(0, 2, roots, firsts, counts);
        context->PSSetConstantBuffers1(0, 2, roots, firsts, counts);
        ID3D11ShaderResourceView* reads[kShaderResources]{};
        ID3D11SamplerState* samplers[kTextureSlots]{};
        ID3D11UnorderedAccessView* writes[kUnorderedAccesses]{};
        slotViews(r, reads, samplers, writes);
        context->VSSetShaderResources(0, kShaderResources, reads);
        context->PSSetShaderResources(0, kShaderResources, reads);
        context->VSSetSamplers(0, kTextureSlots, samplers);
        context->PSSetSamplers(0, kTextureSlots, samplers);
        // A fragment shader's unordered access views follow the pass's render targets.
        bool any = false;
        for (uint32_t i = r.colorCount; i < kUnorderedAccesses; ++i)
            any = any || writes[i] != nullptr;
        if (any) {
            context->OMSetRenderTargetsAndUnorderedAccessViews(D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL, nullptr,
                nullptr, r.colorCount, kUnorderedAccesses - r.colorCount, writes + r.colorCount, nullptr);
        }
        r.graphicsDirty = false;
    }
    if (r.vertexBuffersDirty && p.vertexBufferCount != 0) {
        ID3D11Buffer* buffers[kPrGpuMaxVertexBuffers]{};
        UINT offsets[kPrGpuMaxVertexBuffers]{};
        for (uint32_t i = 0; i < p.vertexBufferCount; ++i) {
            buffers[i] = r.vertexBuffers[i].buffer != 0 ? bufferOf(d, r.vertexBuffers[i].buffer) : nullptr;
            offsets[i] = UINT(r.vertexBuffers[i].offset);
        }
        context->IASetVertexBuffers(0, p.vertexBufferCount, buffers, p.vertexStrides, offsets);
        r.vertexBuffersDirty = false;
    }
}

// Binds what a dispatch takes: the pipeline, the root data and the slots.
void prepareDispatch(D3d11Replay& r) {
    D3d11Device* d = r.d;
    ID3D11DeviceContext4* context = d->context;
    const D3d11Pipeline& p = d->pipelines[r.pipeline];
    PR_ASSERT(p.compute);
    context->CSSetShader(p.kernel, nullptr, 0);
    if (!r.computeDirty)
        return;
    ID3D11Buffer* roots[2] = {r.push.buffer, r.uniforms.buffer};
    UINT firsts[2] = {r.push.first, r.uniforms.first};
    UINT counts[2] = {r.push.count, r.uniforms.count};
    context->CSSetConstantBuffers1(0, 2, roots, firsts, counts);
    ID3D11ShaderResourceView* reads[kShaderResources]{};
    ID3D11SamplerState* samplers[kTextureSlots]{};
    ID3D11UnorderedAccessView* writes[kUnorderedAccesses]{};
    slotViews(r, reads, samplers, writes);
    // A view that the dispatch writes may be bound for reading in the graphics stages.
    ID3D11ShaderResourceView* none[kShaderResources]{};
    context->VSSetShaderResources(0, kShaderResources, none);
    context->PSSetShaderResources(0, kShaderResources, none);
    r.graphicsDirty = true;
    context->CSSetShaderResources(0, kShaderResources, reads);
    context->CSSetSamplers(0, kTextureSlots, samplers);
    context->CSSetUnorderedAccessViews(0, kUnorderedAccesses, writes, nullptr);
    r.computeWrites = true;
    r.computeDirty = false;
}

// Returns the render target or depth stencil view of an attachment, made the first time that a pass takes it.
ID3D11View* PR_NULLABLE attachmentViewOf(D3d11Device* d, const PrGpuAttachment& a, bool depth) {
    D3d11Resource& r = d->resources[a.texture.id];
    for (uint32_t i = 0; i < r.attachmentViews.count; ++i) {
        const D3d11AttachmentView& v = r.attachmentViews.data[i];
        if (v.level == a.level && v.layer == a.layer)
            return v.view;
    }
    uint32_t mip = r.firstLevel + a.level;
    uint32_t slice = r.firstLayer + a.layer;
    bool ms = r.texture.samples > 1;
    bool array = r.resourceLayers > 1;
    ID3D11View* view = nullptr;
    if (depth) {
        D3D11_DEPTH_STENCIL_VIEW_DESC v{};
        v.Format = formatOf(r.texture.format);
        if (ms && array) {
            v.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY;
            v.Texture2DMSArray = D3D11_TEX2DMS_ARRAY_DSV{slice, 1};
        } else if (ms) {
            v.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMS;
        } else if (array) {
            v.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
            v.Texture2DArray = D3D11_TEX2D_ARRAY_DSV{mip, slice, 1};
        } else {
            v.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
            v.Texture2D.MipSlice = mip;
        }
        ID3D11DepthStencilView* made = nullptr;
        if (SUCCEEDED(d->device->CreateDepthStencilView(r.resource, &v, &made)))
            view = made;
    } else {
        D3D11_RENDER_TARGET_VIEW_DESC v{};
        v.Format = formatOf(r.texture.format);
        if (r.texture.kind == PrGpuTextureKind_3d) {
            v.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE3D;
            v.Texture3D = D3D11_TEX3D_RTV{mip, a.layer, 1};
        } else if (ms && array) {
            v.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY;
            v.Texture2DMSArray = D3D11_TEX2DMS_ARRAY_RTV{slice, 1};
        } else if (ms) {
            v.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMS;
        } else if (array) {
            v.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
            v.Texture2DArray = D3D11_TEX2D_ARRAY_RTV{mip, slice, 1};
        } else {
            v.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            v.Texture2D.MipSlice = mip;
        }
        ID3D11RenderTargetView* made = nullptr;
        if (SUCCEEDED(d->device->CreateRenderTargetView(r.resource, &v, &made)))
            view = made;
    }
    if (view != nullptr)
        r.attachmentViews.push(D3d11AttachmentView{a.level, a.layer, view});
    return view;
}

void replayPassBegin(D3d11Replay& r, const PrGpuPassDesc& desc) {
    D3d11Device* d = r.d;
    ID3D11DeviceContext4* context = d->context;
    unbindComputeWrites(r);
    unbindReads(r);
    ID3D11RenderTargetView* colors[kPrGpuMaxColorAttachments]{};
    PrUVec2 extent{};
    for (uint8_t i = 0; i < desc.colorCount; ++i) {
        colors[i] = static_cast<ID3D11RenderTargetView*>(attachmentViewOf(d, desc.color[i], false));
        r.color[i] = desc.color[i];
        extent = levelExtent(d->resources[desc.color[i].texture.id], desc.color[i].level);
    }
    ID3D11DepthStencilView* depth = nullptr;
    if (desc.depthStencil.texture.id != 0) {
        depth = static_cast<ID3D11DepthStencilView*>(attachmentViewOf(d, desc.depthStencil, true));
        if (desc.colorCount == 0)
            extent = levelExtent(d->resources[desc.depthStencil.texture.id], desc.depthStencil.level);
    }
    context->OMSetRenderTargets(desc.colorCount, colors, depth);
    for (uint8_t i = 0; i < desc.colorCount; ++i) {
        if (desc.color[i].load == PrGpuLoad_Clear && colors[i] != nullptr) {
            const PrColor& clear = desc.clearColor[i];
            const float rgba[4] = {clear.r, clear.g, clear.b, clear.a};
            context->ClearRenderTargetView(colors[i], rgba);
        } else if (desc.color[i].load == PrGpuLoad_Discard && colors[i] != nullptr) {
            context->DiscardView(colors[i]);
        }
    }
    if (depth != nullptr) {
        UINT clears = 0;
        if (desc.depthStencil.load == PrGpuLoad_Clear)
            clears |= D3D11_CLEAR_DEPTH;
        bool stencil = d->resources[desc.depthStencil.texture.id].texture.format == PrGpuFormat_D32FS8;
        if (stencil && desc.stencilLoad == PrGpuLoad_Clear)
            clears |= D3D11_CLEAR_STENCIL;
        if (clears != 0)
            context->ClearDepthStencilView(depth, clears, desc.clearDepth, desc.clearStencil);
    }
    D3D11_VIEWPORT viewport{0, 0, float(extent.x), float(extent.y), 0, 1};
    context->RSSetViewports(1, &viewport);
    D3D11_RECT scissor{0, 0, LONG(extent.x), LONG(extent.y)};
    context->RSSetScissorRects(1, &scissor);
    r.inPass = true;
    r.colorCount = desc.colorCount;
    r.pipelineDirty = true;
    r.graphicsDirty = true;
    prMemZero(r.vertexBuffers, sizeof(r.vertexBuffers));
}

// Ends a pass: resolves its attachments that store a resolve, and unbinds them, so that what the pass drew can be read.
void replayPassEnd(D3d11Replay& r) {
    D3d11Device* d = r.d;
    for (uint8_t i = 0; i < r.colorCount; ++i) {
        const PrGpuAttachment& a = r.color[i];
        if (a.store != PrGpuStore_Resolve)
            continue;
        const D3d11Resource& from = d->resources[a.texture.id];
        const D3d11Resource& to = d->resources[a.resolve.id];
        d->context->ResolveSubresource(to.resource, subresourceOf(to, a.level, a.layer), from.resource,
            subresourceOf(from, a.level, a.layer), formatOf(from.texture.format));
    }
    d->context->OMSetRenderTargets(0, nullptr, nullptr);
    r.inPass = false;
    r.colorCount = 0;
    r.graphicsDirty = true;
}

// Copies a region of a texture into a staging texture, which the device's thread copies into a Readback buffer once the
// work is done.
void replayReadTexture(D3d11Replay& r, const D3d11CopyBufferTextureOp& op) {
    D3d11Device* d = r.d;
    const D3d11Resource& t = d->resources[op.texture];
    const PrGpuTextureRegion& region = op.region;
    bool is3d = t.texture.kind == PrGpuTextureKind_3d;
    PrGpuFormatBlock b = prGpuFormatBlock(t.texture.format);
    uint32_t images = is3d ? 1 : region.extent.depth;
    uint32_t rows = (region.extent.height + b.height - 1) / b.height;
    D3d11Readback readback{
        .buffer = op.buffer,
        .offset = op.offset,
        .rowBytes = op.rowBytes,
        .imageBytes = op.layerBytes != 0 ? op.layerBytes : uint64_t(op.rowBytes) * rows,
        .images = images,
        .rows = rows,
        .rowSize = (region.extent.width + b.width - 1) / b.width * b.bytes,
        .depth = is3d ? region.extent.depth : 1,
    };
    D3D11_BOX box = boxOf(t.texture.format, region, is3d);
    if (is3d) {
        D3D11_TEXTURE3D_DESC s{};
        s.Width = box.right - box.left;
        s.Height = box.bottom - box.top;
        s.Depth = region.extent.depth;
        s.MipLevels = 1;
        s.Format = t.format;
        s.Usage = D3D11_USAGE_STAGING;
        s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(d->device->CreateTexture3D(&s, nullptr, &readback.staging3d)))
            return;
        d->context->CopySubresourceRegion(readback.staging3d, 0, 0, 0, 0, t.resource, subresourceOf(t, region.level, 0), &box);
    } else {
        D3D11_TEXTURE2D_DESC s{};
        s.Width = box.right - box.left;
        s.Height = box.bottom - box.top;
        s.MipLevels = 1;
        s.ArraySize = images;
        s.Format = t.format;
        s.SampleDesc.Count = 1;
        s.Usage = D3D11_USAGE_STAGING;
        s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(d->device->CreateTexture2D(&s, nullptr, &readback.staging)))
            return;
        for (uint32_t i = 0; i < images; ++i) {
            d->context->CopySubresourceRegion(readback.staging, i, 0, 0, 0, t.resource,
                subresourceOf(t, region.level, region.firstLayer + i), &box);
        }
    }
    r.readbacks.push(readback);
}

void replayCopyTexture(D3d11Replay& r, const D3d11CopyTextureOp& op) {
    D3d11Device* d = r.d;
    const D3d11Resource& from = d->resources[op.source];
    const D3d11Resource& to = d->resources[op.target];
    bool is3d = from.texture.kind == PrGpuTextureKind_3d;
    const PrGpuTextureRegion& s = op.sourceRegion;
    const PrGpuTextureRegion& t = op.targetRegion;
    uint32_t images = is3d ? 1 : s.extent.depth;
    D3D11_BOX box = boxOf(from.texture.format, s, is3d);
    for (uint32_t i = 0; i < images; ++i) {
        d->context->CopySubresourceRegion(to.resource, subresourceOf(to, t.level, t.firstLayer + i), t.x, t.y,
            is3d ? t.z : 0, from.resource, subresourceOf(from, s.level, s.firstLayer + i), &box);
    }
}

// Opens a group of commands for debuggers and captures.
void replayPushGroup(D3d11Replay& r, const uint8_t* name, uint32_t size) {
    ++r.groups;
    if (r.d->annotation == nullptr)
        return;
    wchar_t* wide = prWindowsWidenCopy(PrStringView{reinterpret_cast<const char*>(name), size}, nullptr);
    r.d->annotation->BeginEvent(wide != nullptr ? wide : L"");
    prMemoryFree(wide);
}

// Replays a command buffer's recording on the immediate context. The caller holds the context mutex.
void replay(D3d11Replay& r) {
    D3d11Device* d = r.d;
    ID3D11DeviceContext4* context = d->context;
    D3d11RootBinding zeros{d->zeros, 0, kPrGpuMaxUniformBytes / 16};
    r.push = zeros;
    r.uniforms = zeros;
    r.blendConstant[0] = r.blendConstant[1] = r.blendConstant[2] = r.blendConstant[3] = 0;
    r.graphicsDirty = true;
    r.computeDirty = true;
    r.pipelineDirty = true;
    const RawArray<uint8_t>& ops = r.c->ops;
    for (uint32_t at = 0; at < ops.count;) {
        D3d11OpHeader h = payloadOf<D3d11OpHeader>(ops.data + at);
        const uint8_t* p = ops.data + at + sizeof(h);
        at += opStride(h);
        switch (h.op) {
            case D3d11Op_Pipeline:
                r.pipeline = payloadOf<uint32_t>(p);
                r.pipelineDirty = true;
                break;
            case D3d11Op_Push:
            case D3d11Op_Uniforms:
                if (r.nextRoot < r.roots.count) {
                    (h.op == D3d11Op_Push ? r.push : r.uniforms) = r.roots.data[r.nextRoot++];
                    r.graphicsDirty = true;
                    r.computeDirty = true;
                }
                break;
            case D3d11Op_Buffer:
            case D3d11Op_RwBuffer:
            case D3d11Op_Texture:
            case D3d11Op_RwTexture: {
                D3d11SlotOp s = payloadOf<D3d11SlotOp>(p);
                D3d11SlotOp* slots = r.buffers;
                if (h.op == D3d11Op_RwBuffer)
                    slots = r.rwBuffers;
                else if (h.op == D3d11Op_Texture)
                    slots = r.textures;
                else if (h.op == D3d11Op_RwTexture)
                    slots = r.rwTextures;
                slots[s.slot] = s;
                r.graphicsDirty = true;
                r.computeDirty = true;
                break;
            }
            case D3d11Op_Dispatch: {
                PrGpuExtent groups = payloadOf<PrGpuExtent>(p);
                prepareDispatch(r);
                context->Dispatch(groups.width, groups.height, groups.depth);
                break;
            }
            case D3d11Op_DispatchIndirect: {
                D3d11IndirectOp op = payloadOf<D3d11IndirectOp>(p);
                prepareDispatch(r);
                context->DispatchIndirect(bufferOf(d, op.buffer), UINT(op.offset));
                break;
            }
            case D3d11Op_CopyBuffer: {
                D3d11CopyBufferOp op = payloadOf<D3d11CopyBufferOp>(p);
                unbindComputeWrites(r);
                D3D11_BOX box{UINT(op.sourceOffset), 0, 0, UINT(op.sourceOffset + op.size), 1, 1};
                context->CopySubresourceRegion(bufferOf(d, op.target), 0, UINT(op.targetOffset), 0, 0,
                    bufferOf(d, op.source), 0, &box);
                if (d->resources[op.target].buffer.memory == PrGpuMemory_Readback)
                    r.readbacks.push(D3d11Readback{.buffer = op.target});
                break;
            }
            case D3d11Op_CopyTexture:
                unbindComputeWrites(r);
                replayCopyTexture(r, payloadOf<D3d11CopyTextureOp>(p));
                break;
            case D3d11Op_CopyBufferToTexture: {
                D3d11CopyBufferTextureOp op = payloadOf<D3d11CopyBufferTextureOp>(p);
                const D3d11Resource& source = d->resources[op.buffer];
                if (source.memory != nullptr) {
                    writeTexels(d, d->resources[op.texture], &op.region, source.memory + op.offset, op.rowBytes,
                        op.layerBytes);
                }
                break;
            }
            case D3d11Op_CopyTextureToBuffer:
                unbindComputeWrites(r);
                replayReadTexture(r, payloadOf<D3d11CopyBufferTextureOp>(p));
                break;
            case D3d11Op_PushGroup:
                replayPushGroup(r, p, h.bytes);
                break;
            case D3d11Op_PopGroup:
                if (r.groups != 0) {
                    --r.groups;
                    if (d->annotation != nullptr)
                        d->annotation->EndEvent();
                }
                break;
            case D3d11Op_PassBegin:
                replayPassBegin(r, payloadOf<PrGpuPassDesc>(p));
                break;
            case D3d11Op_PassEnd:
                replayPassEnd(r);
                break;
            case D3d11Op_Viewport: {
                D3d11ViewportOp op = payloadOf<D3d11ViewportOp>(p);
                D3D11_VIEWPORT v{
                    op.viewport.left, op.viewport.top, op.viewport.right - op.viewport.left,
                    op.viewport.bottom - op.viewport.top, op.minDepth, op.maxDepth,
                };
                context->RSSetViewports(1, &v);
                break;
            }
            case D3d11Op_Scissor: {
                PrIRect s = payloadOf<PrIRect>(p);
                D3D11_RECT rect{s.left, s.top, s.right > s.left ? s.right : s.left, s.bottom > s.top ? s.bottom : s.top};
                context->RSSetScissorRects(1, &rect);
                break;
            }
            case D3d11Op_BlendConstant: {
                PrColor c = payloadOf<PrColor>(p);
                r.blendConstant[0] = c.r;
                r.blendConstant[1] = c.g;
                r.blendConstant[2] = c.b;
                r.blendConstant[3] = c.a;
                r.pipelineDirty = true;
                break;
            }
            case D3d11Op_StencilReference:
                r.stencilReference = payloadOf<uint32_t>(p);
                r.pipelineDirty = true;
                break;
            case D3d11Op_IndexBuffer: {
                D3d11IndexBufferOp op = payloadOf<D3d11IndexBufferOp>(p);
                context->IASetIndexBuffer(bufferOf(d, op.buffer), op.wide ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT,
                    UINT(op.offset));
                break;
            }
            case D3d11Op_VertexBuffer: {
                D3d11VertexBufferOp op = payloadOf<D3d11VertexBufferOp>(p);
                r.vertexBuffers[op.slot] = op;
                r.vertexBuffersDirty = true;
                break;
            }
            case D3d11Op_Draw: {
                D3d11DrawOp op = payloadOf<D3d11DrawOp>(p);
                prepareDraw(r);
                context->DrawInstanced(op.count, op.instanceCount, op.first, op.firstInstance);
                break;
            }
            case D3d11Op_DrawIndexed: {
                D3d11DrawOp op = payloadOf<D3d11DrawOp>(p);
                prepareDraw(r);
                context->DrawIndexedInstanced(op.count, op.instanceCount, op.first, op.vertexOffset, op.firstInstance);
                break;
            }
            case D3d11Op_DrawIndirect:
            case D3d11Op_DrawIndexedIndirect: {
                D3d11IndirectOp op = payloadOf<D3d11IndirectOp>(p);
                prepareDraw(r);
                ID3D11Buffer* arguments = bufferOf(d, op.buffer);
                for (uint32_t i = 0; i < op.count; ++i) {
                    UINT offset = UINT(op.offset + uint64_t(i) * op.stride);
                    if (h.op == D3d11Op_DrawIndirect)
                        context->DrawInstancedIndirect(arguments, offset);
                    else
                        context->DrawIndexedInstancedIndirect(arguments, offset);
                }
                break;
            }
            default:
                break;
        }
    }
    while (r.groups != 0) {
        --r.groups;
        if (d->annotation != nullptr)
            d->annotation->EndEvent();
    }
    // The next command buffer begins with nothing bound.
    unbindComputeWrites(r);
    context->ClearState();
}

// Writes the debug layer's errors and warnings to the standard error. The caller holds the context mutex.
void reportMessages(D3d11Device* d) {
    if (d->messages == nullptr)
        return;
    UINT64 count = d->messages->GetNumStoredMessages();
    for (UINT64 i = 0; i < count; ++i) {
        SIZE_T size = 0;
        if (FAILED(d->messages->GetMessage(i, nullptr, &size)))
            continue;
        D3D11_MESSAGE* message = static_cast<D3D11_MESSAGE*>(prMemoryAlloc(size));
        if (SUCCEEDED(d->messages->GetMessage(i, message, &size)) &&
                message->Severity <= D3D11_MESSAGE_SEVERITY_WARNING)
            fprintf(stderr, "d3d11: %.*s\n", int(message->DescriptionByteLength), message->pDescription);
        prMemoryFree(message);
    }
    d->messages->ClearStoredMessages();
}

PrGpuFence commandsSubmit(PrGpuCommands* commands) {
    D3d11Device* d = deviceOfCommands(commands);
    D3d11Commands* c = commandsOf(commands);
    gpuSharedSubmitted(c);
    D3d11Replay r{};
    r.d = d;
    r.c = c;
    prMutexLock(&d->contextMutex);
    ID3D11DeviceContext4* context = d->context;
    D3d11Clock clock{};
    if ((d->info.features & PrGpuFeatureFlag_Timestamps) != 0) {
        if (d->clocks.count != 0) {
            clock = d->clocks.last();
            d->clocks.pop();
        } else {
            D3D11_QUERY_DESC disjoint{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
            D3D11_QUERY_DESC stamp{D3D11_QUERY_TIMESTAMP, 0};
            if (FAILED(d->device->CreateQuery(&disjoint, &clock.disjoint)) ||
                    FAILED(d->device->CreateQuery(&stamp, &clock.start)) ||
                    FAILED(d->device->CreateQuery(&stamp, &clock.end))) {
                release(clock.disjoint);
                release(clock.start);
                release(clock.end);
                clock = D3d11Clock{};
            }
        }
    }
    if (!d->lost.load()) {
        if (clock.disjoint != nullptr) {
            context->Begin(clock.disjoint);
            context->End(clock.start);
        }
        // The device's buffers take the bytes that the CPU wrote: the Upload buffers', and the stream's.
        prMutexLock(&d->mutex);
        for (uint32_t i = 0; i < d->uploadBuffers.count; ++i) {
            const D3d11Resource& u = d->resources[d->uploadBuffers.data[i]];
            context->UpdateSubresource(u.resource, 0, nullptr, u.memory, 0, 0);
        }
        prMutexUnlock(&d->mutex);
        for (uint32_t i = 0; i < c->blocks.count; ++i) {
            const D3d11Block& block = c->blocks.data[i];
            const D3d11Resource& b = d->resources[block.id];
            UINT used = UINT(alignUp(block.used, uint64_t(16)));
            if (used == 0)
                continue;
            D3D11_BOX box{0, 0, 0, used, 1, 1};
            context->UpdateSubresource(b.resource, 0, &box, b.memory, 0, 0);
        }
        writeRoots(r);
        replay(r);
        if (clock.disjoint != nullptr) {
            context->End(clock.end);
            context->End(clock.disjoint);
        }
    }
    prMutexLock(&d->mutex);
    PrGpuFence fence = ++d->submitted;
    for (uint32_t i = 0; i < d->inFlight.count; ++i) {
        if (d->inFlight.data[i].serial == c->serial)
            d->inFlight.data[i].fence = fence;
    }
    for (uint32_t i = 0; i < c->blocks.count; ++i)
        d->held.push(D3d11Held{fence, c->blocks.data[i].id});
    collect(d);
    prMutexUnlock(&d->mutex);
    if (!d->lost.load() && FAILED(context->Signal(d->fence, fence)))
        d->lost.store(true);
    // DXGI presents a frame once the work before it is done.
    for (uint32_t i = 0; i < c->presents.count && !d->lost.load(); ++i) {
        HRESULT presented = c->presents.data[i]->swapChain->Present(1, 0);
        if (presented == DXGI_ERROR_DEVICE_REMOVED || presented == DXGI_ERROR_DEVICE_RESET)
            d->lost.store(true);
    }
    context->Flush();
    reportMessages(d);
    prMutexUnlock(&d->contextMutex);
    D3d11Submission s{.fence = fence, .clock = clock};
    s.doneCount = c->dones.count;
    if (s.doneCount != 0) {
        s.dones = static_cast<D3d11Done*>(prMemoryAlloc(sizeof(D3d11Done) * s.doneCount));
        prMemCopy(s.dones, c->dones.data, sizeof(D3d11Done) * s.doneCount);
    }
    s.readbackCount = r.readbacks.count;
    if (s.readbackCount != 0) {
        s.readbacks = static_cast<D3d11Readback*>(prMemoryAlloc(sizeof(D3d11Readback) * s.readbackCount));
        prMemCopy(s.readbacks, r.readbacks.data, sizeof(D3d11Readback) * s.readbackCount);
    }
    prMutexLock(&d->doneMutex);
    d->pending.push(s);
    prConditionSignal(&d->pendingSignal);
    prMutexUnlock(&d->doneMutex);
    r.roots.reset();
    r.readbacks.reset();
    for (uint32_t i = 0; i < c->scratch.count; ++i)
        prMemoryFree(c->scratch.data[i]);
    c->scratch.reset();
    c->ops.reset();
    c->blocks.reset();
    c->presents.reset();
    c->dones.reset();
    prMemoryFree(c);
    return fence;
}

// ---------------------------------------------------------------------------------------------------------------------
// Swap chains

// Takes the swap chain's buffer 0 into a resource id. Returns false if the device has no id for it.
bool takeBuffer(D3d11Device* d, D3d11SwapChain* sc) {
    ID3D11Texture2D* buffer = nullptr;
    sc->buffer = 0;
    if (FAILED(sc->swapChain->GetBuffer(0, IID_PPV_ARGS(&buffer))))
        return false;
    PrGpuTextureDesc desc = prGpuTextureDesc2d(sc->info.format, sc->capacity,
        PrGpuTextureUsageFlag_RenderTarget | PrGpuTextureUsageFlag_Sampled);
    sc->buffer = addTexture(d, buffer, desc, sc->format, 1, 1, 0, 0);
    if (sc->buffer == 0) {
        buffer->Release();
        return false;
    }
    return true;
}

// Frees the id of the swap chain's buffer, whose work is done, and the references that it holds.
void releaseBuffer(D3d11Device* d, D3d11SwapChain* sc) {
    prMutexLock(&d->mutex);
    if (sc->buffer != 0)
        freeResource(d, sc->buffer);
    prMutexUnlock(&d->mutex);
    sc->buffer = 0;
    sc->current = PrGpuTexture{};
}

// Shows the swap chain over its window's client area, in a visual of its own. Returns false if DirectComposition
// refuses.
bool compose(D3d11SwapChain* sc) {
    return SUCCEEDED(DCompositionCreateDevice(nullptr, IID_PPV_ARGS(&sc->composition))) &&
        SUCCEEDED(sc->composition->CreateTargetForHwnd(sc->window, TRUE, &sc->target)) &&
        SUCCEEDED(sc->composition->CreateVisual(&sc->visual)) && SUCCEEDED(sc->visual->SetContent(sc->swapChain)) &&
        SUCCEEDED(sc->target->SetRoot(sc->visual)) && SUCCEEDED(sc->composition->Commit());
}

// Releases the swap chain's objects that the device does not own: the composition, the displays' factory and the swap
// chain itself.
void releaseSwapChainObjects(D3d11SwapChain* sc) {
    if (sc->waitable != nullptr)
        CloseHandle(sc->waitable);
    release(sc->visual);
    release(sc->target);
    release(sc->composition);
    release(sc->displays);
    release(sc->swapChain);
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
PrGpuSwapChainInfo infoFor(D3d11SwapChain* sc) {
    const PrGpuSwapChainInfo kSdr{PrGpuFormat_Bgra8, PrColorSpace_Srgb};
    if (!sc->hdr || !displayTakesHdr10(sc->displays, sc->window))
        return kSdr;
    if (presents(sc->swapChain, DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020))
        return PrGpuSwapChainInfo{PrGpuFormat_Rgb10A2, PrColorSpace_Rec2100Pq};
    return PrGpuSwapChainInfo{PrGpuFormat_Rgba16F, PrColorSpace_SrgbLinear};
}

// Makes the swap chain's buffers `info`'s format at `width` by `height`, and presents them in its color space. Every
// reference to the buffers is released first, as resizing them needs. Returns false if DXGI refuses the format.
bool takeInfo(D3d11SwapChain* sc, PrGpuSwapChainInfo info, UINT width, UINT height) {
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
PrGpuSwapChainInfo infoNow(D3d11SwapChain* sc) {
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
    D3d11Device* d = d3d11Of(device);
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
    bool transparent = (desc->flags & PrGpuSwapChainFlag_Transparent) != 0;
    if (transparent) {
        // A swap chain for composition takes only stretched scaling, and its visual shows the buffers unscaled.
        s.Scaling = DXGI_SCALING_STRETCH;
        s.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        if (FAILED(d->factory->CreateSwapChainForComposition(d->device, &s, nullptr, &chain1)))
            return PrStatus_NotSupported;
    } else {
        if (FAILED(d->factory->CreateSwapChainForHwnd(d->device, window, &s, nullptr, nullptr, &chain1)))
            return PrStatus_NotSupported;
        d->factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
    }
    IDXGISwapChain3* native = nullptr;
    chain1->QueryInterface(IID_PPV_ARGS(&native));
    chain1->Release();
    if (native == nullptr)
        return PrStatus_NotSupported;
    native->SetMaximumFrameLatency(2);
    D3d11SwapChain* sc = allocZeroed<D3d11SwapChain>();
    static_cast<PrGpuSwapChain&>(*sc) = kSwapChainFunctions;
    sc->device = device;
    sc->info = info;
    sc->swapChain = native;
    sc->window = window;
    sc->hdr = desc->intent == PrGpuSwapChainIntent_Hdr;
    d->factory->AddRef();
    sc->displays = d->factory;
    if (transparent && !compose(sc)) {
        releaseSwapChainObjects(sc);
        prMemoryFree(sc);
        return PrStatus_NotSupported;
    }
    if (sc->hdr) {
        sc->monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
        PrGpuSwapChainInfo chosen = infoFor(sc);
        if ((chosen.format != info.format || chosen.space != info.space) && !takeInfo(sc, chosen, s.Width, s.Height)) {
            releaseSwapChainObjects(sc);
            prMemoryFree(sc);
            return PrStatus_NotSupported;
        }
    }
    sc->stretch = desc->anchor == PrGpuSwapChainAnchor_Stretch;
    sc->waitable = native->GetFrameLatencyWaitableObject();
    sc->format = formatOf(sc->info.format);
    sc->capacity = PrUVec2{s.Width, s.Height};
    if (!takeBuffer(d, sc)) {
        releaseSwapChainObjects(sc);
        prMemoryFree(sc);
        return PrStatus_Limit;
    }
    *out = sc;
    return PrStatus_Ok;
}

void swapChainDestroy(PrGpuSwapChain* handle) {
    PrGpuDevice* device = static_cast<GpuSharedSwapChain*>(handle)->device;
    D3d11Device* d = d3d11Of(device);
    D3d11SwapChain* sc = static_cast<D3d11SwapChain*>(handle);
    // DXGI lets a window have one swap chain at a time, so the swap chain is gone before this returns, and the window
    // can take another.
    waitIdle(device);
    releaseBuffer(d, sc);
    prMutexLock(&d->contextMutex);
    d->context->ClearState();
    d->context->Flush();
    prMutexUnlock(&d->contextMutex);
    releaseSwapChainObjects(sc);
    prMemoryFree(sc);
}

// The waitable object counts the frames that the swap chain can take, and each wait takes one, so a frame waits once.
void swapChainWait(PrGpuSwapChain* handle) {
    D3d11SwapChain* sc = static_cast<D3d11SwapChain*>(handle);
    if (sc->waited)
        return;
    if (sc->waitable != nullptr)
        WaitForSingleObjectEx(sc->waitable, 1000, TRUE);
    sc->waited = true;
}

PrStatus swapChainAcquire(PrGpuSwapChain* handle, PrUVec2 extent, PrGpuTexture* out) {
    PrGpuDevice* device = static_cast<GpuSharedSwapChain*>(handle)->device;
    D3d11Device* d = d3d11Of(device);
    D3d11SwapChain* sc = static_cast<D3d11SwapChain*>(handle);
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
    if (capacity.x != sc->capacity.x || capacity.y != sc->capacity.y || sc->buffer == 0 || reformat) {
        // Resizing the buffers needs every reference to them released, and the work that uses them done.
        waitIdle(device);
        releaseBuffer(d, sc);
        prMutexLock(&d->contextMutex);
        d->context->ClearState();
        d->context->Flush();
        bool resized = takeInfo(sc, reformat ? info : sc->info, capacity.x, capacity.y);
        prMutexUnlock(&d->contextMutex);
        if (resized)
            sc->capacity = capacity;
        bool taken = takeBuffer(d, sc);
        if (!resized || !taken)
            return PrStatus_InvalidState;
    }
    d->resources[sc->buffer].texture.extent = PrGpuExtent{extent.x, extent.y, 1};
    sc->current = PrGpuTexture{sc->buffer};
    *out = sc->current;
    return PrStatus_Ok;
}

void swapChainPresent(PrGpuSwapChain* handle, PrGpuCommands* commands) {
    D3d11SwapChain* sc = static_cast<D3d11SwapChain*>(handle);
    if (sc->current.id == 0)
        return;
    commandsOf(commands)->presents.push(sc);
    sc->current = PrGpuTexture{};
}

// ---------------------------------------------------------------------------------------------------------------------
// Creation

// Returns true if a device has what the backend needs: constant buffer offsets, dynamic constant buffers that map
// without overwriting, and fences.
bool hasFeatures(ID3D11Device5* device) {
    D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
    D3D11_FEATURE_DATA_D3D11_OPTIONS3 options3{};
    return device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_0 &&
        SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof(options))) &&
        options.ConstantBufferOffsetting && options.MapNoOverwriteOnDynamicConstantBuffer &&
        SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS3, &options3, sizeof(options3)));
}

// Creates a device on the first hardware adapter, in order of performance, that has what the backend needs, with the
// debug layer if `validation` and the layer is installed. Returns null if no adapter has it.
ID3D11Device5* PR_NULLABLE createDevice(IDXGIFactory6* factory, bool validation) {
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
            IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        ID3D11Device* device = nullptr;
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
            UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
            HRESULT made = E_FAIL;
            if (validation) {
                made = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                    flags | D3D11_CREATE_DEVICE_DEBUG, levels, 2, D3D11_SDK_VERSION, &device, nullptr, nullptr);
            }
            if (FAILED(made)) {
                D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels, 2, D3D11_SDK_VERSION,
                    &device, nullptr, nullptr);
            }
        }
        adapter->Release();
        ID3D11Device5* device5 = nullptr;
        if (device != nullptr) {
            device->QueryInterface(IID_PPV_ARGS(&device5));
            device->Release();
        }
        if (device5 != nullptr && hasFeatures(device5))
            return device5;
        release(device5);
    }
    return nullptr;
}

// Names the device for its info, from its adapter's name.
void nameAdapter(D3d11Device* d) {
    IDXGIDevice* dxgi = nullptr;
    IDXGIAdapter* adapter = nullptr;
    DXGI_ADAPTER_DESC desc{};
    if (SUCCEEDED(d->device->QueryInterface(IID_PPV_ARGS(&dxgi))) && SUCCEEDED(dxgi->GetAdapter(&adapter)))
        adapter->GetDesc(&desc);
    release(adapter);
    release(dxgi);
    uint32_t units = uint32_t(lstrlenW(desc.Description));
    uint32_t capacity = uint32_t(sizeof(d->adapter) - 1);
    uint32_t size = prWindowsNarrow(desc.Description, units, d->adapter, capacity);
    if (size == 0 || size > capacity)
        prMemCopy(d->adapter, "Direct3D 11", sizeof("Direct3D 11"));
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
    .setBufferFn = commandsSetBuffer,
    .setRwBufferFn = commandsSetRwBuffer,
    .setTextureFn = commandsSetTexture,
    .setRwTextureFn = commandsSetRwTexture,
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
    .nativeDrawFn = nullptr,
    .setViewportFn = passSetViewport,
    .setScissorFn = passSetScissor,
    .setBlendConstantFn = passSetBlendConstant,
    .setStencilReferenceFn = passSetStencilReference,
    .setIndexBufferFn = passSetIndexBuffer,
    .setVertexBufferFn = passSetVertexBuffer,
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

// Makes the device's D3D11 objects and starts the completion thread. Returns NotSupported if the system has no adapter
// with the features that the backend needs, or if the objects cannot be made.
PrStatus initializeDevice(GpuSharedDevice* device) {
    D3d11Device* d = static_cast<D3d11Device*>(device);
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&d->factory))))
        return PrStatus_NotSupported;
    d->device = createDevice(d->factory, d->options.validation);
    if (d->device == nullptr)
        return PrStatus_NotSupported;
    ID3D11DeviceContext3* context = nullptr;
    d->device->GetImmediateContext3(&context);
    if (context != nullptr) {
        context->QueryInterface(IID_PPV_ARGS(&d->context));
        context->Release();
    }
    if (d->context == nullptr || FAILED(d->device->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&d->fence))))
        return PrStatus_NotSupported;
    d->context->QueryInterface(IID_PPV_ARGS(&d->annotation));
    if (d->options.validation && SUCCEEDED(d->device->QueryInterface(IID_PPV_ARGS(&d->messages))))
        d->messages->SetMessageCountLimit(1024);
    D3D11_BUFFER_DESC ring{kRootRingBytes, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
    void* zeros = prMemoryAllocZeroed(kPrGpuMaxUniformBytes);
    D3D11_SUBRESOURCE_DATA initial{zeros, 0, 0};
    D3D11_BUFFER_DESC zero{kPrGpuMaxUniformBytes, D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    bool made = SUCCEEDED(d->device->CreateBuffer(&ring, nullptr, &d->root)) &&
        SUCCEEDED(d->device->CreateBuffer(&zero, &initial, &d->zeros));
    prMemoryFree(zeros);
    if (!made)
        return PrStatus_NotSupported;
    d->rootSize = kRootRingBytes;
    // The queries that time the work, if the device makes them.
    D3D11_QUERY_DESC disjoint{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    ID3D11Query* probe = nullptr;
    if (SUCCEEDED(d->device->CreateQuery(&disjoint, &probe))) {
        d->info.features |= PrGpuFeatureFlag_Timestamps;
        probe->Release();
    }
    prThreadStart(&d->completion, serveCompletions, d);
    nameAdapter(d);
    d->info.adapter = PrStringView{d->adapter, strlen(d->adapter)};
    return PrStatus_Ok;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrGpuD3d11Options prGpuD3d11OptionsDefault(void) {
    PrGpuD3d11Options options{};
    char value[2] = {};
    if (GetEnvironmentVariableA("PR_D3D11_DEBUG", value, sizeof(value)) == 1)
        options.validation = value[0] != '0';
    return options;
}

PrGpuDevice* prGpuD3d11Create(const PrGpuD3d11Options* PR_NULLABLE options) {
    D3d11Device* d = allocZeroed<D3d11Device>();
    gpuSharedDeviceInit(d);
    d->options = options != nullptr ? *options : prGpuD3d11OptionsDefault();
    prMutexInit(&d->mutex);
    prMutexInit(&d->contextMutex);
    prMutexInit(&d->doneMutex);
    prConditionInit(&d->doneSignal);
    prConditionInit(&d->pendingSignal);
    PrGpuDeviceLimits limits = gpuSharedDeviceLimits(d->options.limits);
    limits.samplers = min(limits.samplers, uint32_t(D3D11_REQ_SAMPLER_OBJECT_COUNT_PER_DEVICE));
    d->info.limits = limits;
    d->resources.init(1, limits.resources);
    d->pipelines.init(1, limits.pipelines);
    d->samplers.init(1, limits.samplers);
    d->serial = 1;

    d->initialize = initializeDevice;
    d->removed = removed;
    d->destroy = destroyDevice;
    d->simulateLossFn = simulateLoss;
    d->isCompleteFn = isComplete;
    d->waitFn = wait;
    d->waitIdleFn = waitIdle;
    d->supportsFormatFn = supportsFormat;
    d->bufferCreateFn = bufferCreate;
    d->bufferRetainFn = bufferRetain;
    d->bufferReleaseFn = bufferRelease;
    d->bufferDescFn = bufferDesc;
    d->bufferStorageHandleFn = bufferStorageHandle;
    d->bufferMapFn = bufferMap;
    d->bufferUploadFn = bufferUpload;
    d->textureCreateFn = textureCreate;
    d->textureCreateViewFn = textureCreateView;
    d->textureRetainFn = textureRetain;
    d->textureReleaseFn = textureRelease;
    d->textureDescFn = textureDesc;
    d->textureStorageHandleFn = textureStorageHandle;
    d->textureUploadFn = textureUpload;
    d->samplerCreate = samplerCreate;
    d->shaderLibraryCreateFn = shaderLibraryCreate;
    d->graphicsPipelineCreateFn = graphicsPipelineCreate;
    d->computePipelineCreateFn = computePipelineCreate;
    d->pipelineRetainFn = pipelineRetain;
    d->pipelineReleaseFn = pipelineRelease;
    d->pipelineGroupSizeFn = pipelineGroupSize;
    d->commandsBeginFn = commandsBegin;
    d->swapChainCreateFn = swapChainCreate;
    d->shaderTarget = PrGpuShaderTarget_Dxbc;
    d->info.backend = "Direct3D 11";
    d->info.features = PrGpuFeatureFlag_TextureCompressionBc;
    d->info.maxTextureSize = D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;
    d->info.maxBufferSlots = kBufferSlots;
    d->info.maxRwBufferSlots = kRwBufferSlots;
    d->info.maxTextureSlots = kTextureSlots;
    d->info.maxRwTextureSlots = kRwTextureSlots;
    d->info.storageOffsetAlignment = 16;
    gpuSharedDeviceStart(d, d->options.pool, d->options.ready, d->options.readyContext);
    return d;
}
