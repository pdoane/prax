// The texture cache. It realizes sources on one device, each as a texture of its own or as a slot of a shared page.

#include "pr/texture_cache.h"

#include "pr/align.h"
#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/atomic.h"
#include "pr/color_space.h"
#include "pr/count_of.h"
#include "pr/float.h"
#include "pr/float16.h"
#include "pr/hash_index.h"
#include "pr/mem.h"
#include "pr/memory.h"
#include "pr/mutex.h"
#include "pr/raw_array.h"
#include "pr/thread.h"

#include "texture/src/texture_source.h"

namespace pr_impl {

constexpr uint64_t kDefaultBudget = uint64_t(256) << 20;

// The number of entries in the Held list that each trim checks again. An entry is in that list if it was held when a
// trim came to evict it.
constexpr uint32_t kHeldChecks = 16;

// The side of the first shared page of a format, the maximum side of any page, and the largest side of a source that a
// page holds. Each new page of a format that already has pages has twice the side of the last page. A few small images
// therefore take only a small page.
constexpr uint32_t kFirstPageSize = 512;
constexpr uint32_t kPageSize = 2048;
constexpr uint32_t kPackedMaxSide = 256;

// Slots start and end on multiples of this value. That keeps the number of columns and shelves in a page small.
constexpr uint32_t kSlotAlign = 4;

// A free run of a shelf's columns.
struct CacheSpan {
    uint32_t x;
    uint32_t width;
};

// A row of a page. The slots of a shelf share one height.
struct CacheShelf {
    uint32_t y;
    uint32_t height;
    RawArray<CacheSpan> free;
};

// A page that is shared by small sources of one format and alpha.
struct CachePage {
    PrGpuTexture texture;
    PrGpuTexture plain;             // a view of the texture through the plain form of a Srgb format, or zero
    PrGpuFormat format;
    PrTextureAlpha alpha;
    uint32_t size;
    uint32_t nextY;                 // top of the first row that no shelf has taken
    uint32_t slots;
    RawArray<CacheShelf> shelves;
};

// The lists that hold settled entries. Each list is ordered by the time that its entries were last asked for.
enum : uint8_t {
    CacheList_None,
    CacheList_Recent,       // arrived entries that a trim may evict, oldest first
    CacheList_Held,         // arrived entries that were held by something else when a trim came to evict them
    CacheList_Passing,      // arrived pictures of sources that change over time, which a trim evicts once they are past
    CacheList_Failed,
    CacheList_Count,
};
typedef uint8_t CacheList;

// The realization of a source, identified by its key (keyOf). Each entry is allocated separately, so a pointer to an
// entry stays valid until the entry is removed.
struct CacheEntry {
    uint64_t key;
    PrTextureCacheState state;
    PrStatus status;                    // the status of a failed realization
    bool asked;                         // true if an ask found the entry pending
    bool started;                       // pending and in flight. A pending entry that has not started is in the queue.
    bool passing;                       // of a picture of a source that changes over time, which is not asked for again
    PrTextureCachePriority priority;    // the highest priority of the asks of a waiting entry
    uint8_t version;                    // the preview of `source` that it realizes, or the preview count for the source
    CacheList list;                     // the list that a settled entry is in
    PrTextureCacheAsk ask;              // the ask that the realization of a waiting entry serves
    uint64_t order;                     // sequence number of the entry's first ask among all entries
    uint64_t lastAsked;                 // the value of `trims` at the last ask for the entry
    const PrTextureSource* source;      // the entry holds a reference to the source
    uint64_t bytes;                     // size in bytes of its texture or of its slot
    PrTextureRegion region;
    PrGpuTexture plain;                 // the region's texture through its plain form, or zero; its page's for a slot
    uint32_t page;                      // index of its page plus one, or zero for a texture of its own
    uint32_t shelf;
    uint32_t at;                        // position of the entry in the cache's `entries`
    uint32_t queueAt;                   // position of a waiting entry in the queue
    CacheEntry* PR_NULLABLE newer;      // neighbors of the entry in its list
    CacheEntry* PR_NULLABLE older;
};

// The entries of one list, from the most recently asked for to the least.
struct CacheEntries {
    CacheEntry* PR_NULLABLE newest;
    CacheEntry* PR_NULLABLE oldest;
};

// The number of cache entries that realize one source. Each of those entries holds a reference to the source. If the
// source has more references than this number, something other than the cache holds it.
struct CacheHolders {
    uint64_t source;                    // id of the source
    uint32_t entries;
};

// The time, in seconds of the cache's clock, for which a source plays after the last ask for it.
constexpr double kPlayedFor = 1;

// A source that changes over time and that a consumer has asked for within the last kPlayedFor seconds. `current` is
// the picture that the source shows at the cache's time. `shown` is the last picture that an ask was answered with
// whole, which asks are answered with until `current` has arrived, however many pictures the source shows meanwhile.
// The player holds a reference to each. `serial` is the source's serial of `current`, and `next` the time at which the
// source shows another picture, or zero.
struct CachePlayer {
    const PrTextureSource* source;
    const PrTextureSource* PR_NULLABLE current;
    const PrTextureSource* PR_NULLABLE shown;
    uint64_t serial;
    double next;
    double askedAt;                     // the cache's time at the last ask for the source
};

struct TextureCache;

// A realization in flight: the chain that it reads, and the destination of its texels.
struct CacheJob {
    TextureCache* cache;
    CacheEntry* entry;
    const PrTextureSource* chain;
    PrTextureCacheFlags flags;
    PrTextureUploadFlags uploadFlags;
    bool packed;
    PrGpuTexture page;              // for a packed job, the texture of the page and the origin of the slot in it
    PrIVec2 origin;
};

// The cost of every operation on the cache depends on the work that it does, not on the number of entries in the cache.
// An ask is a lookup by key. A start takes the head of a priority queue. A trim walks only the entries that it cancels,
// evicts or rechecks.
struct TextureCache {
    PrTextureCacheDesc desc;
    PrMutex lock;
    PrCondition settled;            // signaled when a realization leaves flight
    RawArray<CacheEntry*> entries;
    HashIndex index;                // index of `entries` by key
    RawArray<CacheHolders> holders;
    HashIndex holderIndex;          // index of `holders` by source id
    RawArray<CacheEntry*> queue;    // waiting entries, as a binary heap whose first entry is the next to start
    CacheEntries lists[CacheList_Count];
    RawArray<CachePage*> pages;
    uint64_t bytes;                 // size in bytes of the entries' own textures and of the pages
    uint32_t textures;              // number of entries that have a texture of their own
    uint32_t slots;                 // number of entries in pages
    uint32_t pageCount;
    uint32_t inFlight;
    uint32_t limit;                 // maximum number of realizations in flight, or zero for no limit, without a pool
    uint64_t orders;                // number of entries created
    uint64_t cancelled;
    uint64_t trims;
    uint64_t generation;            // number of moves to another device
    uint64_t realizations;
    Atomic<uint64_t> arrivals;      // number of realizations that have arrived; a consumer reads it without the lock
    bool srgbViews;                 // true if the device views textures through their other sRGB form

