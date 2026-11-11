// GPU devices: resources that shaders reach by handle, pipelines, command buffers and passes, swap chains, and the
// fences that their work completes at. Every renderer is written against this interface, and its backends drive Metal,
// Vulkan and Direct3D 12.
//
// Every buffer, texture and sampler has a handle that is its slot in the device's heap. A shader reads a handle from
// any data that it can read, including another resource, and turns it into the resource (pr/gpu.slang). Creating a
// resource makes it visible to shaders.
//
// Every object may be destroyed as soon as the work that uses it is recorded. The device keeps what the object holds
// until the work submitted before the destruction completes. So a renderer may create a texture, use it and destroy it
// within one frame.
//
// Several threads may use a device at once, each recording its own command buffers. A command buffer is used by one
// thread at a time. Creating and destroying objects, uploads and submissions are thread-safe.
//
// The device tracks no resource states. A barrier names a resource's accesses before and after it, and the caller knows
// them.
//
// A device can be lost: the system takes the GPU away, as when its driver resets after a hang. A lost device never
// comes back. Every call on it and its objects stays safe. The calls that make something new fail with Lost, recorded
// work does nothing, and readbacks see zeros. The application recovers by destroying every object of the device, then
// the device, and making them again on a new device.

#pragma once

#include "pr/base.h"
#include "pr/color.h"
#include "pr/geom.h"
#include "pr/pixels.h"
#include "pr/raw_string.h"
#include "pr/status.h"
#include "pr/string_view.h"
#include "pr/surface.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Devices

// A GPU, created over the application's native device or over one that the backend creates.
typedef struct PrGpuDevice PrGpuDevice;

// A point that a device reaches once the work that returned it is done. Zero is reached on every device.
typedef uint64_t PrGpuFence;

// The library's device backends, which the build turns on (README.md), and Default. Default is the backend that the
// environment variable PR_GPU_BACKEND names (prGpuBackendFromName), if the build has it. Otherwise it is the first
// backend in the build that the system supports, trying the platform's own first and the memory backend last. So a
// program and its tests run on any backend in the build, and a portable program runs on any build that has the memory
// backend.
enum {
    PrGpuBackend_Default,
    PrGpuBackend_Metal,
    PrGpuBackend_Metal4,
    PrGpuBackend_Vulkan,
    PrGpuBackend_D3d12,
    PrGpuBackend_Memory,
    PrGpuBackend_Canvas2d,
    PrGpuBackend_Count,
};
typedef uint8_t PrGpuBackend;

// Features that not every device has.
enum {
    PrGpuFeatureFlag_Timestamps = 1u << 0,              // command buffers report their GPU time
    PrGpuFeatureFlag_TextureCompressionBc = 1u << 1,
    PrGpuFeatureFlag_TextureCompressionAstc = 1u << 2,

    // A fragment shader that reads the target (pr/gpu.slang's prGpuTarget) reads what the fragments before it in the
    // same draw wrote, in the order of the draw's primitives. Only a device with PrGpuFeatureFlag_TargetReads has it.
    PrGpuFeatureFlag_OrderedTargetReads = 1u << 3,

    // Shaders reach resources by handle, from any data that they read (pr/gpu.slang's prGpuGet and prGpuLoad). Without
    // it, shaders reach only the resources bound to the command buffer's slots (prGpuCommandsSetBuffer and the others),
    // which every device has.
    PrGpuFeatureFlag_Bindless = 1u << 4,

    // A graphics pipeline reads the pass's target (PrGpuGraphicsPipelineDesc::readsTarget). Without it, such a pipeline
    // is not supported, and a renderer reads what earlier draws left from a texture of its own.
    PrGpuFeatureFlag_TargetReads = 1u << 5,
};
typedef uint32_t PrGpuFeatureFlags;

// The kinds of native object through which code beneath lib/gpu reaches a device's work. A native draw
// (prGpuPassNativeDraw) gets a pass's object, such as its encoder or its first color attachment's canvas, and
// prGpuTextureNative gets a texture's object. The header of each kind defines its objects. A device hands out some of
// the kinds (prGpuDeviceHasNative).
//
// A memory or canvas device runs no shaders, and runs work when it is submitted.
enum {
    PrGpuNativeKind_Pixels,     // texels in memory (PrGpuNativePixels)
    PrGpuNativeKind_Canvas2d,   // a web page's canvas (pr/gpu_canvas2d.h)
    PrGpuNativeKind_Metal,      // Metal's encoders and textures (pr/gpu_metal.h)
    PrGpuNativeKind_Metal4,     // Metal 4's encoders and textures (pr/gpu_metal4.h)
    PrGpuNativeKind_Vulkan,     // Vulkan's command buffers and images (pr/gpu_vulkan.h)
    PrGpuNativeKind_D3d12,      // Direct3D 12's command lists and resources (pr/gpu_d3d12.h)
    PrGpuNativeKind_Count,
};
typedef uint8_t PrGpuNativeKind;

// The kinds of image, made outside lib/gpu, that a device imports as a texture (prGpuTextureImport). Examples are a
// frame that a video decoder made, or one that a camera or another API drew. A device imports some kinds in place
// (prGpuDeviceImportsInPlace). It copies the texels of other kinds if the build can read them.
enum {
    PrGpuImportKind_Pixels,       // texels in memory (PrGpuImportPixels)
    PrGpuImportKind_CoreVideo,    // a CVPixelBufferRef of one plane in BGRA, backed by an IOSurface
    PrGpuImportKind_WebImage,     // an image of a web page, such as a VideoFrame (pr/gpu_canvas2d.h)
    PrGpuImportKind_Count,
};
typedef uint8_t PrGpuImportKind;

// An image made outside lib/gpu. Its kind says what its handle is.
typedef struct PrGpuImport {
    PrGpuImportKind kind;
    const void* PR_NULLABLE handle;
} PrGpuImport;

// Called once a device is done with what it was given, with the context that it was given with. It may be called on any
// thread, and must not call the device.
typedef void (*PrGpuReleaseFn)(void* PR_NULLABLE context);

// The maximum sizes of a draw's or a dispatch's root data.
enum {
    kPrGpuMaxPushBytes = 128,       // copied into the command, read as push constants where the API has them
    kPrGpuMaxUniformBytes = 65536,  // copied into the command buffer's stream, and read as a uniform buffer
};

// The maximum number of color attachments of a pass.
enum { kPrGpuMaxColorAttachments = 8 };

// What a device is and what it can do. The strings are valid while the device lives.
typedef struct PrGpuDeviceInfo {
    PrStringView adapter;           // the adapter's name, as the system reports it
    PrStringView backend;           // the backend's name, for messages; an application's backend reports its own
    PrGpuFeatureFlags features;
    uint32_t maxTextureSize;        // the largest width or height of a 2D texture

    // The number of handles that the device can give out at once, of resources and of samplers. Zero without
    // PrGpuFeatureFlag_Bindless.
    uint32_t maxResourceHandles;
    uint32_t maxSamplerHandles;

    // The number of slots of each kind that a command buffer binds (prGpuCommandsSetBuffer and the others).
    uint32_t maxBufferSlots;
    uint32_t maxRwBufferSlots;
    uint32_t maxTextureSlots;
    uint32_t maxRwTextureSlots;
} PrGpuDeviceInfo;

// How a device is created.
typedef struct PrGpuDeviceOptions {
    bool validation;        // turns on the API's validation where it is installed

    // Creates a device without PrGpuFeatureFlag_Bindless where the system has it, so that the slots alone are used and
    // tested on any device.
    bool slotsOnly;
} PrGpuDeviceOptions;

