// Textures and the sources that produce them.
//
// A texture source describes a texture (PrTextureDesc) and produces its bits when a read asks for them. A source keeps
// the means to produce the bits, such as a file's path, and never the bits. A consumer can therefore make the texture
// again at any time, for example after a GPU device is lost.
//
// The simplest source is a function that fills the bits (prTextureSourceCreate). Other sources wrap a source and change
// its format. A format's library decodes or transcodes encoded bytes (prTextureSourceReformat). This library converts
// pixels (prTextureSourceCreateConvert) and makes mip levels (prTextureSourceCreateMips). A chain of sources is a load
// pipeline, and its consumer sees one source.
//
// A read goes into memory or into a texture on a device (PrTextureReadDesc). A source that reaches the device by a path
// of its own, such as the device's file I/O, fills the texture itself. For every other source, the library reads the
// bits into memory, uploads them and frees them.
//
// A source may have previews, which are smaller versions of its picture that read faster (prTextureSourcePreview).
//
// Threads. A source is immutable. Any number of threads may read it at once. A read completes on the thread of its
// innermost provider. Format sets are immutable too.

#pragma once

#include "pr/base.h"
#include "pr/blob.h"
#include "pr/byte_view.h"
#include "pr/color.h"
#include "pr/gpu.h"
#include "pr/pixels.h"
#include "pr/status.h"
#include "pr/string_view.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Descriptions

// The largest number of mip levels in a texture, which a side of 32768 texels has.
enum { kPrTextureMaxLevels = 16 };

// How a texture's alpha relates to its color channels.
enum {
    PrTextureAlpha_None,            // the texture is opaque, and its alpha reads as 1
    PrTextureAlpha_Straight,        // the color channels are not multiplied by alpha
    PrTextureAlpha_Premultiplied,   // the color channels are already multiplied by alpha
};
typedef uint8_t PrTextureAlpha;

// The channels of a texture.
enum {
    PrTextureChannelFlag_R = 1u << 0,
    PrTextureChannelFlag_G = 1u << 1,
    PrTextureChannelFlag_B = 1u << 2,
    PrTextureChannelFlag_A = 1u << 3,
};
typedef uint8_t PrTextureChannelFlags;

// How the stored texels map to the displayed picture. With Transpose, the displayed texel at (x, y) is the stored texel
// at (y, x). FlipX then mirrors the picture left to right, and FlipY top to bottom. A consumer that shows the picture
// upright applies the orientation. A consumer that samples the texture as data, such as a material, ignores it.
enum {
    PrTextureOrientationFlag_FlipX = 1u << 0,
    PrTextureOrientationFlag_FlipY = 1u << 1,
    PrTextureOrientationFlag_Transpose = 1u << 2,
};
typedef uint8_t PrTextureOrientationFlags;

// What a texture is.
//
// Layers are those of lib/gpu. A cube has six, one for each face, in the order +X, -X, +Y, -Y, +Z, -Z. An array of
// cubes has six for each element. Each layer has `levelCount` mip levels. Each level is half the size of the one
// before, rounded down, and at least one texel (prTextureLevelExtent).
//
// `format` is a GPU format: plain texels, compressed blocks, or a swizzled form of either (PrGpuSwizzle). The format is
// all that a texture says about color. A format's Srgb form holds values encoded with sRGB's transfer function, which a
// sampler decodes. A plain form holds values that a sampler reads as stored. A texture carries no primaries.
//
// A source uses the format that its bits are in. A source that cannot tell whether its values are color uses the plain
// form. A PNG is an example, because it may hold a normal map. A consumer that samples such values as color relabels
// them to the Srgb form (prTextureSourceCreateConvert).
typedef struct PrTextureDesc {
    PrGpuTextureKind kind;
    PrUVec2 extent;                     // of the first level
    uint32_t depth;                     // the depth of a 3D texture, or one
    uint32_t layerCount;                // at least one
    uint32_t levelCount;                // at least one, at most kPrTextureMaxLevels
    PrGpuFormat format;
    PrTextureAlpha alpha;
    PrTextureOrientationFlags orientation;
} PrTextureDesc;

// Returns the description of a 2D texture with one layer, one level and no orientation.
PrTextureDesc prTextureDesc2d(PrGpuFormat format, PrUVec2 extent, PrTextureAlpha alpha);