    // The sources that the cache plays, indexed by source id, and the time of the host's frame, once the host has set
    // one.
    RawArray<CachePlayer> players;
    HashIndex playerIndex;
    double time;
    bool timed;
};

namespace {

// True if `format`, or the plain format that it is a swizzled form of, holds floats.
bool holdsFloats(PrGpuFormat format);

// Returns the one channel that `channels` names, 0 to 3, or 4 if it names none or several.
uint8_t singleChannel(PrTextureChannelFlags channels);

// The bits of the key of a realization below the id of the source. The low kAskBits bits say whether the realization
// has every level, and give its values, its alpha and the one channel that it holds. The kVersionBits bits above them
// give the version of the source that it realizes.
constexpr uint32_t kAskBits = 8;
constexpr uint32_t kVersionBits = 5;
constexpr uint32_t kKeyBits = kAskBits + kVersionBits;
static_assert(kPrTextureSourceMaxPreviews < 1u << kVersionBits);

// Returns the key of the realization that serves `ask` of `source`. Asks that need the same texture have the same key.
// Encoded values of 8 bits are color read through the plain form if the device views textures through their other sRGB
// form, and are stored as data otherwise. The alpha of an opaque source does not matter. Only data is kept in fewer
// channels.
uint64_t keyOf(const TextureCache& c, const PrTextureSource* source, const PrTextureCacheAsk& ask, uint32_t version) {
    const PrTextureDesc& desc = *prTextureSourceDesc(source);
    PrTextureCacheValues values = ask.values;
    if (values == PrTextureCacheValues_Encoded && !holdsFloats(desc.format))
        values = c.srgbViews ? PrTextureCacheValues_Color : PrTextureCacheValues_Data;
    uint32_t single = values == PrTextureCacheValues_Data ? singleChannel(ask.channels) : 4;
    uint32_t premultiplied = desc.alpha != PrTextureAlpha_None && ask.alpha == PrTextureAlpha_Premultiplied ? 1 : 0;
    uint32_t code = ((ask.flags & PrTextureCacheFlag_Levels) != 0 ? 1 : 0) | uint32_t(values) << 1 |
        premultiplied << 3 | (single < 4 ? single + 1 : 0) << 4;
    return prTextureSourceId(source) << kKeyBits | uint64_t(version) << kAskBits | code;
}

// Returns the version of `source` that an ask of `detail` texels needs. It is the smallest preview whose longer side
// has at least `detail` texels. If there is none, it is the source itself, whose version is its preview count.
uint32_t versionFor(const PrTextureSource* source, uint32_t detail) {
    uint32_t count = prTextureSourcePreviewCount(source);
    if (detail == 0)
        return count;
    for (uint32_t i = 0; i < count; ++i) {
        PrUVec2 extent = prTextureSourceDesc(prTextureSourcePreview(source, i))->extent;
        if ((extent.x > extent.y ? extent.x : extent.y) >= detail)
            return i;
    }
    return count;
}

// Returns the source of version `version` of `source`.
const PrTextureSource* versionOf(const PrTextureSource* source, uint32_t version) {
    return version < prTextureSourcePreviewCount(source) ? prTextureSourcePreview(source, version) : source;
}

// Returns the key of the other realization of the same source. For a key without every level, that is the key with
// every level, and the reverse.
uint64_t siblingOf(uint64_t key) {
    return key ^ 1;
}

// True if `format`, or the plain format that it is a swizzled form of, holds floats.
bool holdsFloats(PrGpuFormat format) {
    switch (prGpuFormatPlain(prGpuFormatStored(format))) {
        case PrGpuFormat_R16F:
        case PrGpuFormat_Rg16F:
        case PrGpuFormat_Rgba16F:
        case PrGpuFormat_R32F:
        case PrGpuFormat_Rg32F:
        case PrGpuFormat_Rgba32F:
        case PrGpuFormat_Rg11B10F:
        case PrGpuFormat_Bc6HUfloat:
        case PrGpuFormat_Bc6HSfloat:
            return true;
        default:
            return false;
    }
}

// Returns true if the conversions of lib/texture read texels of `format`. Such a format is a plain format of 8 bits or
// floats, or a swizzled form of one.
bool isPixels(PrGpuFormat format) {
    switch (prGpuFormatPlain(prGpuFormatStored(format))) {
        case PrGpuFormat_R8:
        case PrGpuFormat_Rg8:
        case PrGpuFormat_Rgba8:
        case PrGpuFormat_Bgra8:
        case PrGpuFormat_R16F:
        case PrGpuFormat_Rg16F:
        case PrGpuFormat_Rgba16F:
        case PrGpuFormat_R32F:
        case PrGpuFormat_Rg32F:
        case PrGpuFormat_Rgba32F:
            return true;
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Encoded floats

// Returns `value` encoded with the sRGB transfer function, mirrored below zero. This is the extended sRGB encoding.
float encodeExtended(float value) {
    float encoded = prColorTransferEncode(PrColorTransfer_Srgb, prF32Abs(value));
    return value < 0 ? -encoded : encoded;
}

// A source that encodes the floats of another source as extended sRGB in half floats. It reads the other source as
// straight Rgba32F, through a conversion of that source.
struct EncodedSource {
    const PrTextureSource* inner;
    PrTextureDesc desc;
};

struct EncodedRead {
    const EncodedSource* source;
    PrTextureRead* outer;
};

void encodedDone(void* PR_NULLABLE context, PrTextureRead* inner) {
    EncodedRead held = *static_cast<EncodedRead*>(context);
    prMemoryFree(context);
    PrStatus status = prTextureReadStatus(inner);
    const PrTextureDesc& desc = held.source->desc;
    const PrTextureDesc& innerDesc = *prTextureSourceDesc(held.source->inner);
    const PrTextureReadDesc& read = *prTextureReadInfo(held.outer);
    bool opaque = desc.alpha == PrTextureAlpha_None;
    bool premultiplied = desc.alpha == PrTextureAlpha_Premultiplied;
    for (uint32_t level = read.firstLevel; status == PrStatus_Ok && level < read.firstLevel + read.levelCount; ++level) {
        PrUVec2 extent = prTextureLevelExtent(desc.extent, level);
        size_t count = size_t(extent.x) * extent.y;
        for (uint32_t layer = read.firstLayer; layer < read.firstLayer + read.layerCount; ++layer) {
            const float* from = reinterpret_cast<const float*>(
                prTextureReadStorage(inner) + prTextureReadOffset(&innerDesc, &read, level, layer));
            uint8_t* to = prTextureReadStorage(held.outer) + prTextureReadOffset(&desc, &read, level, layer);
            for (size_t i = 0; i < count; ++i) {
                float c[4];
                prMemCopy(c, from + i * 4, sizeof(c));
                float a = opaque ? 1 : prF32Clamp(c[3], 0, 1);
                float scale = premultiplied ? a : 1;
                PrFloat16 stored[4];
                for (uint32_t k = 0; k < 3; ++k)
                    stored[k] = prFloat16FromFloat(encodeExtended(c[k]) * scale);
                stored[3] = prFloat16FromFloat(a);
                prMemCopy(to + i * 8, stored, sizeof(stored));
            }
        }
    }
    prTextureReadDestroy(inner);
    prTextureReadComplete(held.outer, status);
}

void encodedRead(void* PR_NULLABLE context, PrTextureRead* read) {
    const EncodedSource* source = static_cast<const EncodedSource*>(context);
    EncodedRead* held = static_cast<EncodedRead*>(prMemoryAlloc(sizeof(EncodedRead)));
    *held = EncodedRead{source, read};
    PrTextureReadDesc inner = *prTextureReadInfo(read);
    inner.storage = nullptr;
    inner.done = encodedDone;
    inner.context = held;
    prTextureReadCreate(source->inner, &inner);
}

// Creates a source of the values of `floats`, encoded as extended sRGB in half floats. The source has alpha `alpha`, or
// is opaque if `floats` is opaque.
const PrTextureSource* createEncoded(const PrTextureSource* floats, PrTextureAlpha alpha) {
    PrTextureConvertDesc straight = prTextureConvertDescDefault(PrGpuFormat_Rgba32F);
    if (prTextureSourceDesc(floats)->alpha != PrTextureAlpha_None)
        straight.alpha = PrTextureAlpha_Straight;
    EncodedSource* source = allocZeroed<EncodedSource>();
    source->inner = prTextureSourceCreateConvert(floats, &straight);
    source->desc = *prTextureSourceDesc(source->inner);
    source->desc.format = PrGpuFormat_Rgba16F;
    if (source->desc.alpha != PrTextureAlpha_None)
        source->desc.alpha = alpha;
    PrTextureSourceDesc desc{
        .texture = source->desc,
        .read = encodedRead,
        .release = [](void* PR_NULLABLE context) {
            EncodedSource* held = static_cast<EncodedSource*>(context);
            prTextureSourceRelease(held->inner);
            prMemoryFree(held);
        },
        .context = source,
    };
    return prTextureSourceCreate(&desc);
}

// ---------------------------------------------------------------------------------------------------------------------
// Chains

// The chain that a realization uploads, and the flags of the upload. `source` carries a reference that the realization
// releases.
struct CacheChain {
    const PrTextureSource* PR_NULLABLE source;
    PrTextureUploadFlags flags;
    PrStatus status;                // NotSupported, or the failure of a reformat, if `source` is null
};

// The block formats that Basis Universal textures transcode to, in order of preference. A texture of one channel takes
// the formats of one channel first.
constexpr PrGpuFormat kTranscodeTargets[] = {
    PrGpuFormat_Astc4x4, PrGpuFormat_Bc7, PrGpuFormat_EacR11, PrGpuFormat_Bc4, PrGpuFormat_EacRg11, PrGpuFormat_Bc5,
    PrGpuFormat_Etc2Rgba8, PrGpuFormat_Bc3, PrGpuFormat_Etc2Rgb8, PrGpuFormat_Bc1,
};

// Returns true if `format` holds one channel.
bool isSingle(PrGpuFormat format) {
    return format == PrGpuFormat_Bc4 || format == PrGpuFormat_EacR11;
}

// Returns the one channel that `channels` names, 0 to 3, or 4 if it names none or several.
uint8_t singleChannel(PrTextureChannelFlags channels) {
    for (uint8_t c = 0; c < 4; ++c) {
        if (channels == PrTextureChannelFlags(1u << c))
            return c;
    }
    return 4;
}

// Returns the channel that an ask reads alone, 0 to 3, or 4 if there is none. Only data is kept in fewer channels than
// it has.
uint8_t singleChannelOf(const PrTextureCacheAsk& ask) {
    return ask.values == PrTextureCacheValues_Data ? singleChannel(ask.channels) : 4;
}

// Returns true if the device samples textures of `format` whose first level is `extent`. A device takes blocks only in
// a first level of whole blocks.
bool samples(const PrGpuDevice* device, PrGpuFormat format, PrUVec2 extent) {
    if (format == PrGpuFormat_None || !prGpuDeviceSupportsFormat(device, format, PrGpuTextureUsageFlag_Sampled))
        return false;
    PrGpuFormatBlock block = prGpuFormatBlock(format);
    return extent.x % block.width == 0 && extent.y % block.height == 0;
}

// Returns the format in which blocks of `blocks` are sampled for `values`. A format of one channel is sampled through
// its Luminance form. That form reads the value as gray, which matches the decoded pixels. Color takes the Srgb form.
// The result is None if color is asked of a format that has no Srgb form.
PrGpuFormat sampledFormat(PrGpuFormat blocks, PrTextureCacheValues values) {
    PrGpuFormat plain = prGpuFormatPlain(blocks);
    PrGpuFormat format = isSingle(plain) ? prGpuFormatSwizzled(plain, PrGpuSwizzle_Luminance) : plain;
    if (values != PrTextureCacheValues_Color)
        return format;
    PrGpuFormat encoded = prGpuFormatSrgb(format);
    return encoded != format ? encoded : PrGpuFormat_None;
}

// Returns the form of `plain` that `source` produces. It is the plain form itself, its Srgb form, or None if the source
// produces neither.
PrGpuFormat producedForm(const PrTextureSource* source, PrGpuFormat plain) {
    if (prTextureSourceProduces(source, plain))
        return plain;
    PrGpuFormat srgb = prGpuFormatSrgb(plain);
    return srgb != plain && prTextureSourceProduces(source, srgb) ? srgb : PrGpuFormat_None;
}

// Returns a chain of `source` relabeled as `format`, which holds the same bits.
CacheChain relabel(const PrTextureSource* source, PrGpuFormat format) {
    if (prTextureSourceDesc(source)->format == format) {
        prTextureSourceRetain(source);
        return CacheChain{.source = source};
    }
    PrTextureConvertDesc convert = prTextureConvertDescDefault(format);
    convert.alpha = prTextureSourceDesc(source)->alpha;
    return CacheChain{.source = prTextureSourceCreateConvert(source, &convert)};
}

// Returns true if blocks with alpha `alpha` serve an ask. Blocks keep their alpha, and a consumer of straight alpha
// cannot take premultiplied blocks.
bool blocksServe(PrTextureAlpha alpha, const PrTextureCacheAsk& ask) {
    return !(alpha == PrTextureAlpha_Premultiplied && ask.alpha == PrTextureAlpha_Straight);
}

// Returns the chain of the blocks of a source that the device samples for the ask. They are the source's own blocks, or
// the blocks that the source transcodes to. Returns a chain without a source if no blocks serve the ask.
CacheChain blocksChain(const TextureCache& c, const PrTextureSource* source, const PrTextureCacheAsk& ask,
        bool transcodes) {
    const PrTextureDesc& desc = *prTextureSourceDesc(source);
    if (prGpuFormatIsCompressed(desc.format) && blocksServe(desc.alpha, ask)) {
        PrGpuFormat sampled = sampledFormat(desc.format, ask.values);
        if (samples(c.desc.device, sampled, desc.extent))
            return relabel(source, sampled);
    }
    if (!transcodes)
        return CacheChain{.status = PrStatus_NotSupported};
    uint8_t single = singleChannelOf(ask);
    PrGpuFormat order[countOf(kTranscodeTargets)];
    uint32_t count = 0;
    for (PrGpuFormat target : kTranscodeTargets) {
        if (single < 4 && isSingle(target))
            order[count++] = target;
    }
    for (PrGpuFormat target : kTranscodeTargets) {
        if (!(single < 4 && isSingle(target)))
            order[count++] = target;
    }
    for (uint32_t i = 0; i < count; ++i) {
        PrGpuFormat target = producedForm(source, order[i]);
        if (target == PrGpuFormat_None || target == desc.format)
            continue;
        PrGpuFormat sampled = sampledFormat(target, ask.values);
        if (!samples(c.desc.device, sampled, desc.extent))
            continue;
        const PrTextureSource* transcoded = nullptr;
        PrStatus status = prTextureSourceReformat(source, target, &transcoded);
        if (status != PrStatus_Ok)
            return CacheChain{.status = status};
        if (!blocksServe(prTextureSourceDesc(transcoded)->alpha, ask)) {
            prTextureSourceRelease(transcoded);
            continue;
        }
        CacheChain chain = relabel(transcoded, sampled);
        prTextureSourceRelease(transcoded);
        return chain;
    }
    return CacheChain{.status = PrStatus_NotSupported};
}

// Returns a source of the pixels of `source`. It is `source` itself if its format is pixels, and otherwise a source of
// the first pixel format that `source` decodes to. Returns null if `source` decodes to no pixels.
const PrTextureSource* PR_NULLABLE decodedPixels(const PrTextureSource* source) {
    if (isPixels(prTextureSourceDesc(source)->format)) {
        prTextureSourceRetain(source);
        return source;
    }
    const PrGpuFormat decoded[] = {PrGpuFormat_Rgba8, PrGpuFormat_Rgba8Srgb, PrGpuFormat_Rgba16F, PrGpuFormat_Rgba32F};
    for (PrGpuFormat candidate : decoded) {
        const PrTextureSource* pixels = nullptr;
        if (prTextureSourceProduces(source, candidate) &&
                prTextureSourceReformat(source, candidate, &pixels) == PrStatus_Ok)
            return pixels;
    }
    return nullptr;
}

// Returns the chain for data of which the ask reads only channel `single`. The chain holds that channel in the one
// channel of R8. The texture is R8 read as red. On a device with swizzled formats it is the Luminance form of R8, or
// the Intensity form for alpha. The device does not render to a swizzled format, so the levels of a swizzled chain are
// generated here. Returns a chain without a source if the device keeps the pixels in four channels instead.
CacheChain singleChain(const TextureCache& c, const PrTextureSource* source, const PrTextureSource* pixels,
        uint8_t single, bool levels) {
    // If the source decodes red alone, the chain reads red alone.
    if (single == 0 && prTextureSourceProduces(source, PrGpuFormat_R8)) {
        CacheChain chain{.flags = PrTextureUploadFlag_MakesLevels};
        chain.status = prTextureSourceReformat(source, PrGpuFormat_R8, &chain.source);
        return chain;
    }
    bool swizzles = (prGpuDeviceInfo(c.desc.device).features & PrGpuFeatureFlag_TextureSwizzle) != 0;
    if (single != 0 && !swizzles)
        return CacheChain{};
    PrGpuFormat format = PrGpuFormat_R8;
    if (single == 3)
        format = PrGpuFormat_R8Intensity;
    else if (single != 0)
        format = PrGpuFormat_R8Luminance;
    PrTextureConvertDesc convert = prTextureConvertDescDefault(format);
    convert.alpha = PrTextureAlpha_None;
    convert.channels[0] = single;
    const PrTextureSource* reduced = prTextureSourceCreateConvert(pixels, &convert);
    if (format == PrGpuFormat_R8 || !levels || prTextureSourceDesc(pixels)->levelCount > 1)
        return CacheChain{.source = reduced, .flags = PrTextureUploadFlag_MakesLevels};
    CacheChain chain{.source = prTextureSourceCreateMips(reduced, 0)};
    prTextureSourceRelease(reduced);
    return chain;
}

// Returns the chain of the pixels of a source for the ask, in the alpha of the ask. The format is R8 for data of one
// channel, Rgba8 or its Srgb form for values of 8 bits, and Rgba16F for floats.
CacheChain pixelsChain(const TextureCache& c, const PrTextureSource* source, const PrTextureCacheAsk& ask, bool levels) {
    const PrTextureSource* pixels = decodedPixels(source);
    if (pixels == nullptr)
        return CacheChain{.status = PrStatus_NotSupported};
    const PrTextureDesc& desc = *prTextureSourceDesc(pixels);
    PrTextureAlpha alpha = desc.alpha == PrTextureAlpha_None ? PrTextureAlpha(PrTextureAlpha_None) : ask.alpha;
    CacheChain chain{.flags = PrTextureUploadFlag_MakesLevels};
    if (holdsFloats(desc.format) && ask.values == PrTextureCacheValues_Encoded) {
        chain.source = createEncoded(pixels, alpha);
    } else if (holdsFloats(desc.format)) {
        PrTextureConvertDesc convert = prTextureConvertDescDefault(PrGpuFormat_Rgba16F);
        convert.alpha = alpha;
        chain.source = prTextureSourceCreateConvert(pixels, &convert);
    } else {
        uint8_t single = singleChannelOf(ask);
        if (single < 4)
            chain = singleChain(c, source, pixels, single, levels);
        if (chain.source == nullptr && chain.status == PrStatus_Ok) {
            // Values of 8 bits keep their encoding. The form of the format says only how a sampler reads them.
            PrGpuFormat target = ask.values == PrTextureCacheValues_Color ? PrGpuFormat_Rgba8Srgb : PrGpuFormat_Rgba8;
            const PrTextureSource* relabeled = pixels;
            prTextureSourceRetain(relabeled);
            if (prGpuFormatIsSrgb(desc.format) != prGpuFormatIsSrgb(target)) {
                prTextureSourceRelease(relabeled);
                PrTextureConvertDesc form = prTextureConvertDescDefault(
                    prGpuFormatIsSrgb(target) ? prGpuFormatSrgb(desc.format) : prGpuFormatPlain(desc.format));
                form.alpha = desc.alpha;
                relabeled = prTextureSourceCreateConvert(pixels, &form);
            }
            PrTextureConvertDesc convert = prTextureConvertDescDefault(target);
            convert.alpha = alpha;
            chain = CacheChain{
                .source = prTextureSourceCreateConvert(relabeled, &convert),
                .flags = PrTextureUploadFlag_MakesLevels,
            };
            prTextureSourceRelease(relabeled);
        }
    }
    prTextureSourceRelease(pixels);
    return chain;
}

// Returns the chain that realizes `source` for `ask`. Blocks that the device samples stay blocks. The exception is an
// ask that needs levels that the texture lacks, if the blocks decode. The cache then keeps the pixels, so that the
// device generates the levels. Levels do more for a minified texture than blocks save. A texture of several levels
// transcodes to blocks that the device samples. Anything else realizes as pixels.
CacheChain chainOf(const TextureCache& c, const PrTextureSource* source, const PrTextureCacheAsk& ask) {
    const PrTextureDesc& desc = *prTextureSourceDesc(source);
    if (desc.kind != PrGpuTextureKind_2d) {
        prTextureSourceRetain(source);
        return CacheChain{.source = source};
    }
    bool levels = (ask.flags & PrTextureCacheFlag_Levels) != 0;
    bool decodes = producedForm(source, PrGpuFormat_Rgba8) != PrGpuFormat_None || isPixels(desc.format);
    bool chained = levels && desc.levelCount == 1 && prGpuMipLevels(desc.extent) > 1;
    if (!(decodes && chained)) {
        CacheChain blocks = blocksChain(c, source, ask, !chained);
        if (blocks.source != nullptr || blocks.status != PrStatus_NotSupported)
            return blocks;
    }
    CacheChain chain = pixelsChain(c, source, ask, levels);
    if (!levels)
        chain.flags = 0;
    return chain;
}

// Returns true if `device` views textures through their other sRGB form. One texture of encoded color then serves both
// asks of encoded values and asks of linear color.
bool viewsSrgb(const PrGpuDevice* device) {
    return prGpuDeviceSupportsFormat(device, PrGpuFormat_Rgba8Srgb,
        PrGpuTextureUsageFlag_Sampled | PrGpuTextureUsageFlag_SrgbViews);
}

// Returns true if the cache packs sources on its device by default: on a device that draws through a GPU and binds
// textures to slots. There a texture of its own costs a renderer a bind, and a draw where the renderer batches by
// texture. A device with bindless reaches every texture by handle, so a page would only take hardware sampling and mip
// levels from its slots. A memory device reads texels in place, so a page would only add a copy.
bool packsByDefault(const TextureCache& c) {
    return !prGpuDeviceHasNative(c.desc.device, PrGpuNativeKind_Pixels) &&
        (prGpuDeviceInfo(c.desc.device).features & PrGpuFeatureFlag_Bindless) == 0;
}

// Returns true if a page can hold the pixels of `chain` for an ask with `flags`.
bool packs(const TextureCache& c, const PrTextureSource* chain, PrTextureCacheFlags flags) {
    if (chain->desc.usage != 0)
        return false;
    if (c.desc.pack == PrTextureCachePack_Never || (flags & PrTextureCacheFlag_Levels) != 0)
        return false;
    if (c.desc.pack == PrTextureCachePack_Auto && !packsByDefault(c))
        return false;
    const PrTextureDesc& desc = *prTextureSourceDesc(chain);
    return desc.kind == PrGpuTextureKind_2d && desc.layerCount == 1 && desc.levelCount == 1 &&
        (desc.format == PrGpuFormat_Rgba8 || desc.format == PrGpuFormat_Rgba8Srgb) && desc.extent.x != 0 &&
        desc.extent.y != 0 && desc.extent.x <= kPackedMaxSide && desc.extent.y <= kPackedMaxSide;
}

// ---------------------------------------------------------------------------------------------------------------------
// Pages

// Returns the height of the shelves that hold a slot `height` texels tall. It is the next power of two, starting at
// kSlotAlign * 2.
uint32_t shelfHeightOf(uint32_t height) {
    uint32_t shelf = kSlotAlign * 2;
    while (shelf < height)
        shelf *= 2;
    return shelf;
}

// Takes `width` columns of a shelf's free runs. Returns false if no run is wide enough.
bool takeColumns(CacheShelf& shelf, uint32_t width, uint32_t* x) {
    for (uint32_t i = 0; i < shelf.free.count; ++i) {
        CacheSpan& span = shelf.free.data[i];
        if (span.width < width)
            continue;
        *x = span.x;
        span.x += width;
        span.width -= width;
        if (span.width == 0)
            shelf.free.erase(i);
        return true;
    }
    return false;
}

// Returns the columns of a slot to its shelf, and merges them with the free runs beside them.
void freeColumns(CacheShelf& shelf, uint32_t x, uint32_t width) {
    uint32_t at = 0;
    while (at < shelf.free.count && shelf.free.data[at].x < x)
        ++at;
    shelf.free.insert(at, CacheSpan{x, width});
    if (at + 1 < shelf.free.count && shelf.free.data[at].x + shelf.free.data[at].width == shelf.free.data[at + 1].x) {
        shelf.free.data[at].width += shelf.free.data[at + 1].width;
        shelf.free.erase(at + 1);
    }
    if (at > 0 && shelf.free.data[at - 1].x + shelf.free.data[at - 1].width == shelf.free.data[at].x) {
        shelf.free.data[at - 1].width += shelf.free.data[at].width;
        shelf.free.erase(at);
    }
}

// Places a slot of `extent` in the page. Returns false if it has no room.
bool placeIn(CachePage& page, PrUVec2 extent, uint32_t* shelfAt, PrIVec2* origin) {
    uint32_t width = alignUp(extent.x, kSlotAlign);
    uint32_t height = shelfHeightOf(extent.y);
    uint32_t x = 0;
    for (uint32_t i = 0; i < page.shelves.count; ++i) {
        CacheShelf& shelf = page.shelves.data[i];
        if (shelf.height == height && takeColumns(shelf, width, &x)) {
            *shelfAt = i;
            *origin = PrIVec2{int32_t(x), int32_t(shelf.y)};
            return true;
        }
    }
    if (page.nextY + height > page.size)
        return false;
    CacheShelf shelf{.y = page.nextY, .height = height};
    shelf.free.push(CacheSpan{0, page.size});
    page.nextY += height;
    page.shelves.push(shelf);
    CacheShelf& added = page.shelves.data[page.shelves.count - 1];
    takeColumns(added, width, &x);
    *shelfAt = page.shelves.count - 1;
    *origin = PrIVec2{int32_t(x), int32_t(added.y)};
    return true;
}

void destroyPage(TextureCache& c, CachePage* page) {
    c.bytes -= uint64_t(page->size) * page->size * 4;
    --c.pageCount;
    prGpuTextureRelease(c.desc.device, page->texture);
    if (page->plain.id != 0)
        prGpuTextureRelease(c.desc.device, page->plain);
    for (CacheShelf& shelf : page->shelves.span())
        shelf.free.reset();
    page->shelves.reset();
    prMemoryFree(page);
}

// Places a slot of `extent` in a page of `format` and `alpha`. Creates a page if none has room. Returns false if a page
// cannot be created.
bool placeSlot(TextureCache& c, PrGpuFormat format, PrTextureAlpha alpha, PrUVec2 extent, CacheEntry& entry, PrIVec2* origin) {
    for (uint32_t i = 0; i < c.pages.count; ++i) {
        // The position of a freed page is null until a new page takes it.
        CachePage* page = c.pages.data[i];
        if (page != nullptr && page->format == format && page->alpha == alpha && placeIn(*page, extent, &entry.shelf, origin)) {
            entry.page = i + 1;
            ++page->slots;
            return true;
        }
    }
    uint32_t size = kFirstPageSize;
    for (const CachePage* page : c.pages.span()) {
        if (page != nullptr && page->format == format && page->alpha == alpha && page->size * 2 > size)
            size = page->size * 2;
    }
    if (size > kPageSize)
        size = kPageSize;
    uint32_t most = prGpuDeviceInfo(c.desc.device).maxTextureSize;
    if (most != 0 && most < size)
        size = most;
    PrGpuTextureDesc desc = prGpuTextureDesc2d(format, PrUVec2{size, size}, PrGpuTextureUsageFlag_Sampled);
    desc.name = "texture cache page";
    bool viewed = c.srgbViews && prGpuFormatIsSrgb(format);
    if (viewed)
        desc.usage |= PrGpuTextureUsageFlag_SrgbViews;
    PrGpuTexture texture{};
    if (prGpuTextureCreate(c.desc.device, &desc, &texture) != PrStatus_Ok)
        return false;
    PrGpuTexture plain{};
    PrGpuTextureViewDesc plainView{.format = prGpuFormatPlain(format)};
    if (viewed && prGpuTextureCreateView(c.desc.device, texture, plainView, &plain) != PrStatus_Ok) {
        prGpuTextureRelease(c.desc.device, texture);
        return false;
    }
    CachePage* page = allocZeroed<CachePage>();
    *page = CachePage{.texture = texture, .plain = plain, .format = format, .alpha = alpha, .size = size};
    c.bytes += uint64_t(size) * size * 4;
    ++c.pageCount;
    // The new page takes the position of a freed page if there is one. The indices that entries hold for the other
    // pages then stay valid.
    uint32_t at = 0;
    while (at < c.pages.count && c.pages.data[at] != nullptr)
        ++at;
    if (at == c.pages.count)
        c.pages.push(nullptr);
    c.pages.data[at] = page;
    if (!placeIn(*page, extent, &entry.shelf, origin))
        return false;
    entry.page = at + 1;
    ++page->slots;
    return true;
}

// Frees what the entry created on the device. That is its texture, or its slot. The page of a slot is also freed after
// its last slot is freed.
void freeEntry(TextureCache& c, CacheEntry& entry) {
    if (entry.page != 0) {
        CachePage* page = c.pages.data[entry.page - 1];
        PrRect r = entry.region.rect;
        freeColumns(page->shelves.data[entry.shelf], uint32_t(r.left), alignUp(uint32_t(r.right - r.left), kSlotAlign));
        if (--page->slots == 0) {
            destroyPage(c, page);
            c.pages.data[entry.page - 1] = nullptr;
        }
        --c.slots;
    } else if (entry.region.texture.texture.id != 0) {
        prGpuTextureRelease(c.desc.device, entry.region.texture.texture);
        if (entry.plain.id != 0)
            prGpuTextureRelease(c.desc.device, entry.plain);
        c.bytes -= entry.bytes;
        --c.textures;
    }
    entry.page = 0;
    entry.bytes = 0;
    entry.region = PrTextureRegion{};
    entry.plain = PrGpuTexture{};
}

// ---------------------------------------------------------------------------------------------------------------------
// Entries

CacheEntry* PR_NULLABLE entryOf(const TextureCache& c, uint64_t key) {
    uint32_t at = c.index.find(KeyIndex::hashOf(key), [&](uint32_t i) { return c.entries.data[i]->key == key; });
    return at != kHashMissing ? c.entries.data[at] : nullptr;
}

uint32_t holdersAt(const TextureCache& c, uint64_t source) {
    return c.holderIndex.find(KeyIndex::hashOf(source), [&](uint32_t i) { return c.holders.data[i].source == source; });
}

// Returns true if something other than the cache holds the source of the entry. A drawing may then name the region of
// the entry.
bool isHeld(const TextureCache& c, const CacheEntry& entry) {
    uint32_t at = holdersAt(c, prTextureSourceId(entry.source));
    return prTextureSourceReferences(entry.source) > c.holders.data[at].entries;
}

// Removes the entry from its list.
void unlist(TextureCache& c, CacheEntry* entry) {
    if (entry->list == CacheList_None)
        return;
    CacheEntries& list = c.lists[entry->list];
    if (entry->newer != nullptr)
        entry->newer->older = entry->older;
    else
        list.newest = entry->older;
    if (entry->older != nullptr)
        entry->older->newer = entry->newer;
    else
        list.oldest = entry->newer;
    entry->newer = nullptr;
    entry->older = nullptr;
    entry->list = CacheList_None;
}

// Adds the entry to `list` as the newest entry if `newest` is set, and as the oldest otherwise.
void enlist(TextureCache& c, CacheEntry* entry, CacheList list, bool newest) {
    unlist(c, entry);
    CacheEntries& entries = c.lists[list];
    entry->list = list;
    if (newest) {
        entry->older = entries.newest;
        if (entries.newest != nullptr)
            entries.newest->newer = entry;
        entries.newest = entry;
        if (entries.oldest == nullptr)
            entries.oldest = entry;
    } else {
        entry->newer = entries.oldest;
        if (entries.oldest != nullptr)
            entries.oldest->older = entry;
        entries.oldest = entry;
        if (entries.newest == nullptr)
            entries.newest = entry;
    }
}

// The order of waiting realizations. A realization ranks before another if its priority is higher. Ties go to the one
// asked for in a later frame, then to the smaller version, then to the one asked for first.
uint32_t rankOf(PrTextureCachePriority priority) {
    switch (priority) {
        case PrTextureCachePriority_High: return 2;
        case PrTextureCachePriority_Normal: return 1;
        case PrTextureCachePriority_Low: break;
    }
    return 0;
}

bool ranksBefore(const CacheEntry& a, const CacheEntry& b) {
    if (a.priority != b.priority)
        return rankOf(a.priority) > rankOf(b.priority);
    if (a.lastAsked != b.lastAsked)
        return a.lastAsked > b.lastAsked;
    if (a.version != b.version)
        return a.version < b.version;
    return a.order < b.order;
}

void queueSet(TextureCache& c, uint32_t at, CacheEntry* entry) {
    c.queue.data[at] = entry;
    entry->queueAt = at;
}

// Moves the entry at position `at` of the queue to the position that its rank gives it. Call it after the rank of the
// entry changed or the entry took the position of another.
void queueFix(TextureCache& c, uint32_t at) {
    CacheEntry* entry = c.queue.data[at];
    while (at != 0) {
        uint32_t parent = (at - 1) / 2;
        if (!ranksBefore(*entry, *c.queue.data[parent]))
            break;
        queueSet(c, at, c.queue.data[parent]);
        at = parent;
    }
    for (;;) {
        uint32_t child = at * 2 + 1;
        if (child >= c.queue.count)
            break;
        if (child + 1 < c.queue.count && ranksBefore(*c.queue.data[child + 1], *c.queue.data[child]))
            ++child;
        if (!ranksBefore(*c.queue.data[child], *entry))
            break;
        queueSet(c, at, c.queue.data[child]);
        at = child;
    }
    queueSet(c, at, entry);
}

void queuePush(TextureCache& c, CacheEntry* entry) {
    c.queue.push(entry);
    entry->queueAt = c.queue.count - 1;
    queueFix(c, entry->queueAt);
}

void queueRemove(TextureCache& c, CacheEntry* entry) {
    uint32_t at = entry->queueAt;
    CacheEntry* last = c.queue.data[c.queue.count - 1];
    c.queue.count -= 1;
    if (last != entry) {
        queueSet(c, at, last);
        queueFix(c, at);
    }
}

// Removes the entry from the cache and frees it. That frees what it created on the device, releases its reference to
// its source, and removes it from the queue or from its list.
void removeEntry(TextureCache& c, CacheEntry* entry) {
    if (entry->state == PrTextureCacheState_Pending && !entry->started)
        queueRemove(c, entry);
    unlist(c, entry);
    freeEntry(c, *entry);
    uint64_t source = prTextureSourceId(entry->source);
    uint32_t holder = holdersAt(c, source);
    if (--c.holders.data[holder].entries == 0) {
        uint32_t last = c.holders.count - 1;
        c.holderIndex.erase(KeyIndex::hashOf(source), holder);
        if (holder != last) {
            c.holderIndex.relocate(KeyIndex::hashOf(c.holders.data[last].source), last, holder);
            c.holders.data[holder] = c.holders.data[last];
        }
        c.holders.count = last;
    }
    uint32_t last = c.entries.count - 1;
    c.index.erase(KeyIndex::hashOf(entry->key), entry->at);
    if (entry->at != last) {
        CacheEntry* moved = c.entries.data[last];
        c.index.relocate(KeyIndex::hashOf(moved->key), last, entry->at);
        c.entries.data[entry->at] = moved;
        moved->at = entry->at;
    }
    c.entries.count = last;
    prTextureSourceRelease(entry->source);
    prMemoryFree(entry);
}

// Returns the ask for which the cache realizes version `version` of `source`. The cache generates the mip levels that a
// consumer asks for only if the consumer minifies, which means that the version has more texels than the detail of the
// ask.
PrTextureCacheAsk askFor(const PrTextureSource* source, PrTextureCacheAsk ask, uint32_t version) {
    // A texture that its source writes on the device, as a compute shader does, is in the source's own format: a Srgb
    // form is not writable. Encoded color is read as stored, so it needs no Srgb form.
    bool written = source->desc.usage != 0 && !holdsFloats(source->desc.texture.format);
    if (written && ask.values == PrTextureCacheValues_Encoded)
        ask.values = PrTextureCacheValues_Data;
    if ((ask.flags & PrTextureCacheFlag_Levels) != 0 && ask.detail != 0) {
        PrUVec2 extent = prTextureSourceDesc(versionOf(source, version))->extent;
        if ((extent.x > extent.y ? extent.x : extent.y) <= ask.detail)
            ask.flags &= PrTextureCacheFlags(~PrTextureCacheFlag_Levels);
    }
    return ask;
}

// Returns the entry that holds version `version` of `source` for `ask`, or null if none has arrived. A realization with
// every level also serves an ask of one level, so one texture of the version serves both.
CacheEntry* PR_NULLABLE readyEntry(TextureCache& c, const PrTextureSource* source, const PrTextureCacheAsk& ask,
        uint32_t version) {
    PrTextureCacheAsk resolved = askFor(source, ask, version);
    uint64_t key = keyOf(c, source, resolved, version);
    if ((resolved.flags & PrTextureCacheFlag_Levels) == 0) {
        CacheEntry* levels = entryOf(c, siblingOf(key));
        if (levels != nullptr && levels->state == PrTextureCacheState_Ready)
            return levels;
    }
    CacheEntry* entry = entryOf(c, key);
    return entry != nullptr && entry->state == PrTextureCacheState_Ready ? entry : nullptr;
}

// Records an ask for a settled entry in this frame. The entry becomes the newest of the entries that a trim may evict.
void touch(TextureCache& c, CacheEntry* entry) {
    entry->lastAsked = c.trims;
    if (entry->list == CacheList_Recent || entry->list == CacheList_Held)
        enlist(c, entry, CacheList_Recent, true);
    else if (entry->list == CacheList_Passing)
        enlist(c, entry, CacheList_Passing, true);
}

// Returns the entry of version `version` of `source` for `ask`, which has not arrived. Creates the entry and adds it to
// the queue if the cache does not have it. Marks the entry as asked for in this frame, and raises its priority to the
// priority of the ask if that is higher.
CacheEntry* waitingEntry(TextureCache& c, const PrTextureSource* source, const PrTextureCacheAsk& ask, uint32_t version,
        bool passing = false) {
    PrTextureCacheAsk resolved = askFor(source, ask, version);
    uint64_t key = keyOf(c, source, resolved, version);
    CacheEntry* entry = entryOf(c, key);
    if (entry == nullptr) {
        entry = allocZeroed<CacheEntry>();
        *entry = CacheEntry{
            .key = key,
            .state = PrTextureCacheState_Pending,
            .passing = passing,
            .priority = resolved.priority,
            .version = uint8_t(version),
            .ask = resolved,
            .order = c.orders++,
            .lastAsked = c.trims,
            .source = source,
            .at = c.entries.count,
        };
        prTextureSourceRetain(source);
        c.index.insert(KeyIndex::hashOf(key), c.entries.count);
        c.entries.push(entry);
        uint64_t id = prTextureSourceId(source);
        uint32_t holder = holdersAt(c, id);
        if (holder == kHashMissing) {
            c.holderIndex.insert(KeyIndex::hashOf(id), c.holders.count);
            c.holders.push(CacheHolders{id, 1});
        } else {
            ++c.holders.data[holder].entries;
        }
        queuePush(c, entry);
        return entry;
    }
    bool waiting = entry->state == PrTextureCacheState_Pending && !entry->started;
    bool moved = entry->lastAsked != c.trims || rankOf(resolved.priority) > rankOf(entry->priority);
    entry->lastAsked = c.trims;
    if (rankOf(resolved.priority) > rankOf(entry->priority))
        entry->priority = resolved.priority;
    if (waiting && moved)
        queueFix(c, entry->queueAt);
    if (entry->state == PrTextureCacheState_Failed)
        enlist(c, entry, CacheList_Failed, true);
    return entry;
}

// Returns the region of an entry for `ask`. An ask of encoded values reads the texture through its plain form, if the
// entry has that view.
PrTextureRegion regionFor(const CacheEntry& entry, const PrTextureCacheAsk& ask) {
    PrTextureRegion region = entry.region;
    if (ask.values == PrTextureCacheValues_Encoded && entry.plain.id != 0)
        region.texture.texture = entry.plain;
    return region;
}

// ---------------------------------------------------------------------------------------------------------------------
// Realizations

// Starts the realization of a waiting entry and returns its job. The entry leaves the queue. If the device samples
// nothing that the source produces, the entry fails and the function returns null. Called with the lock held.
CacheJob* PR_NULLABLE prepare(TextureCache& c, CacheEntry* entry) {
    queueRemove(c, entry);
    PrTextureCacheAsk ask = entry->ask;
    CacheChain made = chainOf(c, versionOf(entry->source, entry->version), ask);
    if (made.source == nullptr) {
        entry->state = PrTextureCacheState_Failed;
        entry->status = made.status != PrStatus_Ok ? made.status : PrStatus_NotSupported;
        enlist(c, entry, CacheList_Failed, true);
        return nullptr;
    }
    const PrTextureSource* chain = made.source;
    CacheJob* job = allocZeroed<CacheJob>();
    *job = CacheJob{.cache = &c, .entry = entry, .chain = chain, .flags = ask.flags, .uploadFlags = made.flags};
    const PrTextureDesc& desc = *prTextureSourceDesc(chain);
    if (c.srgbViews && prGpuFormatIsSrgb(desc.format))
        job->uploadFlags |= PrTextureUploadFlag_SrgbViews;
    if (packs(c, chain, ask.flags)) {
        PrIVec2 origin{};
        if (placeSlot(c, desc.format, desc.alpha == PrTextureAlpha_None ? ask.alpha : desc.alpha, desc.extent,
                *entry, &origin)) {
            const CachePage& page = *c.pages.data[entry->page - 1];
            job->packed = true;
            job->page = page.texture;
            job->origin = origin;
            entry->plain = page.plain;
            entry->region = PrTextureRegion{
                .texture = PrTexture{.texture = page.texture, .alpha = page.alpha},
                .extent = PrUVec2{page.size, page.size},
                .levelCount = 1,
                .rect = PrRect{
                    float(origin.x), float(origin.y), float(origin.x + int32_t(desc.extent.x)),
                    float(origin.y + int32_t(desc.extent.y)),
                },
                .source = prTextureSourceDesc(entry->source)->extent,
            };
            entry->bytes = uint64_t(desc.extent.x) * desc.extent.y * 4;
            ++c.slots;
        }
    }
    if (job->packed)
        job->uploadFlags = 0;
    entry->started = true;
    ++c.inFlight;
    ++c.realizations;
    return job;
}

// Starts as many waiting realizations as the limit on realizations in flight allows, best first. Appends their jobs to
// `jobs`. The caller launches the jobs after it releases the lock. A realization of High priority starts even past the
// limit. Called with the lock held.
void pump(TextureCache& c, RawArray<CacheJob*>* jobs) {
    while (c.queue.count != 0) {
        CacheEntry* best = c.queue.data[0];
        bool full = c.limit != 0 && c.inFlight >= c.limit;
        if (full && best->priority != PrTextureCachePriority_High)
            return;
        CacheJob* job = prepare(c, best);
        if (job != nullptr)
            jobs->push(job);
    }
}

void run(void* PR_NULLABLE context);

// Runs `jobs` on the pool of the cache, or on the calling thread if the cache has no pool. Frees the array. Called
// without the lock.
void launch(TextureCache& c, RawArray<CacheJob*>* jobs) {
    for (CacheJob* job : jobs->span()) {
        if (c.desc.tasks != nullptr)
            prTaskPoolPost(c.desc.tasks, run, job);
        else
            run(job);
    }
    jobs->reset();
}

// Completes a realization. The entry takes the texture or slot that the realization made. The host is told that the
// entry arrived. The realizations that waited for a place in flight start. A texture that the device can view through
// its other sRGB form gets a view through its plain form here, for asks of encoded values.
void finish(CacheJob* job, PrStatus status, PrTexture texture) {
    TextureCache& c = *job->cache;
    PrGpuTexture plain{};
    uint32_t levels = 1;
    if (status == PrStatus_Ok && !job->packed) {
        PrGpuTextureDesc made = prGpuTextureDesc(c.desc.device, texture.texture);
        levels = made.levels;
        PrGpuTextureViewDesc view{.kind = made.kind, .format = prGpuFormatPlain(made.format)};
        if ((made.usage & PrGpuTextureUsageFlag_SrgbViews) != 0 && prGpuFormatIsSrgb(made.format))
            status = prGpuTextureCreateView(c.desc.device, texture.texture, view, &plain);
    }
    prMutexLock(&c.lock);
    // The entry is in flight, so nothing removed it.
    CacheEntry* entry = job->entry;
    PR_ASSERT(entry->state == PrTextureCacheState_Pending && entry->started);
    bool asked = entry->asked;
    if (status == PrStatus_Ok) {
        entry->state = PrTextureCacheState_Ready;
        if (!job->packed) {
            PrUVec2 extent = prGpuTextureExtent(c.desc.device, texture.texture);
            const PrTextureDesc& desc = *prTextureSourceDesc(job->chain);
            entry->region = PrTextureRegion{
                .texture = texture,
                .extent = extent,
                .levelCount = levels,
                .rect = PrRect{0, 0, float(desc.extent.x), float(desc.extent.y)},
                .source = prTextureSourceDesc(entry->source)->extent,
            };
            entry->plain = plain;
            entry->bytes = prTextureUploadSize(c.desc.device, &desc, job->uploadFlags);
            c.bytes += entry->bytes;
            ++c.textures;
        }
        enlist(c, entry, entry->passing ? CacheList_Passing : CacheList_Recent, true);
        c.arrivals.fetchAdd(1);
    } else {
        if (texture.texture.id != 0)
            prGpuTextureRelease(c.desc.device, texture.texture);
        freeEntry(c, *entry);
        entry->state = PrTextureCacheState_Failed;
        entry->status = status;
        enlist(c, entry, CacheList_Failed, true);
    }
    entry->asked = false;
    entry->started = false;
    prTextureSourceRelease(job->chain);
    prMemoryFree(job);
    // The callback runs before the realization leaves flight, so that it finishes before the cache can be destroyed.
    if (asked && c.desc.arrived != nullptr) {
        prMutexUnlock(&c.lock);
        c.desc.arrived(c.desc.arrivedContext);
        prMutexLock(&c.lock);
    }
    --c.inFlight;
    RawArray<CacheJob*> next;
    pump(c, &next);
    prConditionBroadcast(&c.settled);
    prMutexUnlock(&c.lock);
    launch(c, &next);
}

void run(void* PR_NULLABLE context) {
    CacheJob* job = static_cast<CacheJob*>(context);
    if (job->packed) {
        // The texels of the slot are read into the page in the way that a source is read into a texture of its own.
        PrTextureReadDesc read{
            .levelCount = 1,
            .layerCount = 1,
            .device = job->cache->desc.device,
            .texture = job->page,
            .origin = job->origin,
            .done = [](void* PR_NULLABLE held, PrTextureRead* read) {
                PrStatus status = prTextureReadStatus(read);
                prTextureReadDestroy(read);
                finish(static_cast<CacheJob*>(held), status, PrTexture{});
            },
            .context = job,
        };
        prTextureReadCreate(job->chain, &read);
        return;
    }
    prTextureUpload(job->cache->desc.device, job->chain, job->uploadFlags, "texture cache",
        [](void* PR_NULLABLE held, PrStatus status, PrTexture texture) {
            finish(static_cast<CacheJob*>(held), status, texture);
        }, job);
}

// Blocks until every realization has arrived or failed. That covers the realizations in flight and the waiting ones.
// The waiting ones start as those in flight make room.
void waitAll(TextureCache& c) {
    prMutexLock(&c.lock);
    while (c.inFlight != 0 || c.queue.count != 0)
        prConditionWait(&c.settled, &c.lock);
    prMutexUnlock(&c.lock);
}

// Cancels the realizations that wait to start, and blocks until none is in flight. Returns with the lock held.
void stopAll(TextureCache& c) {
    prMutexLock(&c.lock);
    while (c.queue.count != 0)
        removeEntry(c, c.queue.data[c.queue.count - 1]);
    while (c.inFlight != 0)
        prConditionWait(&c.settled, &c.lock);
}

// Frees every entry and every page. No realization waits or is in flight. Stops playing the source at `at`, and
// releases the pictures that the player holds.
void stopPlayer(TextureCache& c, uint32_t at) {
    CachePlayer player = c.players.data[at];
    uint32_t last = c.players.count - 1;
    c.playerIndex.erase(KeyIndex::hashOf(prTextureSourceId(player.source)), at);
    if (at != last) {
        c.playerIndex.relocate(KeyIndex::hashOf(prTextureSourceId(c.players.data[last].source)), last, at);
        c.players.data[at] = c.players.data[last];
    }
    c.players.count = last;
    prTextureSourceRelease(player.shown);
    prTextureSourceRelease(player.current);
    prTextureSourceRelease(player.source);
}

void freeAll(TextureCache& c) {
    while (c.players.count != 0)
        stopPlayer(c, c.players.count - 1);
    while (c.entries.count != 0)
        removeEntry(c, c.entries.data[c.entries.count - 1]);
    for (CachePage* page : c.pages.span()) {
        if (page != nullptr)
            destroyPage(c, page);
    }
    c.pages.clear();
}

// Settles `entry`, a waiting realization of every level of a version, from `plain`, the same version without levels in
// a texture of its own, and returns true. The device copies the first level and makes the others from it, in commands
// of their own that the call submits. An ask comes from the recording of a draw list, which the device runs later, so
// the levels are made before any draw samples them, and nothing waits for the entry. Returns false, leaving the entry
// waiting, if the device cannot make the levels. Called with the lock held.
bool levelsFrom(TextureCache& c, CacheEntry* entry, const CacheEntry& plain) {
    PrGpuDevice* device = c.desc.device;
    PrGpuTexture from = plain.region.texture.texture;
    PrGpuTextureDesc desc = prGpuTextureDesc(device, from);
    desc.levels = prGpuMipLevels(PrUVec2{desc.extent.width, desc.extent.height});
    desc.usage |= PrGpuTextureUsageFlag_RenderTarget;
    desc.name = "texture cache";
    PrGpuTexture texture{};
    if (!prGpuDeviceSupportsFormat(device, desc.format, desc.usage) || !prGpuFormatFilters(desc.format) ||
            prGpuTextureCreate(device, &desc, &texture) != PrStatus_Ok)
        return false;
    PrGpuTexture view{};
    if (plain.plain.id != 0 && prGpuTextureCreateView(device, texture,
            PrGpuTextureViewDesc{.kind = desc.kind, .format = prGpuFormatPlain(desc.format)}, &view) != PrStatus_Ok) {
        prGpuTextureRelease(device, texture);
        return false;
    }
    PrGpuCommands* commands = nullptr;
    if (!prGpuCommandsBegin(device, nullptr, &commands)) {
        prGpuTextureRelease(device, view);
        prGpuTextureRelease(device, texture);
        return false;
    }
    // Every texture of the cache rests in the accesses of shader reads.
    PrGpuSubresources all{};
    PrGpuSubresources first{.levels = 1};
    prGpuCommandsTextureBarrier(commands, from, all, PrGpuAccessFlag_ShaderRead, PrGpuAccessFlag_CopyRead);
    prGpuCommandsTextureBarrier(commands, texture, first, PrGpuAccessFlag_Discard, PrGpuAccessFlag_CopyWrite);
    PrGpuTextureRegion whole{.extent = {desc.extent.width, desc.extent.height, 1}};
    prGpuCommandsCopyTexture(commands, from, &whole, texture, &whole);
    prGpuCommandsTextureBarrier(commands, from, all, PrGpuAccessFlag_CopyRead, PrGpuAccessFlag_ShaderRead);
    prGpuCommandsGenerateMipmaps(commands, texture, PrGpuAccessFlag_CopyWrite);
    prGpuCommandsSubmit(commands);
    queueRemove(c, entry);
    entry->state = PrTextureCacheState_Ready;
    entry->region = plain.region;
    entry->region.texture.texture = texture;
    entry->region.levelCount = desc.levels;
    entry->plain = view;
    entry->bytes = prTextureUploadSize(device, prTextureSourceDesc(versionOf(entry->source, entry->version)),
        PrTextureUploadFlag_MakesLevels);
    c.bytes += entry->bytes;
    ++c.textures;
    enlist(c, entry, CacheList_Recent, true);
    return true;
}

// Answers an ask for one still source: the region of the smallest version that has arrived with the detail that the ask
// needs, or of a smaller version while that one is realized. Starts what is missing. An ask for levels of a version
// that the cache holds without them makes them from that texture at once (levelsFrom). `passing` says that the source
// is a picture of a source that changes over time. Called with the lock held.
PrTextureCacheState answer(TextureCache& c, const PrTextureSource* source, PrTextureCacheAsk ask, bool passing,
        PrTextureRegion* out) {
    // A picture that is due starts at once. A realization that has started is not cancelled by the trim of a frame in
    // which its consumer, which may record on another thread, has not asked yet.
    if (passing)
        ask.priority = PrTextureCachePriority_High;
    uint32_t count = prTextureSourcePreviewCount(source);
    uint32_t need = versionFor(source, ask.detail);
    for (uint32_t version = need; version <= count; ++version) {
        CacheEntry* ready = readyEntry(c, source, ask, version);
        if (ready != nullptr) {
            touch(c, ready);
            *out = regionFor(*ready, ask);
            return PrTextureCacheState_Ready;
        }
    }
    CacheEntry* entry = waitingEntry(c, source, ask, need, passing);
    if (entry->state == PrTextureCacheState_Pending && !entry->started && (entry->ask.flags & PrTextureCacheFlag_Levels) != 0) {
        const CacheEntry* plain = entryOf(c, siblingOf(entry->key));
        if (plain != nullptr && plain->state == PrTextureCacheState_Ready && plain->page == 0 &&
                plain->region.levelCount == 1 && levelsFrom(c, entry, *plain)) {
            touch(c, entry);
            *out = regionFor(*entry, ask);
            return PrTextureCacheState_Ready;
        }
    }
    PrTextureCacheState state = entry->state;
    if (state == PrTextureCacheState_Pending) {
        entry->asked = true;
        for (uint32_t version = need; version-- > 0;) {
            CacheEntry* smaller = readyEntry(c, source, ask, version);
            if (smaller != nullptr) {
                touch(c, smaller);
                *out = regionFor(*smaller, ask);
                state = PrTextureCacheState_Partial;
                break;
            }
        }
        // The smallest preview arrives long before the needed version, so the consumer has something to show. A cache
        // without a pool realizes inside the ask. There the preview would only delay the needed version.
        if (state == PrTextureCacheState_Pending && need != 0 && c.desc.tasks != nullptr)
            waitingEntry(c, source, ask, 0, passing)->asked = true;
    }
    return state;
}

// Returns the position of the player of `source`, which is created if the cache does not play the source yet.
uint32_t playerOf(TextureCache& c, const PrTextureSource* source) {
    uint64_t id = prTextureSourceId(source);
    uint32_t at = c.playerIndex.find(KeyIndex::hashOf(id), [&](uint32_t i) {
        return prTextureSourceId(c.players.data[i].source) == id;
    });
    if (at != kHashMissing)
        return at;
    prTextureSourceRetain(source);
    c.playerIndex.insert(KeyIndex::hashOf(id), c.players.count);
    c.players.push(CachePlayer{.source = source});
    return c.players.count - 1;
}

// Asks the player's source what it shows at the cache's time, if the time of its next picture has come. A new picture
// becomes the player's current one.
void advance(TextureCache& c, CachePlayer& player) {
    if (player.current != nullptr && (player.next == 0 || c.time < player.next))
        return;
    PrTextureFrame frame{};
    prTextureSourceFrameAt(player.source, c.time, &frame);
    player.next = frame.next;
    if (player.current != nullptr && frame.serial == player.serial) {
        prTextureSourceRelease(frame.source);
        return;
    }
    prTextureSourceRelease(player.current);
    player.current = frame.source;
    player.serial = frame.serial;
}

// Answers an ask for a source that changes over time: the region of the picture that the source shows at the cache's
// time. While that picture is being realized, the answer is the last picture that an ask was answered with, and before
// any, the source's own first picture. Writes the source of the answer's region to `*picture`, with a reference for the
// caller, or null if the answer has no region. Called with the lock held.
PrTextureCacheState play(TextureCache& c, const PrTextureSource* source, const PrTextureCacheAsk& ask,
        PrTextureRegion* out, const PrTextureSource* PR_NULLABLE* PR_NONNULL picture) {
    // The player is read by position after each call that may add an entry. No call here adds a player.
    uint32_t at = playerOf(c, source);
    c.players.data[at].askedAt = c.time;
    advance(c, c.players.data[at]);
    const PrTextureSource* current = c.players.data[at].current;
    const PrTextureSource* answered = current;
    PrTextureCacheState state = answer(c, current, ask, current != source, out);
    if (state == PrTextureCacheState_Ready) {
        CachePlayer& player = c.players.data[at];
        if (player.shown != current) {
            prTextureSourceRetain(current);
            prTextureSourceRelease(player.shown);
            player.shown = current;
        }
    } else {
        const PrTextureSource* last = c.players.data[at].shown != nullptr ? c.players.data[at].shown : source;
        PrTextureRegion earlier{};
        PrTextureCacheState had = PrTextureCacheState_Pending;
        if (last != current)
            had = answer(c, last, ask, last != source, &earlier);
        bool better = had == PrTextureCacheState_Partial && state != PrTextureCacheState_Partial;
        if (had == PrTextureCacheState_Ready || better) {
            *out = earlier;
            state = had;
            answered = last;
        }
    }
    if (state == PrTextureCacheState_Ready || state == PrTextureCacheState_Partial) {
        prTextureSourceRetain(answered);
        *picture = answered;
    }
    out->playing = true;
    return state;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrTextureCache : TextureCache {};

PrTextureCacheAsk prTextureCacheAskEncoded(PrTextureCacheFlags flags) {
    return PrTextureCacheAsk{
        .flags = flags,
        .values = PrTextureCacheValues_Encoded,
        .alpha = PrTextureAlpha_Premultiplied,
    };
}

PrTextureCacheDesc prTextureCacheDescDefault(PrGpuDevice* PR_NULLABLE device) {
    return PrTextureCacheDesc{.device = device, .budget = kDefaultBudget, .pack = PrTextureCachePack_Auto};
}

PrTextureCache* prTextureCacheCreate(const PrTextureCacheDesc* desc) {
    PrTextureCache* cache = allocZeroed<PrTextureCache>();
    cache->desc = *desc;
    cache->srgbViews = desc->device != nullptr && viewsSrgb(desc->device);
    if (desc->tasks != nullptr)
        cache->limit = desc->concurrency != 0 ? desc->concurrency : prTaskPoolWidth(desc->tasks);
    prMutexInit(&cache->lock);
    prConditionInit(&cache->settled);
    return cache;
}

void prTextureCacheDestroy(PrTextureCache* PR_NULLABLE cache) {
    if (cache == nullptr)
        return;
    stopAll(*cache);
    freeAll(*cache);
    prMutexUnlock(&cache->lock);
    cache->entries.reset();
    cache->index.reset();
    cache->holders.reset();
    cache->holderIndex.reset();
    cache->queue.reset();
    cache->pages.reset();
    cache->players.reset();
    cache->playerIndex.reset();
    prMutexReset(&cache->lock);
    prConditionReset(&cache->settled);
    prMemoryFree(cache);
}

PrGpuDevice* prTextureCacheDevice(const PrTextureCache* cache) {
    return cache->desc.device;
}

void prTextureCacheSetDevice(PrTextureCache* cache, PrGpuDevice* PR_NULLABLE device) {
    stopAll(*cache);
    freeAll(*cache);
    cache->desc.device = device;
    cache->srgbViews = device != nullptr && viewsSrgb(device);
    ++cache->generation;
    prMutexUnlock(&cache->lock);
}

uint64_t prTextureCacheGeneration(const PrTextureCache* cache) {
    return cache->generation;
}

void prTextureCacheSetTime(PrTextureCache* cache, double time) {
    TextureCache& c = *cache;
    prMutexLock(&c.lock);
    c.time = time;
    c.timed = true;
    // A source whose next picture is due is asked what it shows when a consumer next asks for it. The arrival makes the
    // consumers that keep drawings ask.
    bool due = false;
    for (const CachePlayer& player : c.players.span())
        due = due || (player.next != 0 && time >= player.next);
    if (due)
        c.arrivals.fetchAdd(1);
    prMutexUnlock(&c.lock);
}

double prTextureCacheNextFrame(const PrTextureCache* cache) {
    PrMutex* lock = const_cast<PrMutex*>(&cache->lock);
    prMutexLock(lock);
    double next = 0;
    for (const CachePlayer& player : cache->players.span()) {
        if (player.next != 0 && (next == 0 || player.next < next))
            next = player.next;
    }
    prMutexUnlock(lock);
    return next;
}

uint64_t prTextureCacheArrivals(const PrTextureCache* cache) {
    return cache->arrivals.load();
}

PrTextureCacheState prTextureCacheRealize(PrTextureCache* cache, const PrTextureSource* source,
        PrTextureCacheAsk ask, PrTextureRegion* out) {
    PR_ASSERT(ask.alpha == PrTextureAlpha_Straight || ask.alpha == PrTextureAlpha_Premultiplied);
    TextureCache& c = *cache;
    *out = PrTextureRegion{};
    // Only the host changes the device, and no thread asks during that call, so the read needs no lock.
    if (c.desc.device == nullptr)
        return PrTextureCacheState_Pending;
    prMutexLock(&c.lock);
    PrTextureCacheState state = answer(c, source, ask, false, out);
    RawArray<CacheJob*> jobs;
    pump(c, &jobs);
    bool ran = jobs.count != 0 && c.desc.tasks == nullptr;
    prMutexUnlock(&c.lock);
    launch(c, &jobs);
    // Without a pool, the realizations ran in the call, so the ask's answer has changed.
    if (ran)
        return prTextureCacheRealize(cache, source, ask, out);
    return state;
}

PrTextureCacheState prTextureCachePlay(PrTextureCache* cache, const PrTextureSource* source, PrTextureCacheAsk ask,
        PrTextureRegion* out, const PrTextureSource* PR_NULLABLE* PR_NONNULL picture) {
    PR_ASSERT(ask.alpha == PrTextureAlpha_Straight || ask.alpha == PrTextureAlpha_Premultiplied);
    TextureCache& c = *cache;
    *out = PrTextureRegion{};
    *picture = nullptr;
    if (c.desc.device == nullptr)
        return PrTextureCacheState_Pending;
    prMutexLock(&c.lock);
    PrTextureCacheState state;
    if (c.timed && prTextureSourcePlays(source)) {
        state = play(c, source, ask, out, picture);
    } else {
        state = answer(c, source, ask, false, out);
        if (state == PrTextureCacheState_Ready || state == PrTextureCacheState_Partial) {
            prTextureSourceRetain(source);
            *picture = source;
        }
    }
    RawArray<CacheJob*> jobs;
    pump(c, &jobs);
    bool ran = jobs.count != 0 && c.desc.tasks == nullptr;
    prMutexUnlock(&c.lock);
    launch(c, &jobs);
    // Without a pool, the realizations ran in the call, so the ask's answer has changed.
    if (ran) {
        prTextureSourceRelease(*picture);
        return prTextureCachePlay(cache, source, ask, out, picture);
    }
    return state;
}

PrTextureCacheState prTextureCacheWait(PrTextureCache* cache, const PrTextureSource* source, PrTextureCacheAsk ask,
        PrTextureRegion* out) {
    PR_ASSERT(cache->desc.device != nullptr);
    ask.priority = PrTextureCachePriority_High;
    uint32_t need = versionFor(source, ask.detail);
    uint64_t key = keyOf(*cache, source, askFor(source, ask, need), need);
    for (;;) {
        PrTextureCacheState state = prTextureCacheRealize(cache, source, ask, out);
        if (state == PrTextureCacheState_Ready || state == PrTextureCacheState_Failed)
            return state;
        prMutexLock(&cache->lock);
        for (;;) {
            const CacheEntry* entry = entryOf(*cache, key);
            if (entry == nullptr || entry->state != PrTextureCacheState_Pending)
                break;
            prConditionWait(&cache->settled, &cache->lock);
        }
        prMutexUnlock(&cache->lock);
    }
}

PrStatus prTextureCacheStatus(PrTextureCache* cache, const PrTextureSource* source, PrTextureCacheAsk ask) {
    uint32_t need = versionFor(source, ask.detail);
    prMutexLock(&cache->lock);
    const CacheEntry* entry = entryOf(*cache, keyOf(*cache, source, askFor(source, ask, need), need));
    PrStatus status = entry != nullptr && entry->state == PrTextureCacheState_Failed ? entry->status : PrStatus_Ok;
    prMutexUnlock(&cache->lock);
    return status;
}

void prTextureCacheWaitAll(PrTextureCache* cache) {
    waitAll(*cache);
}

void prTextureCacheTrim(PrTextureCache* cache) {
    TextureCache& c = *cache;
    prMutexLock(&c.lock);
    uint64_t current = c.trims;
    // Cancels each realization that waits to start and that no ask of this frame wants. The queue holds only what the
    // last frames asked for, so the walk is short.
    for (uint32_t at = c.queue.count; at-- > 0;) {
        if (at < c.queue.count && c.queue.data[at]->lastAsked != current) {
            removeEntry(c, c.queue.data[at]);
            ++c.cancelled;
            // The entry that took the position of the removed entry is checked too.
            at = at < c.queue.count ? at + 1 : c.queue.count;
        }
    }
    // Forgets each failure that no ask of this frame wants and whose source only the cache holds.
    for (CacheEntry* entry = c.lists[CacheList_Failed].oldest; entry != nullptr && entry->lastAsked != current;) {
        CacheEntry* newer = entry->newer;
        if (!isHeld(c, *entry))
            removeEntry(c, entry);
        entry = newer;
    }
    // An entry that was held when a trim came to evict it may have been released since. Each trim checks a few of these
    // entries again, oldest first. Every entry is checked in time, and the cost of a trim does not grow with their
    // number.
    for (uint32_t checked = 0; checked < kHeldChecks; ++checked) {
        CacheEntry* entry = c.lists[CacheList_Held].oldest;
        if (entry == nullptr)
            break;
        enlist(c, entry, isHeld(c, *entry) ? CacheList_Held : CacheList_Recent, isHeld(c, *entry));
    }
    // A source that no consumer has asked for in the last second of the cache's time stops playing. A consumer that
    // records on another thread asks less often than the host's frames end, so one frame without an ask is not enough.
    for (uint32_t at = c.players.count; at-- > 0;) {
        if (c.time - c.players.data[at].askedAt > kPlayedFor)
            stopPlayer(c, at);
    }
    // A picture of such a source is not asked for again once the source shows another. Its texture goes as soon as no
    // drawing holds its source, whatever the budget. A picture that a drawing still holds is checked again later.
    for (uint32_t checked = 0; checked < kHeldChecks; ++checked) {
        CacheEntry* entry = c.lists[CacheList_Passing].oldest;
        if (entry == nullptr || entry->lastAsked == current)
            break;
        if (isHeld(c, *entry))
            enlist(c, entry, CacheList_Passing, true);
        else
            removeEntry(c, entry);
    }
    // While the cache is over its budget, it evicts the textures that no ask of this frame wants, least recently asked
    // for first. A texture whose source something else holds stays, because a drawing may name its region.
    while (c.bytes > c.desc.budget) {
        CacheEntry* entry = c.lists[CacheList_Recent].oldest;
        if (entry == nullptr || entry->lastAsked == current)
            break;
        if (isHeld(c, *entry))
            enlist(c, entry, CacheList_Held, true);
        else
            removeEntry(c, entry);
    }
    ++c.trims;
    prMutexUnlock(&c.lock);
}

PrTextureCacheInfo prTextureCacheInfo(const PrTextureCache* cache) {
    PrMutex* lock = const_cast<PrMutex*>(&cache->lock);
    prMutexLock(lock);
    PrTextureCacheInfo info{
        .realizations = cache->realizations,
        .bytes = cache->bytes,
        .textures = cache->textures,
        .pages = cache->pageCount,
        .slots = cache->slots,
        .waiting = cache->queue.count,
        .inFlight = cache->inFlight,
        .cancelled = cache->cancelled,
    };
    prMutexUnlock(lock);
    return info;
}
