// The image cache: the gallery's files decoded at full size and as thumbnails.

#include "examples/alchemy/image_cache.h"

#include "pr/array.h"
#include "pr/cache.h"
#include "pr/file.h"
#include "pr/function.h"
#include "pr/image_process.h"
#include "pr/string.h"

namespace alchemy {

// The context of the cache of one scale.
struct ScaleCache {
    ImageCache* images;
    ImageScale scale;
    PrCache* cache;
};

// A cache of decoded images for each scale, whose loads decode on the pool.
struct ImageCache {
    PrTaskPool* pool;
    PrImageCodec* codec;
    Function<void()> wake;
    ScaleCache scales[2];               // by ImageScale
};

namespace {

// The frames that an image may go unrequested before it is freed: a thumbnail scrolled out of the browser, or an image
// that the viewport no longer shows.
constexpr uint32_t kThumbnailIdleFrames = 600;
constexpr uint32_t kFullIdleFrames = 120;

// A decode on the pool, which finishes its load.
struct Decode {
    const ScaleCache* scale;
    String path;
    PrCacheLoad* load;
};

void runDecode(void* PR_NULLABLE context) {
    Decode* decode = static_cast<Decode*>(context);
    ImageCache* images = decode->scale->images;
    const PrDrawImage* image = nullptr;
    PrByteView mapping{};
    if (prFileMap(decode->path, &mapping)) {
        image = imageDecode(images->codec, mapping.data, mapping.size);
        prFileUnmap(&mapping);
        if (image != nullptr && decode->scale->scale == ImageScale_Thumbnail) {
            const PrDrawImage* full = image;
            image = imageShrink(full, kThumbnailMaxEdge);
            prDrawImageRelease(full);
        }
    }
    prCacheLoadFinish(decode->load, image);
    delete decode;
}

void* startDecode(void* PR_NULLABLE context, PrStringView path, PrCacheLoad* load) {
    const ScaleCache* scale = static_cast<const ScaleCache*>(context);
    prTaskPoolPost(scale->images->pool, runDecode, new Decode{scale, path, load});
    return nullptr;
}

void releaseImage(void* PR_NULLABLE, const void* image) {
    prDrawImageRelease(static_cast<const PrDrawImage*>(image));
}

void wakeOwner(void* PR_NULLABLE context) {
    ImageCache* images = static_cast<const ScaleCache*>(context)->images;
    if (images->wake)
        images->wake();
}

} // namespace

ImageCache* imageCacheCreate(PrTaskPool* pool, PrImageCodec* codec, Function<void()> wake) {
    ImageCache* images = new ImageCache();
    images->pool = pool;
    images->codec = codec;
    images->wake = move(wake);
    const ImageScale scales[] = {ImageScale_Full, ImageScale_Thumbnail};
    for (ImageScale scale : scales) {
        ScaleCache& entry = images->scales[scale];
        entry.images = images;
        entry.scale = scale;
        PrCacheDesc desc{
            .load = startDecode,
            .release = releaseImage,
            .wake = wakeOwner,
            .context = &entry,
            .idlePumps = scale == ImageScale_Full ? kFullIdleFrames : kThumbnailIdleFrames,
        };
        entry.cache = prCacheCreate(&desc);
    }
    return images;
}

void imageCacheDestroy(ImageCache* PR_NULLABLE cache) {
    if (cache == nullptr)
        return;
    for (ScaleCache& scale : cache->scales)
        prCacheDestroy(scale.cache);
    delete cache;
}

const PrDrawImage* PR_NULLABLE imageCacheGet(ImageCache* cache, PrStringView path, ImageScale scale) {
    return static_cast<const PrDrawImage*>(prCacheValue(cache->scales[scale].cache, path));
}

bool imageCacheFailed(ImageCache* cache, PrStringView path, ImageScale scale) {
    return prCacheKeyState(cache->scales[scale].cache, path) == PrCacheState_Failed;
}

void imageCacheForget(ImageCache* cache, PrStringView path) {
    for (ScaleCache& scale : cache->scales)
        prCacheRemove(scale.cache, path);
}

bool imageCachePump(ImageCache* cache) {
    bool landed = false;
    for (ScaleCache& scale : cache->scales)
        landed = prCachePump(scale.cache) || landed;
    return landed;
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
    Array<uint8_t> level;
    while (above.extent.x > maxEdge || above.extent.y > maxEdge) {
        PrUVec2 next{above.extent.x > 1 ? above.extent.x / 2 : 1, above.extent.y > 1 ? above.extent.y / 2 : 1};
        Array<uint8_t> below(size_t(next.x) * next.y * pixelSize);
        prImageMipLevel(above, PrPixels{below.data(), next.x * pixelSize, next}, format);
        level = move(below);
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