// Returns the extent of the displayed picture: the extent of the first level, with its axes swapped if the orientation
// transposes.
PrUVec2 prTextureDescDisplayedExtent(const PrTextureDesc* desc);

// Returns the extent of mip level `level` of a texture whose first level has `extent`.
PrUVec2 prTextureLevelExtent(PrUVec2 extent, uint32_t level);

// Returns the size in bytes of level `level` of one layer. The rows of texels or blocks are packed with no padding. The
// slices of a 3D texture follow one another.
size_t prTextureLevelSize(const PrTextureDesc* desc, uint32_t level);

// Limits on the textures that a format opens.
typedef struct PrTextureLimits {
    uint64_t pixels;                    // the most texels in the first level
    uint64_t workingBytes;              // the most bytes that a read holds at once, such as a level that it inflates
} PrTextureLimits;

// Returns limits that admit a texture of 8192 by 8192 texels.
PrTextureLimits prTextureLimitsDefault(void);

// ---------------------------------------------------------------------------------------------------------------------
// Sources

// Produces a texture's bits on demand. A source is immutable and reference counted.
typedef struct PrTextureSource PrTextureSource;

// One read of a source's bits. A read completes once.
typedef struct PrTextureRead PrTextureRead;

// Begins a read. It is called with the source's `context`. The provider writes the levels and layers that the read
// names (prTextureReadInfo) to the read's storage (prTextureReadStorage). It then completes the read
// (prTextureReadComplete), during the call or later, on any thread.
//
// A provider that reads encoded bytes reads them again from their byte provider (pr/blob.h) for each read.
//
// A provider that fills textures (PrTextureSourceFlag_FillsTextures) also receives reads into a texture. Such a read
// has no storage. The provider writes the levels and layers to the read's texture instead.
typedef void (*PrTextureSourceReadFn)(void* PR_NULLABLE context, PrTextureRead* read);

// Creates a source of the same texture in `format`, which is one of PrTextureSourceDesc::formats. It is called with the
// source's `context`. Returns the status that prTextureSourceReformat reports.
typedef PrStatus (*PrTextureSourceReformatFn)(void* PR_NULLABLE context, PrGpuFormat format,
    const PrTextureSource* PR_NULLABLE* PR_NONNULL out);

// Called with the source's `context` when the last reference to the source is released, on the releasing thread.
typedef void (*PrTextureSourceReleaseFn)(void* PR_NULLABLE context);

// The largest number of other formats and of previews that a source has.
enum {
    kPrTextureSourceMaxFormats = 32,
    kPrTextureSourceMaxPreviews = 16,
};

// What a source that changes over time shows at one time: an animation's frame, or a video's.
//
// `source` is the picture, a still source with the description of the source that it is a picture of. The caller holds
// a reference to it. `serial` names the picture among the pictures of its source: two answers with the same serial are
// the same picture. `next` is the time at which the source shows another picture, on the clock of the times that the
// source is asked with, or zero if it shows this picture from then on.
typedef struct PrTextureFrame {
    const PrTextureSource* source;
    uint64_t serial;
    double next;
} PrTextureFrame;

// Writes what a source shows at `time`, in seconds on the caller's clock, to `*out`. A null `out->source` stands for
// the source itself, which is the first picture of an animation. The provider keeps what it needs between calls, such
// as the time at which an animation began. Any thread may call it.
//
// Pictures that are read in order read fastest: a format that stores a picture as its difference from the one before
// composes them as it goes.
typedef void (*PrTextureSourceFrameFn)(void* PR_NULLABLE context, double time, PrTextureFrame* out);

// How a source's provider serves reads.
enum {
    // The provider fills the texture of a read into a texture (PrTextureReadDesc::device). It writes the texture on the
    // device, with no copy of the bits in main memory. A procedural source does this with a compute shader or with
    // render passes. Without the flag, the library serves a read into a texture: it reads the bits into memory, uploads
    // them and frees them.
    PrTextureSourceFlag_FillsTextures = 1u << 0,
};
typedef uint8_t PrTextureSourceFlags;

