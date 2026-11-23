// The image cache: the texture sources of the gallery's files. A texture catalog names the sources by their paths.

#include "apps/alchemy/image_cache.h"

#include "pr/array.h"
#include "pr/function.h"
#include "pr/image_texture_source.h"
#include "pr/mem.h"
#include "pr/texture_catalog.h"

namespace alchemy {

struct ImageCache {
    Function<void()> wake;
    PrTextureFormatSet* formats;
    PrTextureCatalog* catalog;
    Array<const PrTextureSource*> returned;     // the images of the frame's requests, each with a reference
};

ImageCache* imageCacheCreate(PrTaskPool* pool, const PrImageCodecSet* codec, Function<void()> wake) {
    ImageCache* images = new ImageCache();
    images->wake = move(wake);
    // The gallery shows its images, so their colors are read as sRGB.
    PrTextureFormat* formats[] = {prImageTextureFormatCreate(codec, PrImageTextureColor_Srgb)};
    images->formats = prTextureFormatSetCreate(formats, 1);
    PrTextureCatalogDesc desc = prTextureCatalogDescDefault(images->formats);
    desc.tasks = pool;
    desc.opened = [](void* PR_NULLABLE context) {
        ImageCache* opened = static_cast<ImageCache*>(context);
        if (opened->wake)
            opened->wake();
    };
    desc.openedContext = images;
    images->catalog = prTextureCatalogCreate(&desc);
    return images;
}

void imageCacheDestroy(ImageCache* PR_NULLABLE cache) {
    if (cache == nullptr)
        return;
    for (const PrTextureSource* image : cache->returned)
        prTextureSourceRelease(image);
    prTextureCatalogDestroy(cache->catalog);
    prTextureFormatSetDestroy(cache->formats);
    delete cache;
}

const PrTextureSource* PR_NULLABLE imageCacheGet(ImageCache* cache, PrStringView path) {
    const PrTextureSource* image = nullptr;
    if (prTextureCatalogSource(cache->catalog, path, &image) != PrTextureCatalogState_Ready)
        return nullptr;
    cache->returned.push(image);
    return image;
}

bool imageCacheFailed(ImageCache* cache, PrStringView path) {
    return prTextureCatalogStatus(cache->catalog, path) != PrStatus_Ok;
}

void imageCacheForget(ImageCache* cache, PrStringView path) {
    prTextureCatalogForget(cache->catalog, path);
}

void imageCachePump(ImageCache* cache) {
    for (const PrTextureSource* image : cache->returned)
        prTextureSourceRelease(image);
    cache->returned.clear();
    prTextureCatalogTrim(cache->catalog);
}

const PrTextureSource* PR_NULLABLE imageDecode(const PrImageCodecSet* codec, const void* bytes, size_t size) {
    const PrTextureSource* image = nullptr;
    PrByteView view{static_cast<const uint8_t*>(bytes), size};
    if (prImageTextureSourceOpenBytes(view, codec, nullptr, PrImageTextureColor_Srgb, &image) != PrStatus_Ok)
        return nullptr;
    return image;
}

} // namespace alchemy