// Returns the backend that `name` names, as prGpuBackendName spells it, ignoring case, or Count if it names none.
PrGpuBackend prGpuBackendFromName(PrStringView name);

// Returns the name of a backend in lowercase, as a command line or PR_GPU_BACKEND spells it.
PrStringView prGpuBackendName(PrGpuBackend backend);

// Returns true if the build has the backend. The build has Default if it has any backend.
bool prGpuBackendIsBuilt(PrGpuBackend backend);

// Returns the default options: without validation.
PrGpuDeviceOptions prGpuDeviceOptionsDefault(void);

// Creates a device by the backend on the system's default adapter. Null options mean the defaults. A backend's own
// header creates one over the application's native device. Returns NotSupported if the build does not have the backend
// or the system has no adapter that it supports.
PrStatus prGpuDeviceCreate(PrGpuBackend backend, const PrGpuDeviceOptions* PR_NULLABLE options,
    PrGpuDevice* PR_NULLABLE* PR_NONNULL out);

// Waits for the device's work to finish, then frees the device. Every object of the device must be destroyed first.
void prGpuDeviceDestroy(PrGpuDevice* PR_NULLABLE device);

// Returns what the device is and what it can do.
PrGpuDeviceInfo prGpuDeviceInfo(const PrGpuDevice* device);

// Returns true if the device hands out native objects of `kind`.
bool prGpuDeviceHasNative(const PrGpuDevice* device, PrGpuNativeKind kind);

// Returns true if the device imports images of `kind` in place, without a copy (prGpuTextureImport).
bool prGpuDeviceImportsInPlace(const PrGpuDevice* device, PrGpuImportKind kind);

// Returns true if the device is lost. Once true, it stays true.
bool prGpuDeviceIsLost(const PrGpuDevice* device);

// Loses the device as the system would, so that an application can test its recovery.
void prGpuDeviceSimulateLoss(PrGpuDevice* device);

// Returns true if the device has reached `fence`. A lost device has reached every fence.
bool prGpuDeviceIsComplete(const PrGpuDevice* device, PrGpuFence fence);

// Waits until the device reaches `fence`. Only an application that must read what the GPU wrote waits, and it waits
// where it reads, not where it submits.
void prGpuDeviceWait(PrGpuDevice* device, PrGpuFence fence);

// Waits until the work submitted before the call is done.
void prGpuDeviceWaitIdle(PrGpuDevice* device);

// ---------------------------------------------------------------------------------------------------------------------
// Formats

// The formats of texels. A format's components are named from the lowest address or the lowest bits. A Srgb format
// converts from sRGB's encoding to linear light where a shader reads it, and back where a pass writes or blends it.
// Every other color format holds what a shader writes unchanged, so blending acts on the values as written.
enum {
    PrGpuFormat_None,

    PrGpuFormat_R8,
    PrGpuFormat_Rg8,
    PrGpuFormat_Rgba8,
    PrGpuFormat_Rgba8Srgb,
    PrGpuFormat_Bgra8,
    PrGpuFormat_Bgra8Srgb,
    PrGpuFormat_Rgb10A2,
    PrGpuFormat_Rg11B10F,
    PrGpuFormat_R16F,
    PrGpuFormat_Rg16F,
    PrGpuFormat_Rgba16F,
    PrGpuFormat_R32F,
    PrGpuFormat_Rg32F,
    PrGpuFormat_Rgba32F,
    PrGpuFormat_R8Uint,
    PrGpuFormat_R16Uint,
    PrGpuFormat_R32Uint,
    PrGpuFormat_Rg32Uint,
    PrGpuFormat_Rgba32Uint,

    PrGpuFormat_D16,
    PrGpuFormat_D32F,
    PrGpuFormat_D32FS8,

    // Block-compressed formats, with PrGpuFeatureFlag_TextureCompressionBc or _Astc.
    PrGpuFormat_Bc1,
    PrGpuFormat_Bc1Srgb,
    PrGpuFormat_Bc3,
    PrGpuFormat_Bc3Srgb,
    PrGpuFormat_Bc4,
    PrGpuFormat_Bc5,
    PrGpuFormat_Bc6HUfloat,
    PrGpuFormat_Bc7,
    PrGpuFormat_Bc7Srgb,
    PrGpuFormat_Astc4x4,
    PrGpuFormat_Astc4x4Srgb,

    PrGpuFormat_Count,
};
typedef uint8_t PrGpuFormat;

// The size of a block of a format: the texels that one block covers on each axis, and its bytes. A format that is not
// block-compressed has blocks of one texel.
typedef struct PrGpuFormatBlock {
    uint8_t width;
    uint8_t height;
    uint8_t bytes;
} PrGpuFormatBlock;

// Returns the block of `format`, which is not None.
PrGpuFormatBlock prGpuFormatBlock(PrGpuFormat format);

// Returns true if `format` holds depth, stencil or both.
bool prGpuFormatIsDepthStencil(PrGpuFormat format);

// Return the texture format that holds a pixel format's pixels as they are laid out in memory, and the pixel format of
// a texture format's texels, or None for a texture format that no pixel format matches. Neither converts colors: a Srgb
// format has the pixel format of its encoded bytes.
PrGpuFormat prGpuFormatFromPixelFormat(PrPixelFormat format);
bool prGpuFormatToPixelFormat(PrGpuFormat format, PrPixelFormat* out);

// ---------------------------------------------------------------------------------------------------------------------
// Buffers

// A buffer in device memory. The value is the buffer's identity and the handle that shaders read it through. Zero is no
// buffer.
typedef struct PrGpuBuffer {
    uint32_t id;
} PrGpuBuffer;

// How a buffer may be used. Every buffer may be read by shaders through its handle, and be the source or target of a
// copy.
enum {
    PrGpuBufferUsageFlag_Storage = 1u << 0,         // written by shaders
    PrGpuBufferUsageFlag_Index = 1u << 1,
    PrGpuBufferUsageFlag_Indirect = 1u << 2,
};
typedef uint8_t PrGpuBufferUsageFlags;

// Where a buffer's memory lives.
enum {
    PrGpuMemory_Device,     // device memory, written by uploads and by the GPU
    PrGpuMemory_Upload,     // memory that the application writes through a mapping and the GPU reads
    PrGpuMemory_Readback,   // memory that the GPU writes and the application reads through a mapping
};
typedef uint8_t PrGpuMemory;

// The size, uses and memory of a buffer.
typedef struct PrGpuBufferDesc {
    uint64_t size;
    PrGpuBufferUsageFlags usage;
    PrGpuMemory memory;
    PrStringView name;      // a name for debuggers and captures, or empty
} PrGpuBufferDesc;

// Creates a buffer as `desc` describes, with zeroed contents. Returns Limit if the device has no memory or handle left
// for it, and Lost if the device is lost. On failure, `*out` is zero.
PrStatus prGpuBufferCreate(PrGpuDevice* device, const PrGpuBufferDesc* desc, PrGpuBuffer* out);

// Destroys the buffer. The work submitted before the call may still use it, and work submitted after must not.
void prGpuBufferDestroy(PrGpuDevice* device, PrGpuBuffer buffer);

// Returns the description that the buffer was created with.
PrGpuBufferDesc prGpuBufferDesc(const PrGpuDevice* device, PrGpuBuffer buffer);

// Returns the handle that shaders write the buffer through, which requires PrGpuBufferUsageFlag_Storage. It may differ
// from the buffer's id, which shaders read it through.
uint32_t prGpuBufferStorageHandle(const PrGpuDevice* device, PrGpuBuffer buffer);