// What a source is created with. `context` is passed to the functions, and stays valid until `release` is called.
typedef struct PrTextureSourceDesc {
    PrTextureDesc texture;
    PrTextureSourceFlags flags;

    // The uses that a texture must have for the provider to fill it, beside being sampled. A compute shader needs
    // Storage, and render passes need RenderTarget. It is zero for a source that does not fill textures. A consumer
    // creates the texture with these uses, in the source's own format, and gives it a texture of its own.
    PrGpuTextureUsageFlags usage;

    PrTextureSourceReadFn read;

    // The other formats that the source can produce by decoding or transcoding, and the function that creates a source
    // in one of them. At most kPrTextureSourceMaxFormats. `texture.format` is not listed.
    PrTextureSourceReformatFn PR_NULLABLE reformat;
    const PrGpuFormat* PR_NULLABLE formats;
    uint32_t formatCount;

    // Smaller versions of the same picture, smallest first. Each is a source with one level that reads faster than this
    // source. Examples are an embedded thumbnail and a reduced decode. Each preview has this source's kind, layers,
    // alpha and orientation. The source retains its previews. At most kPrTextureSourceMaxPreviews.
    const PrTextureSource* PR_NONNULL const* PR_NULLABLE previews;
    uint32_t previewCount;

    // The function that says what the source shows at a time, for a source whose picture changes: an animated image, or
    // a video. It is null for a still source. The source's own description and reads are those of its first picture,
    // which is what a consumer that does not play it shows.
    PrTextureSourceFrameFn PR_NULLABLE frame;

    PrTextureSourceReleaseFn PR_NULLABLE release;
    void* PR_NULLABLE context;
} PrTextureSourceDesc;

// Creates a source. The source copies `desc->formats` and retains `desc->previews`. The caller holds the one reference.
const PrTextureSource* prTextureSourceCreate(const PrTextureSourceDesc* desc);

// Creates a source that keeps a copy of `pixels`. `desc` describes a 2D texture with one level and one layer, in a
// plain format or a swizzled form of one, with the extent of `pixels`. If `desc->alpha` is None, the copy's alpha is
// set to one.
//
// Use this for pixels that nothing can produce again, such as a canvas's snapshot. A source of a file should read the
// file instead, so that no copy of its pixels stays in memory. The caller holds the one reference.
const PrTextureSource* prTextureSourceCreatePixels(const PrTextureDesc* desc, PrPixelView pixels);

// Creates a source that keeps a copy of `pixels` as a 2D texture of `format` and `alpha` (prTextureSourceCreatePixels).
const PrTextureSource* prTextureSourceCreateCopy(PrPixelView pixels, PrPixelFormat format, PrTextureAlpha alpha);

// Add and release a reference. The last release calls the source's `release` function. A source that wraps another
// source releases it then.
void prTextureSourceRetain(const PrTextureSource* source);
void prTextureSourceRelease(const PrTextureSource* PR_NULLABLE source);

// Returns the source's description, which never changes.
const PrTextureDesc* prTextureSourceDesc(const PrTextureSource* source);

// Returns the number of references to the source. A cache uses it to tell whether anything else holds the source.
// Another thread may change the count at any time.
uint32_t prTextureSourceReferences(const PrTextureSource* source);

// Returns a number that identifies the source. No other source in the process ever has the same number.
uint64_t prTextureSourceId(const PrTextureSource* source);

// Writes up to `capacity` of the source's other formats to `out` and returns their number. These are the formats that
// the source produces directly, by transcoding to other blocks or decoding to pixels of another precision. The source's
// own format is not among them.
uint32_t prTextureSourceFormats(const PrTextureSource* source, PrGpuFormat* PR_NULLABLE out, uint32_t capacity);

// Returns true if `format` is the source's own format or one of its other formats.
bool prTextureSourceProduces(const PrTextureSource* source, PrGpuFormat format);

// Return the number of the source's previews, and the preview at `index`. Previews are ordered smallest first. A
// preview is valid while the source is.
//
// A consumer that shows the picture smaller than it is reads the smallest preview that is large enough. A consumer that
// waits for the picture shows a preview until the picture arrives. The texture cache does both
// (PrTextureCacheAsk::detail).
uint32_t prTextureSourcePreviewCount(const PrTextureSource* source);
const PrTextureSource* prTextureSourcePreview(const PrTextureSource* source, uint32_t index);

