// The WebGPU GPU backend: tables of the device's resources, and command buffers that record their commands as words,
// which the page's side of the backend (gpu_webgpu.js) decodes into WebGPU's commands when they are submitted.
//
// A resource's id is its index in the device's table, and the page's side keeps its WebGPU object at the same index. A
// destroyed resource is retired until the work that may use it is done, and only then is its id given out again.
//
// A command buffer's stream is blocks of memory in the program, each with a buffer of its own on the GPU, which the
// submission copies the used part of each block into. Upload and readback buffers have a copy of their bytes in the
// program as well: a submission copies every upload buffer to the GPU, and a fence is reached once the readback buffers
// that its work wrote are copied back.

#include "pr/gpu_webgpu.h"

#include "pr/align.h"
#include "pr/alloc.h"
#include "pr/app_surface.h"
#include "pr/assert.h"
#include "pr/mem.h"
#include "pr/min_max.h"
#include "pr/raw_array.h"
#include "pr/raw_string.h"

#include "gpu/backends/shared/src/gpu_shared_device.h"

#include <emscripten/emscripten.h>

// The page's side of the backend (gpu_webgpu.js). Every id is the device's index of the object, and zero is none.
extern "C" bool prGpuWebgpuWaits();
extern "C" bool prGpuWebgpuRequest(void* device, bool wait);
extern "C" void prGpuWebgpuWait(uint32_t device);
extern "C" void prGpuWebgpuRelease(uint32_t device);
extern "C" void prGpuWebgpuSimulateLoss(uint32_t device);
extern "C" bool prGpuWebgpuBufferCreate(uint32_t device, uint32_t id, uint64_t size, uint32_t memory, uint8_t* shadow);
extern "C" void prGpuWebgpuResourceRelease(uint32_t device, uint32_t id);
extern "C" void prGpuWebgpuBufferWrite(uint32_t device, uint32_t id, uint64_t offset, const void* data, uint64_t size);
extern "C" bool prGpuWebgpuTextureCreate(uint32_t device, uint32_t id, uint32_t kind, uint32_t format, uint32_t usage,
    uint32_t levels, uint32_t samples, uint32_t width, uint32_t height, uint32_t depth);
extern "C" bool prGpuWebgpuTextureCreateView(uint32_t device, uint32_t id, uint32_t texture, uint32_t kind, uint32_t format,
    uint32_t firstLevel, uint32_t levels, uint32_t firstLayer, uint32_t layers);
extern "C" void prGpuWebgpuTextureWrite(uint32_t device, uint32_t id, uint32_t level, uint32_t x, uint32_t y, uint32_t z,
    uint32_t width, uint32_t height, uint32_t depth, const void* data, uint32_t rowBytes, uint32_t rows, uint64_t size);
extern "C" bool prGpuWebgpuSamplerCreate(uint32_t device, uint32_t id, const uint32_t* words);
extern "C" bool prGpuWebgpuPipelineCreate(uint32_t device, uint32_t id, const uint32_t* words, uint32_t wordCount);
extern "C" void prGpuWebgpuPipelineRelease(uint32_t device, uint32_t id);
extern "C" void prGpuWebgpuSubmit(uint32_t device, const uint32_t* words, uint32_t wordCount, const uint32_t* streams,
    uint32_t streamCount, uint64_t fence);
extern "C" uint32_t prGpuWebgpuCanvasAttach(uint32_t device, const char* selector, size_t selectorSize, uint32_t width,
    uint32_t height, bool transparent, uint32_t* format);
extern "C" void prGpuWebgpuCanvasRelease(uint32_t device, uint32_t canvas);
extern "C" void prGpuWebgpuCanvasAcquire(uint32_t device, uint32_t canvas, uint32_t id, uint32_t width, uint32_t height);