// Returns the bytes of an Upload or Readback buffer, valid while the buffer lives. The application synchronizes its
// writes and reads with the GPU's through fences.
void* prGpuBufferMap(const PrGpuDevice* device, PrGpuBuffer buffer);

// ---------------------------------------------------------------------------------------------------------------------
// Textures

// An image in device memory, or a view of part of one. The value is the texture's identity and the handle that shaders
// sample it through. Zero is no texture.
typedef struct PrGpuTexture {
    uint32_t id;
} PrGpuTexture;

// How a texture may be used. Every texture may be the source or target of a copy.
enum {
    PrGpuTextureUsageFlag_Sampled = 1u << 0,        // read by shaders
    PrGpuTextureUsageFlag_Storage = 1u << 1,        // read and written by shaders without a sampler
    PrGpuTextureUsageFlag_RenderTarget = 1u << 2,   // a color attachment of passes
    PrGpuTextureUsageFlag_DepthStencil = 1u << 3,   // the depth and stencil attachment of passes
};
typedef uint8_t PrGpuTextureUsageFlags;

// The shape of a texture. A layer is one element of an array, and a cube has six layers, one for each face.
enum {
    PrGpuTextureKind_2d,
    PrGpuTextureKind_2dArray,
    PrGpuTextureKind_Cube,
    PrGpuTextureKind_CubeArray,
    PrGpuTextureKind_3d,
};
typedef uint8_t PrGpuTextureKind;

// The extent of a texture's first level. `depth` is the depth of a 3D texture, and the number of layers of any other.
typedef struct PrGpuExtent {
    uint32_t width;
    uint32_t height;
    uint32_t depth;
} PrGpuExtent;

// The shape, format, uses, levels, samples and extent of a texture.
typedef struct PrGpuTextureDesc {
    PrGpuTextureKind kind;
    PrGpuFormat format;
    PrGpuTextureUsageFlags usage;
    uint8_t levels;         // the number of mip levels, at least one
    uint8_t samples;        // the samples per texel, at least one
    PrGpuExtent extent;
    PrStringView name;      // a name for debuggers and captures, or empty
} PrGpuTextureDesc;

// Returns a description of a 2D texture of one level and one sample.
PrGpuTextureDesc prGpuTextureDesc2d(PrGpuFormat format, PrUVec2 extent, PrGpuTextureUsageFlags usage);

// Returns the number of levels of a full mip chain of a texture of `extent`, down to one texel.
uint8_t prGpuMipLevels(PrUVec2 extent);

// The levels and layers that a view or a barrier covers. A zero `levels` or `layers` covers every one from the first.
typedef struct PrGpuSubresources {
    uint8_t firstLevel;
    uint8_t levels;
    uint16_t firstLayer;
    uint16_t layers;
} PrGpuSubresources;

// Returns true if `device` can create textures of `format` with every use in `usage`.
bool prGpuDeviceSupportsFormat(const PrGpuDevice* device, PrGpuFormat format, PrGpuTextureUsageFlags usage);

// Creates a texture as `desc` describes, with undefined contents, in the accesses of PrGpuAccessFlag_ShaderRead for the
// work submitted after the call. Returns Limit if the device has no memory or handle left for it, or the extent is
// larger than the device allows, and Lost if the device is lost. On failure, `*out` is zero.
PrStatus prGpuTextureCreate(PrGpuDevice* device, const PrGpuTextureDesc* desc, PrGpuTexture* out);

// Creates a view of `subresources` of `texture`, of shape `kind`. A view has handles and may be an attachment, and it
// shares its texture's memory. A view may outlive the destruction of its texture, which keeps the memory.
PrStatus prGpuTextureCreateView(PrGpuDevice* device, PrGpuTexture texture, PrGpuTextureKind kind,
    PrGpuSubresources subresources, PrGpuTexture* out);

// Destroys the texture or view. The work submitted before the call may still use it, and work submitted after must not.
void prGpuTextureDestroy(PrGpuDevice* device, PrGpuTexture texture);

// Returns the description that the texture was created with. A view reports the shape, the levels and the extent that
// it covers.
PrGpuTextureDesc prGpuTextureDesc(const PrGpuDevice* device, PrGpuTexture texture);

// Returns the extent of the texture's first level in two dimensions.
PrUVec2 prGpuTextureExtent(const PrGpuDevice* device, PrGpuTexture texture);

// Returns the handle that a shader reads and writes the texture through without a sampler, which requires
// PrGpuTextureUsageFlag_Storage and covers the first level of the texture or view. The texture's id is the handle that
// a shader samples it through.
uint32_t prGpuTextureStorageHandle(const PrGpuDevice* device, PrGpuTexture texture);

// Creates a 2D texture over `pixels` in `format`, with one level, one sample and the extent of the pixels. `format`
// must match a pixel format (prGpuFormatToPixelFormat). The texture's texels are the pixels, with rows `pixels.stride`
// bytes apart. The caller keeps the pixels alive while the texture lives. It reads or writes them only while no work
// that was submitted for the texture is unfinished. Requires PrGpuNativeKind_Pixels. Returns NotSupported for another
// format or usage.
PrStatus prGpuTextureCreateOverPixels(PrGpuDevice* device, PrGpuFormat format, PrPixels pixels,
    PrGpuTextureUsageFlags usage, PrGpuTexture* out);

// The native object of PrGpuNativeKind_Canvas2d, for both a texture and a pass's attachment: a canvas of a web page, by
// the name that the page's side of the backend gives it (pr/gpu_canvas2d.h). Zero names no canvas.
typedef uint32_t PrGpuCanvas;

// The handle of a PrGpuImportKind_Pixels image: texels in memory in `format`, which must match a pixel format
// (prGpuFormatToPixelFormat).
typedef struct PrGpuImportPixels {
    PrPixelView pixels;
    PrGpuFormat format;
} PrGpuImportPixels;

// The native object of PrGpuNativeKind_Pixels: texels in memory in `format`, with rows `pixels.stride` bytes apart.
typedef struct PrGpuNativePixels {
    PrPixels pixels;
    PrGpuFormat format;
} PrGpuNativePixels;

// Makes `*texture` a texture of an image made outside lib/gpu. The texture is 2D, with one level and the extent and
// format of the image, and shaders and native draws sample it.
//
// `*texture` is zero, or a texture that an earlier import on the device made, which the import replaces. Work submitted
// before the call reads what the texture held before it, and work submitted after the call reads the image. A device
// that copies the image may write it into the same texture, and keep its handle. Any other replaced texture is
// destroyed.
//
// A device that imports the image's kind in place makes the texture over the image, without a copy. It takes a
// reference of its own to the image, so the caller may drop its reference once this returns. The device drops its
// reference once the texture is destroyed and the work that read it is done. Any other device copies the image's texels
// into the texture and keeps nothing of the image.
//
// The device calls `release`, if it is not null, once it is done with the image. The image must hold what it shows when
// the call is made.
//
// Returns NotSupported for an image whose texels the build cannot read, and Limit if the device has no handle or memory
// left. On failure, `*texture` is left as it was, and `release` is never called: the caller keeps the image.
PrStatus prGpuTextureImport(PrGpuDevice* device, const PrGpuImport* image, PrGpuReleaseFn PR_NULLABLE release,
    void* PR_NULLABLE context, PrGpuTexture* texture);

// Locks the texels of an image made outside lib/gpu for reading on the CPU, and writes them to `out`. They stay valid
// until prGpuImportUnlock. This is for code that draws on the CPU; a device imports an image without it. Returns false,
// and locks nothing, for an image whose texels the build cannot read, such as an image of a web page.
bool prGpuImportLock(const PrGpuImport* image, PrGpuImportPixels* out);