// Returns true if the source's picture changes over time (PrTextureSourceDesc::frame).
bool prTextureSourcePlays(const PrTextureSource* source);

// Writes what the source shows at `time` to `*out` (PrTextureSourceFrameFn). `out->source` is never null: it is the
// source itself for the first picture, and for a still source. The caller releases it.
void prTextureSourceFrameAt(const PrTextureSource* source, double time, PrTextureFrame* out);

// Creates a source of the same texture in `format` and writes it to `*out`. If `format` is the source's own format, the
// result is the source itself, with a new reference. Otherwise the format's library creates a source that decodes or
// transcodes to `format`. Returns NotSupported if the source does not produce `format`. On failure, `*out` is null.
PrStatus prTextureSourceReformat(const PrTextureSource* source, PrGpuFormat format,
    const PrTextureSource* PR_NULLABLE* PR_NONNULL out);

// ---------------------------------------------------------------------------------------------------------------------
// Reads

// Called once when a read completes, on the thread that completes it.
typedef void (*PrTextureReadDoneFn)(void* PR_NULLABLE context, PrTextureRead* read);

// What a read asks for, and where the bits go. The read covers `levelCount` levels from `firstLevel` and `layerCount`
// layers from `firstLayer`. `done` is called when the read completes, if it is not null.
//
// A read into memory has a null `device`. The bits go to `storage`. If `storage` is null, the read allocates the
// storage and frees it when the read is destroyed. The storage holds prTextureReadSize bytes. The levels are in order.
// Within a level, the layers are in order, and each takes prTextureLevelSize bytes.
//
// A read into a texture names a `device` and a `texture` on it, and has a null `storage`. The texture has the source's
// format, or a format with the same bits, such as its other sRGB form. Level `firstLevel + i` of the source goes to
// level `i` of the texture. Its first texel goes to `origin`, halved `i` times. Layer `firstLayer + j` goes to layer
// `j`.
//
// A consumer that samples on a device reads into a texture. A source that fills textures then uses its own path to the
// device (PrTextureSourceFlag_FillsTextures). For any other source, the library uploads the bits and frees them before
// the read completes.
typedef struct PrTextureReadDesc {
    uint32_t firstLevel;
    uint32_t levelCount;
    uint32_t firstLayer;
    uint32_t layerCount;
    uint8_t* PR_NULLABLE storage;
    PrGpuDevice* PR_NULLABLE device;
    PrGpuTexture texture;
    PrIVec2 origin;
    PrTextureReadDoneFn PR_NULLABLE done;
    void* PR_NULLABLE context;              // passed to `done`
} PrTextureReadDesc;

// Returns the description of a read of every level and layer of `desc` into storage that the read allocates, with no
// callback.
PrTextureReadDesc prTextureReadDescAll(const PrTextureDesc* desc);

// Return the size in bytes of the storage of a read into memory, and the offset of one layer of one level within it.
size_t prTextureReadSize(const PrTextureDesc* desc, const PrTextureReadDesc* read);
size_t prTextureReadOffset(const PrTextureDesc* desc, const PrTextureReadDesc* read, uint32_t level, uint32_t layer);

// Starts a read of the source. The read retains the source. The caller destroys the read after it completes
// (prTextureReadDestroy), in `done` or later.
PrTextureRead* prTextureReadCreate(const PrTextureSource* source, const PrTextureReadDesc* desc);

// Returns true if the read has completed. Any thread may call it.
bool prTextureReadIsDone(const PrTextureRead* read);

// Blocks the calling thread until the read completes.
void prTextureReadWait(PrTextureRead* read);

// Returns the status of a completed read: Ok, or the provider's failure.
PrStatus prTextureReadStatus(const PrTextureRead* read);

// Returns the storage of a read into memory, which holds the bits after the read succeeds. It is valid until the read
// is destroyed. A completed read into a texture has no storage, and returns null.
uint8_t* PR_NULLABLE prTextureReadStorage(const PrTextureRead* read);

// Frees a completed read, and its storage if the read allocated it.
void prTextureReadDestroy(PrTextureRead* PR_NULLABLE read);

