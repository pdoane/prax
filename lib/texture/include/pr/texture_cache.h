// A texture cache holds the GPU textures that texture sources produce on one device. A host creates one cache for each
// device and gives it to every consumer that samples on that device: lib/draw's renderers, the layout tree, Studio's
// views, and a 3D renderer. Two consumers that read an image the same way share one texture.
//
// Asking. A consumer asks for a source each time it samples it (prTextureCacheRealize). The ask states what the
// consumer reads (PrTextureCacheAsk). To realize a source is to create its texture: the cache chooses the format,
// creates the texture and reads the source's bits into it (prTextureUpload). The first ask queues the realization.
// Later asks return the region of the texture once it has arrived.
//
// Detail. An ask states how many texels the consumer shows (PrTextureCacheAsk::detail). A source may have previews,
// which are smaller versions of its picture (prTextureSourcePreview). The cache realizes the smallest version that has
// the detail. If that version has not arrived and a smaller one has, the ask returns the smaller one
// (PrTextureCacheState_Partial).
//
// Order. Realizations wait in a queue. The cache keeps at most as many in flight as its task pool is wide. It starts
// the waiting realization with the highest priority first. Among equal priorities, it starts the one asked for in the
// latest frame, and then the smaller version. A trim cancels each waiting realization that no ask of the last frame
// asked for (prTextureCacheTrim). A consumer that scrolls past many images therefore reads only the ones that it still
// shows.
//
// Threads. Any thread may ask, wait and trim. Realizations run on the task pool that the host gave the cache. Without a
// pool, a realization runs inside the ask that starts it, on the asking thread. The cache calls the host's `arrived`
// function when a realization that an ask was waiting for arrives or fails, so that the host draws again.
//
// Lifetime. The cache keeps a region while anything else holds its source, because a drawing that keeps the source may
// still draw the region. If only the cache holds the source, a trim may evict the region once the cache's textures
// exceed its budget.
//
// Placement. A region is a whole texture, or a slot in a page that small sources share. The cache decides. A consumer
// samples only the region's rect.
//
// Device loss. The host moves the cache to a new device (prTextureCacheSetDevice). Each source is realized again there
// when a consumer next asks for it. The bits come from the source, so the texture of a file comes from the file. The
// cache keeps no copy of a texture in memory.

#pragma once

#include "pr/base.h"
#include "pr/gpu.h"
#include "pr/status.h"
#include "pr/task.h"
#include "pr/texture.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The textures that sources produce on one device.
typedef struct PrTextureCache PrTextureCache;

// Options of an ask.
enum {
    // The consumer samples mip levels where it minifies. The cache decides whether the texture needs them. It makes
    // every level, down to one texel, if the version that it realizes has more texels than the ask's detail, or if the
    // detail is zero. Otherwise the texture has the levels of its source. PrTextureRegion::levelCount tells the
    // consumer what it got. A texture with made levels is a texture of its own, not a slot in a page, and it also
    // serves asks without the flag.
    PrTextureCacheFlag_Levels = 1u << 0,
};
typedef uint8_t PrTextureCacheFlags;

// How a consumer reads a texture's values. This decides the texture's format and how the cache filters the mip levels
// that it makes.
enum {
    // Color that a shader reads as stored, encoded with sRGB's transfer function. lib/draw blends encoded color. On a
    // device that views a texture through both of its sRGB forms (PrGpuTextureUsageFlag_SrgbViews), 8-bit values share
    // the texture of a Color ask, and the region names it through its plain form. On other devices they are stored in a
    // plain format, and made levels average the encoded values. Floats are stored as half floats in extended sRGB,
    // mirrored below zero.
    PrTextureCacheValues_Encoded,

    // Color that a shader reads in linear light, such as a material's base color. 8-bit values are stored in a format's
    // Srgb form, which a sampler decodes. Floats are stored as linear half floats. Made levels average in linear light.
    PrTextureCacheValues_Color,