// Unlocks the texels of an image that prGpuImportLock locked.
void prGpuImportUnlock(const PrGpuImport* image);

// Writes the texture's native object of kind `kind` to `out` and returns true. The device must hand out the kind, and
// the kind's header names the object. Returns false for a texture that the kind does not hold, such as a texture that
// no canvas holds. The object of PrGpuNativeKind_Pixels is the first level and layer of the texture, which the caller
// may draw into.
//
// A device that runs work when it is submitted hands out the native objects of its textures only while a native draw
// runs (prGpuPassNativeDraw). A device that hands out several kinds keeps them in step, so that each holds what work
// through the others drew. On any other device, the objects are valid while the texture lives.
bool prGpuTextureNative(PrGpuDevice* device, PrGpuTexture texture, PrGpuNativeKind kind, void* out);

// ---------------------------------------------------------------------------------------------------------------------
// Samplers

// How a shader filters and addresses a texture. The value is the sampler's identity and its handle in the sampler heap.
// Zero is no sampler.
typedef struct PrGpuSampler {
    uint32_t id;
} PrGpuSampler;

// How a sampler filters between texels, between levels, or between both.
enum {
    PrGpuFilter_Nearest,
    PrGpuFilter_Linear,
};
typedef uint8_t PrGpuFilter;

// How a sampler addresses texels outside a texture, along one axis.
enum {
    PrGpuAddress_Clamp,
    PrGpuAddress_Repeat,
    PrGpuAddress_Mirror,
    PrGpuAddress_Border,    // transparent black
};
typedef uint8_t PrGpuAddress;

// How depth comparisons and depth tests compare a value with what a texture holds.
enum {
    PrGpuCompare_Never,
    PrGpuCompare_Less,
    PrGpuCompare_Equal,
    PrGpuCompare_LessEqual,
    PrGpuCompare_Greater,
    PrGpuCompare_NotEqual,
    PrGpuCompare_GreaterEqual,
    PrGpuCompare_Always,
};
typedef uint8_t PrGpuCompare;

// How a sampler filters and addresses. Its levels of detail are clamped to [minLod, maxLod] after `lodBias`.
typedef struct PrGpuSamplerDesc {
    PrGpuFilter magnify;
    PrGpuFilter minify;
    PrGpuFilter mip;
    PrGpuAddress address[3];        // by axis: u, v and w
    uint8_t maxAnisotropy;          // 1 for none, up to 16
    PrGpuCompare compare;           // Never for a sampler that does not compare
    float minLod;
    float maxLod;
    float lodBias;
} PrGpuSamplerDesc;

// Returns a sampler description with the given filters and address modes, on every axis, and the full range of levels.
PrGpuSamplerDesc prGpuSamplerDescDefault(PrGpuFilter filter, PrGpuAddress address);

// Returns a sampler for `desc`. Equal descriptions return the same sampler, which lives as long as the device, so
// samplers are never destroyed. Returns Limit if the device has no sampler handle left.
PrStatus prGpuSamplerIntern(PrGpuDevice* device, const PrGpuSamplerDesc* desc, PrGpuSampler* out);

// ---------------------------------------------------------------------------------------------------------------------
// Uploads

// Copies `size` bytes from `data` into `buffer` at `offset`. The command buffers submitted after the call read the new
// bytes, and the upload waits for the work submitted before it. Several uploads to one buffer before a submission land
// as if copied in order, the last write of each byte winning. The data may be of any size, and the device stages it as
// it can. The device copies `data` before the call returns.
void prGpuBufferUpload(PrGpuDevice* device, PrGpuBuffer buffer, uint64_t offset, const void* data, uint64_t size);

// Texels of a texture: an origin and an extent in texels, at one level of a range of layers. For a 3D texture, `z` and
// the extent's depth are in texels, and `firstLayer` is zero. For any other, `z` is zero and the extent's depth is the
// number of layers.
typedef struct PrGpuTextureRegion {
    uint8_t level;
    uint16_t firstLayer;
    uint32_t x;
    uint32_t y;
    uint32_t z;
    PrGpuExtent extent;
} PrGpuTextureRegion;

// Returns the region of a 2D texture's first level at `origin`, `extent` texels in size.
PrGpuTextureRegion prGpuTextureRegion2d(PrUVec2 origin, PrUVec2 extent);

// Copies texels from `data` into `region` of `texture`. `rowBytes` is the distance between rows of blocks in `data`,
// and `layerBytes` between layers or depth slices. The command buffers submitted after the call read the new texels,
// and the upload waits for the work submitted before it. Several uploads to one texture before a submission land in
// order. The device copies `data` before the call returns.
//
// The texture is in the accesses of PrGpuAccessFlag_ShaderRead before and after the upload: work that left it in any
// other access ends with a barrier to them.
void prGpuTextureUpload(PrGpuDevice* device, PrGpuTexture texture, const PrGpuTextureRegion* region,
    const void* data, uint32_t rowBytes, uint32_t layerBytes);

// Writes `texels` into the first level of a 2D texture at `origin`, in the texture's format, as an upload. Texels
// outside the texture are dropped.
void prGpuTextureWrite(PrGpuDevice* device, PrGpuTexture texture, PrIVec2 origin, PrPixelView texels);

// Waits for the work submitted before the call, then copies the texels of the first level of a 2D texture at `origin`
// into `out`, in the texture's format. The size of `out` is the size read. Texels outside the texture are zero. The
// texture is in the accesses of PrGpuAccessFlag_ShaderRead or of PrGpuAccessFlag_CopyRead, as `access` says.
void prGpuTextureRead(PrGpuDevice* device, PrGpuTexture texture, uint32_t access, PrIVec2 origin, PrPixels out);

// ---------------------------------------------------------------------------------------------------------------------
// Shaders and pipelines

// Compiled shaders, as one library that the build produced (cmake/shader_library.cmake). A library holds code for each
// backend that the platform builds, names its entry points, and records each one's stage and, for compute, its group
// size.
typedef struct PrGpuShaderLibrary PrGpuShaderLibrary;

// Creates a library from `bytes`, which stay owned by the caller and must outlive the library. Returns InvalidData if
// the bytes are not a library, and NotSupported if it has no code for the device's backend.
PrStatus prGpuShaderLibraryCreate(PrGpuDevice* device, const void* bytes, uint64_t size,
    PrGpuShaderLibrary* PR_NULLABLE* PR_NONNULL out);

// Destroys the library. Pipelines created from it keep what they need.
void prGpuShaderLibraryDestroy(PrGpuShaderLibrary* PR_NULLABLE library);

// A compiled state of the GPU for draws or dispatches. Zero is no pipeline.
typedef struct PrGpuPipeline {
    uint32_t id;
} PrGpuPipeline;

// The primitives that a draw's vertices form.
enum {
    PrGpuTopology_TriangleList,
    PrGpuTopology_TriangleStrip,
    PrGpuTopology_LineList,
    PrGpuTopology_LineStrip,
    PrGpuTopology_PointList,
};
typedef uint8_t PrGpuTopology;

// The facing of the triangles that a draw discards.
enum {
    PrGpuCull_None,
    PrGpuCull_Front,
    PrGpuCull_Back,
};
typedef uint8_t PrGpuCull;

