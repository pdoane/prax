// GPU devices: resources that shaders reach by handle, pipelines, command buffers and passes, swap chains, and the
// fences that their work completes at. Every renderer is written against this interface, and its backends drive Metal,
// Vulkan and Direct3D 12.
//
// Every buffer, texture and sampler has a handle that is its slot in the device's heap. A shader reads a handle from
// any data that it can read, including another resource, and turns it into the resource (pr/gpu.slang). Creating a
// resource makes it visible to shaders.
//
// Buffers, textures and pipelines are reference counted, and the release of the last reference destroys one. Every
// object may be destroyed as soon as the work that uses it is recorded. The device keeps what the object holds until
// the work submitted before the destruction completes. So a renderer may create a texture, use it and release it within
// one frame.
//
// Several threads may use a device at once, each recording its own command buffers. A command buffer is used by one
// thread at a time. Creating, retaining, releasing and destroying objects, uploads and submissions are thread-safe.
//
// The device tracks no resource states. A barrier names a resource's accesses before and after it, and the caller knows
// them.
//
// A device can be lost: the system takes the GPU away, as when its driver resets after a hang. A lost device never
// comes back. Every call on it and its objects stays safe. The calls that make something new fail with Lost, recorded
// work does nothing, and readbacks see zeros. The application recovers by destroying every object of the device, then
// the device, and making them again on a new device.

#pragma once

#include "pr/app_surface.h"
#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/color.h"
#include "pr/geom.h"
#include "pr/pixels.h"
#include "pr/raw_string.h"
#include "pr/status.h"
#include "pr/string_view.h"
#include "pr/task.h"
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

// Features that not every device has.
enum {
    PrGpuFeatureFlag_Timestamps = 1u << 0,              // command buffers report their GPU time

    // The device samples the block-compressed formats of a family (PrGpuFormatFamily): every format of the family, but
    // for a format that prGpuDeviceSupportsFormat refuses, which is the answer for one format.
    PrGpuFeatureFlag_TextureCompressionBc = 1u << 1,
    PrGpuFeatureFlag_TextureCompressionEtc2 = 1u << 2,
    PrGpuFeatureFlag_TextureCompressionAstc = 1u << 3,      // the Unorm and Srgb formats: the LDR profile
    PrGpuFeatureFlag_TextureCompressionAstcHdr = 1u << 4,   // the Hdr formats

    // A fragment shader that reads the target (pr/gpu.slang's prGpuTarget) reads what the fragments before it in the
    // same draw wrote, in the order of the draw's primitives. Only a device with PrGpuFeatureFlag_TargetReads has it.
    PrGpuFeatureFlag_OrderedTargetReads = 1u << 5,

    // Shaders reach resources by handle, from any data that they read (pr/gpu.slang's prGpuGet and prGpuLoad). Without
    // it, shaders reach only the resources bound to the command buffer's slots (prGpuCommandsSetBuffer and the others),
    // which every device has.
    PrGpuFeatureFlag_Bindless = 1u << 6,

    // A graphics pipeline reads the pass's target (PrGpuGraphicsPipelineDesc::readsTarget). Without it, such a pipeline
    // is not supported, and a renderer reads what earlier draws left from a texture of its own.
    PrGpuFeatureFlag_TargetReads = 1u << 7,