// For a provider. Return the source that the read reads, and what the read asks for.
const PrTextureSource* prTextureReadSource(const PrTextureRead* read);
const PrTextureReadDesc* prTextureReadInfo(const PrTextureRead* read);

// For a provider. Completes the read with `status`. Pass Ok once the storage or the texture holds the bits. The read's
// `done` callback runs during the call. A read must be completed exactly once.
void prTextureReadComplete(PrTextureRead* read, PrStatus status);

// ---------------------------------------------------------------------------------------------------------------------
// Sources that wrap a source

// Constants for PrTextureConvertDesc::channels: the target channel is zero or one, and takes no source channel.
enum {
    kPrTextureChannelZero = 4,
    kPrTextureChannelOne = 5,
};

// A conversion of a source's pixels.
//
// The target has `format`, which is a plain format or a swizzled form of one, and `alpha`. Target channel `c`, counted
// from red, takes source channel `channels[c]` as a sampler reads it. The value is 0 to 3, or one of the constants
// above.
//
// Between two 8-bit formats, the values keep their encoding. A target in a format's Srgb form over plain values only
// states that the values are sRGB encoded. Between 8 bits and floats, the values convert as a sampler reads them: an
// Srgb form decodes to linear floats, and floats encode into an Srgb form.
//
// The transfer function is applied to straight alpha. Premultiplied values are divided by alpha first, and multiplied
// again afterwards if the target is premultiplied.
typedef struct PrTextureConvertDesc {
    PrGpuFormat format;
    PrTextureAlpha alpha;
    uint8_t channels[4];
} PrTextureConvertDesc;

// Returns a conversion to `format` that keeps the source's alpha, channels and values.
PrTextureConvertDesc prTextureConvertDescDefault(PrGpuFormat format);

// Creates a source that converts `source` as `desc` says. The new source retains `source`.
//
// A conversion that changes only the sRGB form or the swizzle is a relabel. It passes the source's bits through
// unchanged, and it works for any format, including blocks. Any other conversion needs plain formats or their swizzled
// forms.
const PrTextureSource* prTextureSourceCreateConvert(const PrTextureSource* source, const PrTextureConvertDesc* desc);

// Creates a source with `levelCount` mip levels, or with every level down to one texel if `levelCount` is zero. The new
// source retains `source`. Each level that `source` lacks is the average of two by two texels of the level before. An
// Srgb form is averaged in linear light. `source` must have a plain format or a swizzled form of one.
const PrTextureSource* prTextureSourceCreateMips(const PrTextureSource* source, uint32_t levelCount);

// One tile of a grid of tiles: its source and its cell. The cell is a column from the left and a row from the top.
typedef struct PrTextureTile {
    const PrTextureSource* source;
    PrUVec2 cell;
} PrTextureTile;

// A texture made of a grid of tiles, such as the UDIM tiles of one image (the MaterialX Specification, "Filename
// Substitutions"). Every cell has the extent of the largest tile on each axis. That extent is halved until the grid
// fits within `maxExtent` texels on each side.
typedef struct PrTextureTilesDesc {
    const PrTextureTile* PR_NULLABLE tiles;
    uint32_t tileCount;
    PrUVec2 grid;               // columns and rows
    uint32_t maxExtent;
} PrTextureTilesDesc;

// Creates a source of a 2D texture with one level that holds the tiles in their cells. The new source retains the
// tiles' sources.
//
// The first level of a tile fills its cell. If their extents differ, the tile is resampled: a cell texel is the average
// of the tile texels that it covers, or a linear interpolation if it covers less than one. A cell with no tile is
// transparent black.
//
// The texture is Rgba8 if every tile decodes to 8 bits a channel, and the values keep their encoding. Otherwise it is
// Rgba16F, with the values that a sampler reads. The alpha is straight if any tile has alpha. A tile that decodes to no
// plain format is left out. Returns null if no tile is left.
const PrTextureSource* PR_NULLABLE prTextureSourceCreateTiles(const PrTextureTilesDesc* desc);