// What blending multiplies the source or the target by. Source and Target name colors where they weight colors, and
// alphas where they weight alphas.
enum {
    PrGpuBlendFactor_Zero,
    PrGpuBlendFactor_One,
    PrGpuBlendFactor_Source,
    PrGpuBlendFactor_OneMinusSource,
    PrGpuBlendFactor_SourceAlpha,
    PrGpuBlendFactor_OneMinusSourceAlpha,
    PrGpuBlendFactor_Target,
    PrGpuBlendFactor_OneMinusTarget,
    PrGpuBlendFactor_TargetAlpha,
    PrGpuBlendFactor_OneMinusTargetAlpha,
    PrGpuBlendFactor_SourceAlphaSaturated,
    PrGpuBlendFactor_Constant,
    PrGpuBlendFactor_OneMinusConstant,
    PrGpuBlendFactor_Source1,               // the second output of the fragment shader, for dual-source blending
    PrGpuBlendFactor_OneMinusSource1,
};
typedef uint8_t PrGpuBlendFactor;

// How blending combines the weighted source and target.
enum {
    PrGpuBlendOp_Add,
    PrGpuBlendOp_Subtract,
    PrGpuBlendOp_ReverseSubtract,
    PrGpuBlendOp_Min,
    PrGpuBlendOp_Max,
};
typedef uint8_t PrGpuBlendOp;

// The channels of a color attachment that a draw writes.
enum {
    PrGpuColorWriteFlag_Red = 1u << 0,
    PrGpuColorWriteFlag_Green = 1u << 1,
    PrGpuColorWriteFlag_Blue = 1u << 2,
    PrGpuColorWriteFlag_Alpha = 1u << 3,
    PrGpuColorWriteFlag_All = 0xf,
};
typedef uint8_t PrGpuColorWriteFlags;

// How one color attachment blends what a fragment shader writes with what it holds. Without `enabled`, the attachment
// takes what the shader writes. A zero `write` writes every channel.
typedef struct PrGpuBlend {
    bool enabled;
    PrGpuBlendFactor colorSource;
    PrGpuBlendFactor colorTarget;
    PrGpuBlendOp colorOp;
    PrGpuBlendFactor alphaSource;
    PrGpuBlendFactor alphaTarget;
    PrGpuBlendOp alphaOp;
    PrGpuColorWriteFlags write;
} PrGpuBlend;

// Returns blending of premultiplied colors: one and one minus source alpha, for color and alpha alike.
PrGpuBlend prGpuBlendPremultiplied(void);

// What the stencil test does to a stencil value.
enum {
    PrGpuStencilOp_Keep,
    PrGpuStencilOp_Zero,
    PrGpuStencilOp_Replace,
    PrGpuStencilOp_IncrementClamp,
    PrGpuStencilOp_DecrementClamp,
    PrGpuStencilOp_Invert,
    PrGpuStencilOp_IncrementWrap,
    PrGpuStencilOp_DecrementWrap,
};
typedef uint8_t PrGpuStencilOp;

// The stencil test and its updates for one facing of primitives.
typedef struct PrGpuStencilFace {
    PrGpuStencilOp fail;
    PrGpuStencilOp depthFail;
    PrGpuStencilOp pass;
    PrGpuCompare compare;
} PrGpuStencilFace;

// The formats of a pass's attachments, which a graphics pipeline is compiled for and a pass must match.
typedef struct PrGpuAttachmentFormats {
    uint8_t colorCount;
    PrGpuFormat color[kPrGpuMaxColorAttachments];   // by color attachment
    PrGpuFormat depthStencil;                       // None for a pass without one
    uint8_t samples;                                // at least one
} PrGpuAttachmentFormats;

// A pipeline for draws. A null `fragment` makes a pipeline that writes only depth and stencil. Vertex data is read by
// shaders through handles, so a pipeline has no vertex layout.
typedef struct PrGpuGraphicsPipelineDesc {
    const PrGpuShaderLibrary* library;
    PrStringView vertex;
    PrStringView fragment;

    PrGpuTopology topology;
    PrGpuCull cull;
    bool frontClockwise;            // true if a clockwise triangle faces front, in a pass's target with y down

    // The depth test, and the bias added to the depth that a primitive writes.
    bool depthTest;
    bool depthWrite;
    PrGpuCompare depthCompare;
    float depthBias;
    float depthBiasSlope;
    float depthBiasClamp;

    // The stencil test. Its reference value is set on the pass.
    bool stencilTest;
    uint8_t stencilReadMask;
    uint8_t stencilWriteMask;
    PrGpuStencilFace stencilFront;
    PrGpuStencilFace stencilBack;

    PrGpuAttachmentFormats formats;
    PrGpuBlend blend[kPrGpuMaxColorAttachments];    // by color attachment

    // True if the fragment shader reads the pass's first color attachment at its pixel (pr/gpu.slang's prGpuTarget),
    // which a device with PrGpuFeatureFlag_TargetReads supports. The attachment's blending must be disabled.
    bool readsTarget;

    PrStringView name;                              // a name for debuggers and captures, or empty
} PrGpuGraphicsPipelineDesc;

// Returns the defaults of a graphics pipeline over `library`: triangle lists, no culling, no depth or stencil test, no
// blending, and one sample.
PrGpuGraphicsPipelineDesc prGpuGraphicsPipelineDescDefault(const PrGpuShaderLibrary* library);

// Creates a pipeline for draws, or for dispatches of the compute entry point `compute` of `library`. Returns NotFound
// if the library has no entry point by a name that the description gives, WrongType if an entry point is of the wrong
// stage, and NotSupported for a state that the device does not have, or for an entry point that reaches resources by
// handle on a device without PrGpuFeatureFlag_Bindless. On failure, `*out` is zero.
PrStatus prGpuPipelineCreateGraphics(PrGpuDevice* device, const PrGpuGraphicsPipelineDesc* desc, PrGpuPipeline* out);
PrStatus prGpuPipelineCreateCompute(PrGpuDevice* device, const PrGpuShaderLibrary* library, PrStringView compute,
    PrStringView name, PrGpuPipeline* out);

// Destroys the pipeline. The work submitted before the call may still use it.
void prGpuPipelineDestroy(PrGpuDevice* device, PrGpuPipeline pipeline);

// Returns the number of threads in each dimension of a group of the compute pipeline, as its shader declares them.
PrGpuExtent prGpuPipelineGroupSize(const PrGpuDevice* device, PrGpuPipeline pipeline);

// ---------------------------------------------------------------------------------------------------------------------
// Command buffers

// Work recorded for a device: passes, dispatches, copies and barriers. A command buffer records into a native command
// buffer of the device's own, or into the application's, between the application's own work.
typedef struct PrGpuCommands PrGpuCommands;

// How a command buffer begins.
typedef struct PrGpuCommandsOptions {
    // The application's native command buffer to record into, as the backend's header says, or null for one of the
    // device's own.
    void* PR_NULLABLE native;
} PrGpuCommandsOptions;

// Returns the default options: a command buffer of the device's own.
PrGpuCommandsOptions prGpuCommandsOptionsDefault(void);

// Begins an empty command buffer. A null `options` means the defaults. Returns false if the device is lost. On failure,
// `*out` is null.
bool prGpuCommandsBegin(PrGpuDevice* device, const PrGpuCommandsOptions* PR_NULLABLE options,
    PrGpuCommands* PR_NULLABLE* PR_NONNULL out);

// Returns the device that the command buffer records for.
PrGpuDevice* prGpuCommandsDevice(const PrGpuCommands* commands);

