// A texture cache holds the GPU textures that texture sources produce on one device. A host creates one cache for each
// device and gives it to every consumer that samples on that device: lib/draw's renderers, the layout tree, Studio's
// views, and a 3D renderer. Two consumers that read an image the same way share one texture.
//
// Asking. A consumer asks for a source each time it samples it (prTextureCacheRealize). The ask states what the
// consumer reads (PrTextureCacheAsk). To realize a source is to create its texture: the source answers the ask's need
// on the cache's device with the form that serves it (prTextureSourceAnswer), the cache samples that form as the ask
// reads it, creates the texture and reads the bits into it (prTextureUpload). The cache relabels blocks to their
// sampled form, keeps one channel of data in R8 or its swizzled forms, converts pixels to the ask's format and alpha,
// and has the device make the levels that an ask needs. The first ask queues the realization. Later asks return the
// region of the texture once it has arrived.
//
// Detail. An ask states how many texels the consumer shows (PrTextureCacheAsk::detail). A source may have previews,
// which are smaller versions of its picture (prTextureSourcePreview). The cache realizes the smallest version that has
// the detail. If that version has not arrived and a smaller one has, the ask returns the smaller one
// (PrTextureCacheState_Partial).
//
// Playing. A source whose picture changes (prTextureSourceShow) plays on a timeline that the consumer names
// (prTextureCachePlay). The cache asks the source what it shows only when the answer may have changed: when the
// timeline reaches the time that the last answer gave, or when the source notifies its notifier. A source that said
// that its picture never changes costs an ask no call into it. Each picture is a still source that the cache realizes
// as any other, and two timelines that show the same picture of a source share its texture. A picture that the source
// shows as an approximation, such as the rows of an image that have arrived, is answered as Partial, as a smaller
// version is.
//
// Order. Realizations wait in a queue. The cache keeps at most as many in flight as its task pool is wide. It starts
// the waiting realization with the highest priority first. Among equal priorities, it starts the one asked for in the
// latest frame, and then the smaller version. A trim cancels each waiting realization that no ask of the last frame
// asked for (prTextureCacheTrim). A consumer that scrolls past many images therefore reads only the ones that it still
// shows.
//
// Threads. Any thread may ask and trim. Realizations run on the task pool that the host gave the cache. Without a pool,
// a realization runs inside the ask that starts it, on the asking thread. The cache calls the host's `arrived` function
// when a realization that an ask was waiting for arrives or fails, so that the host draws again.
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
// cache keeps no copy of a texture in memory. What the cache learned of the sources that it plays stays, so a source
// shows the same picture on the new device.

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
    //
    // The source is read once for both. If the cache holds the version without levels when an ask needs them, the ask
    // submits the work that makes them from that texture on the device, and is Ready. The consumer records the draws
    // that sample the texture after the ask, and the device runs them after that work.
    PrTextureCacheFlag_Levels = 1u << 0,
};
typedef uint8_t PrTextureCacheFlags;

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
    PrTextureValues values;
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

    // The region holds an approximation, and the cache reports an arrival when a better one comes: the needed version
    // has not arrived and the region holds a smaller one, or the source shows a picture that is not complete.
    PrTextureCacheState_Partial,

    PrTextureCacheState_Pending,    // no version of the source has arrived
    PrTextureCacheState_Failed,     // the source did not read or upload. The cache does not try again.
};
typedef uint8_t PrTextureCacheState;

// Whether the cache packs small sources into shared pages.
enum {
    PrTextureCachePack_Auto,        // on a device that draws through a GPU without bindless
    PrTextureCachePack_Never,
    PrTextureCachePack_Always,      // every source that fits a page
};
typedef uint8_t PrTextureCachePack;

// Called with the host's context when a realization that an ask was waiting for arrives or fails, and when a source
// that the cache plays notifies that it shows another picture. It runs on the thread that completed the realization or
// notified. It must not call the cache.
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

// Frees the cache and its textures. Cancels the realizations that have not started, in the cache's queue or in its
// pool's, and waits for those that are running.
void prTextureCacheDestroy(PrTextureCache* PR_NULLABLE cache);

// Returns the device that the cache realizes on, or null while it has none.
PrGpuDevice* PR_NULLABLE prTextureCacheDevice(const PrTextureCache* cache);

