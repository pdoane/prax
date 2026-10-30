// The image cache: the gallery's files decoded at full size and as thumbnails.

#include "examples/alchemy/image_cache.h"

#include "pr/file.h"
#include "pr/image_process.h"

#include "examples/alchemy/text.h"

#include <condition_variable>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

namespace alchemy {

// The decoded images of files, and the decodes on the pool.
struct ImageCache {
    enum {
        LoadState_Loading,
        LoadState_Ready,
        LoadState_Failed,
    };
    typedef uint8_t LoadState;

    struct Entry {
        LoadState state = ImageCache::LoadState_Loading;
        const PrDrawImage* PR_NULLABLE image = nullptr;
        uint64_t lastFrame = 0;
    };

    // A decode in flight, which a pool thread fills and the pump adopts.
    struct Decode {
        ImageCache* cache;
        std::string path;
        ImageScale scale;
        const PrDrawImage* PR_NULLABLE image;
    };

    PrTaskPool* pool;
    PrImageCodec* codec;
    std::function<void()> wake;
    uint64_t frame = 1;
    std::map<std::pair<std::string, ImageScale>, Entry> entries;

    // The decodes that landed, and the count in flight, which freeing waits for.
    std::mutex mutex;
    std::condition_variable idle;
    std::vector<Decode*> landed;
    uint32_t inFlight = 0;
};

namespace {

typedef ImageCache::Decode Decode;
typedef ImageCache::Entry Entry;
typedef std::map<std::pair<std::string, ImageScale>, Entry> Entries;

// The frames that an image may go unrequested before it is freed: a thumbnail scrolled out of the browser, or an image
// that the viewport no longer shows.
constexpr uint64_t kThumbnailIdleFrames = 600;
constexpr uint64_t kFullIdleFrames = 120;

void runDecode(void* PR_NULLABLE context) {
    Decode* decode = static_cast<Decode*>(context);
    ImageCache* cache = decode->cache;
    PrFileMapping mapping{};
    if (prFileMap(decode->path, &mapping)) {
        const PrDrawImage* image = imageDecode(cache->codec, mapping.data, mapping.size);
        prFileUnmap(&mapping);
        if (image != nullptr && decode->scale == ImageScale_Thumbnail) {
            decode->image = imageShrink(image, kThumbnailMaxEdge);
            prDrawImageRelease(image);
        } else {
            decode->image = image;
        }
    }
    {
        std::lock_guard<std::mutex> lock(cache->mutex);
        cache->landed.push_back(decode);
        --cache->inFlight;
        cache->idle.notify_all();
    }
    if (cache->wake)
        cache->wake();
}

} // namespace

ImageCache* imageCacheCreate(PrTaskPool* pool, PrImageCodec* codec, std::function<void()> wake) {
    ImageCache* cache = new ImageCache();
    cache->pool = pool;
    cache->codec = codec;
    cache->wake = std::move(wake);
    return cache;
}

void imageCacheDestroy(ImageCache* PR_NULLABLE cache) {
    if (cache == nullptr)
        return;
    {
        std::unique_lock<std::mutex> lock(cache->mutex);
        cache->idle.wait(lock, [cache] { return cache->inFlight == 0; });
    }
    for (Decode* decode : cache->landed) {
        prDrawImageRelease(decode->image);
        delete decode;
    }
    for (std::pair<const std::pair<std::string, ImageScale>, Entry>& entry : cache->entries)
        prDrawImageRelease(entry.second.image);
    delete cache;
}

const PrDrawImage* PR_NULLABLE imageCacheGet(ImageCache* cache, const std::string& path, ImageScale scale) {
    std::pair<std::string, ImageScale> key{path, scale};
    Entries::iterator found = cache->entries.find(key);
    if (found != cache->entries.end()) {
        found->second.lastFrame = cache->frame;
        return found->second.image;
    }
    cache->entries[key].lastFrame = cache->frame;
    {
        std::lock_guard<std::mutex> lock(cache->mutex);
        ++cache->inFlight;
    }
    prTaskPoolPost(cache->pool, runDecode, new Decode{cache, path, scale, nullptr});
    return nullptr;
}

bool imageCacheFailed(ImageCache* cache, const std::string& path, ImageScale scale) {
    Entries::iterator found = cache->entries.find({path, scale});
    return found != cache->entries.end() && found->second.state == ImageCache::LoadState_Failed;
}

void imageCacheForget(ImageCache* cache, const std::string& path) {
    for (ImageScale scale : {ImageScale_Full, ImageScale_Thumbnail}) {
        Entries::iterator found = cache->entries.find({path, scale});
        if (found == cache->entries.end())
            continue;
        prDrawImageRelease(found->second.image);
        cache->entries.erase(found);
    }
}

bool imageCachePump(ImageCache* cache) {
    std::vector<Decode*> landed;
    {
        std::lock_guard<std::mutex> lock(cache->mutex);
        landed.swap(cache->landed);
    }
    for (Decode* decode : landed) {
        Entries::iterator found = cache->entries.find({decode->path, decode->scale});
        // A decode of a file that was forgotten while it ran is dropped.
        if (found == cache->entries.end() || found->second.state != ImageCache::LoadState_Loading) {
            prDrawImageRelease(decode->image);
        } else {
            found->second.image = decode->image;
            found->second.state = decode->image != nullptr ? ImageCache::LoadState_Ready : ImageCache::LoadState_Failed;
        }
        delete decode;
    }
    for (Entries::iterator it = cache->entries.begin(); it != cache->entries.end();) {
        uint64_t idle = it->first.second == ImageScale_Full ? kFullIdleFrames : kThumbnailIdleFrames;
        if (it->second.state != ImageCache::LoadState_Loading && cache->frame - it->second.lastFrame > idle) {
            prDrawImageRelease(it->second.image);
            it = cache->entries.erase(it);
        } else {
            ++it;
        }
    }
    ++cache->frame;
    return !landed.empty();
}

const PrDrawImage* PR_NULLABLE imageDecode(PrImageCodec* codec, const void* bytes, size_t size) {
    const PrDrawImage* image = nullptr;
    PrByteView view{static_cast<const uint8_t*>(bytes), size};
    if (prDrawImageCreateFromBytes(codec, view, &image, nullptr) != PrStatus_Ok)
        return nullptr;
    return image;
}

const PrDrawImage* imageShrink(const PrDrawImage* image, uint32_t maxEdge) {
    PrUVec2 extent = prDrawImageExtent(image);
    PrPixelFormat format = prDrawImageFormat(image);
    // An image in half floats holds extended sRGB, which a shrunk copy would have to carry, so only 8-bit images
    // shrink.
    bool shrinkable = format == PrPixelFormat_Rgba8 || format == PrPixelFormat_Bgra8;
    if (!shrinkable || (extent.x <= maxEdge && extent.y <= maxEdge)) {
        prDrawImageRetain(image);
        return image;
    }
    uint32_t pixelSize = 4;
    PrPixelView above = prDrawImagePixels(image);
    std::vector<uint8_t> level;
    while (above.extent.x > maxEdge || above.extent.y > maxEdge) {
        PrUVec2 next{above.extent.x > 1 ? above.extent.x / 2 : 1, above.extent.y > 1 ? above.extent.y / 2 : 1};
        std::vector<uint8_t> below(size_t(next.x) * next.y * pixelSize);
        prImageMipLevel(above, PrPixels{below.data(), next.x * pixelSize, next}, format);
        level.swap(below);
        above = PrPixelView{level.data(), next.x * pixelSize, next};
    }
    PrDrawImageDesc desc{
        .pixels = above,
        .format = format,
        .alpha = prDrawImageAlpha(image),
        .space = PrColorSpace_Srgb,
    };
    const PrDrawImage* shrunk = nullptr;
    if (prDrawImageCreate(&desc, &shrunk) != PrStatus_Ok) {
        prDrawImageRetain(image);
        return image;
    }
    return shrunk;
}

} // namespace alchemy