    // The device samples the swizzled formats (PrGpuSwizzle) of each format that it samples, as the hardware places the
    // channels, with no work in a shader.
    PrGpuFeatureFlag_TextureSwizzle = 1u << 8,
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

// The maximum numbers of a pipeline's vertex buffers and vertex attributes (PrGpuVertexLayout).
enum {
    kPrGpuMaxVertexBuffers = 8,
    kPrGpuMaxVertexAttributes = 16,
};

// The numbers of objects that a device holds at once: buffers and textures, views among them; samplers; and pipelines.
// A device allocates its tables for them when it is created, and a creation past one returns Limit. With
// PrGpuFeatureFlag_Bindless, they are also the numbers of handles that shaders reach resources and samplers by.
typedef struct PrGpuDeviceLimits {
    uint32_t resources;
    uint32_t samplers;
    uint32_t pipelines;
} PrGpuDeviceLimits;

// The limits that a device takes when its options ask for none: 65536 resources, 2048 samplers and 4096 pipelines.
PrGpuDeviceLimits prGpuDeviceLimitsDefault(void);

// What a device is and what it can do. The strings are valid while the device lives.
typedef struct PrGpuDeviceInfo {
    PrStringView adapter;           // the adapter's name, as the system reports it
    PrStringView backend;           // the backend's name, for messages; an application's backend reports its own
    PrGpuFeatureFlags features;
    uint32_t maxTextureSize;        // the largest width or height of a 2D texture

    // The numbers of samples per texel that the device gives a texture that passes draw to, in any format that it
    // renders to, as the sum of the numbers. Each is a power of two, and 1 is always among them: a device that has 1, 2
    // and 4 samples reports 7.
    uint32_t sampleCounts;

    // The objects that the device holds at once: what its options asked for, or less where its API allows less.
    PrGpuDeviceLimits limits;

    // The number of slots of each kind that a command buffer binds (prGpuCommandsSetBuffer and the others).
    uint32_t maxBufferSlots;
    uint32_t maxRwBufferSlots;
    uint32_t maxTextureSlots;
    uint32_t maxRwTextureSlots;

    uint32_t storageOffsetAlignment;    // of a buffer's offset where it is bound to a slot, a power of two
} PrGpuDeviceInfo;

// Called once, when a device's creation is done, whether the creation succeeded or not (prGpuDeviceLostStatus). It runs
// on the thread that ran the creation: a thread of the pool, or the creating thread during the constructor if there is
// no pool. On the web, the page's event loop calls it.
typedef void (*PrGpuDeviceReadyFn)(void* PR_NULLABLE context, PrGpuDevice* device);

// How a device is created.
typedef struct PrGpuDeviceOptions {
    bool validation;        // turns on the API's validation where it is installed

    // Creates a device without PrGpuFeatureFlag_Bindless where the system has it, so that a renderer's slot path runs
    // and is tested beside its bindless path. A backend whose devices always have bindless creates none, and
    // prGpuDeviceCreate returns null for it.
    bool slotsOnly;

    // The pool that runs the device's creation as a job, or null to create the device during the constructor. A
    // creation can take hundreds of milliseconds. On the web, creation is asynchronous with or without a pool.
    PrTaskPool* PR_NULLABLE pool;

    // Called when the device's creation is done, or null for no call.
    PrGpuDeviceReadyFn PR_NULLABLE ready;
    void* PR_NULLABLE readyContext;

    // The objects that the device holds at once. A limit of zero takes the default's (prGpuDeviceLimitsDefault).
    PrGpuDeviceLimits limits;
} PrGpuDeviceOptions;

// Returns the default options: without validation, and with the default limits.
PrGpuDeviceOptions prGpuDeviceOptionsDefault(void);

// Waits for the device's creation and its work to finish, then frees the device. Every object of the device must be
// destroyed first.
void prGpuDeviceDestroy(PrGpuDevice* PR_NULLABLE device);

// Returns true once the device's creation is done, whether it succeeded or failed. Once true, it stays true. Everything
// that the creation made is visible to the calling thread after the call returns true.
bool prGpuDeviceIsReady(const PrGpuDevice* device);

// Returns why the device is lost: the status that its creation failed with, such as NotSupported for a system without
// an adapter that the backend supports, or Lost for a device that the system took away. Returns Ok for a device that is
// not lost.
PrStatus prGpuDeviceLostStatus(const PrGpuDevice* device);

// Returns what the device is and what it can do. Before the device is ready, only the backend's name is set.
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

// The formats of texels. A format's components are named from the lowest address or the lowest bits, as Metal and
// Direct3D name them. An unsuffixed format holds unsigned normalized values, Snorm signed normalized ones, Uint and
// Sint integers, and F floats. A Srgb format converts from sRGB's encoding to linear values where a shader reads it,
// and back where a pass writes or blends it. Every other color format holds what a shader writes unchanged, so blending
// acts on the values as written.
//
// A swizzled format, named for its PrGpuSwizzle after the format that it stores its texels in, is that format sampled
// with its channels placed otherwise. It is only sampled, and only on a device with PrGpuFeatureFlag_TextureSwizzle.
//
// The values are fixed, since records of textures store them (PrGpuTextureRecord): a format is added at the end of the
// enumeration, never between two. A device that has no format of the set refuses it (prGpuDeviceSupportsFormat).
enum {
    PrGpuFormat_None,

    // 8 bits a channel
    PrGpuFormat_R8,
    PrGpuFormat_R8Snorm,
    PrGpuFormat_R8Uint,
    PrGpuFormat_R8Sint,
    PrGpuFormat_R8Srgb,
    PrGpuFormat_R8Alpha,
    PrGpuFormat_R8Luminance,
    PrGpuFormat_R8Intensity,
    PrGpuFormat_R8Mask,
    PrGpuFormat_Rg8,
    PrGpuFormat_Rg8Snorm,
    PrGpuFormat_Rg8Uint,
    PrGpuFormat_Rg8Sint,
    PrGpuFormat_Rg8Srgb,
    PrGpuFormat_Rg8LuminanceAlpha,
    PrGpuFormat_Rgba8,
    PrGpuFormat_Rgba8Snorm,
    PrGpuFormat_Rgba8Uint,
    PrGpuFormat_Rgba8Sint,
    PrGpuFormat_Rgba8Srgb,
    PrGpuFormat_Bgra8,
    PrGpuFormat_Bgra8Srgb,