// The state of the native command buffer that the device's work changes. An application that records its own work into
// the same native command buffer sets this state again. The device never binds descriptor sets or heaps in the
// application's pass, so they are not in the set.
enum {
    PrGpuNativeStateFlag_Pipeline = 1u << 0,
    PrGpuNativeStateFlag_Push = 1u << 1,
    PrGpuNativeStateFlag_IndexBuffer = 1u << 2,
    PrGpuNativeStateFlag_Viewport = 1u << 3,
    PrGpuNativeStateFlag_Scissor = 1u << 4,
    PrGpuNativeStateFlag_BlendConstant = 1u << 5,
    PrGpuNativeStateFlag_StencilReference = 1u << 6,
    PrGpuNativeStateFlag_Bindings = 1u << 7,      // Metal's argument table, and Direct3D 12's root signature
};
typedef uint8_t PrGpuNativeStateFlags;

// Hands the native command buffer to the application, which records its own work after the call. Returns the state that
// the device's work changed since the last call. The application sets that state again, or marks it stale in its own
// cache, before it records.
//
// Outside a pass, the call ends what the device has open in the native command buffer, such as a Metal encoder. In a
// pass, the pass stays open. The application calls it after the device's work and before its own, and before it ends
// the native command buffer. A native draw (prGpuPassNativeDraw) is the same handoff within a pass, made by code
// beneath lib/gpu.
PrGpuNativeStateFlags prGpuNativeStateSync(PrGpuCommands* commands);

// Submits the command buffer to the device's queue, and returns the fence that its work completes at. Fences increase
// in the order of submission. Uploads made before the call are visible to its work. A command buffer that records into
// the application's is submitted once the application has ended its native command buffer, which the application does
// not submit itself. The command buffer is released.
PrGpuFence prGpuCommandsSubmit(PrGpuCommands* commands);

// Submits the command buffer and waits for its work. Returns the seconds that the device spent on it, which are zero
// without PrGpuFeatureFlag_Timestamps or if the device was lost first.
double prGpuCommandsSubmitAndWait(PrGpuCommands* commands);

// Allocates `size` bytes of the command buffer's stream: memory that the GPU reads, valid until the command buffer's
// work completes, aligned to 256 bytes. Returns the bytes for the caller to write, the handle of the buffer that holds
// them in `*buffer`, and their offset in that buffer in `*offset`. The stream grows as recording needs it, so the call
// always succeeds. If the device has no memory left for it, the device is lost, and the call returns memory that the
// GPU never reads.
void* prGpuCommandsStream(PrGpuCommands* commands, uint64_t size, PrGpuBuffer* buffer, uint64_t* offset);

// Sets the root data of the draws and dispatches recorded after the call. Push data is at most kPrGpuMaxPushBytes, and
// uniforms at most kPrGpuMaxUniformBytes. Both are copied.
void prGpuCommandsSetPush(PrGpuCommands* commands, const void* data, uint32_t size);
void prGpuCommandsSetUniforms(PrGpuCommands* commands, const void* data, uint32_t size);

// Sets the pipeline of the dispatches recorded after the call, or, in a pass, of the draws.
void prGpuCommandsSetPipeline(PrGpuCommands* commands, PrGpuPipeline pipeline);

// The alignment of the offset at which a buffer is bound to a slot, which the command buffer's stream meets.
enum { kPrGpuSlotOffsetAlignment = 256 };

// Bind resources to the command buffer's slots for the draws and dispatches recorded after the call, as root data is
// set. A slot is of one kind, numbered from zero below the device's maximum of its kind (PrGpuDeviceInfo), and is
// shared by every stage of a pipeline. A shader declares the slots that it reads (pr/gpu.slang's PR_GPU_BUFFER and the
// others). A zero buffer or texture unbinds the slot.
//
// A Buffer slot is read from `offset` bytes into the buffer, which is a multiple of kPrGpuSlotOffsetAlignment. An
// RwBuffer slot may also be written, and its buffer has PrGpuBufferUsageFlag_Storage. A Texture slot is sampled with
// its sampler, or read without one if the sampler is zero, and its texture has PrGpuTextureUsageFlag_Sampled. An
// RwTexture slot is read and written without a sampler, and its texture has PrGpuTextureUsageFlag_Storage.
//
// A native draw (prGpuPassNativeDraw) unbinds every slot.
void prGpuCommandsSetBuffer(PrGpuCommands* commands, uint32_t slot, PrGpuBuffer buffer, uint64_t offset);
void prGpuCommandsSetRwBuffer(PrGpuCommands* commands, uint32_t slot, PrGpuBuffer buffer, uint64_t offset);
void prGpuCommandsSetTexture(PrGpuCommands* commands, uint32_t slot, PrGpuTexture texture, PrGpuSampler sampler);
void prGpuCommandsSetRwTexture(PrGpuCommands* commands, uint32_t slot, PrGpuTexture texture);

// Records dispatches outside any pass: `groups` groups of the pipeline's group size, or the groups that a
// PrGpuDispatchIndirect at `offset` of `buffer` names.
void prGpuCommandsDispatch(PrGpuCommands* commands, PrGpuExtent groups);
void prGpuCommandsDispatchIndirect(PrGpuCommands* commands, PrGpuBuffer buffer, uint64_t offset);

// The arguments of a draw, as an indirect draw reads them.
typedef struct PrGpuDrawIndirect {
    uint32_t vertexCount;
    uint32_t instanceCount;
    uint32_t firstVertex;
    uint32_t firstInstance;
} PrGpuDrawIndirect;

// The arguments of an indexed draw, as an indirect draw reads them.
typedef struct PrGpuDrawIndexedIndirect {
    uint32_t indexCount;
    uint32_t instanceCount;
    uint32_t firstIndex;
    int32_t vertexOffset;
    uint32_t firstInstance;
} PrGpuDrawIndexedIndirect;

// The groups of a dispatch, as an indirect dispatch reads them.
typedef struct PrGpuDispatchIndirect {
    uint32_t x;
    uint32_t y;
    uint32_t z;
} PrGpuDispatchIndirect;

// Records copies outside any pass. A texture region names one level and a range of layers, and the buffer's bytes are
// laid out as `rowBytes` and `layerBytes` say. Copies are ordered with other work only by barriers: the source is in
// PrGpuAccessFlag_CopyRead and the target in PrGpuAccessFlag_CopyWrite.
void prGpuCommandsCopyBuffer(PrGpuCommands* commands, PrGpuBuffer source, uint64_t sourceOffset, PrGpuBuffer target,
    uint64_t targetOffset, uint64_t size);
void prGpuCommandsCopyTexture(PrGpuCommands* commands, PrGpuTexture source, const PrGpuTextureRegion* sourceRegion,
    PrGpuTexture target, const PrGpuTextureRegion* targetRegion);
void prGpuCommandsCopyBufferToTexture(PrGpuCommands* commands, PrGpuBuffer source, uint64_t sourceOffset,
    uint32_t rowBytes, uint32_t layerBytes, PrGpuTexture target, const PrGpuTextureRegion* targetRegion);
void prGpuCommandsCopyTextureToBuffer(PrGpuCommands* commands, PrGpuTexture source,
    const PrGpuTextureRegion* sourceRegion, PrGpuBuffer target, uint64_t targetOffset, uint32_t rowBytes,
    uint32_t layerBytes);

// Records filling each mip level of a 2D texture after the first from the level before it, outside any pass. Each texel
// of a level is the average of the texels that it covers in the level before. The texture must have
// PrGpuTextureUsageFlag_Sampled and _RenderTarget, and a format that filters. Its first level is in `before` until the
// call, and every level is in the accesses of PrGpuAccessFlag_ShaderRead after it.
void prGpuCommandsGenerateMipmaps(PrGpuCommands* commands, PrGpuTexture texture, uint32_t before);

