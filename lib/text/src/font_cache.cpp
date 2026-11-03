// The face and font states that a shaper keeps (font_cache.h).

#include "text/src/font_cache.h"

#include "pr/align.h"
#include "pr/alloc.h"
#include "pr/integer.h"
#include "pr/raw_array.h"

namespace pr_impl {

struct CachedFace {
    const uint8_t* data;
    uint32_t index;
    const PrFont* owner;                // a reference to a font of these bytes
    uint32_t fonts;                     // the number of kept fonts that read the face
    uint64_t used;
};

struct CachedFont {
    CachedFace* face;
    const PrFont* font;                 // a reference
    uint64_t id;
    uint64_t used;
};

struct FontCache {
    RawArray<CachedFace*> faces;
    RawArray<CachedFont*> fonts;
    uint64_t clock;
};

namespace {

// The number of fonts kept, and of kept faces that no kept font reads. The least recently used is freed first.
constexpr uint32_t kFonts = 64;
constexpr uint32_t kIdleFaces = 8;

// The alignment of the backend's state, which starts after the fields of an entry.
constexpr size_t kStateAlignment = 16;

constexpr size_t stateOffset(size_t size) {
    return alignUp(size, kStateAlignment);
}

void* stateOf(CachedFace* face) {
    return reinterpret_cast<uint8_t*>(face) + stateOffset(sizeof(CachedFace));
}

void* stateOf(CachedFont* font) {
    return reinterpret_cast<uint8_t*>(font) + stateOffset(sizeof(CachedFont));
}

FontCache* cacheOf(PrTextShaper* shaper) {
    if (shaper->fonts == nullptr)
        shaper->fonts = reinterpret_cast<PrTextFontCache*>(allocZeroed<FontCache>());
    return reinterpret_cast<FontCache*>(shaper->fonts);
}

void destroyFace(PrTextShaper* shaper, CachedFace* face) {
    shaper->destroyFace(shaper, stateOf(face));
    prFontRelease(face->owner);
    prMemoryFree(face);
}

void destroyFont(PrTextShaper* shaper, CachedFont* font) {
    shaper->destroyFont(shaper, stateOf(font));
    --font->face->fonts;
    prFontRelease(font->font);
    prMemoryFree(font);
}

// Frees the idle faces beyond the number kept, least recently used first.
void trimFaces(PrTextShaper* shaper, FontCache* cache) {
    for (;;) {
        uint32_t idle = 0;
        uint32_t oldest = cache->faces.count;
        for (uint32_t i = 0; i < cache->faces.count; ++i) {
            if (cache->faces.data[i]->fonts != 0)
                continue;
            ++idle;
            if (oldest == cache->faces.count || cache->faces.data[i]->used < cache->faces.data[oldest]->used)
                oldest = i;
        }
        if (idle <= kIdleFaces)
            return;
        destroyFace(shaper, cache->faces.data[oldest]);
        cache->faces.data[oldest] = cache->faces.last();
        cache->faces.pop();
    }
}

// Returns the face for the bytes and index of the font, read on its first use, or null if the backend cannot read it.
CachedFace* PR_NULLABLE faceOf(PrTextShaper* shaper, FontCache* cache, const PrFont* font) {
    const uint8_t* data = prFontData(font).data;
    uint32_t index = prFontIndex(font);
    for (uint32_t i = 0; i < cache->faces.count; ++i) {
        CachedFace* face = cache->faces.data[i];
        if (face->data == data && face->index == index) {
            face->used = cache->clock;
            return face;
        }
    }
    CachedFace* face =
        static_cast<CachedFace*>(prMemoryAllocZeroed(stateOffset(sizeof(CachedFace)) + shaper->faceSize));
    if (!shaper->createFace(shaper, font, stateOf(face))) {
        prMemoryFree(face);
        return nullptr;
    }
    face->data = data;
    face->index = index;
    face->owner = font;
    prFontRetain(font);
    face->used = cache->clock;
    cache->faces.push(face);
    return face;
}

} // namespace

bool textFontCacheFind(PrTextShaper* shaper, const PrFont* font, void* PR_NULLABLE* PR_NONNULL face,
        void* PR_NULLABLE* PR_NONNULL state) {
    FontCache* cache = cacheOf(shaper);
    uint64_t id = prFontId(font);
    ++cache->clock;
    for (uint32_t i = 0; i < cache->fonts.count; ++i) {
        CachedFont* cached = cache->fonts.data[i];
        if (cached->id == id) {
            cached->used = cache->clock;
            cached->face->used = cache->clock;
            *face = stateOf(cached->face);
            *state = stateOf(cached);
            return true;
        }
    }
    CachedFace* cachedFace = faceOf(shaper, cache, font);
    if (cachedFace == nullptr)
        return false;
    CachedFont* cached =
        static_cast<CachedFont*>(prMemoryAllocZeroed(stateOffset(sizeof(CachedFont)) + shaper->fontSize));
    cached->face = cachedFace;
    ++cachedFace->fonts;
    cached->font = font;
    prFontRetain(font);
    cached->id = id;
    cached->used = cache->clock;
    shaper->createFont(shaper, stateOf(cachedFace), font, stateOf(cached));
    if (cache->fonts.count < kFonts) {
        cache->fonts.push(cached);
    } else {
        uint32_t oldest = 0;
        for (uint32_t i = 1; i < cache->fonts.count; ++i) {
            if (cache->fonts.data[i]->used < cache->fonts.data[oldest]->used)
                oldest = i;
        }
        destroyFont(shaper, cache->fonts.data[oldest]);
        cache->fonts.data[oldest] = cached;
        trimFaces(shaper, cache);
    }
    *face = stateOf(cachedFace);
    *state = stateOf(cached);
    return true;
}

void textFontCacheDestroy(PrTextShaper* shaper) {
    if (shaper->fonts == nullptr)
        return;
    FontCache* cache = reinterpret_cast<FontCache*>(shaper->fonts);
    for (uint32_t i = 0; i < cache->fonts.count; ++i)
        destroyFont(shaper, cache->fonts.data[i]);
    cache->fonts.reset();
    for (uint32_t i = 0; i < cache->faces.count; ++i)
        destroyFace(shaper, cache->faces.data[i]);
    cache->faces.reset();
    prMemoryFree(cache);
    shaper->fonts = nullptr;
}

} // namespace pr_impl