    // 16 bits a channel
    PrGpuFormat_R16,
    PrGpuFormat_R16Snorm,
    PrGpuFormat_R16Uint,
    PrGpuFormat_R16Sint,
    PrGpuFormat_R16F,
    PrGpuFormat_R16Alpha,
    PrGpuFormat_R16Luminance,
    PrGpuFormat_R16Intensity,
    PrGpuFormat_R16Mask,
    PrGpuFormat_R16FAlpha,
    PrGpuFormat_R16FLuminance,
    PrGpuFormat_R16FIntensity,
    PrGpuFormat_R16FMask,
    PrGpuFormat_Rg16,
    PrGpuFormat_Rg16Snorm,
    PrGpuFormat_Rg16Uint,
    PrGpuFormat_Rg16Sint,
    PrGpuFormat_Rg16F,
    PrGpuFormat_Rg16LuminanceAlpha,
    PrGpuFormat_Rg16FLuminanceAlpha,
    PrGpuFormat_Rgba16,
    PrGpuFormat_Rgba16Snorm,
    PrGpuFormat_Rgba16Uint,
    PrGpuFormat_Rgba16Sint,
    PrGpuFormat_Rgba16F,

    // 32 bits a channel
    PrGpuFormat_R32Uint,
    PrGpuFormat_R32Sint,
    PrGpuFormat_R32F,
    PrGpuFormat_R32FAlpha,
    PrGpuFormat_R32FLuminance,
    PrGpuFormat_R32FIntensity,
    PrGpuFormat_R32FMask,
    PrGpuFormat_Rg32Uint,
    PrGpuFormat_Rg32Sint,
    PrGpuFormat_Rg32F,
    PrGpuFormat_Rg32FLuminanceAlpha,
    PrGpuFormat_Rgba32Uint,
    PrGpuFormat_Rgba32Sint,
    PrGpuFormat_Rgba32F,

    // Packed into 16 or 32 bits
    PrGpuFormat_B5G6R5,
    PrGpuFormat_Bgr5A1,
    PrGpuFormat_Bgra4,
    PrGpuFormat_Rgb10A2,
    PrGpuFormat_Rgb10A2Uint,
    PrGpuFormat_Bgr10A2,
    PrGpuFormat_Rg11B10F,
    PrGpuFormat_Rgb9E5F,        // RGB of 9 bits each that share an exponent of 5

    // Depth and stencil
    PrGpuFormat_D16,
    PrGpuFormat_D24S8,
    PrGpuFormat_D32F,
    PrGpuFormat_D32FS8,
    PrGpuFormat_S8,

    // Block-compressed formats, which a device samples and does nothing else with. Their texels are stored as rows of
    // blocks from the first row of texels, and a level whose extent is not a multiple of the block's has whole blocks
    // that reach past it.

    // BC, with PrGpuFeatureFlag_TextureCompressionBc: blocks of 4 by 4 texels.
    PrGpuFormat_Bc1,            // RGB with one bit of alpha, in 8 bytes
    PrGpuFormat_Bc1Srgb,
    PrGpuFormat_Bc2,            // RGB as Bc1, and four bits of alpha a texel
    PrGpuFormat_Bc2Srgb,
    PrGpuFormat_Bc3,            // RGB as Bc1, and alpha as Bc4
    PrGpuFormat_Bc3Srgb,
    PrGpuFormat_Bc4,            // one channel, in 8 bytes
    PrGpuFormat_Bc4Snorm,
    PrGpuFormat_Bc4Alpha,
    PrGpuFormat_Bc4Luminance,
    PrGpuFormat_Bc4Intensity,
    PrGpuFormat_Bc4Mask,
    PrGpuFormat_Bc5,            // two channels, each as Bc4
    PrGpuFormat_Bc5Snorm,
    PrGpuFormat_Bc5LuminanceAlpha,
    PrGpuFormat_Bc6HUfloat,     // RGB of half floats, without a sign
    PrGpuFormat_Bc6HSfloat,     // and with one
    PrGpuFormat_Bc7,            // RGBA
    PrGpuFormat_Bc7Srgb,

    // ETC2 and EAC, with PrGpuFeatureFlag_TextureCompressionEtc2: blocks of 4 by 4 texels.
    PrGpuFormat_Etc2Rgb8,       // RGB, in 8 bytes. ETC1's blocks are blocks of it
    PrGpuFormat_Etc2Rgb8Srgb,
    PrGpuFormat_Etc2Rgb8A1,     // RGB with one bit of alpha, in 8 bytes
    PrGpuFormat_Etc2Rgb8A1Srgb,
    PrGpuFormat_Etc2Rgba8,      // RGB as Etc2Rgb8 after alpha as EAC, of 8 bits
    PrGpuFormat_Etc2Rgba8Srgb,
    PrGpuFormat_EacR11,         // one channel of 11 bits, in 8 bytes
    PrGpuFormat_EacR11Snorm,
    PrGpuFormat_EacR11Alpha,
    PrGpuFormat_EacR11Luminance,
    PrGpuFormat_EacR11Intensity,
    PrGpuFormat_EacR11Mask,
    PrGpuFormat_EacRg11,        // two channels, each as EacR11
    PrGpuFormat_EacRg11Snorm,
    PrGpuFormat_EacRg11LuminanceAlpha,