// Called on the device's thread once a command buffer's work is done, with the seconds that the device spent on it,
// which are zero without PrGpuFeatureFlag_Timestamps or if the device was lost first.
typedef void (*PrGpuDoneFn)(void* PR_NULLABLE context, double seconds);

// Calls `done` once the command buffer's work is done.
void prGpuCommandsOnDone(PrGpuCommands* commands, PrGpuDoneFn done, void* PR_NULLABLE context);

// Called on the device's thread once a command buffer's work is done, with the texels that a read recorded, in the
// texture's format. The texels are valid only during the call. They are empty if the device was lost first.
typedef void (*PrGpuReadFn)(void* PR_NULLABLE context, PrPixelView texels);

// Records a read of the first level of a 2D texture, outside any pass, and calls `read` with its texels once the
// command buffer's work is done. The texture is in `access` before and after the read.
void prGpuCommandsReadTexture(PrGpuCommands* commands, PrGpuTexture texture, uint32_t access, PrGpuReadFn read,
    void* PR_NULLABLE context);

// Opens or closes a named group of commands for debuggers and captures. Groups nest.
void prGpuCommandsPushGroup(PrGpuCommands* commands, PrStringView name);
void prGpuCommandsPopGroup(PrGpuCommands* commands);

// ---------------------------------------------------------------------------------------------------------------------
// Barriers

// How work reads or writes a resource. A barrier waits for the accesses before it and makes their writes visible to the
// accesses after it. On APIs with image layouts, a texture's accesses also decide its layout.
enum {
    PrGpuAccessFlag_IndexRead = 1u << 0,
    PrGpuAccessFlag_IndirectRead = 1u << 1,
    PrGpuAccessFlag_VertexRead = 1u << 2,           // shader reads in the vertex stage
    PrGpuAccessFlag_VertexWrite = 1u << 3,
    PrGpuAccessFlag_FragmentRead = 1u << 4,
    PrGpuAccessFlag_FragmentWrite = 1u << 5,
    PrGpuAccessFlag_ComputeRead = 1u << 6,
    PrGpuAccessFlag_ComputeWrite = 1u << 7,
    PrGpuAccessFlag_CopyRead = 1u << 8,
    PrGpuAccessFlag_CopyWrite = 1u << 9,
    PrGpuAccessFlag_ColorAttachment = 1u << 10,
    PrGpuAccessFlag_DepthStencilRead = 1u << 11,
    PrGpuAccessFlag_DepthStencilWrite = 1u << 12,
    PrGpuAccessFlag_Present = 1u << 13,

    // In the accesses before a barrier, says that the resource's contents need not survive it. The other accesses still
    // say what the barrier waits for.
    PrGpuAccessFlag_Discard = 1u << 14,

    PrGpuAccessFlag_ShaderRead = PrGpuAccessFlag_VertexRead | PrGpuAccessFlag_FragmentRead |
        PrGpuAccessFlag_ComputeRead,
    PrGpuAccessFlag_ShaderWrite = PrGpuAccessFlag_VertexWrite | PrGpuAccessFlag_FragmentWrite |
        PrGpuAccessFlag_ComputeWrite,
};
typedef uint32_t PrGpuAccessFlags;

// Records a barrier on a buffer, or on subresources of a texture, outside any pass. The caller knows each resource's
// accesses and states them.
void prGpuCommandsBufferBarrier(PrGpuCommands* commands, PrGpuBuffer buffer, PrGpuAccessFlags before, PrGpuAccessFlags after);
void prGpuCommandsTextureBarrier(PrGpuCommands* commands, PrGpuTexture texture, PrGpuSubresources subresources,
    PrGpuAccessFlags before, PrGpuAccessFlags after);

// Records a barrier on all memory outside any pass. It changes no texture's layout.
void prGpuCommandsGlobalBarrier(PrGpuCommands* commands, PrGpuAccessFlags before, PrGpuAccessFlags after);

// ---------------------------------------------------------------------------------------------------------------------
// Passes

// Drawing to a set of attachments, recorded into a command buffer. A pass is either the device's own, which the device
// begins and ends, or it wraps a pass that the application has open in its native command buffer. No barriers are
// recorded in a pass.
typedef struct PrGpuPass PrGpuPass;

// What a pass does with an attachment's contents when it begins.
enum {
    PrGpuLoad_Keep,
    PrGpuLoad_Clear,
    PrGpuLoad_Discard,      // the contents are undefined
};
typedef uint8_t PrGpuLoad;

// What a pass does with an attachment's contents when it ends.
enum {
    PrGpuStore_Keep,
    PrGpuStore_Discard,
    PrGpuStore_Resolve,     // resolves the samples into `resolve`, and discards them
};
typedef uint8_t PrGpuStore;

// An attachment: a texture, or one level and layer of it, and what the pass does with it.
typedef struct PrGpuAttachment {
    PrGpuTexture texture;
    PrGpuTexture resolve;   // the target of PrGpuStore_Resolve, or zero
    uint8_t level;
    uint16_t layer;
    PrGpuLoad load;
    PrGpuStore store;
} PrGpuAttachment;

// The attachments of a pass of the device's own, and the values that the pass clears them to. Or, a pass that the
// application has open in its native command buffer.
typedef struct PrGpuPassDesc {
    // A pass that the application began in the native command buffer that the commands record into, as the backend's
    // header describes it. `nativeFormats` are its attachment formats, and `nativeExtent` is its extent in target
    // pixels. The device begins nothing, and reads none of the fields below.
    void* PR_NULLABLE native;
    PrGpuAttachmentFormats nativeFormats;
    PrUVec2 nativeExtent;

    uint8_t colorCount;
    PrGpuAttachment color[kPrGpuMaxColorAttachments];   // by color attachment
    PrColor clearColor[kPrGpuMaxColorAttachments];      // the values that Clear writes, by color attachment
    PrGpuAttachment depthStencil;                       // zero texture for none
    PrGpuLoad stencilLoad;
    PrGpuStore stencilStore;
    float clearDepth;
    uint8_t clearStencil;
    PrStringView name;                                  // a name for debuggers and captures, or empty
} PrGpuPassDesc;

// Returns a pass over one color attachment, which it loads as `load` says, clears to `clear` if it clears it, and
// keeps.
PrGpuPassDesc prGpuPassDescColor(PrGpuTexture texture, PrGpuLoad load, PrColor clear);

// Begins a pass. The attachments of a pass of the device's own must be in the accesses of
// PrGpuAccessFlag_ColorAttachment and _DepthStencilWrite. A command buffer has at most one pass open.
PrGpuPass* prGpuPassBegin(PrGpuCommands* commands, const PrGpuPassDesc* desc);

// Ends the pass. Whoever begins a pass ends it. A pass of the device's own ends. A pass of the application's stays
// open, so that the application can draw more and end it.
void prGpuPassEnd(PrGpuPass* pass);

// Draws into a pass from beneath lib/gpu. `native` is the pass's object of the native draw's kind, which the kind's
// header names. On a device that runs work when it is submitted, the object is that of the first color attachment, at
// the attachment's level and layer, and the draw reaches the textures that it reads through prGpuTextureNative.
typedef void (*PrGpuNativeDrawFn)(void* PR_NULLABLE context, const void* native);

// Records a native draw of kind `kind` into the pass, in order with the pass's other work. The device must hand out the
// kind. The device calls `draw` while it records the draw, or, if it runs work when it is submitted, while the draw's
// work runs. A caller writes the same code for both.
//
// The draw may change any state of the pass that the native object reaches, as an application's own work does
// (prGpuNativeStateSync). So the pass's pipeline, root data, index buffer, viewport, scissor, blend constant and
// stencil reference are unset after it, and the pass's next draw sets those that it uses.
void prGpuPassNativeDraw(PrGpuPass* pass, PrGpuNativeKind kind, PrGpuNativeDrawFn draw, void* PR_NULLABLE context);