// Moves the cache to `device`, as after the old device is lost. Cancels the realizations that have not started, as a
// destroy does, waits for those that are running, and frees every texture on the old device. Each source is realized
// again on `device` when it is next asked for. Failures are forgotten too. Every region that the cache returned becomes
// invalid, and the cache's generation advances. No other thread may ask during the call.
//
// A null `device` leaves the cache without a device, for the time between the loss of one device and the creation of
// the next. The host may then free the old device. A cache without a device answers every ask with Pending and starts
// nothing.
void prTextureCacheSetDevice(PrTextureCache* cache, PrGpuDevice* PR_NULLABLE device);

// Returns the number of times that the cache has moved to another device. A host that keeps drawings of regions records
// the generation when it draws them. If the generation has changed, the regions are invalid, and the host draws again.
uint64_t prTextureCacheGeneration(const PrTextureCache* cache);

// Sets the time of the frame that the host draws, in seconds on the host's clock, which the timelines of the sources
// that the cache plays are on (prTextureCachePlay). The cache reports an arrival when the time reaches the time at
// which a source that it plays shows another picture, so that the host draws again. A cache whose time was never set is
// at time zero.
void prTextureCacheSetTime(PrTextureCache* cache, double time);

// Returns the earliest time on the host's clock at which a source that the cache plays shows another picture, or zero
// if there is none. The host draws a frame at that time. A source that changes only when it notifies adds no time.
double prTextureCacheNextFrame(const PrTextureCache* cache);

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
//
// A source whose picture changes when it notifies, such as an image whose bytes are arriving, may not have its own
// picture yet. The ask answers with what the source shows at the start of a timeline (prTextureCachePlay), Partial
// until that is complete, and no realization waits for bytes.
PrTextureCacheState prTextureCacheRealize(PrTextureCache* cache, const PrTextureSource* source,
    PrTextureCacheAsk ask, PrTextureRegion* out);

// Asks as prTextureCacheRealize does, and plays a source whose picture changes: the answer is the region of the picture
// that the source shows at the time of `timeline` at the cache's time, marked as playing. While that picture is being
// realized, the answer is the last picture that this timeline was answered with whole. A still source is answered as
// prTextureCacheRealize answers it.
//
// A null `timeline` is the cache's own timeline for the source, which starts at the cache's time of the first ask that
// plays the source on it and plays at rate one. Consumers that keep no timeline of their own share it. The cache reads
// `*timeline` during the call. A consumer that pauses or seeks its timeline asks again with the timeline as it is now.
//
// `*picture` is the source whose texture the region is: the picture that shows, or `source` itself. The caller holds a
// reference to it. A consumer that keeps a drawing of the region keeps the picture for as long, as a draw list does
// (prDrawListHoldSource), so that the cache keeps the texture, and then releases its own reference. `*picture` is null
// if the answer has no region.
//
// A source plays on a timeline while consumers ask for it there, and for a second of the cache's time after the last
// ask. prTextureCacheRealize does not play: it answers with the source's own picture, which is what a row of thumbnails
// shows.
PrTextureCacheState prTextureCachePlay(PrTextureCache* cache, const PrTextureSource* source, PrTextureCacheAsk ask,
    const PrTextureTimeline* PR_NULLABLE timeline, PrTextureRegion* out, const PrTextureSource* PR_NULLABLE* PR_NONNULL picture);

// Asks as prTextureCacheRealize does, at High priority, and blocks the calling thread until the version that the ask
// needs arrives or fails. Returns Ready or Failed. It serves lib/canvas alone, whose drawImage a script reads back in
// the same turn; a rolling draw list of the canvas's bitmap, replayed by the CPU renderer, is to replace the wait.
// Every other consumer asks, draws what there is, and draws again when the cache reports an arrival. A source whose
// picture changes when it notifies is answered at once, as prTextureCacheRealize answers it, since its bytes may never
// complete.
PrTextureCacheState prTextureCacheWait(PrTextureCache* cache, const PrTextureSource* source, PrTextureCacheAsk ask,
    PrTextureRegion* out);

// Returns why `source` failed for `ask`: the status of the read or the upload, or NotSupported if the device samples no
// format that the source produces. For a source that the cache plays, it is also the failure that stopped its picture
// changing (PrTextureShow::status), as for bytes that stopped arriving, while the cache shows its last picture. Returns
// Ok if the source has not failed or the cache does not have it.
PrStatus prTextureCacheStatus(PrTextureCache* cache, const PrTextureSource* source, PrTextureCacheAsk ask);

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