// ---------------------------------------------------------------------------------------------------------------------
// Format sets
//
// A format makes sources from the encoded bytes of one file format. It is a backend of this library
// (pr/texture_backend.h), and it lives in the library of its file format. lib/ktx makes the format of KTX 2.0
// (pr/ktx_texture_source.h). lib/image makes the format of every image that an image codec set decodes
// (pr/image_texture_source.h).
//
// A program builds a set of the formats that it wants, or takes the selector's set of every format
// (pr/texture_select.h). It gives the set to the libraries that load textures, so those libraries depend on no file
// format.

// Makes sources from the bytes of one file format.
typedef struct PrTextureFormat PrTextureFormat;

// A list of formats in priority order.
typedef struct PrTextureFormatSet PrTextureFormatSet;

// Frees a format that no set owns.
void prTextureFormatDestroy(PrTextureFormat* PR_NULLABLE format);

// Returns true if the format reads the MIME type `type`. `type` is lowercase and has no parameters.
bool prTextureFormatSupportsType(const PrTextureFormat* format, PrStringView type);

// Creates a set of `count` formats, in priority order, and takes ownership of them. The set never changes, so any
// thread may use it.
PrTextureFormatSet* prTextureFormatSetCreate(PrTextureFormat* PR_NONNULL const* PR_NULLABLE formats, uint32_t count);

// Frees the set and its formats. Sources that the formats made stay valid.
void prTextureFormatSetDestroy(PrTextureFormatSet* PR_NULLABLE set);

// Returns true if a format in the set reads the MIME type `type`.
bool prTextureFormatSetSupportsType(const PrTextureFormatSet* set, PrStringView type);

// Receives the result of an open. On success, `source` is the new source, and the callee owns a reference to it. On
// failure, `source` is null. It runs on the thread that completed the read of the bytes. That may be the thread that
// called prTextureFormatSetOpen, during the call.
typedef void (*PrTextureOpenFn)(void* PR_NULLABLE context, PrStatus status, const PrTextureSource* PR_NULLABLE source);

// Opens the resource that `bytes` provides as a source, and passes the result to `done`.
//
// The call reads the bytes, which may complete later (pr/blob.h). It classifies the first bytes and `name`
// (pr/mime_sniff.h). The first format in the set that reads the media type makes the source. If a format returns
// NotSupported for the bytes, the next format tries. The source retains `bytes` and reads them again for each read.
//
// A null `limits` means the defaults. The status is the failure of reading the bytes, NotSupported if no format opens
// them, Limit if the texture is past the limits, or InvalidData if the bytes do not parse.
void prTextureFormatSetOpen(const PrTextureFormatSet* set, const PrBlobSource* bytes, PrStringView name,
    const PrTextureLimits* PR_NULLABLE limits, PrTextureOpenFn done, void* PR_NULLABLE context);

// Opens a source as prTextureFormatSetOpen does, and blocks the calling thread until the bytes are read. Returns the
// same status. On failure, `*out` is null.
PrStatus prTextureFormatSetRead(const PrTextureFormatSet* set, const PrBlobSource* bytes, PrStringView name,
    const PrTextureLimits* PR_NULLABLE limits, const PrTextureSource* PR_NULLABLE* PR_NONNULL out);

// ---------------------------------------------------------------------------------------------------------------------
// Textures

// A texture as a shader samples it: a GPU texture and its alpha. The GPU texture's format says how a sampler reads the
// values. An Srgb form is decoded.
typedef struct PrTexture {
    PrGpuTexture texture;
    PrTextureAlpha alpha;
} PrTexture;

// The part of a texture that holds one source's picture. A consumer samples only `rect`. The rect may be the whole
// texture, or a part of a texture that other sources share.
//
// The rect holds the whole picture. Its texels are those of the version that was realized, which is the source or one
// of its previews (prTextureSourcePreview). A consumer that addresses the picture in the texels of the source converts
// with prTextureRegionRect.
typedef struct PrTextureRegion {
    PrTexture texture;
    PrUVec2 extent;             // of the texture's first level
    uint32_t layer;
    uint32_t levelCount;        // the mip levels of the texture. A consumer minifies from them if there are several.
    PrRect rect;                // in texels of the texture
    PrUVec2 source;             // the extent of the source's first level, or zero if the rect is in the source's texels

    // True if the region is of a picture of a source that plays (prTextureCachePlay). A later ask may answer with
    // another picture, so a consumer that keeps a drawing of the region asks again when the cache reports an arrival.
    bool playing;
} PrTextureRegion;