    // ASTC, in 16 bytes a block, by the texels of a block: three formats of each, in the order Unorm, Srgb and Hdr.
    // Unorm and Srgb read blocks of the LDR profile, with PrGpuFeatureFlag_TextureCompressionAstc. Hdr reads blocks of
    // either profile as floats, with PrGpuFeatureFlag_TextureCompressionAstcHdr.
    PrGpuFormat_Astc4x4,
    PrGpuFormat_Astc4x4Srgb,
    PrGpuFormat_Astc4x4Hdr,
    PrGpuFormat_Astc5x4,
    PrGpuFormat_Astc5x4Srgb,
    PrGpuFormat_Astc5x4Hdr,
    PrGpuFormat_Astc5x5,
    PrGpuFormat_Astc5x5Srgb,
    PrGpuFormat_Astc5x5Hdr,
    PrGpuFormat_Astc6x5,
    PrGpuFormat_Astc6x5Srgb,
    PrGpuFormat_Astc6x5Hdr,
    PrGpuFormat_Astc6x6,
    PrGpuFormat_Astc6x6Srgb,
    PrGpuFormat_Astc6x6Hdr,
    PrGpuFormat_Astc8x5,
    PrGpuFormat_Astc8x5Srgb,
    PrGpuFormat_Astc8x5Hdr,
    PrGpuFormat_Astc8x6,
    PrGpuFormat_Astc8x6Srgb,
    PrGpuFormat_Astc8x6Hdr,
    PrGpuFormat_Astc8x8,
    PrGpuFormat_Astc8x8Srgb,
    PrGpuFormat_Astc8x8Hdr,
    PrGpuFormat_Astc10x5,
    PrGpuFormat_Astc10x5Srgb,
    PrGpuFormat_Astc10x5Hdr,
    PrGpuFormat_Astc10x6,
    PrGpuFormat_Astc10x6Srgb,
    PrGpuFormat_Astc10x6Hdr,
    PrGpuFormat_Astc10x8,
    PrGpuFormat_Astc10x8Srgb,
    PrGpuFormat_Astc10x8Hdr,
    PrGpuFormat_Astc10x10,
    PrGpuFormat_Astc10x10Srgb,
    PrGpuFormat_Astc10x10Hdr,
    PrGpuFormat_Astc12x10,
    PrGpuFormat_Astc12x10Srgb,
    PrGpuFormat_Astc12x10Hdr,
    PrGpuFormat_Astc12x12,
    PrGpuFormat_Astc12x12Srgb,
    PrGpuFormat_Astc12x12Hdr,

