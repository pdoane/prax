// Font hinters: the backends that fit glyph outlines to the pixel grid of the size at which they are drawn.

#include "pr/font_hinter.h"

#include "pr/ascii.h"
#include "pr/assert.h"

#if defined(PR_FONT_HAS_FREETYPE)
#include "pr/font_freetype.h"
#endif

namespace pr_impl {

namespace {

constexpr PrStringView kBackendNames[PrFontHinterBackend_Count] = {"default", "freetype"};

// The backends that the build has, which Default creates the first of, and then Default, so that a build without
// backends creates none.
constexpr PrFontHinterBackend kBuilt[] = {
#if defined(PR_FONT_HAS_FREETYPE)
    PrFontHinterBackend_FreeType,
#endif
    PrFontHinterBackend_Default,
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrFontHinterBackend prFontHinterBackendFromName(PrStringView name) {
    for (PrFontHinterBackend backend = 0; backend < PrFontHinterBackend_Count; ++backend) {
        if (prAsciiEqualsFold(kBackendNames[backend], name))
            return backend;
    }
    return PrFontHinterBackend_Count;
}

PrStringView prFontHinterBackendName(PrFontHinterBackend backend) {
    return backend < PrFontHinterBackend_Count ? kBackendNames[backend] : PrStringView{};
}

bool prFontHinterBackendIsBuilt(PrFontHinterBackend backend) {
    for (PrFontHinterBackend built : kBuilt) {
        if (built != PrFontHinterBackend_Default && (built == backend || backend == PrFontHinterBackend_Default))
            return true;
    }
    return false;
}

PrFontHinter* PR_NULLABLE prFontHinterCreate(PrFontHinterBackend backend) {
    switch (backend == PrFontHinterBackend_Default ? kBuilt[0] : backend) {
#if defined(PR_FONT_HAS_FREETYPE)
        case PrFontHinterBackend_FreeType: return prFontFreeTypeCreate();
#endif
        default: return nullptr;
    }
}

void prFontHinterDestroy(PrFontHinter* PR_NULLABLE hinter) {
    if (hinter != nullptr && hinter->destroy != nullptr)
        hinter->destroy(hinter);
}

bool prFontHinterOutline(PrFontHinter* hinter, const PrFont* font, uint16_t glyph, float pixelsPerEm,
        PrFontHinting hinting, float embolden, PrPath* out) {
    PR_ASSERT(hinting != PrFontHinting_None && pixelsPerEm > 0);
    return hinter->outline(hinter, font, glyph, pixelsPerEm, hinting, embolden, out);
}

bool prFontHinterAdvance(PrFontHinter* hinter, const PrFont* font, uint16_t glyph, float pixelsPerEm,
        PrFontHinting hinting, float* out) {
    PR_ASSERT(hinting != PrFontHinting_None && pixelsPerEm > 0);
    return hinter->advance(hinter, font, glyph, pixelsPerEm, hinting, out);
}

void prFontHinterForget(PrFontHinter* hinter, uint64_t fontId) {
    if (hinter->forget != nullptr)
        hinter->forget(hinter, fontId);
}