    // Data that a shader reads as stored, such as normals or roughness. Values are stored in a plain format, and floats
    // as half floats. Made levels average the stored values.
    PrTextureCacheValues_Data,
};
typedef uint8_t PrTextureCacheValues;

// How soon a consumer needs a texture. The cache starts waiting realizations in this order.
enum {
    PrTextureCachePriority_Normal,
    PrTextureCachePriority_Low,         // not shown yet, such as the images next to the one on screen
    PrTextureCachePriority_High,        // the user is waiting for it: it starts at once, even if the pool is full
};
typedef uint8_t PrTextureCachePriority;

// What a consumer reads of a source's texture. Asks that need the same texture share one realization:
//
// - An Encoded ask for 8-bit values shares the texture of a Color ask on a device with sRGB views, and the texture of a
//   Data ask on other devices.
// - `alpha` does not matter for an opaque source.
// - A Data ask that reads one channel may be stored in one channel, apart from asks that read more.
typedef struct PrTextureCacheAsk {
    PrTextureCacheFlags flags;
    PrTextureCacheValues values;
    PrTextureAlpha alpha;               // Straight or Premultiplied. The texture of an opaque source stays None.
    PrTextureChannelFlags channels;     // the channels that the consumer reads, or zero for all of them
    PrTextureCachePriority priority;

    // The number of texels that the consumer shows along the texture's longer side, or zero for all of them. The cache
    // realizes the smallest version of the source that has this many. A larger version that the cache already holds
    // also serves the ask.
    uint32_t detail;
} PrTextureCacheAsk;

// Returns the ask that lib/draw makes: Encoded values, premultiplied alpha, every channel, full detail, normal
// priority, with `flags`.
PrTextureCacheAsk prTextureCacheAskEncoded(PrTextureCacheFlags flags);

// The answer to an ask.
enum {
    PrTextureCacheState_Ready,      // the region holds a version with the detail that the ask needs
    PrTextureCacheState_Partial,    // the needed version has not arrived, and the region holds a smaller one
    PrTextureCacheState_Pending,    // no version of the source has arrived
    PrTextureCacheState_Failed,     // the source did not read or upload. The cache does not try again.
};
typedef uint8_t PrTextureCacheState;

// Whether the cache packs small sources into shared pages.
enum {
    PrTextureCachePack_Auto,        // on a device that draws through a GPU, and not on a memory device
    PrTextureCachePack_Never,
    PrTextureCachePack_Always,      // every source that fits a page
};
typedef uint8_t PrTextureCachePack;

// Called with the host's context when a realization that an ask was waiting for arrives or fails. It runs on the thread
// that completed the realization. It must not call the cache.
typedef void (*PrTextureCacheArrivedFn)(void* PR_NULLABLE context);

// What a cache is created with.
typedef struct PrTextureCacheDesc {
    // The device that the cache realizes on. It outlives the cache, or prTextureCacheSetDevice replaces it. Null
    // creates a cache without a device, which the host gives one later.
    PrGpuDevice* PR_NULLABLE device;
    PrTaskPool* PR_NULLABLE tasks;              // runs the realizations, and outlives the cache
    uint32_t concurrency;                       // the most realizations in flight, or zero for the pool's width
    uint64_t budget;                            // bytes of textures and pages past which a trim evicts
    PrTextureCachePack pack;
    PrTextureCacheArrivedFn PR_NULLABLE arrived;
    void* PR_NULLABLE arrivedContext;
} PrTextureCacheDesc;

// Returns the description of a cache on `device` with no pool, a budget of 256 MiB, and Auto packing.
PrTextureCacheDesc prTextureCacheDescDefault(PrGpuDevice* PR_NULLABLE device);

// Creates a cache.
PrTextureCache* prTextureCacheCreate(const PrTextureCacheDesc* desc);

// Frees the cache and its textures. Cancels the waiting realizations first, and blocks until those in flight complete.
void prTextureCacheDestroy(PrTextureCache* PR_NULLABLE cache);