    PrGpuFormat_Count,
};
typedef uint8_t PrGpuFormat;

// The number of block sizes of ASTC, and of formats of each.
enum {
    kPrGpuAstcBlockSizes = 14,
    kPrGpuAstcFormatsPerBlockSize = 3,
};

// Where a swizzled format places the channels that it stores, as the red, green, blue and alpha that a shader reads.
enum {
    PrGpuSwizzle_None,              // as stored
    PrGpuSwizzle_Alpha,             // (0, 0, 0, R)
    PrGpuSwizzle_Luminance,         // (R, R, R, 1)
    PrGpuSwizzle_Intensity,         // (R, R, R, R)
    PrGpuSwizzle_Mask,              // (1, 1, 1, R)
    PrGpuSwizzle_LuminanceAlpha,    // (R, R, R, G)
    PrGpuSwizzle_Count,
};
typedef uint8_t PrGpuSwizzle;

// The families of formats, which a device has as a whole or not at all.
enum {
    PrGpuFormatFamily_Plain,    // texels that are not compressed, which every device has in part
    PrGpuFormatFamily_Bc,
    PrGpuFormatFamily_Etc2,
    PrGpuFormatFamily_Astc,
    PrGpuFormatFamily_AstcHdr,
    PrGpuFormatFamily_Count,
};
typedef uint8_t PrGpuFormatFamily;

// The size of a block of a format: the texels that one block covers on each axis, and its bytes. A format that is not
// block-compressed has blocks of one texel.
typedef struct PrGpuFormatBlock {
    uint8_t width;
    uint8_t height;
    uint8_t bytes;
} PrGpuFormatBlock;

// Returns the block of `format`, which is not None.
PrGpuFormatBlock prGpuFormatBlock(PrGpuFormat format);

// Returns the family of `format`, and whether it is block-compressed: of any family but Plain.
PrGpuFormatFamily prGpuFormatFamily(PrGpuFormat format);
bool prGpuFormatIsCompressed(PrGpuFormat format);

// Returns the feature that a device with the formats of `family` reports, or zero for Plain.
PrGpuFeatureFlags prGpuFormatFamilyFeature(PrGpuFormatFamily family);

// Return the format that reads `format`'s texels as sRGB encoded, and the one that reads them as they are stored, or
// `format` itself if it has no such form.
PrGpuFormat prGpuFormatSrgb(PrGpuFormat format);
PrGpuFormat prGpuFormatPlain(PrGpuFormat format);

// Returns true if `format` is a Srgb format.
bool prGpuFormatIsSrgb(PrGpuFormat format);

// Returns the ASTC format of blocks of `block` texels, Unorm, or None if ASTC has no such block. Its Srgb and Hdr forms
// follow it.
PrGpuFormat prGpuFormatAstc(PrUVec2 block);

// Return the bytes of one row of blocks of a level of `width` texels, the rows of blocks of a level of `height` texels,
// and the bytes of a level of `extent` texels with nothing between its rows.
uint32_t prGpuFormatRowBytes(PrGpuFormat format, uint32_t width);
uint32_t prGpuFormatRows(PrGpuFormat format, uint32_t height);
uint64_t prGpuFormatLevelBytes(PrGpuFormat format, PrUVec2 extent);

// Returns the swizzle of `format`, None for a format that is not swizzled.
PrGpuSwizzle prGpuFormatSwizzle(PrGpuFormat format);

// Returns the format that `format` stores its texels in: the format that a swizzled one is named after, and otherwise
// `format` itself.
PrGpuFormat prGpuFormatStored(PrGpuFormat format);

// Returns the format that stores its texels in `stored` and reads them with `swizzle`, `stored` itself for None, or
// None if no format does: the swizzles of one channel are of R8, R16, R16F, R32F, Bc4 and EacR11, and LuminanceAlpha is
// of Rg8, Rg16, Rg16F, Rg32F, Bc5 and EacRg11.
PrGpuFormat prGpuFormatSwizzled(PrGpuFormat stored, PrGpuSwizzle swizzle);

// Returns true if `format` holds depth, stencil or both.
bool prGpuFormatIsDepthStencil(PrGpuFormat format);

// Returns true if every device that renders to `format` also filters it linearly, so that the device can make its mip
// levels from the first (prGpuCommandsGenerateMipmaps).
bool prGpuFormatFilters(PrGpuFormat format);

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
    PrGpuBufferUsageFlag_Storage = 1u << 0,         // bound to slots, and written by shaders
    PrGpuBufferUsageFlag_Index = 1u << 1,
    PrGpuBufferUsageFlag_Indirect = 1u << 2,
    PrGpuBufferUsageFlag_Vertex = 1u << 3,
    PrGpuBufferUsageFlag_All = PrGpuBufferUsageFlag_Storage | PrGpuBufferUsageFlag_Index | PrGpuBufferUsageFlag_Indirect |
        PrGpuBufferUsageFlag_Vertex,
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

// Take and drop a reference to the buffer. A buffer is created with one reference, and the release of its last destroys
// it: the work submitted before that release may still use it, and work submitted after must not. Each holder of a
// buffer, such as a graph that imports it, takes a reference of its own, so the creator releases its own whenever it is
// done. Any thread retains and releases. Zero is ignored.
void prGpuBufferRetain(PrGpuDevice* device, PrGpuBuffer buffer);
void prGpuBufferRelease(PrGpuDevice* device, PrGpuBuffer buffer);

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

// The shape of a texture. A layer is one element of an array, and a cube has six layers, one for each face: the faces
// toward +x, -x, +y, -y, +z and -z, in that order. A shader samples a cube along a direction, and the face that the
// direction's largest component names holds the texel. A face's columns and rows run as the cube maps of every API run.
// On the four faces around the y axis, rows run toward -y, and columns run toward -z on the face toward +x, +x on +z,
// +z on -x, and -x on -z. On the faces toward +y and -y, columns run toward +x, and rows toward +z and -z.
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

// How the bytes of a texture that a record describes are laid out.
enum {
    PrGpuTextureLayout_DeviceOptimal,   // as the device keeps a texture: nothing reads its bytes
    PrGpuTextureLayout_Linear,          // levels from the first, each its layers' rows of blocks with nothing between
    PrGpuTextureLayout_LinearPitch256,  // the same, with each row at a multiple of 256 bytes, as a copy takes them
    PrGpuTextureLayout_Count,
};
typedef uint8_t PrGpuTextureLayout;

// A texture's description in 64 bits, with no pointer, to store ahead of the texture's bytes: in a file, or in a table
// that says what every texture of a scene is before any is read. From the lowest bit, the first word holds the layout
// in 8 bits, the kind in 4, the format in 8, and in 11 the extent's depth less one, which is the depth of a 3D texture
// and the layers of any other. The second holds the width less one and the height less one in 14 bits each, and the
// levels less one in 4. The bits that are left are zero. A PrGpuFormat and a PrGpuTextureKind keep their values, so a
// stored record stays what it was.
typedef struct PrGpuTextureRecord {
    uint32_t words[2];
} PrGpuTextureRecord;

// The most that a record holds.
enum {
    kPrGpuTextureRecordMaxSide = 16384,
    kPrGpuTextureRecordMaxDepth = 2048,
    kPrGpuTextureRecordMaxLevels = 16,
};

// Makes the record of a texture of `desc` whose bytes are laid out as `layout`. A record holds neither a texture's
// uses, its samples nor its name. Returns false for a description that a record does not hold: of more than one sample,
// or past the most that a record holds.
bool prGpuTextureRecordMake(const PrGpuTextureDesc* desc, PrGpuTextureLayout layout, PrGpuTextureRecord* out);

// Returns the description that `record` holds, of one sample, for a texture that is sampled, and its layout in
// `*layout`. Returns false for a record that prGpuTextureRecordMake did not make: its description is then zero.
bool prGpuTextureRecordRead(PrGpuTextureRecord record, PrGpuTextureDesc* desc, PrGpuTextureLayout* layout);

// Returns the bytes of every level and layer of the texture that `record` describes, in its layout, or zero for the
// device's own.
uint64_t prGpuTextureRecordBytes(PrGpuTextureRecord record);

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
// work submitted after the call. The first level of a texture of a block-compressed format is whole blocks: its extent
// is a multiple of the block's, which some devices require, and the levels after it are as small as those of any
// texture. Returns Limit if the device has no memory or handle left for it, or the extent is larger than the device
// allows, NotSupported for a first level that is not whole blocks, and Lost if the device is lost. On failure, `*out`
// is zero.
PrStatus prGpuTextureCreate(PrGpuDevice* device, const PrGpuTextureDesc* desc, PrGpuTexture* out);

// Creates a view of `subresources` of `texture`, of shape `kind`. A view has handles and may be an attachment, and it
// shares its texture's memory. A view may outlive the destruction of its texture, which keeps the memory.
PrStatus prGpuTextureCreateView(PrGpuDevice* device, PrGpuTexture texture, PrGpuTextureKind kind,
    PrGpuSubresources subresources, PrGpuTexture* out);

// Take and drop a reference to the texture or view, as buffers' references are taken and dropped (prGpuBufferRetain).
// The release of the last destroys it: the work submitted before that release may still use it, and work submitted
// after must not.
void prGpuTextureRetain(PrGpuDevice* device, PrGpuTexture texture);
void prGpuTextureRelease(PrGpuDevice* device, PrGpuTexture texture);

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

// Copies texels from `data` into `region` of `texture`. `rowBytes` is the distance between rows of blocks in `data`. It
// is at least the bytes of one row of the region's blocks, and need not be a multiple of a block's bytes, so the rows
// of an image in memory are uploaded at their stride. `layerBytes` is the distance between layers or depth slices: a
// multiple of `rowBytes`, or zero for layers that follow one another. The command buffers submitted after the call read
// the new texels, and the upload waits for the work submitted before it. Several uploads to one texture before a
// submission land in order. The device copies `data` before the call returns.
//
// The texture is in the accesses of PrGpuAccessFlag_ShaderRead before and after the upload: work that left it in any
// other access ends with a barrier to them.
void prGpuTextureUpload(PrGpuDevice* device, PrGpuTexture texture, const PrGpuTextureRegion* region,
    const void* data, uint32_t rowBytes, uint32_t layerBytes);

// Writes `texels` into the first level of a 2D texture at `origin`, in the texture's format, as an upload. Texels
// outside the texture are dropped.
void prGpuTextureWrite(PrGpuDevice* device, PrGpuTexture texture, PrIVec2 origin, PrPixelView texels);

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

// The kinds of slot that a command buffer binds (prGpuCommandsSetBuffer and the others).
enum {
    PrGpuSlotKind_Buffer,
    PrGpuSlotKind_RwBuffer,
    PrGpuSlotKind_Texture,      // with its sampler
    PrGpuSlotKind_RwTexture,
    PrGpuSlotKind_Count,
};
typedef uint8_t PrGpuSlotKind;

// A slot that a library's source declares (pr/gpu.slang's PR_GPU_BUFFER and the others), by the name of its variable. A
// caller binds a resource that a shader reads by name at the slot that the source gave it, so it holds no number of its
// own that could drift from the source's. The name is valid while the library's bytes are.
typedef struct PrGpuShaderSlot {
    PrStringView name;
    PrGpuSlotKind kind;
    uint32_t slot;
} PrGpuShaderSlot;

// Writes up to `capacity` of the slots that the source of a library's bytes declares to `out`, and returns how many it
// declares. Returns zero for bytes that are not a library. Reads the bytes alone, so it needs no device.
uint32_t prGpuShaderLibrarySlots(PrByteView library, PrGpuShaderSlot* PR_NULLABLE out, uint32_t capacity);

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

// The formats of vertex attributes in vertex buffers, by the type and the number of their components, from the lowest
// address. A Float or Half attribute reads as floats, Unorm and Snorm as floats normalized to [0, 1] and [-1, 1], and
// Uint and Sint as integers.
enum {
    PrGpuVertexFormat_None,
    PrGpuVertexFormat_Float,
    PrGpuVertexFormat_Float2,
    PrGpuVertexFormat_Float3,
    PrGpuVertexFormat_Float4,
    PrGpuVertexFormat_Half2,
    PrGpuVertexFormat_Half4,
    PrGpuVertexFormat_Uint,
    PrGpuVertexFormat_Uint2,
    PrGpuVertexFormat_Uint3,
    PrGpuVertexFormat_Uint4,
    PrGpuVertexFormat_Sint,
    PrGpuVertexFormat_Sint2,
    PrGpuVertexFormat_Sint3,
    PrGpuVertexFormat_Sint4,
    PrGpuVertexFormat_Unorm8x4,
    PrGpuVertexFormat_Snorm8x4,
    PrGpuVertexFormat_Uint8x4,
    PrGpuVertexFormat_Unorm16x2,
    PrGpuVertexFormat_Unorm16x4,
    PrGpuVertexFormat_Snorm16x2,
    PrGpuVertexFormat_Snorm16x4,
    PrGpuVertexFormat_Count,
};
typedef uint8_t PrGpuVertexFormat;

// How a vertex buffer advances: by vertex, or by instance.
enum {
    PrGpuVertexStep_Vertex,
    PrGpuVertexStep_Instance,
};
typedef uint8_t PrGpuVertexStep;

// A vertex buffer of a pipeline: the stride in bytes between its elements, and how it advances.
typedef struct PrGpuVertexBufferLayout {
    uint32_t stride;
    PrGpuVertexStep step;
} PrGpuVertexBufferLayout;

// An attribute of the vertex shader's input: its format, the vertex buffer that it reads, and its offset in bytes into
// that buffer's elements, a multiple of 4.
typedef struct PrGpuVertexAttribute {
    PrGpuVertexFormat format;
    uint8_t buffer;
    uint16_t offset;
} PrGpuVertexAttribute;

// The vertex input of a pipeline, which the hardware fetches ahead of the vertex shader. Attribute k is the shader's
// input at location k (pr/gpu.slang's PR_GPU_ATTRIBUTE). A pipeline without attributes fetches nothing, and its shaders
// pull their vertices from buffers.
typedef struct PrGpuVertexLayout {
    uint8_t bufferCount;
    uint8_t attributeCount;
    PrGpuVertexBufferLayout buffers[kPrGpuMaxVertexBuffers];    // by vertex buffer
    PrGpuVertexAttribute attributes[kPrGpuMaxVertexAttributes]; // by location
} PrGpuVertexLayout;

// Returns the size in bytes of an attribute of `format`.
uint32_t prGpuVertexFormatSize(PrGpuVertexFormat format);

// A pipeline for draws. A null `fragment` makes a pipeline that writes only depth and stencil. Its vertex shader reads
// its vertices through the vertex layout, or pulls them from buffers through handles or slots.
typedef struct PrGpuGraphicsPipelineDesc {
    const PrGpuShaderLibrary* library;
    PrStringView vertex;
    PrStringView fragment;
    PrGpuVertexLayout vertexLayout;

    PrGpuTopology topology;
    PrGpuCull cull;
    bool frontClockwise;            // true if a triangle faces front when its vertices run clockwise on the target

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

// Take and drop a reference to the pipeline, as buffers' references are taken and dropped (prGpuBufferRetain). The
// release of the last destroys it, and the work submitted before that release may still use it.
void prGpuPipelineRetain(PrGpuDevice* device, PrGpuPipeline pipeline);
void prGpuPipelineRelease(PrGpuDevice* device, PrGpuPipeline pipeline);

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
    PrGpuNativeStateFlag_VertexBuffers = 1u << 8,
};
typedef uint16_t PrGpuNativeStateFlags;

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

// Allocates `size` bytes of the command buffer's stream: memory that the GPU reads, valid until the command buffer's
// work completes. Returns the bytes for the caller to write, the handle of the buffer that holds them in `*buffer`, and
// their offset in that buffer in `*offset`. Shaders may read the bytes through the buffer's handle. `usage` names the
// other ways that the GPU reads them, as a buffer's usage does, except that the GPU never writes them. The offset is a
// multiple of the device's storageOffsetAlignment (PrGpuDeviceInfo) for PrGpuBufferUsageFlag_Storage, and of 16
// otherwise.
//
// The stream grows as recording needs it, so the call always succeeds. Each call may return a different buffer. If the
// device has no memory left for the bytes, the device is lost, and the call returns memory that the GPU never reads.
void* prGpuCommandsStream(PrGpuCommands* commands, uint64_t size, PrGpuBufferUsageFlags usage, PrGpuBuffer* buffer,
    uint64_t* offset);

// Allocates the `count` parts of `parts` one after another in one buffer of the command buffer's stream, as
// prGpuCommandsStream allocates bytes for `usage`, and copies each part's bytes. A part with null data is left for the
// caller to write. Writes each part's offset in `*buffer` to `offsets`, which holds `count` offsets. Returns the bytes
// of the first part. Part `i` starts `offsets[i] - offsets[0]` bytes after them.
void* prGpuCommandsStreamGather(PrGpuCommands* commands, const PrByteView* parts, uint32_t count,
    PrGpuBufferUsageFlags usage, PrGpuBuffer* buffer, uint64_t* offsets);

// Sets the root data of the draws and dispatches recorded after the call. Push data is at most kPrGpuMaxPushBytes, and
// uniforms at most kPrGpuMaxUniformBytes. Both are copied.
void prGpuCommandsSetPush(PrGpuCommands* commands, const void* data, uint32_t size);
void prGpuCommandsSetUniforms(PrGpuCommands* commands, const void* data, uint32_t size);

// Sets the pipeline of the dispatches recorded after the call, or, in a pass, of the draws.
void prGpuCommandsSetPipeline(PrGpuCommands* commands, PrGpuPipeline pipeline);

// Bind resources to the command buffer's slots for the draws and dispatches recorded after the call, as root data is
// set. A slot is of one kind, numbered from zero below the device's maximum of its kind (PrGpuDeviceInfo), and is
// shared by every stage of a pipeline. A shader's source declares its slots (pr/gpu.slang's PR_GPU_BUFFER and the
// others). A zero buffer, texture or sampler unbinds it.
//
// Every entry point of a source has every slot that the source declares, whether or not its code reads the slot. So a
// draw or dispatch needs each of those slots bound: a slot's buffer or texture, and a Texture slot's sampler if the
// source samples the texture. Entry points that should not share slots are compiled from separate sources.
//
// A Buffer slot is read from `offset` bytes into the buffer, which is a multiple of the device's storageOffsetAlignment
// (PrGpuDeviceInfo). An RwBuffer slot may also be written. The buffer of either has PrGpuBufferUsageFlag_Storage, or is
// a buffer of the command buffer's stream allocated for it. A Texture slot is sampled with its sampler, or read by
// texel without it, and its texture has PrGpuTextureUsageFlag_Sampled. An RwTexture slot is read and written without a
// sampler, and its texture has PrGpuTextureUsageFlag_Storage.
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
// laid out as `rowBytes` and `layerBytes` say: rows of blocks `rowBytes` apart, a multiple of the bytes of the format's
// block, and layers `layerBytes` apart, a multiple of `rowBytes`, or zero for layers that follow one another. Copies
// are ordered with other work only by barriers: the source is in PrGpuAccessFlag_CopyRead and the target in
// PrGpuAccessFlag_CopyWrite. A copy into a texture reads from an Upload buffer, and a copy from a texture writes to a
// Readback buffer.
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
// call, and every level is in the accesses of PrGpuAccessFlag_ShaderRead after it. The call unbinds Texture slot 0.
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