// Converts `rect` from the texels of the source to the texels of the region's version, and returns it relative to the
// region's texture.
//
// `rect` is in the picture as `orientation` displays it. Pass zero for a rect in stored texels. With an orientation
// that transposes, the rect's horizontal axis is the stored vertical axis, and each axis is scaled by the version's
// extent along the stored axis that it shows. The result is in displayed texels of the version, offset by the region's
// origin. A consumer that draws the picture upright samples that rect.
PrRect prTextureRegionRect(const PrTextureRegion* region, PrRect rect, PrTextureOrientationFlags orientation);

// Options of an upload.
enum {
    // If the source has one level, the texture gets every level down to one texel, and the device makes them from the
    // first. This needs a format that the device filters and renders to.
    PrTextureUploadFlag_MakesLevels = 1u << 0,

    // The texture can also be viewed through the other sRGB form of its format (PrGpuTextureUsageFlag_SrgbViews), if
    // the format has both forms and the device allows it.
    PrTextureUploadFlag_SrgbViews = 1u << 1,
};
typedef uint8_t PrTextureUploadFlags;

// Receives the result of an upload. On success, the callee owns the reference to `texture`. On failure, `texture` is
// zero. It runs on the thread that completed the read of the source.
typedef void (*PrTextureUploadFn)(void* PR_NULLABLE context, PrStatus status, PrTexture texture);

// Creates a sampled GPU texture for `source` on `device`, reads every level and layer of the source into it, and passes
// the texture to `done`. The read may complete later. `name` labels the GPU texture for tools.
//
// The upload keeps none of the bits. Uploading the source again, as after a lost device, reads them again.
//
// The status is the failure of the read, NotSupported if the device does not sample the format, a failure of
// prGpuTextureCreate, or Lost if the device is lost before the texture is filled.
void prTextureUpload(PrGpuDevice* device, const PrTextureSource* source, PrTextureUploadFlags flags, PrStringView name,
    PrTextureUploadFn done, void* PR_NULLABLE context);

// Uploads `source` as prTextureUpload does, and blocks the calling thread until the texture is filled. Returns the same
// status. On failure, `*out` is zero.
PrStatus prTextureUploadRead(PrGpuDevice* device, const PrTextureSource* source, PrTextureUploadFlags flags,
    PrStringView name, PrTexture* out);

// Returns the size in bytes of the GPU texture that prTextureUpload creates for `desc` with `flags` on `device`. The
// size covers every level of every layer, including levels that the device makes.
uint64_t prTextureUploadSize(const PrGpuDevice* device, const PrTextureDesc* desc, PrTextureUploadFlags flags);

// ---------------------------------------------------------------------------------------------------------------------
// Helpers for sources

// The type of one channel of a stored pixel, in the machine's byte order. Unsigned integers map to [0, 1]. For I16,
// -32767 to 32767 maps to [-1, 1].
enum {
    PrTextureSample_U8,
    PrTextureSample_U16,
    PrTextureSample_I16,
    PrTextureSample_F16,
    PrTextureSample_F32,
    PrTextureSample_U32,
};
typedef uint8_t PrTextureSample;

// The channels of a stored pixel, in order.
enum {
    PrTextureChannels_Gray,
    PrTextureChannels_GrayAlpha,
    PrTextureChannels_Rg,
    PrTextureChannels_Rgb,
    PrTextureChannels_Rgba,
};
typedef uint8_t PrTextureChannels;

// Returns the size in bytes of one stored pixel.
uint32_t prTexturePixelSize(PrTextureChannels channels, PrTextureSample sample);

// Unpacks the stored pixels at `stored`, which have `channels` and `sample`, into `out`. `out` has the same extent and
// has `format`: R8, Rgba8, Rgba16F or Rgba32F.
//
// The values are not converted. Gray goes to red, green and blue. Alpha is one if the pixels store none. R8 takes the
// first channel only, which is gray or red. An 8-bit format clamps each value to [0, 1] and rounds it to the nearest of
// 255 steps.
void prTexturePixelsUnpack(PrPixelView stored, PrTextureChannels channels, PrTextureSample sample, PrGpuFormat format,
    PrPixels out);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