namespace pr_impl {

using namespace pr;

namespace {

// The commands of a command buffer as it records them, each an operation's word and its arguments. A float is its bits,
// and a 64-bit offset is two words, low then high. gpu_webgpu.js decodes them by the same numbers.
enum {
    WebgpuOp_PassBegin = 1,         // color count, (texture, resolve, level, layer, load, store, clear rgba) for each
                                    // color, depth texture, level, layer, load, store, stencil load, stencil store,
                                    // clear depth, clear stencil
    WebgpuOp_PassEnd,
    WebgpuOp_Pipeline,              // pipeline
    WebgpuOp_Push,                  // buffer, offset
    WebgpuOp_Uniforms,              // buffer, offset, size
    WebgpuOp_Buffer,                // slot, buffer, offset
    WebgpuOp_RwBuffer,              // slot, buffer, offset
    WebgpuOp_Texture,               // slot, texture, sampler
    WebgpuOp_RwTexture,             // slot, texture
    WebgpuOp_VertexBuffer,          // slot, buffer, offset
    WebgpuOp_IndexBuffer,           // buffer, offset, wide
    WebgpuOp_Viewport,              // x, y, width, height, min depth, max depth
    WebgpuOp_Scissor,               // x, y, width, height
    WebgpuOp_BlendConstant,         // rgba
    WebgpuOp_StencilReference,      // reference
    WebgpuOp_Draw,                  // vertex count, instance count, first vertex, first instance
    WebgpuOp_DrawIndexed,           // index count, instance count, first index, vertex offset, first instance
    WebgpuOp_DrawIndirect,          // buffer, offset, count, stride
    WebgpuOp_DrawIndexedIndirect,   // buffer, offset, count, stride
    WebgpuOp_Dispatch,              // x, y, z
    WebgpuOp_DispatchIndirect,      // buffer, offset
    WebgpuOp_CopyBuffer,            // source, offset (2), target, offset (2), size (2)
    WebgpuOp_CopyBufferToTexture,   // buffer, offset (2), row bytes, rows, block height, texture, region (8)
    WebgpuOp_CopyTextureToBuffer,   // texture, region (8), buffer, offset (2), row bytes, rows, block height
    WebgpuOp_CopyTexture,           // source, region (8), target, region (8)
};
typedef uint32_t WebgpuOp;

// The page's texture formats (GPUTextureFormat) that the backend uses, in the order of gpu_webgpu.js's names of them.
// ASTC's follow WebgpuFormat_Astc, two to a block size, Unorm and then Srgb, in the order of PrGpuFormat's block sizes.
enum : uint32_t {
    WebgpuFormat_None,
    WebgpuFormat_R8Unorm,
    WebgpuFormat_Rg8Unorm,
    WebgpuFormat_R16Unorm,
    WebgpuFormat_Rg16Unorm,
    WebgpuFormat_Rgba8Unorm,
    WebgpuFormat_Rgba8UnormSrgb,
    WebgpuFormat_Bgra8Unorm,
    WebgpuFormat_Bgra8UnormSrgb,
    WebgpuFormat_Rgb10A2Unorm,
    WebgpuFormat_Rg11B10Ufloat,
    WebgpuFormat_R16Float,
    WebgpuFormat_Rg16Float,
    WebgpuFormat_Rgba16Float,
    WebgpuFormat_R32Float,
    WebgpuFormat_Rg32Float,
    WebgpuFormat_Rgba32Float,
    WebgpuFormat_R8Uint,
    WebgpuFormat_R16Uint,
    WebgpuFormat_R32Uint,
    WebgpuFormat_Rg32Uint,
    WebgpuFormat_Rgba32Uint,
    WebgpuFormat_Depth16Unorm,
    WebgpuFormat_Depth32Float,
    WebgpuFormat_Depth32FloatStencil8,
    WebgpuFormat_Bc1,
    WebgpuFormat_Bc1Srgb,
    WebgpuFormat_Bc2,
    WebgpuFormat_Bc2Srgb,
    WebgpuFormat_Bc3,
    WebgpuFormat_Bc3Srgb,
    WebgpuFormat_Bc4,
    WebgpuFormat_Bc4Snorm,
    WebgpuFormat_Bc5,
    WebgpuFormat_Bc5Snorm,
    WebgpuFormat_Bc6HUfloat,
    WebgpuFormat_Bc6HSfloat,
    WebgpuFormat_Bc7,
    WebgpuFormat_Bc7Srgb,
    WebgpuFormat_Etc2Rgb8,
    WebgpuFormat_Etc2Rgb8Srgb,
    WebgpuFormat_Etc2Rgb8A1,
    WebgpuFormat_Etc2Rgb8A1Srgb,
    WebgpuFormat_Etc2Rgba8,
    WebgpuFormat_Etc2Rgba8Srgb,
    WebgpuFormat_EacR11,
    WebgpuFormat_EacR11Snorm,
    WebgpuFormat_EacRg11,
    WebgpuFormat_EacRg11Snorm,
    WebgpuFormat_Astc,
};
typedef uint32_t WebgpuFormat;

// Returns the page's format of `format`, or none if the page has none. WebGPU places no channels otherwise, so a
// swizzled format has the format that stores it, which a device without PrGpuFeatureFlag_TextureSwizzle never samples.
WebgpuFormat webgpuFormatOf(PrGpuFormat format) {
    const PrGpuFormat stored = prGpuFormatStored(format);
    if (prGpuFormatFamily(stored) == PrGpuFormatFamily_Astc) {
        const uint32_t size = (prGpuFormatPlain(stored) - PrGpuFormat_Astc4x4) / kPrGpuAstcFormatsPerBlockSize;
        return WebgpuFormat_Astc + size * 2 + (prGpuFormatIsSrgb(stored) ? 1 : 0);
    }
    switch (stored) {
        case PrGpuFormat_R8:
            return WebgpuFormat_R8Unorm;
        case PrGpuFormat_Rg8:
            return WebgpuFormat_Rg8Unorm;
        case PrGpuFormat_R16:
            return WebgpuFormat_R16Unorm;
        case PrGpuFormat_Rg16:
            return WebgpuFormat_Rg16Unorm;
        case PrGpuFormat_Rgba8:
            return WebgpuFormat_Rgba8Unorm;
        case PrGpuFormat_Rgba8Srgb:
            return WebgpuFormat_Rgba8UnormSrgb;
        case PrGpuFormat_Bgra8:
            return WebgpuFormat_Bgra8Unorm;
        case PrGpuFormat_Bgra8Srgb:
            return WebgpuFormat_Bgra8UnormSrgb;
        case PrGpuFormat_Rgb10A2:
            return WebgpuFormat_Rgb10A2Unorm;
        case PrGpuFormat_Rg11B10F:
            return WebgpuFormat_Rg11B10Ufloat;
        case PrGpuFormat_R16F:
            return WebgpuFormat_R16Float;
        case PrGpuFormat_Rg16F:
            return WebgpuFormat_Rg16Float;
        case PrGpuFormat_Rgba16F:
            return WebgpuFormat_Rgba16Float;
        case PrGpuFormat_R32F:
            return WebgpuFormat_R32Float;
        case PrGpuFormat_Rg32F:
            return WebgpuFormat_Rg32Float;
        case PrGpuFormat_Rgba32F:
            return WebgpuFormat_Rgba32Float;
        case PrGpuFormat_R8Uint:
            return WebgpuFormat_R8Uint;
        case PrGpuFormat_R16Uint:
            return WebgpuFormat_R16Uint;
        case PrGpuFormat_R32Uint:
            return WebgpuFormat_R32Uint;
        case PrGpuFormat_Rg32Uint:
            return WebgpuFormat_Rg32Uint;
        case PrGpuFormat_Rgba32Uint:
            return WebgpuFormat_Rgba32Uint;
        case PrGpuFormat_D16:
            return WebgpuFormat_Depth16Unorm;
        case PrGpuFormat_D32F:
            return WebgpuFormat_Depth32Float;
        case PrGpuFormat_D32FS8:
            return WebgpuFormat_Depth32FloatStencil8;
        case PrGpuFormat_Bc1:
            return WebgpuFormat_Bc1;
        case PrGpuFormat_Bc1Srgb:
            return WebgpuFormat_Bc1Srgb;
        case PrGpuFormat_Bc2:
            return WebgpuFormat_Bc2;
        case PrGpuFormat_Bc2Srgb:
            return WebgpuFormat_Bc2Srgb;
        case PrGpuFormat_Bc3:
            return WebgpuFormat_Bc3;
        case PrGpuFormat_Bc3Srgb:
            return WebgpuFormat_Bc3Srgb;
        case PrGpuFormat_Bc4:
            return WebgpuFormat_Bc4;
        case PrGpuFormat_Bc4Snorm:
            return WebgpuFormat_Bc4Snorm;
        case PrGpuFormat_Bc5:
            return WebgpuFormat_Bc5;
        case PrGpuFormat_Bc5Snorm:
            return WebgpuFormat_Bc5Snorm;
        case PrGpuFormat_Bc6HUfloat:
            return WebgpuFormat_Bc6HUfloat;
        case PrGpuFormat_Bc6HSfloat:
            return WebgpuFormat_Bc6HSfloat;
        case PrGpuFormat_Bc7:
            return WebgpuFormat_Bc7;
        case PrGpuFormat_Bc7Srgb:
            return WebgpuFormat_Bc7Srgb;
        case PrGpuFormat_Etc2Rgb8:
            return WebgpuFormat_Etc2Rgb8;
        case PrGpuFormat_Etc2Rgb8Srgb:
            return WebgpuFormat_Etc2Rgb8Srgb;
        case PrGpuFormat_Etc2Rgb8A1:
            return WebgpuFormat_Etc2Rgb8A1;
        case PrGpuFormat_Etc2Rgb8A1Srgb:
            return WebgpuFormat_Etc2Rgb8A1Srgb;
        case PrGpuFormat_Etc2Rgba8:
            return WebgpuFormat_Etc2Rgba8;
        case PrGpuFormat_Etc2Rgba8Srgb:
            return WebgpuFormat_Etc2Rgba8Srgb;
        case PrGpuFormat_EacR11:
            return WebgpuFormat_EacR11;
        case PrGpuFormat_EacR11Snorm:
            return WebgpuFormat_EacR11Snorm;
        case PrGpuFormat_EacRg11:
            return WebgpuFormat_EacRg11;
        case PrGpuFormat_EacRg11Snorm:
            return WebgpuFormat_EacRg11Snorm;
        default:
            return WebgpuFormat_None;
    }
}

// Returns the format of a canvas that the page prefers.
PrGpuFormat formatOfCanvas(WebgpuFormat format) {
    switch (format) {
        case WebgpuFormat_Rgba8Unorm:
            return PrGpuFormat_Rgba8;
        case WebgpuFormat_Rgba16Float:
            return PrGpuFormat_Rgba16F;
        default:
            return PrGpuFormat_Bgra8;
    }
}

// The optional features of WebGPU that a device has, as gpu_webgpu.js's prGpuWebgpuFeatureNames lists them.
enum {
    WebgpuFeature_Timestamps = 1u << 0,
    WebgpuFeature_Float32Filterable = 1u << 1,      // textures of 32-bit floats are filtered
    WebgpuFeature_CompressionBc = 1u << 2,
    WebgpuFeature_CompressionAstc = 1u << 3,
    WebgpuFeature_Rg11B10Renderable = 1u << 4,
    WebgpuFeature_CompressionEtc2 = 1u << 5,
};
typedef uint32_t WebgpuFeatures;

// What a resource is: a free entry, a buffer, a texture, or a view of a texture.
enum {
    WebgpuKind_Free,
    WebgpuKind_Buffer,
    WebgpuKind_Texture,
};
typedef uint8_t WebgpuKind;

// A buffer or a texture. An upload or readback buffer has a copy of its bytes in the program, which a stream block has
// too.
struct WebgpuResource {
    WebgpuKind kind;
    bool stream;
    PrGpuBufferDesc buffer;
    PrGpuTextureDesc texture;
    uint8_t* PR_NULLABLE shadow;
};

// A resource or a pipeline that was destroyed, kept until the device reaches `fence` with no command buffer open.
struct WebgpuRetired {
    uint32_t id;
    PrGpuFence fence;
    bool pipeline;
};

// A pipeline: whether it dispatches, and its group size if so.
struct WebgpuPipeline {
    bool used;
    bool compute;
    PrGpuExtent groupSize;
};

// A block of a command buffer's stream: its buffer, and the bytes given out from it.
struct WebgpuBlock {
    uint32_t buffer;
    uint64_t used;
};

// A callback of a command buffer, called once the device reaches `fence`.
struct WebgpuDone {
    PrGpuFence fence;
    PrGpuDoneFn done;
    void* PR_NULLABLE context;
};

struct WebgpuPass;

struct WebgpuDevice : GpuSharedDevice {
    uint32_t page;                      // the page's id of the GPUDevice, or zero before it is ready
    bool waits;                         // the program suspends to wait for the page (prGpuWebgpuWaits)
    bool lost;
    uint32_t errors;                    // the validation errors that the page reported
    WebgpuFeatures webgpuFeatures;
    GpuSharedTable<WebgpuResource> resources;
    GpuSharedTable<PrGpuSamplerDesc> samplers;
    GpuSharedTable<WebgpuPipeline> pipelines;
    RawArray<WebgpuRetired> retired;
    RawArray<uint32_t> freeBlocks;      // the buffers of kBlockSize stream blocks that no command buffer holds
    RawArray<WebgpuBlock> spentBlocks;  // the blocks of submitted work, by the fences in `spentFences`
    RawArray<PrGpuFence> spentFences;
    RawArray<WebgpuDone> dones;         // in the order of their fences
    uint32_t open;                      // the number of command buffers that are recording
    PrGpuFence submitted;
    PrGpuFence reached;
};

struct WebgpuCommands : GpuSharedCommands {
    RawArray<uint32_t> words;
    RawArray<WebgpuBlock> blocks;
    RawArray<WebgpuDone> dones;         // with a fence of zero until the submission gives it
    RawArray<void*> scratch;            // the stream's memory once the device is lost, which the GPU never reads
    WebgpuPass* PR_NULLABLE pass;
};

struct WebgpuPass : GpuSharedPass {};

// The bytes of a stream block. A larger allocation takes a block of its own, which is destroyed once its work is done.
constexpr uint64_t kBlockSize = 1u << 20;

WebgpuDevice* webgpuOf(PrGpuDevice* device) {
    return static_cast<WebgpuDevice*>(device);
}

const WebgpuDevice* webgpuOf(const PrGpuDevice* device) {
    return static_cast<const WebgpuDevice*>(device);
}

WebgpuCommands* commandsOf(PrGpuCommands* commands) {
    return static_cast<WebgpuCommands*>(commands);
}

WebgpuDevice* deviceOfCommands(PrGpuCommands* commands) {
    return webgpuOf(static_cast<GpuSharedCommands*>(commands)->device);
}

void emit(WebgpuCommands* c, uint32_t word) {
    c->words.push(word);
}

void emitFloat(WebgpuCommands* c, float value) {
    uint32_t bits = 0;
    prMemCopy(&bits, &value, sizeof(bits));
    c->words.push(bits);
}

void emit64(WebgpuCommands* c, uint64_t value) {
    c->words.push(uint32_t(value));
    c->words.push(uint32_t(value >> 32));
}

// Emits a region of a texture of `format`, with its extent in whole blocks: WebGPU names a copy's extent so, and the
// blocks reach past a level that ends inside its last ones.
void emitRegion(WebgpuCommands* c, const PrGpuTextureRegion& r, PrGpuFormat format) {
    PrGpuFormatBlock block = prGpuFormatBlock(format);
    emit(c, r.level);
    emit(c, r.firstLayer);
    emit(c, r.x);
    emit(c, r.y);
    emit(c, r.z);
    emit(c, alignUp(r.extent.width, uint32_t(block.width)));
    emit(c, alignUp(r.extent.height, uint32_t(block.height)));
    emit(c, r.extent.depth);
}

// Returns a new resource id, or zero if the device has none left.
uint32_t addResource(WebgpuDevice* d, const WebgpuResource& resource) {
    const uint32_t id = d->resources.take();
    if (id != 0)
        d->resources[id] = resource;
    return id;
}

// Frees the retired resources that no work may use any more, and the ids that they held.
void collect(WebgpuDevice* d) {
    if (d->open != 0)
        return;
    uint32_t kept = 0;
    for (uint32_t i = 0; i < d->retired.count; ++i) {
        const WebgpuRetired& r = d->retired.data[i];
        if (r.fence > d->reached) {
            d->retired.data[kept++] = r;
            continue;
        }
        if (r.pipeline) {
            d->pipelines[r.id] = WebgpuPipeline{};
            if (d->page != 0)
                prGpuWebgpuPipelineRelease(d->page, r.id);
            d->pipelines.free(r.id);
            continue;
        }
        WebgpuResource& resource = d->resources[r.id];
        prMemoryFree(resource.shadow);
        resource = WebgpuResource{};
        if (d->page != 0)
            prGpuWebgpuResourceRelease(d->page, r.id);
        d->resources.free(r.id);
    }
    d->retired.count = kept;
}

void retire(WebgpuDevice* d, uint32_t id, bool pipeline = false) {
    d->retired.push(WebgpuRetired{id, d->submitted + 1, pipeline});
    collect(d);
}

// ---------------------------------------------------------------------------------------------------------------------
// The device

// Requests the page's device and waits for it, in a program that suspends.
PrStatus initialize(GpuSharedDevice* device) {
    WebgpuDevice* d = static_cast<WebgpuDevice*>(device);
    if (!prGpuWebgpuRequest(d, true))
        return PrStatus_NotSupported;
    return d->page != 0 ? PrStatus_Ok : PrStatus_NotSupported;
}

bool removed(const GpuSharedDevice* device) {
    return static_cast<const WebgpuDevice*>(device)->lost;
}

void destroyDevice(GpuSharedDevice* device) {
    WebgpuDevice* d = static_cast<WebgpuDevice*>(device);
    gpuSharedDeviceReset(d);
    for (uint32_t id = d->resources.firstId; id < d->resources.nextId; ++id)
        prMemoryFree(d->resources[id].shadow);
    if (d->page != 0)
        prGpuWebgpuRelease(d->page);
    d->resources.reset();
    d->samplers.reset();
    d->pipelines.reset();
    d->retired.reset();
    d->freeBlocks.reset();
    d->spentBlocks.reset();
    d->spentFences.reset();
    d->dones.reset();
    prMemoryFree(d);
}

// The page loses a device without telling the work that it had, so simulating a loss loses it in the page too.
void simulateLoss(PrGpuDevice* device) {
    WebgpuDevice* d = webgpuOf(device);
    d->lost = true;
    if (d->page != 0)
        prGpuWebgpuSimulateLoss(d->page);
}

bool isComplete(const PrGpuDevice* device, PrGpuFence fence) {
    const WebgpuDevice* d = webgpuOf(device);
    return d->lost || d->reached >= fence;
}

// The page's thread reaches fences only between its tasks. A program that suspends waits for the page to reach the
// fence, and any other returns at once.
void wait(PrGpuDevice* device, PrGpuFence fence) {
    WebgpuDevice* d = webgpuOf(device);
    while (d->waits && !d->lost && d->reached < fence)
        prGpuWebgpuWait(d->page);
}

void waitIdle(PrGpuDevice* device) {
    wait(device, webgpuOf(device)->submitted);
}

// A shader samples a texture with a filter, so a format of 32-bit floats is sampled only where the device filters it.
bool supportsFormat(const PrGpuDevice* device, PrGpuFormat format, PrGpuTextureUsageFlags usage) {
    WebgpuFeatures features = webgpuOf(device)->webgpuFeatures;
    const PrGpuTextureUsageFlags written = PrGpuTextureUsageFlag_DepthStencil | PrGpuTextureUsageFlag_Storage;
    // The page has a family of blocks as a feature, and no format for ASTC's HDR profile.
    constexpr WebgpuFeatures kFamilyFeatures[PrGpuFormatFamily_Count] = {
        0, WebgpuFeature_CompressionBc, WebgpuFeature_CompressionEtc2, WebgpuFeature_CompressionAstc, 0,
    };
    if (webgpuFormatOf(format) == WebgpuFormat_None)
        return false;
    if (prGpuFormatIsCompressed(format))
        return usage == PrGpuTextureUsageFlag_Sampled && (features & kFamilyFeatures[prGpuFormatFamily(format)]) != 0;
    switch (format) {
        case PrGpuFormat_Rg11B10F:
            return (usage & written) == 0 &&
                ((usage & PrGpuTextureUsageFlag_RenderTarget) == 0 || (features & WebgpuFeature_Rg11B10Renderable) != 0);
        case PrGpuFormat_R32F:
        case PrGpuFormat_Rg32F:
        case PrGpuFormat_Rgba32F:
            return (usage & PrGpuTextureUsageFlag_DepthStencil) == 0 &&
                ((usage & PrGpuTextureUsageFlag_Sampled) == 0 || (features & WebgpuFeature_Float32Filterable) != 0);
        case PrGpuFormat_D16:
        case PrGpuFormat_D32F:
        case PrGpuFormat_D32FS8:
            return (usage & (PrGpuTextureUsageFlag_RenderTarget | PrGpuTextureUsageFlag_Storage)) == 0;
        // The formats of 16-bit unsigned values are a feature of the page's that the device does not ask for.
        case PrGpuFormat_R16:
        case PrGpuFormat_Rg16:
            return false;
        default:
            return (usage & PrGpuTextureUsageFlag_DepthStencil) == 0;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Buffers

// The memory of a buffer as the page's side takes it: PrGpuMemory, or a stream block's, whose used part each submission
// copies.
constexpr uint32_t kStreamMemory = 3;

// Creates a buffer, which is a stream block if `stream` is set.
PrStatus createBuffer(WebgpuDevice* d, const PrGpuBufferDesc* desc, bool stream, PrGpuBuffer* out) {
    WebgpuResource resource{.kind = WebgpuKind_Buffer, .stream = stream, .buffer = *desc};
    resource.buffer.name = PrStringView{};
    // WebGPU copies whole 4-byte words, and the copy in the program covers them.
    uint64_t size = alignUp(desc->size, 16);
    if (desc->memory != PrGpuMemory_Device)
        resource.shadow = static_cast<uint8_t*>(prMemoryAllocZeroed(size_t(size)));
    const uint32_t id = addResource(d, resource);
    if (id == 0 || !prGpuWebgpuBufferCreate(d->page, id, size, stream ? kStreamMemory : desc->memory, resource.shadow)) {
        prMemoryFree(resource.shadow);
        if (id != 0) {
            d->resources[id] = WebgpuResource{};
            d->resources.free(id);
        }
        return PrStatus_Limit;
    }
    out->id = id;
    return PrStatus_Ok;
}

PrStatus bufferCreate(PrGpuDevice* device, const PrGpuBufferDesc* desc, PrGpuBuffer* out) {
    return createBuffer(webgpuOf(device), desc, false, out);
}

void bufferRetain(PrGpuDevice* device, PrGpuBuffer buffer) {
    webgpuOf(device)->resources.retain(buffer.id);
}

void bufferRelease(PrGpuDevice* device, PrGpuBuffer buffer) {
    if (!webgpuOf(device)->resources.release(buffer.id))
        return;
    retire(webgpuOf(device), buffer.id);
}

PrGpuBufferDesc bufferDesc(const PrGpuDevice* device, PrGpuBuffer buffer) {
    return webgpuOf(device)->resources[buffer.id].buffer;
}

// WebGPU has no bindless, so no handle reaches a buffer.
uint32_t bufferStorageHandle(const PrGpuDevice*, PrGpuBuffer buffer) {
    return buffer.id;
}

void* bufferMap(const PrGpuDevice* device, PrGpuBuffer buffer) {
    uint8_t* shadow = webgpuOf(device)->resources[buffer.id].shadow;
    PR_ASSERT(shadow != nullptr);
    return shadow;
}

void bufferUpload(PrGpuDevice* device, PrGpuBuffer buffer, uint64_t offset, const void* data, uint64_t size) {
    WebgpuDevice* d = webgpuOf(device);
    WebgpuResource& r = d->resources[buffer.id];
    if (r.shadow != nullptr)
        prMemCopy(r.shadow + offset, data, size_t(size));
    // WebGPU writes whole words, so a write of any other size to a buffer with a copy in the program copies the words
    // that it covers, and the page merges the bytes of any other buffer into its words.
    uint64_t first = offset & ~uint64_t(3);
    uint64_t end = alignUp(offset + size, 4);
    if ((first == offset && end == offset + size) || r.shadow == nullptr) {
        prGpuWebgpuBufferWrite(d->page, buffer.id, offset, data, size);
        return;
    }
    uint8_t* words = static_cast<uint8_t*>(prMemoryAllocZeroed(size_t(end - first)));
    if (r.shadow != nullptr)
        prMemCopy(words, r.shadow + first, size_t(end - first));
    prMemCopy(words + (offset - first), data, size_t(size));
    prGpuWebgpuBufferWrite(d->page, buffer.id, first, words, end - first);
    prMemoryFree(words);
}

// ---------------------------------------------------------------------------------------------------------------------
// Textures and samplers

PrStatus textureCreate(PrGpuDevice* device, const PrGpuTextureDesc* desc, PrGpuTexture* out) {
    WebgpuDevice* d = webgpuOf(device);
    WebgpuResource resource{.kind = WebgpuKind_Texture, .texture = *desc};
    resource.texture.name = PrStringView{};
    const uint32_t id = addResource(d, resource);
    if (id == 0)
        return PrStatus_Limit;
    if (!prGpuWebgpuTextureCreate(d->page, id, desc->kind, webgpuFormatOf(desc->format), desc->usage, desc->levels,
            desc->samples, desc->extent.width, desc->extent.height, desc->extent.depth)) {
        d->resources[id] = WebgpuResource{};
        d->resources.free(id);
        return PrStatus_NotSupported;
    }
    out->id = id;
    return PrStatus_Ok;
}

PrStatus textureCreateView(PrGpuDevice* device, PrGpuTexture texture, PrGpuTextureKind kind,
        PrGpuSubresources subresources, PrGpuTexture* out) {
    WebgpuDevice* d = webgpuOf(device);
    // A view reports what it covers: its levels from the first, each level's extent, and its layers. Zero levels or
    // layers cover every one from the first.
    PrGpuTextureDesc desc = d->resources[texture.id].texture;
    uint32_t levels = subresources.levels != 0 ? subresources.levels : desc.levels - subresources.firstLevel;
    uint32_t layers = subresources.layers != 0 ? subresources.layers : desc.extent.depth - subresources.firstLayer;
    if (desc.kind == PrGpuTextureKind_3d)
        layers = 1;
    desc.kind = kind;
    desc.levels = uint8_t(levels);
    desc.extent.width = max(desc.extent.width >> subresources.firstLevel, 1u);
    desc.extent.height = max(desc.extent.height >> subresources.firstLevel, 1u);
    desc.extent.depth = kind == PrGpuTextureKind_3d ? max(desc.extent.depth >> subresources.firstLevel, 1u) : layers;
    WebgpuResource resource{.kind = WebgpuKind_Texture, .texture = desc};
    const uint32_t id = addResource(d, resource);
    if (id == 0)
        return PrStatus_Limit;
    if (!prGpuWebgpuTextureCreateView(d->page, id, texture.id, kind, webgpuFormatOf(desc.format),
            subresources.firstLevel, levels, subresources.firstLayer, layers)) {
        d->resources[id] = WebgpuResource{};
        d->resources.free(id);
        return PrStatus_NotSupported;
    }
    out->id = id;
    return PrStatus_Ok;
}

void textureRetain(PrGpuDevice* device, PrGpuTexture texture) {
    webgpuOf(device)->resources.retain(texture.id);
}

void textureRelease(PrGpuDevice* device, PrGpuTexture texture) {
    if (!webgpuOf(device)->resources.release(texture.id))
        return;
    gpuSharedTextureForget(webgpuOf(device), texture);
    retire(webgpuOf(device), texture.id);
}

PrGpuTextureDesc textureDesc(const PrGpuDevice* device, PrGpuTexture texture) {
    return webgpuOf(device)->resources[texture.id].texture;
}

uint32_t textureStorageHandle(const PrGpuDevice*, PrGpuTexture texture) {
    return texture.id;
}

// Returns the rows of blocks from one layer of a copy's bytes to the next, which WebGPU names in rows: as many as
// `layerBytes` holds, or the region's own rows where layers follow one another.
uint32_t rowsPerLayer(PrGpuFormat format, const PrGpuTextureRegion& region, uint32_t rowBytes, uint32_t layerBytes) {
    uint32_t blockHeight = prGpuFormatBlock(format).height;
    uint32_t rows = (region.extent.height + blockHeight - 1) / blockHeight;
    return layerBytes != 0 && rowBytes != 0 ? layerBytes / rowBytes : rows;
}

void textureUpload(PrGpuDevice* device, PrGpuTexture texture, const PrGpuTextureRegion* region, const void* data,
        uint32_t rowBytes, uint32_t layerBytes) {
    WebgpuDevice* d = webgpuOf(device);
    PrGpuFormat format = d->resources[texture.id].texture.format;
    uint32_t rows = rowsPerLayer(format, *region, rowBytes, layerBytes);
    uint32_t blockHeight = prGpuFormatBlock(format).height;
    uint32_t lastRows = (region->extent.height + blockHeight - 1) / blockHeight;
    // The bytes reach the last row of the last layer, which need not run a whole `rowBytes`.
    uint32_t lastRow = prGpuFormatBlock(format).bytes *
        ((region->extent.width + prGpuFormatBlock(format).width - 1) / prGpuFormatBlock(format).width);
    uint64_t size = uint64_t(rowBytes) * rows * (region->extent.depth - 1) + uint64_t(rowBytes) * (lastRows - 1) + lastRow;
    PrGpuFormatBlock block = prGpuFormatBlock(format);
    prGpuWebgpuTextureWrite(d->page, texture.id, region->level, region->x, region->y, region->z + region->firstLayer,
        alignUp(region->extent.width, uint32_t(block.width)), alignUp(region->extent.height, uint32_t(block.height)),
        region->extent.depth, data, rowBytes, rows, size);
}

PrStatus samplerCreate(PrGpuDevice* device, const PrGpuSamplerDesc* desc, PrGpuSampler* out) {
    WebgpuDevice* d = webgpuOf(device);
    uint32_t minLod = 0;
    uint32_t maxLod = 0;
    prMemCopy(&minLod, &desc->minLod, sizeof(minLod));
    prMemCopy(&maxLod, &desc->maxLod, sizeof(maxLod));
    const uint32_t words[] = {
        desc->magnify, desc->minify, desc->mip, desc->address[0], desc->address[1], desc->address[2],
        desc->maxAnisotropy, desc->compare, minLod, maxLod,
    };
    const uint32_t id = d->samplers.take();
    if (id == 0)
        return PrStatus_Limit;
    if (!prGpuWebgpuSamplerCreate(d->page, id, words)) {
        d->samplers.free(id);
        return PrStatus_Limit;
    }
    d->samplers[id] = *desc;
    out->id = id;
    return PrStatus_Ok;
}

// ---------------------------------------------------------------------------------------------------------------------
// Shaders and pipelines

void shaderLibraryDestroy(PrGpuShaderLibrary* library) {
    GpuSharedLibrary* l = static_cast<GpuSharedLibrary*>(library);
    prMemoryFree(l->entries);
    prMemoryFree(l);
}

constexpr PrGpuShaderLibrary kLibraryFunctions = {
    .destroyFn = shaderLibraryDestroy,
    .entriesFn = gpuSharedLibraryEntries,
};

// The page compiles each entry point's WGSL as a pipeline needs it, so the library holds only its entry points.
PrStatus shaderLibraryCreate(PrGpuDevice* device, PrGpuShaderEntry* entries, uint32_t entryCount,
        PrGpuShaderLibrary* PR_NULLABLE* PR_NONNULL out) {
    GpuSharedLibrary* library = allocZeroed<GpuSharedLibrary>();
    static_cast<PrGpuShaderLibrary&>(*library) = kLibraryFunctions;
    library->device = device;
    library->entryCount = entryCount;
    library->entries = entries;
    *out = library;
    return PrStatus_Ok;
}

// Returns a new pipeline id, or zero if the device has none left.
uint32_t addPipeline(WebgpuDevice* d, const WebgpuPipeline& pipeline) {
    const uint32_t id = d->pipelines.take();
    if (id != 0)
        d->pipelines[id] = pipeline;
    return id;
}

void pushEntry(RawArray<uint32_t>& words, const PrGpuShaderEntry* PR_NULLABLE entry) {
    words.push(entry != nullptr ? uint32_t(uintptr_t(entry->code)) : 0);
    words.push(entry != nullptr ? uint32_t(entry->codeSize) : 0);
    words.push(entry != nullptr ? uint32_t(uintptr_t(entry->name.data)) : 0);
    words.push(entry != nullptr ? uint32_t(entry->name.size) : 0);
}

void pushFloat(RawArray<uint32_t>& words, float value) {
    uint32_t bits = 0;
    prMemCopy(&bits, &value, sizeof(bits));
    words.push(bits);
}

// Creates the page's pipeline from words that gpu_webgpu.js's pipelineCreate decodes. Returns NotSupported if the page
// cannot make it, as for shaders that reach resources by handle.
PrStatus pipelineCreate(WebgpuDevice* d, const RawArray<uint32_t>& words, const WebgpuPipeline& pipeline, PrGpuPipeline* out) {
    const uint32_t id = addPipeline(d, pipeline);
    if (id == 0)
        return PrStatus_Limit;
    if (!prGpuWebgpuPipelineCreate(d->page, id, words.data, words.count)) {
        d->pipelines[id] = WebgpuPipeline{};
        d->pipelines.free(id);
        return PrStatus_NotSupported;
    }
    out->id = id;
    return PrStatus_Ok;
}

PrStatus graphicsPipelineCreate(PrGpuDevice* device, const PrGpuGraphicsPipelineDesc* desc,
        const PrGpuShaderEntry* vertex, const PrGpuShaderEntry* PR_NULLABLE fragment, PrGpuPipeline* out) {
    RawArray<uint32_t> words;
    words.push(0);
    pushEntry(words, vertex);
    pushEntry(words, fragment);
    words.push(desc->topology);
    words.push(desc->cull);
    words.push(desc->frontClockwise);
    words.push(desc->depthTest);
    words.push(desc->depthWrite);
    words.push(desc->depthCompare);
    pushFloat(words, desc->depthBias);
    pushFloat(words, desc->depthBiasSlope);
    pushFloat(words, desc->depthBiasClamp);
    words.push(desc->stencilTest);
    words.push(desc->stencilReadMask);
    words.push(desc->stencilWriteMask);
    const PrGpuStencilFace faces[] = {desc->stencilFront, desc->stencilBack};
    for (const PrGpuStencilFace& face : faces) {
        words.push(face.fail);
        words.push(face.depthFail);
        words.push(face.pass);
        words.push(face.compare);
    }
    words.push(desc->formats.colorCount);
    for (uint32_t i = 0; i < desc->formats.colorCount; ++i) {
        const PrGpuBlend& b = desc->blend[i];
        words.push(webgpuFormatOf(desc->formats.color[i]));
        words.push(b.enabled);
        words.push(b.colorSource);
        words.push(b.colorTarget);
        words.push(b.colorOp);
        words.push(b.alphaSource);
        words.push(b.alphaTarget);
        words.push(b.alphaOp);
        words.push(b.write != 0 ? b.write : PrGpuColorWriteFlag_All);
    }
    words.push(webgpuFormatOf(desc->formats.depthStencil));
    words.push(desc->formats.samples);
    const PrGpuVertexLayout& layout = desc->vertexLayout;
    words.push(layout.bufferCount);
    for (uint32_t i = 0; i < layout.bufferCount; ++i) {
        words.push(layout.buffers[i].stride);
        words.push(layout.buffers[i].step);
    }
    words.push(layout.attributeCount);
    for (uint32_t i = 0; i < layout.attributeCount; ++i) {
        words.push(layout.attributes[i].format);
        words.push(layout.attributes[i].buffer);
        words.push(layout.attributes[i].offset);
    }
    PrStatus status = pipelineCreate(webgpuOf(device), words, WebgpuPipeline{.used = true}, out);
    words.reset();
    return status;
}

PrStatus computePipelineCreate(PrGpuDevice* device, const PrGpuShaderLibrary*, const PrGpuShaderEntry* compute,
        PrStringView, PrGpuPipeline* out) {
    RawArray<uint32_t> words;
    words.push(1);
    pushEntry(words, compute);
    PrStatus status = pipelineCreate(webgpuOf(device), words,
        WebgpuPipeline{.used = true, .compute = true, .groupSize = compute->groupSize}, out);
    words.reset();
    return status;
}

// Work recorded before the release may still use the pipeline, so its id is retired as a resource's is.
void pipelineRetain(PrGpuDevice* device, PrGpuPipeline pipeline) {
    webgpuOf(device)->pipelines.retain(pipeline.id);
}

void pipelineRelease(PrGpuDevice* device, PrGpuPipeline pipeline) {
    if (!webgpuOf(device)->pipelines.release(pipeline.id))
        return;
    retire(webgpuOf(device), pipeline.id, true);
}

PrGpuExtent pipelineGroupSize(const PrGpuDevice* device, PrGpuPipeline pipeline) {
    return webgpuOf(device)->pipelines[pipeline.id].groupSize;
}

// ---------------------------------------------------------------------------------------------------------------------
// Command buffers

extern const PrGpuCommands kCommandsFunctions;
extern const PrGpuPass kPassFunctions;

bool commandsBegin(PrGpuDevice* device, const PrGpuCommandsOptions* options, PrGpuCommands* PR_NULLABLE* PR_NONNULL out) {
    PR_ASSERT(options->native == nullptr);
    (void)options;
    WebgpuCommands* c = allocZeroed<WebgpuCommands>();
    static_cast<PrGpuCommands&>(*c) = kCommandsFunctions;
    c->device = device;
    ++webgpuOf(device)->open;
    *out = c;
    return true;
}

// Calls the callbacks of the fences that the device reached, in order, and recycles what the work held. The callbacks
// of `fence` itself take `seconds`, the time that its work took on the GPU.
void reach(WebgpuDevice* d, PrGpuFence fence, double seconds) {
    if (fence > d->reached)
        d->reached = fence;
    uint32_t kept = 0;
    for (uint32_t i = 0; i < d->spentBlocks.count; ++i) {
        uint32_t spent = d->spentBlocks.data[i].buffer;
        if (d->spentFences.data[i] > d->reached) {
            d->spentBlocks.data[kept] = d->spentBlocks.data[i];
            d->spentFences.data[kept] = d->spentFences.data[i];
            ++kept;
        } else if (d->resources[spent].buffer.size == kBlockSize) {
            d->freeBlocks.push(spent);
        } else {
            retire(d, spent);
        }
    }
    d->spentBlocks.count = kept;
    d->spentFences.count = kept;
    uint32_t ran = 0;
    while (ran < d->dones.count && d->dones.data[ran].fence <= d->reached) {
        WebgpuDone done = d->dones.data[ran++];
        done.done(done.context, done.fence == fence ? seconds : 0);
    }
    if (ran != 0) {
        prMemMove(d->dones.data, d->dones.data + ran, sizeof(WebgpuDone) * (d->dones.count - ran));
        d->dones.count -= ran;
    }
    collect(d);
}

PrGpuFence commandsSubmit(PrGpuCommands* commands) {
    WebgpuCommands* c = commandsOf(commands);
    WebgpuDevice* d = deviceOfCommands(commands);
    gpuSharedSubmitted(c);
    PrGpuFence fence = ++d->submitted;
    RawArray<uint32_t> streams;
    for (uint32_t i = 0; i < c->blocks.count; ++i) {
        streams.push(c->blocks.data[i].buffer);
        streams.push(uint32_t(c->blocks.data[i].used));
    }
    if (!d->lost)
        prGpuWebgpuSubmit(d->page, c->words.data, c->words.count, streams.data, c->blocks.count, fence);
    streams.reset();
    for (uint32_t i = 0; i < c->blocks.count; ++i) {
        d->spentBlocks.push(c->blocks.data[i]);
        d->spentFences.push(fence);
    }
    for (uint32_t i = 0; i < c->dones.count; ++i) {
        WebgpuDone done = c->dones.data[i];
        done.fence = fence;
        d->dones.push(done);
    }
    --d->open;
    c->words.reset();
    c->blocks.reset();
    c->dones.reset();
    for (uint32_t i = 0; i < c->scratch.count; ++i)
        prMemoryFree(c->scratch.data[i]);
    c->scratch.reset();
    prMemoryFree(c);
    // A lost device reaches every fence at once.
    if (d->lost)
        reach(d, fence, 0);
    return fence;
}

// The alignment of root data in the stream, which WebGPU requires of a uniform buffer's dynamic offset.
constexpr uint64_t kRootAlignment = 256;

// Allocates `size` bytes of the stream at `alignment`, in a block that the command buffer holds.
void* stream(PrGpuCommands* commands, uint64_t size, uint64_t alignment, PrGpuBuffer* buffer, uint64_t* offset) {
    WebgpuCommands* c = commandsOf(commands);
    WebgpuDevice* d = deviceOfCommands(commands);
    WebgpuBlock* block = c->blocks.count != 0 ? &c->blocks.last() : nullptr;
    uint64_t at = block != nullptr ? alignUp(block->used, alignment) : 0;
    if (block == nullptr || at + size > d->resources[block->buffer].buffer.size) {
        uint32_t id = 0;
        if (size <= kBlockSize && d->freeBlocks.count != 0) {
            id = d->freeBlocks.last();
            d->freeBlocks.pop();
        } else {
            PrGpuBufferDesc desc{
                .size = size > kBlockSize ? alignUp(size, 256) : kBlockSize,
                .usage = PrGpuBufferUsageFlag_All,
                .memory = PrGpuMemory_Upload,
            };
            PrGpuBuffer made{};
            if (createBuffer(d, &desc, true, &made) != PrStatus_Ok) {
                // Out of memory loses the device, and the caller writes into memory that the GPU never reads.
                d->lost = true;
                void* scratch = prMemoryAlloc(size_t(size));
                c->scratch.push(scratch);
                buffer->id = 0;
                *offset = 0;
                return scratch;
            }
            id = made.id;
        }
        c->blocks.push(WebgpuBlock{id, 0});
        block = &c->blocks.last();
        at = 0;
    }
    block->used = at + size;
    buffer->id = block->buffer;
    *offset = at;
    return d->resources[block->buffer].shadow + at;
}

void* commandsStream(PrGpuCommands* commands, uint64_t size, PrGpuBufferUsageFlags usage, PrGpuBuffer* buffer, uint64_t* offset) {
    return stream(commands, size, gpuSharedStreamAlignment(deviceOfCommands(commands), usage), buffer, offset);
}

void commandsSetPush(PrGpuCommands* commands, const void* data, uint32_t size) {
    WebgpuCommands* c = commandsOf(commands);
    PrGpuBuffer buffer{};
    uint64_t offset = 0;
    uint8_t* bytes = static_cast<uint8_t*>(stream(commands, kPrGpuMaxPushBytes, kRootAlignment, &buffer, &offset));
    prMemCopy(bytes, data, size);
    prMemZero(bytes + size, kPrGpuMaxPushBytes - size);
    emit(c, WebgpuOp_Push);
    emit(c, buffer.id);
    emit(c, uint32_t(offset));
}

void commandsSetUniforms(PrGpuCommands* commands, const void* data, uint32_t size) {
    WebgpuCommands* c = commandsOf(commands);
    uint32_t bound = alignUp(size, 16u);
    PrGpuBuffer buffer{};
    uint64_t offset = 0;
    uint8_t* bytes = static_cast<uint8_t*>(stream(commands, bound, kRootAlignment, &buffer, &offset));
    prMemCopy(bytes, data, size);
    prMemZero(bytes + size, bound - size);
    emit(c, WebgpuOp_Uniforms);
    emit(c, buffer.id);
    emit(c, uint32_t(offset));
    emit(c, bound);
}

void commandsSetPipeline(PrGpuCommands* commands, PrGpuPipeline pipeline) {
    WebgpuCommands* c = commandsOf(commands);
    emit(c, WebgpuOp_Pipeline);
    emit(c, pipeline.id);
}

void setBufferSlot(PrGpuCommands* commands, WebgpuOp op, uint32_t slot, PrGpuBuffer buffer, uint64_t offset) {
    WebgpuCommands* c = commandsOf(commands);
    emit(c, op);
    emit(c, slot);
    emit(c, buffer.id);
    emit(c, uint32_t(offset));
}

void commandsSetBuffer(PrGpuCommands* commands, uint32_t slot, PrGpuBuffer buffer, uint64_t offset) {
    setBufferSlot(commands, WebgpuOp_Buffer, slot, buffer, offset);
}

void commandsSetRwBuffer(PrGpuCommands* commands, uint32_t slot, PrGpuBuffer buffer, uint64_t offset) {
    setBufferSlot(commands, WebgpuOp_RwBuffer, slot, buffer, offset);
}

void commandsSetTexture(PrGpuCommands* commands, uint32_t slot, PrGpuTexture texture, PrGpuSampler sampler) {
    WebgpuCommands* c = commandsOf(commands);
    emit(c, WebgpuOp_Texture);
    emit(c, slot);
    emit(c, texture.id);
    emit(c, sampler.id);
}

void commandsSetRwTexture(PrGpuCommands* commands, uint32_t slot, PrGpuTexture texture) {
    WebgpuCommands* c = commandsOf(commands);
    emit(c, WebgpuOp_RwTexture);
    emit(c, slot);
    emit(c, texture.id);
}

void commandsDispatch(PrGpuCommands* commands, PrGpuExtent groups) {
    gpuSharedCheckOutsidePass(commands);
    WebgpuCommands* c = commandsOf(commands);
    emit(c, WebgpuOp_Dispatch);
    emit(c, groups.width);
    emit(c, groups.height);
    emit(c, groups.depth);
}

void commandsDispatchIndirect(PrGpuCommands* commands, PrGpuBuffer buffer, uint64_t offset) {
    gpuSharedCheckOutsidePass(commands);
    WebgpuCommands* c = commandsOf(commands);
    emit(c, WebgpuOp_DispatchIndirect);
    emit(c, buffer.id);
    emit(c, uint32_t(offset));
}

void commandsCopyBuffer(PrGpuCommands* commands, PrGpuBuffer source, uint64_t sourceOffset, PrGpuBuffer target,
        uint64_t targetOffset, uint64_t size) {
    gpuSharedCheckOutsidePass(commands);
    WebgpuCommands* c = commandsOf(commands);
    emit(c, WebgpuOp_CopyBuffer);
    emit(c, source.id);
    emit64(c, sourceOffset);
    emit(c, target.id);
    emit64(c, targetOffset);
    emit64(c, size);
}

void commandsCopyTexture(PrGpuCommands* commands, PrGpuTexture source, const PrGpuTextureRegion* sourceRegion,
        PrGpuTexture target, const PrGpuTextureRegion* targetRegion) {
    gpuSharedCheckOutsidePass(commands);
    WebgpuCommands* c = commandsOf(commands);
    const GpuSharedTable<WebgpuResource>& resources = deviceOfCommands(commands)->resources;
    emit(c, WebgpuOp_CopyTexture);
    emit(c, source.id);
    emitRegion(c, *sourceRegion, resources[source.id].texture.format);
    emit(c, target.id);
    emitRegion(c, *targetRegion, resources[target.id].texture.format);
}

void commandsCopyBufferToTexture(PrGpuCommands* commands, PrGpuBuffer source, uint64_t sourceOffset, uint32_t rowBytes,
        uint32_t layerBytes, PrGpuTexture target, const PrGpuTextureRegion* targetRegion) {
    gpuSharedCheckOutsidePass(commands);
    WebgpuCommands* c = commandsOf(commands);
    emit(c, WebgpuOp_CopyBufferToTexture);
    emit(c, source.id);
    emit64(c, sourceOffset);
    emit(c, rowBytes);
    PrGpuFormat format = deviceOfCommands(commands)->resources[target.id].texture.format;
    emit(c, rowsPerLayer(format, *targetRegion, rowBytes, layerBytes));
    emit(c, prGpuFormatBlock(format).height);
    emit(c, target.id);
    emitRegion(c, *targetRegion, format);
}

void commandsCopyTextureToBuffer(PrGpuCommands* commands, PrGpuTexture source, const PrGpuTextureRegion* sourceRegion,
        PrGpuBuffer target, uint64_t targetOffset, uint32_t rowBytes, uint32_t layerBytes) {
    gpuSharedCheckOutsidePass(commands);
    WebgpuCommands* c = commandsOf(commands);
    emit(c, WebgpuOp_CopyTextureToBuffer);
    PrGpuFormat format = deviceOfCommands(commands)->resources[source.id].texture.format;
    emit(c, source.id);
    emitRegion(c, *sourceRegion, format);
    emit(c, target.id);
    emit64(c, targetOffset);
    emit(c, rowBytes);
    emit(c, rowsPerLayer(format, *sourceRegion, rowBytes, layerBytes));
    emit(c, prGpuFormatBlock(format).height);
}

void commandsOnDone(PrGpuCommands* commands, PrGpuDoneFn done, void* PR_NULLABLE context) {
    commandsOf(commands)->dones.push(WebgpuDone{0, done, context});
}

void commandsPushGroup(PrGpuCommands*, PrStringView) {}

void commandsPopGroup(PrGpuCommands*) {}

// WebGPU orders the accesses of its work itself.
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

PrGpuNativeStateFlags nativeStateSync(PrGpuCommands*) {
    return 0;
}

// ---------------------------------------------------------------------------------------------------------------------
// Passes

void emitAttachment(WebgpuCommands* c, const PrGpuAttachment& a) {
    emit(c, a.texture.id);
    emit(c, a.resolve.id);
    emit(c, a.level);
    emit(c, a.layer);
    emit(c, a.load);
    emit(c, a.store);
}

PrGpuPass* commandsPassBegin(PrGpuCommands* commands, const PrGpuPassDesc* desc) {
    WebgpuCommands* c = commandsOf(commands);
    WebgpuDevice* d = deviceOfCommands(commands);
    PR_ASSERT(desc->native == nullptr);
    gpuSharedPassBegan(c);
    WebgpuPass* pass = allocZeroed<WebgpuPass>();
    static_cast<PrGpuPass&>(*pass) = kPassFunctions;
    pass->commands = c;
    pass->formats.colorCount = desc->colorCount;
    pass->formats.samples = 1;
    const PrGpuAttachment* first = desc->colorCount != 0 ? &desc->color[0] : &desc->depthStencil;
    if (first->texture.id != 0) {
        const PrGpuTextureDesc& t = d->resources[first->texture.id].texture;
        pass->extent = PrUVec2{t.extent.width >> first->level, t.extent.height >> first->level};
        pass->formats.samples = t.samples;
    }
    emit(c, WebgpuOp_PassBegin);
    emit(c, desc->colorCount);
    for (uint32_t i = 0; i < desc->colorCount; ++i) {
        pass->formats.color[i] = d->resources[desc->color[i].texture.id].texture.format;
        emitAttachment(c, desc->color[i]);
        emitFloat(c, desc->clearColor[i].r);
        emitFloat(c, desc->clearColor[i].g);
        emitFloat(c, desc->clearColor[i].b);
        emitFloat(c, desc->clearColor[i].a);
    }
    if (desc->depthStencil.texture.id != 0)
        pass->formats.depthStencil = d->resources[desc->depthStencil.texture.id].texture.format;
    emitAttachment(c, desc->depthStencil);
    emit(c, desc->stencilLoad);
    emit(c, desc->stencilStore);
    emitFloat(c, desc->clearDepth);
    emit(c, desc->clearStencil);
    c->pass = pass;
    return pass;
}

WebgpuCommands* passCommands(PrGpuPass* pass) {
    return static_cast<WebgpuCommands*>(static_cast<GpuSharedPass*>(pass)->commands);
}

void passEnd(PrGpuPass* handle) {
    WebgpuCommands* c = passCommands(handle);
    emit(c, WebgpuOp_PassEnd);
    gpuSharedPassEnded(c);
    c->pass = nullptr;
    prMemoryFree(static_cast<WebgpuPass*>(handle));
}

void passSetViewport(PrGpuPass* pass, PrRect viewport, float minDepth, float maxDepth) {
    WebgpuCommands* c = passCommands(pass);
    emit(c, WebgpuOp_Viewport);
    emitFloat(c, viewport.left);
    emitFloat(c, viewport.top);
    emitFloat(c, viewport.right - viewport.left);
    emitFloat(c, viewport.bottom - viewport.top);
    emitFloat(c, minDepth);
    emitFloat(c, maxDepth);
}

void passSetScissor(PrGpuPass* pass, PrIRect scissor) {
    WebgpuCommands* c = passCommands(pass);
    emit(c, WebgpuOp_Scissor);
    emit(c, uint32_t(scissor.left));
    emit(c, uint32_t(scissor.top));
    emit(c, uint32_t(scissor.right - scissor.left));
    emit(c, uint32_t(scissor.bottom - scissor.top));
}

void passSetBlendConstant(PrGpuPass* pass, PrColor constant) {
    WebgpuCommands* c = passCommands(pass);
    emit(c, WebgpuOp_BlendConstant);
    emitFloat(c, constant.r);
    emitFloat(c, constant.g);
    emitFloat(c, constant.b);
    emitFloat(c, constant.a);
}

void passSetStencilReference(PrGpuPass* pass, uint8_t reference) {
    WebgpuCommands* c = passCommands(pass);
    emit(c, WebgpuOp_StencilReference);
    emit(c, reference);
}

void passSetIndexBuffer(PrGpuPass* pass, PrGpuBuffer buffer, uint64_t offset, bool wide) {
    WebgpuCommands* c = passCommands(pass);
    emit(c, WebgpuOp_IndexBuffer);
    emit(c, buffer.id);
    emit(c, uint32_t(offset));
    emit(c, wide);
}

void passSetVertexBuffer(PrGpuPass* pass, uint32_t slot, PrGpuBuffer buffer, uint64_t offset) {
    WebgpuCommands* c = passCommands(pass);
    emit(c, WebgpuOp_VertexBuffer);
    emit(c, slot);
    emit(c, buffer.id);
    emit(c, uint32_t(offset));
}

void passDraw(PrGpuPass* pass, uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex,
        uint32_t firstInstance) {
    WebgpuCommands* c = passCommands(pass);
    emit(c, WebgpuOp_Draw);
    emit(c, vertexCount);
    emit(c, instanceCount);
    emit(c, firstVertex);
    emit(c, firstInstance);
}

void passDrawIndexed(PrGpuPass* pass, uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex,
        int32_t vertexOffset, uint32_t firstInstance) {
    WebgpuCommands* c = passCommands(pass);
    emit(c, WebgpuOp_DrawIndexed);
    emit(c, indexCount);
    emit(c, instanceCount);
    emit(c, firstIndex);
    emit(c, uint32_t(vertexOffset));
    emit(c, firstInstance);
}

void passDrawIndirect(PrGpuPass* pass, PrGpuBuffer buffer, uint64_t offset, uint32_t count, uint32_t stride) {
    WebgpuCommands* c = passCommands(pass);
    emit(c, WebgpuOp_DrawIndirect);
    emit(c, buffer.id);
    emit(c, uint32_t(offset));
    emit(c, count);
    emit(c, stride);
}

void passDrawIndexedIndirect(PrGpuPass* pass, PrGpuBuffer buffer, uint64_t offset, uint32_t count, uint32_t stride) {
    WebgpuCommands* c = passCommands(pass);
    emit(c, WebgpuOp_DrawIndexedIndirect);
    emit(c, buffer.id);
    emit(c, uint32_t(offset));
    emit(c, count);
    emit(c, stride);
}

// ---------------------------------------------------------------------------------------------------------------------
// Swap chains

// A swap chain over a page's canvas, by its selector, which the page configures for the device once the first frame
// gives its extent. The canvas's texture of each frame takes the id `texture`.
struct WebgpuSwapChain : GpuSharedSwapChain {
    PrRawString selector;
    bool transparent;
    uint32_t canvas;            // the page's id of the canvas's context, or zero until the first frame
    PrUVec2 extent;
    uint32_t texture;
};

extern const PrGpuSwapChain kSwapChainFunctions;

PrStatus swapChainCreate(PrGpuDevice* device, const PrAppSurface* surface, const PrGpuSwapChainDesc* desc,
        PrGpuSwapChain* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    if (surface->kind != PrAppSurfaceKind_HtmlCanvas)
        return PrStatus_NotSupported;
    WebgpuSwapChain* s = allocZeroed<WebgpuSwapChain>();
    static_cast<PrGpuSwapChain&>(*s) = kSwapChainFunctions;
    s->device = device;
    s->info = PrGpuSwapChainInfo{.format = PrGpuFormat_Bgra8, .space = PrColorSpace_Srgb};
    s->transparent = (desc->flags & PrGpuSwapChainFlag_Transparent) != 0;
    s->selector = prRawStringCopy(static_cast<const char*>(surface->handle));
    *out = s;
    return PrStatus_Ok;
}

void swapChainDestroy(PrGpuSwapChain* swapChain) {
    WebgpuSwapChain* s = static_cast<WebgpuSwapChain*>(swapChain);
    WebgpuDevice* d = webgpuOf(s->device);
    if (s->texture != 0)
        retire(d, s->texture);
    if (s->canvas != 0)
        prGpuWebgpuCanvasRelease(d->page, s->canvas);
    prRawStringReset(&s->selector);
    prMemoryFree(s);
}

// The page paces its frames with its animation frames, so the swap chain has nothing to wait for.
void swapChainWait(PrGpuSwapChain*) {}

PrStatus swapChainAcquire(PrGpuSwapChain* swapChain, PrUVec2 extent, PrGpuTexture* out) {
    WebgpuSwapChain* s = static_cast<WebgpuSwapChain*>(swapChain);
    WebgpuDevice* d = webgpuOf(s->device);
    if (d->lost)
        return PrStatus_Lost;
    if (s->canvas == 0) {
        PrStringView selector = prRawStringView(&s->selector);
        WebgpuFormat format = WebgpuFormat_Bgra8Unorm;
        s->canvas = prGpuWebgpuCanvasAttach(d->page, selector.data, selector.size, extent.x, extent.y, s->transparent, &format);
        if (s->canvas == 0)
            return PrStatus_Lost;
        s->info.format = formatOfCanvas(format);
    }
    // The canvas's texture of each frame is new, so its id names it anew.
    if (s->texture == 0) {
        s->texture = addResource(d, WebgpuResource{.kind = WebgpuKind_Texture});
        if (s->texture == 0)
            return PrStatus_Limit;
    }
    d->resources[s->texture].texture = prGpuTextureDesc2d(s->info.format, extent, PrGpuTextureUsageFlag_RenderTarget);
    prGpuWebgpuCanvasAcquire(d->page, s->canvas, s->texture, extent.x, extent.y);
    s->extent = extent;
    out->id = s->texture;
    return PrStatus_Ok;
}

// The page shows what the frame drew once the task that submitted it ends.
void swapChainPresent(PrGpuSwapChain*, PrGpuCommands*) {}

// ---------------------------------------------------------------------------------------------------------------------
// The functions

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
    .passBeginFn = commandsPassBegin,
};

constexpr PrGpuPass kPassFunctions = {
    .commandsFn = gpuSharedPassCommands,
    .formatsFn = gpuSharedPassFormats,
    .extentFn = gpuSharedPassExtent,
    .endFn = passEnd,
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

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrGpuDevice* prGpuWebgpuCreate(const PrGpuDeviceOptions* PR_NULLABLE options) {
    PrGpuDeviceOptions o = options != nullptr ? *options : prGpuDeviceOptionsDefault();
    WebgpuDevice* d = allocZeroed<WebgpuDevice>();
    gpuSharedDeviceInit(d);
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
    d->shaderTarget = PrGpuShaderTarget_Wgsl;
    d->info.backend = "WebGPU";
    d->info.adapter = "WebGPU";
    d->info.maxBufferSlots = 4;
    d->info.maxRwBufferSlots = 4;
    d->info.maxTextureSlots = 8;
    d->info.maxRwTextureSlots = 4;
    d->info.sampleCounts = 1 | 4;   // WebGPU has textures of 1 and of 4 samples
    const PrGpuDeviceLimits limits = gpuSharedDeviceLimits(o.limits);
    d->info.limits = limits;
    d->resources.init(1, limits.resources);
    d->samplers.init(1, limits.samplers);
    d->pipelines.init(1, limits.pipelines);
    // A program that suspends makes the device as the other backends do, as a job of the pool or during the call.
    // Elsewhere, the page's event loop finishes the creation, and a page without WebGPU fails it at once.
    d->waits = prGpuWebgpuWaits();
    if (d->waits) {
        d->initialize = initialize;
        gpuSharedDeviceStart(d, o.pool, o.ready, o.readyContext);
    } else {
        gpuSharedDeviceStartLater(d, o.ready, o.readyContext);
        if (!prGpuWebgpuRequest(d, false))
            gpuSharedDeviceFinish(d, PrStatus_NotSupported);
    }
    return d;
}

uint32_t prGpuWebgpuValidationErrors(const PrGpuDevice* device) {
    return webgpuOf(device)->errors;
}

EMSCRIPTEN_KEEPALIVE void prGpuWebgpuCreated(void* device, uint32_t page, uint32_t maxTextureSize,
        uint32_t storageOffsetAlignment, uint32_t features) {
    WebgpuDevice* d = static_cast<WebgpuDevice*>(device);
    d->page = page;
    if (page != 0) {
        d->info.maxTextureSize = maxTextureSize;
        d->info.storageOffsetAlignment = storageOffsetAlignment > 16 ? storageOffsetAlignment : 16;
    }
    d->webgpuFeatures = features;
    if ((features & WebgpuFeature_Timestamps) != 0)
        d->info.features |= PrGpuFeatureFlag_Timestamps;
    if ((features & WebgpuFeature_CompressionBc) != 0)
        d->info.features |= PrGpuFeatureFlag_TextureCompressionBc;
    if ((features & WebgpuFeature_CompressionAstc) != 0)
        d->info.features |= PrGpuFeatureFlag_TextureCompressionAstc;
    if ((features & WebgpuFeature_CompressionEtc2) != 0)
        d->info.features |= PrGpuFeatureFlag_TextureCompressionEtc2;
    if (!d->waits)
        gpuSharedDeviceFinish(d, page != 0 ? PrStatus_Ok : PrStatus_NotSupported);
}

EMSCRIPTEN_KEEPALIVE void prGpuWebgpuReached(void* device, double fence, double seconds) {
    reach(static_cast<WebgpuDevice*>(device), PrGpuFence(fence), seconds);
}

EMSCRIPTEN_KEEPALIVE void prGpuWebgpuLost(void* device) {
    WebgpuDevice* d = static_cast<WebgpuDevice*>(device);
    d->lost = true;
    reach(d, d->submitted, 0);
}

EMSCRIPTEN_KEEPALIVE void prGpuWebgpuError(void* device) {
    ++static_cast<WebgpuDevice*>(device)->errors;
}