// Returns the device that the cache realizes on, or null while it has none.
PrGpuDevice* PR_NULLABLE prTextureCacheDevice(const PrTextureCache* cache);

// Moves the cache to `device`, as after the old device is lost. Cancels the waiting realizations, blocks until those in
// flight complete, and frees every texture on the old device. Each source is realized again on `device` when it is next
// asked for. Failures are forgotten too. Every region that the cache returned becomes invalid, and the cache's
// generation advances. No other thread may ask during the call.
//
// A null `device` leaves the cache without a device, for the time between the loss of one device and the creation of
// the next. The host may then free the old device. A cache without a device answers every ask with Pending and starts
// nothing, and prTextureCacheWait must not be called.
void prTextureCacheSetDevice(PrTextureCache* cache, PrGpuDevice* PR_NULLABLE device);

// Returns the number of times that the cache has moved to another device. A host that keeps drawings of regions records
// the generation when it draws them. If the generation has changed, the regions are invalid, and the host draws again.
uint64_t prTextureCacheGeneration(const PrTextureCache* cache);

// Returns the number of realizations that have arrived since the cache was created. A consumer that kept a drawing made
// while an ask was Pending or Partial records the count with it. If the count has changed, a texture has arrived, and
// the consumer asks and draws again.
uint64_t prTextureCacheArrivals(const PrTextureCache* cache);

// Returns the state of `source` for `ask`, and queues its realization if the cache does not have it. For Ready and
// Partial, writes the region to `*out`. Otherwise zeroes `*out`.
//
// The cache holds a reference to the source. The region stays valid, with its texels unchanged, while anything else
// holds the source. A drawing that keeps the source may therefore keep the region. If only the cache holds the source,
// a trim may evict the region once a frame passes without an ask for it.
//
// A consumer that draws a Partial region asks again after the cache reports an arrival.
PrTextureCacheState prTextureCacheRealize(PrTextureCache* cache, const PrTextureSource* source,
    PrTextureCacheAsk ask, PrTextureRegion* out);

// Asks as prTextureCacheRealize does, at High priority, and blocks the calling thread until the version that the ask
// needs arrives or fails. Returns Ready or Failed.
PrTextureCacheState prTextureCacheWait(PrTextureCache* cache, const PrTextureSource* source, PrTextureCacheAsk ask,
    PrTextureRegion* out);

// Returns why `source` failed for `ask`: the status of the read or the upload, or NotSupported if the device samples no
// format that the source produces. Returns Ok if the source has not failed or the cache does not have it.
PrStatus prTextureCacheStatus(PrTextureCache* cache, const PrTextureSource* source, PrTextureCacheAsk ask);

// Blocks until every realization that is waiting or in flight has arrived or failed.
void prTextureCacheWaitAll(PrTextureCache* cache);

// Marks the end of a frame. An entry is idle if no ask since the last trim asked for it.
//
// - Cancels each idle realization that has not started.
// - Forgets each idle failure whose source only the cache holds.
// - While the cache's textures exceed the budget, evicts idle textures whose sources only the cache holds, least
//   recently asked first.
//
// The cost is proportional to what the trim cancels and evicts, not to the number of textures in the cache. A texture
// whose source something else holds is never evicted. If its holder lets go later, a following trim finds it.
void prTextureCacheTrim(PrTextureCache* cache);

// Counts of what a cache holds and has done.
typedef struct PrTextureCacheInfo {
    uint64_t realizations;                      // realizations started since the cache was created
    uint64_t bytes;                             // of the textures and pages that the cache holds
    uint32_t textures;                          // sources that have a texture of their own
    uint32_t pages;                             // shared pages
    uint32_t slots;                             // sources in the pages
    uint32_t waiting;                           // realizations that have not started
    uint32_t inFlight;                          // realizations that have started and not completed
    uint64_t cancelled;                         // realizations that a trim cancelled before they started
} PrTextureCacheInfo;

// Returns the cache's counts.
PrTextureCacheInfo prTextureCacheInfo(const PrTextureCache* cache);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