    // Every access that writes a resource.
    PrGpuAccessFlag_Write = PrGpuAccessFlag_ShaderWrite | PrGpuAccessFlag_CopyWrite | PrGpuAccessFlag_ColorAttachment |
        PrGpuAccessFlag_DepthStencilWrite,

    // Every access that a pass makes to its attachments.
    PrGpuAccessFlag_Attachment = PrGpuAccessFlag_ColorAttachment | PrGpuAccessFlag_DepthStencilRead |
        PrGpuAccessFlag_DepthStencilWrite,
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
// recorded in a pass. Passes of the device's own are in order on their attachments: a pass loads and writes them after
// the passes before it in the queue have written them, so passes that use a texture only as an attachment need no
// barrier between them.
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

    // Resolves the samples into `resolve`, and keeps them, for a later pass that draws more over them and resolves
    // again.
    PrGpuStore_ResolveKeep,
};
typedef uint8_t PrGpuStore;

// An attachment: a texture, or one level and layer of it, and what the pass does with it.
typedef struct PrGpuAttachment {
    PrGpuTexture texture;
    PrGpuTexture resolve;   // the target of PrGpuStore_Resolve and PrGpuStore_ResolveKeep, or zero
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

// Sets the vertex buffer `slot`, below kPrGpuMaxVertexBuffers, of the draws recorded after the call, read from `offset`
// bytes into the buffer, a multiple of 4. The buffer has PrGpuBufferUsageFlag_Vertex, or is a buffer of the command
// buffer's stream. A native draw (prGpuPassNativeDraw) unsets every vertex buffer.
void prGpuPassSetVertexBuffer(PrGpuPass* pass, uint32_t slot, PrGpuBuffer buffer, uint64_t offset);

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

    // Presents the textures' alpha, with premultiplied colors, so that a window with a backdrop shows it where the
    // alpha is less than one (PrAppWindowBackdrop). Without the flag, the window shows the textures as opaque. A window
    // keeps the choice of its first swap chain: on Windows, a transparent swap chain on a window that has shown an
    // opaque one shows that one's last frame through it. A window that may show a backdrop takes the flag from its
    // first swap chain on.
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
// presents within the window's display transaction: from the thread that runs the window, during its frame
// (PrAppHandler::frame), so that a resize shows each frame at the window's size. Returns NotSupported for a kind of
// surface the device cannot present to, and Lost if the device is lost. On failure, `*out` is null.
PrStatus prGpuSwapChainCreate(PrGpuDevice* device, const PrAppSurface* surface, const PrGpuSwapChainDesc* desc,
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
