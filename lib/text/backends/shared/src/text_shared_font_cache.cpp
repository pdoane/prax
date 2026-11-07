// The face and font states that a shaper keeps, which the backends share.

#include "text/backends/shared/src/text_shared_font_cache.h"

#include "pr/align.h"
#include "pr/alloc.h"
#include "pr/integer.h"
#include "pr/raw_array.h"

namespace pr_impl {

struct TextSharedFace {
    const uint8_t* data;
    uint32_t index;
    const PrFont* owner;                // a reference to a font of these bytes
    uint32_t fonts;                     // the number of kept fonts that read the face
    uint64_t used;
};

struct TextSharedFont {
    TextSharedFace* face;
    const PrFont* font;                 // a reference
    uint64_t id;
    uint64_t used;
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

void* stateOf(TextSharedFace* face) {
    return reinterpret_cast<uint8_t*>(face) + stateOffset(sizeof(TextSharedFace));
}

void* stateOf(TextSharedFont* font) {
    return reinterpret_cast<uint8_t*>(font) + stateOffset(sizeof(TextSharedFont));
}

void destroyFace(PrTextShaper* shaper, TextSharedFontCache* cache, TextSharedFace* face) {
    cache->states->destroyFace(shaper, stateOf(face));
    prFontRelease(face->owner);
    prMemoryFree(face);
}

void destroyFont(PrTextShaper* shaper, TextSharedFontCache* cache, TextSharedFont* font) {
    cache->states->destroyFont(shaper, stateOf(font));
    --font->face->fonts;
    prFontRelease(font->font);
    prMemoryFree(font);
}

// Frees the idle faces beyond the number kept, least recently used first.
void trimFaces(PrTextShaper* shaper, TextSharedFontCache* cache) {
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
        destroyFace(shaper, cache, cache->faces.data[oldest]);
        cache->faces.data[oldest] = cache->faces.last();
        cache->faces.pop();
    }
}

// Returns the face for the bytes and index of the font, read on its first use, or null if the backend cannot read it.
TextSharedFace* PR_NULLABLE faceOf(PrTextShaper* shaper, TextSharedFontCache* cache, const PrFont* font) {
    const uint8_t* data = prFontData(font).data;
    uint32_t index = prFontIndex(font);
    for (uint32_t i = 0; i < cache->faces.count; ++i) {
        TextSharedFace* face = cache->faces.data[i];
        if (face->data == data && face->index == index) {
            face->used = cache->clock;
            return face;
        }
    }
    TextSharedFace* face = static_cast<TextSharedFace*>(prMemoryAllocZeroed(stateOffset(sizeof(TextSharedFace)) +
        cache->states->faceSize));
    if (!cache->states->createFace(shaper, font, stateOf(face))) {
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

// Writes the states of the font and of its face to `face` and `state`, creating them on the first use of the font. They
// are kept at least until the next call. Returns false and writes nothing if the backend cannot read the face.
bool find(PrTextShaper* shaper, TextSharedFontCache* cache, const PrFont* font, void* PR_NULLABLE* PR_NONNULL face,
        void* PR_NULLABLE* PR_NONNULL state) {
    uint64_t id = prFontId(font);
    ++cache->clock;
    for (uint32_t i = 0; i < cache->fonts.count; ++i) {
        TextSharedFont* cached = cache->fonts.data[i];
        if (cached->id == id) {
            cached->used = cache->clock;
            cached->face->used = cache->clock;
            *face = stateOf(cached->face);
            *state = stateOf(cached);
            return true;
        }
    }
    TextSharedFace* cachedFace = faceOf(shaper, cache, font);
    if (cachedFace == nullptr)
        return false;
    TextSharedFont* cached = static_cast<TextSharedFont*>(prMemoryAllocZeroed(stateOffset(sizeof(TextSharedFont)) +
        cache->states->fontSize));
    cached->face = cachedFace;
    ++cachedFace->fonts;
    cached->font = font;
    prFontRetain(font);
    cached->id = id;
    cached->used = cache->clock;
    cache->states->createFont(shaper, stateOf(cachedFace), font, stateOf(cached));
    if (cache->fonts.count < kFonts) {
        cache->fonts.push(cached);
    } else {
        uint32_t oldest = 0;
        for (uint32_t i = 1; i < cache->fonts.count; ++i) {
            if (cache->fonts.data[i]->used < cache->fonts.data[oldest]->used)
                oldest = i;
        }
        destroyFont(shaper, cache, cache->fonts.data[oldest]);
        cache->fonts.data[oldest] = cached;
        trimFaces(shaper, cache);
    }
    *face = stateOf(cachedFace);
    *state = stateOf(cached);
    return true;
}

} // namespace

PrStatus textSharedFontCacheShape(PrTextShaper* shaper, TextSharedFontCache* cache, const PrTextRun* run,
        PrTextGlyphBuffer* out) {
    void* face = nullptr;
    void* font = nullptr;
    if (!find(shaper, cache, run->font, &face, &font))
        return PrStatus_NotSupported;
    return cache->states->shape(shaper, face, font, run, out);
}

void textSharedFontCacheReset(PrTextShaper* shaper, TextSharedFontCache* cache) {
    for (uint32_t i = 0; i < cache->fonts.count; ++i)
        destroyFont(shaper, cache, cache->fonts.data[i]);
    cache->fonts.reset();
    for (uint32_t i = 0; i < cache->faces.count; ++i)
        destroyFace(shaper, cache, cache->faces.data[i]);
    cache->faces.reset();
}

} // namespace pr_impl