// Returns the command buffer that the pass records into, through which its draws set their pipeline and root data.
PrGpuCommands* prGpuPassCommands(const PrGpuPass* pass);

// Returns the formats of the pass's attachments, which the pipelines of its draws must be compiled for.
PrGpuAttachmentFormats prGpuPassFormats(const PrGpuPass* pass);

// Returns the extent of the pass's attachments, in target pixels.
PrUVec2 prGpuPassExtent(const PrGpuPass* pass);

// Set the dynamic state of the draws recorded after the call: the viewport and depth range, the scissor rectangle in
// target pixels, the blend constant and the stencil reference. A pass begins with the viewport and scissor covering its
// attachments.
void prGpuPassSetViewport(PrGpuPass* pass, PrRect viewport, float minDepth, float maxDepth);
void prGpuPassSetScissor(PrGpuPass* pass, PrIRect scissor);
void prGpuPassSetBlendConstant(PrGpuPass* pass, PrColor constant);
void prGpuPassSetStencilReference(PrGpuPass* pass, uint8_t reference);

// Sets the index buffer of the indexed draws recorded after the call. Indices are bound because the hardware fetches
// them ahead of the vertex shader and reuses the vertices that it has transformed.
void prGpuPassSetIndexBuffer(PrGpuPass* pass, PrGpuBuffer buffer, uint64_t offset, bool wide);

// Record draws with the pass's pipeline and root data, which are set through the pass's command buffer. An indirect
// draw reads `count` arguments, `stride` bytes apart, from `buffer` at `offset`.
void prGpuPassDraw(PrGpuPass* pass, uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex,
    uint32_t firstInstance);
void prGpuPassDrawIndexed(PrGpuPass* pass, uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex,
    int32_t vertexOffset, uint32_t firstInstance);
void prGpuPassDrawIndirect(PrGpuPass* pass, PrGpuBuffer buffer, uint64_t offset, uint32_t count, uint32_t stride);
void prGpuPassDrawIndexedIndirect(PrGpuPass* pass, PrGpuBuffer buffer, uint64_t offset, uint32_t count, uint32_t stride);

// ---------------------------------------------------------------------------------------------------------------------
// Swap chains

// The textures that a device presents to an application's surface, one frame at a time.
typedef struct PrGpuSwapChain PrGpuSwapChain;

// What a swap chain shows, from which its device chooses the format and the color space of its textures
// (PrGpuSwapChainInfo).
enum {
    // sRGB colors no brighter than SDR white. The textures hold encoded sRGB in 8 bits.
    PrGpuSwapChainIntent_Sdr,

    // Colors past sRGB's gamut and SDR white, where the window's display shows them. The textures hold what the
    // platform presents such colors in for the display, as each backend documents, and for a display that shows no more
    // than sRGB, what SDR holds, so that an application draws no more than the display shows.
    PrGpuSwapChainIntent_Hdr,
};
typedef uint8_t PrGpuSwapChainIntent;

// How a swap chain presents.
enum {
    // Presents as soon as the frame's commands are done, from any thread, outside the window's display transaction.
    // While a window is being resized, it can show a frame at the wrong size.
    PrGpuSwapChainFlag_Threaded = 1 << 0,

    // Presents the textures' alpha, with premultiplied colors, so that whatever is behind a window shows where the
    // alpha is less than one. Without the flag, the window shows the textures as opaque.
    PrGpuSwapChainFlag_Transparent = 1 << 1,
};
typedef uint8_t PrGpuSwapChainFlags;

// Where a window shows a frame of another size than its own, as while the window is resized and its frame at the new
// size is still to come.
enum {
    // At the window's top left, at the frame's own size, so that what the frame draws stays in place. The window cuts
    // off the frame past its edges.
    PrGpuSwapChainAnchor_TopLeft,

    // Scaled to fill the window.
    PrGpuSwapChainAnchor_Stretch,
};
typedef uint8_t PrGpuSwapChainAnchor;

// What a swap chain shows, and how it presents.
typedef struct PrGpuSwapChainDesc {
    PrGpuSwapChainIntent intent;
    PrGpuSwapChainFlags flags;
    PrGpuSwapChainAnchor anchor;
} PrGpuSwapChainDesc;

// The format and the color space of the textures that a swap chain presents, which its device chooses for the swap
// chain's intent. They combine as follows:
//
// - Srgb on Bgra8: sRGB, encoded, in [0, 1]. A shader writes and blends the encoded values.
// - Srgb on Rgba16F: extended sRGB, encoded. Values outside [0, 1] show colors past sRGB's gamut and, on a display with
//   headroom, brighter than SDR white.
// - SrgbLinear on Rgba16F: extended sRGB in linear light, which is scRGB.
// - Rec2100Pq on Rgb10A2: HDR10. A shader writes Rec. 2100 colors encoded with PQ, and the display shows their
//   absolute luminance.
//
// The device converts no colors.
typedef struct PrGpuSwapChainInfo {
    PrGpuFormat format;
    PrColorSpace space;
} PrGpuSwapChainInfo;

// Creates a swap chain that presents to `surface`, which must outlive it. Without PrGpuSwapChainFlag_Threaded, a frame
// presents within the window's display transaction: from the thread that runs the window, while it draws a frame, so
// that a resize shows each frame at the window's size. Returns NotSupported for a kind of surface the device cannot
// present to, and Lost if the device is lost. On failure, `*out` is null.
PrStatus prGpuSwapChainCreate(PrGpuDevice* device, const PrSurface* surface, const PrGpuSwapChainDesc* desc,
    PrGpuSwapChain* PR_NULLABLE* PR_NONNULL out);

// Waits for the frames in flight, then frees the swap chain and the textures of its images.
void prGpuSwapChainDestroy(PrGpuSwapChain* PR_NULLABLE swapChain);

// Returns the format and the color space of the textures that the swap chain presents. They follow the window's display
// for the swap chain's intent, and change when a texture is acquired after the window moves to another display or the
// display's mode changes, so an application reads them for each texture that it acquires.
PrGpuSwapChainInfo prGpuSwapChainInfo(const PrGpuSwapChain* swapChain);

// Waits until the swap chain can take another frame: until the display has taken enough of the frames presented before.
// An application that calls it before it reads input and builds a frame builds the frame from the latest input. If the
// application has not waited since its last frame, acquiring a texture waits as this does. On a platform that waits
// only to acquire a texture, such as Metal, this returns at once.
void prGpuSwapChainWait(PrGpuSwapChain* swapChain);

// Acquires the texture that the next frame draws to, at `extent` in target pixels. The texture is a render target that
// may be copied from, in the accesses of Discard until the frame draws to it. Returns InvalidState if the surface
// cannot provide a texture now, such as while its window is hidden or the extent is empty, Limit for an extent larger
// than the device allows, and Lost if the device is lost. On failure, `*out` is zero.
PrStatus prGpuSwapChainAcquire(PrGpuSwapChain* swapChain, PrUVec2 extent, PrGpuTexture* out);

// Presents the acquired texture once `commands` are done. The texture must be in the accesses of
// PrGpuAccessFlag_Present by the end of the commands. The application submits `commands` after this call, and does not
// use the texture again.
void prGpuSwapChainPresent(PrGpuSwapChain* swapChain, PrGpuCommands* commands);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
