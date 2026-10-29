// The FreeType backend of font hinters.
//
// The hinter owns one FreeType library and a face for each font that it hints. A face is not safe to use from two
// threads at once, and a library is not safe to make faces from on two threads at once, so the mutex of the hinter
// guards both. Each face keeps the last few sizes that it hinted at.

#include "pr/font_freetype.h"

#include "pr/alloc.h"
#include "pr/array.h"
#include "pr/float.h"
#include "pr/memory.h"
#include "pr/mutex.h"

#include "freetype/freetype.h"
#include "freetype/ftmm.h"
#include "freetype/ftmodapi.h"
#include "freetype/ftoutln.h"
#include "freetype/ftsizes.h"
#include "freetype/tttables.h"

namespace pr_impl {

namespace {

// The number of sizes that a face keeps loaded.
constexpr uint32_t kHintedSizes = 8;

// A size loaded for hinted outlines, and when it was last used.
struct HintedSize {
    FT_Size size;
    float pixelsPerEm;
    uint64_t used;
};

// The face of one font.
struct Face {
    uint64_t fontId;
    FT_Face face;
    bool instructed;                    // the font has its own instructions (fpgm)
    HintedSize sizes[kHintedSizes];
    uint64_t clock;
};

struct FreeTypeHinter : PrFontHinter {
    PrMutex mutex;
    FT_MemoryRec_ memory;
    FT_Library library;
    Array<Face*> faces;
};

void* ftAlloc(FT_Memory, long size) {
    return prMemoryAlloc(size_t(size));
}

void ftFree(FT_Memory, void* block) {
    prMemoryFree(block);
}

void* ftRealloc(FT_Memory, long, long size, void* block) {
    return prMemoryRealloc(block, size_t(size));
}

void closeFace(Face* face) {
    if (face->face != nullptr)
        FT_Done_Face(face->face);
    prMemoryFree(face);
}

// Returns the face of the font, opened at its axis values on first use, or null if FreeType cannot open it. The mutex
// is held.
Face* faceOf(FreeTypeHinter* hinter, const PrFont* font) {
    uint64_t id = prFontId(font);
    for (uint32_t i = 0; i < hinter->faces.count; ++i) {
        if (hinter->faces.data[i]->fontId == id)
            return hinter->faces.data[i];
    }
    if (hinter->library == nullptr)
        return nullptr;
    Face* face = allocZeroed<Face>();
    face->fontId = id;
    PrByteView bytes = prFontData(font);
    if (FT_New_Memory_Face(hinter->library, bytes.data, FT_Long(bytes.size), FT_Long(prFontIndex(font)), &face->face)
        != 0) {
        face->face = nullptr;
        closeFace(face);
        return nullptr;
    }
    uint32_t axes = prFontAxisCount(font);
    if (axes != 0 && FT_HAS_MULTIPLE_MASTERS(face->face)) {
        Array<FT_Fixed> coords;
        for (uint32_t i = 0; i < axes; ++i)
            coords.push(FT_Fixed(roundF32(prFontAxisValue(font, i) * 65536)));
        FT_Set_Var_Design_Coordinates(face->face, coords.count, coords.data);
        coords.reset();
    }
    FT_ULong length = 0;
    face->instructed = FT_Load_Sfnt_Table(face->face, FT_MAKE_TAG('f', 'p', 'g', 'm'), 0, nullptr, &length) == 0
                       && length != 0;
    hinter->faces.push(face);
    return face;
}

// Activates a size of the face that fits `pixelsPerEm`, loaded once and kept among the last few requested, and sets the
// load flags that hint at that size. Returns false if the size cannot be made. The mutex is held.
bool activate(Face* face, float pixelsPerEm, PrFontHinting hinting, FT_Int32* flags) {
    ++face->clock;
    HintedSize* slot = nullptr;
    for (HintedSize& h : face->sizes) {
        if (h.size != nullptr && h.pixelsPerEm == pixelsPerEm) {
            slot = &h;
            break;
        }
    }
    if (slot == nullptr) {
        slot = &face->sizes[0];
        for (HintedSize& h : face->sizes) {
            if (h.size == nullptr) {
                slot = &h;
                break;
            }
            if (h.used < slot->used)
                slot = &h;
        }
        if (slot->size != nullptr)
            FT_Done_Size(slot->size);
        slot->size = nullptr;
        if (FT_New_Size(face->face, &slot->size) != 0) {
            slot->size = nullptr;
            return false;
        }
        FT_Activate_Size(slot->size);
        if (FT_Set_Char_Size(face->face, 0, FT_F26Dot6(roundF32(pixelsPerEm * 64)), 72, 72) != 0) {
            FT_Done_Size(slot->size);
            slot->size = nullptr;
            return false;
        }
        slot->pixelsPerEm = pixelsPerEm;
    }
    slot->used = face->clock;
    FT_Activate_Size(slot->size);
    *flags = FT_LOAD_NO_BITMAP;
    if (hinting == PrFontHinting_Light)
        *flags |= FT_LOAD_TARGET_LIGHT | FT_LOAD_FORCE_AUTOHINT;
    else
        *flags |= FT_LOAD_TARGET_NORMAL | (face->instructed ? 0 : FT_LOAD_FORCE_AUTOHINT);
    return true;
}

// Loads the glyph hinted at the size. Returns false if FreeType cannot load it. The mutex is held.
bool loadHinted(FreeTypeHinter* hinter, const PrFont* font, uint16_t glyph, float pixelsPerEm, PrFontHinting hinting,
                Face** out) {
    Face* face = faceOf(hinter, font);
    FT_Int32 flags = 0;
    if (face == nullptr || !activate(face, pixelsPerEm, hinting, &flags)
        || FT_Load_Glyph(face->face, glyph, flags) != 0)
        return false;
    *out = face;
    return true;
}

// The path that an outline decomposes into, and the scale from FreeType's 26.6 pixels to ems.
struct Decompose {
    PrPath* path;
    float scale;
    bool open;
};

PrVec2 pointOf(const Decompose& d, const FT_Vector* v) {
    return PrVec2{float(v->x) * d.scale, -float(v->y) * d.scale};
}

int moveTo(const FT_Vector* to, void* user) {
    Decompose& d = *static_cast<Decompose*>(user);
    if (d.open)
        prPathClose(d.path);
    prPathMoveTo(d.path, pointOf(d, to));
    d.open = true;
    return 0;
}

int lineTo(const FT_Vector* to, void* user) {
    Decompose& d = *static_cast<Decompose*>(user);
    prPathLineTo(d.path, pointOf(d, to));
    return 0;
}

int conicTo(const FT_Vector* control, const FT_Vector* to, void* user) {
    Decompose& d = *static_cast<Decompose*>(user);
    prPathQuadTo(d.path, pointOf(d, control), pointOf(d, to));
    return 0;
}

int cubicTo(const FT_Vector* control1, const FT_Vector* control2, const FT_Vector* to, void* user) {
    Decompose& d = *static_cast<Decompose*>(user);
    prPathCubicTo(d.path, pointOf(d, control1), pointOf(d, control2), pointOf(d, to));
    return 0;
}

void destroyHinter(PrFontHinter* base) {
    FreeTypeHinter* hinter = static_cast<FreeTypeHinter*>(base);
    for (uint32_t i = 0; i < hinter->faces.count; ++i)
        closeFace(hinter->faces.data[i]);
    hinter->faces.reset();
    if (hinter->library != nullptr)
        FT_Done_Library(hinter->library);
    prMutexReset(&hinter->mutex);
    prMemoryFree(hinter);
}

bool hintOutline(PrFontHinter* base, const PrFont* font, uint16_t glyph, float pixelsPerEm, PrFontHinting hinting,
                 float embolden, PrPath* out) {
    FreeTypeHinter* hinter = static_cast<FreeTypeHinter*>(base);
    prMutexLock(&hinter->mutex);
    Face* face = nullptr;
    bool loaded = loadHinted(hinter, font, glyph, pixelsPerEm, hinting, &face);
    if (loaded) {
        FT_GlyphSlot slot = face->face->glyph;
        if (slot->format == FT_GLYPH_FORMAT_OUTLINE && slot->outline.n_contours != 0) {
            FT_Pos strength = FT_Pos(roundF32(2 * embolden * 64 * pixelsPerEm));
            if (strength != 0) {
                // FreeType grows the outline right and up by the whole strength, so half of it is moved back.
                FT_Outline_EmboldenXY(&slot->outline, strength, strength);
                FT_Outline_Translate(&slot->outline, -strength / 2, -strength / 2);
            }
            FT_Outline_Funcs funcs{moveTo, lineTo, conicTo, cubicTo, 0, 0};
            Decompose d{out, 1 / (64 * pixelsPerEm), false};
            FT_Outline_Decompose(&slot->outline, &funcs, &d);
            if (d.open)
                prPathClose(out);
        }
    }
    prMutexUnlock(&hinter->mutex);
    return loaded;
}

bool hintAdvance(PrFontHinter* base, const PrFont* font, uint16_t glyph, float pixelsPerEm, PrFontHinting hinting,
                 float* out) {
    FreeTypeHinter* hinter = static_cast<FreeTypeHinter*>(base);
    prMutexLock(&hinter->mutex);
    Face* face = nullptr;
    bool loaded = loadHinted(hinter, font, glyph, pixelsPerEm, hinting, &face);
    if (loaded)
        *out = float(face->face->glyph->advance.x) / 64;
    prMutexUnlock(&hinter->mutex);
    return loaded;
}

void forget(PrFontHinter* base, uint64_t fontId) {
    FreeTypeHinter* hinter = static_cast<FreeTypeHinter*>(base);
    prMutexLock(&hinter->mutex);
    for (uint32_t i = 0; i < hinter->faces.count; ++i) {
        if (hinter->faces.data[i]->fontId == fontId) {
            closeFace(hinter->faces.data[i]);
            hinter->faces.data[i] = hinter->faces.last();
            hinter->faces.pop();
            break;
        }
    }
    prMutexUnlock(&hinter->mutex);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrFontHinter* prFontFreeTypeCreate(void) {
    FreeTypeHinter* hinter = allocZeroed<FreeTypeHinter>();
    hinter->destroy = destroyHinter;
    hinter->outline = hintOutline;
    hinter->advance = hintAdvance;
    hinter->forget = forget;
    prMutexInit(&hinter->mutex);
    hinter->memory.alloc = ftAlloc;
    hinter->memory.free = ftFree;
    hinter->memory.realloc = ftRealloc;
    if (FT_New_Library(&hinter->memory, &hinter->library) == 0)
        FT_Add_Default_Modules(hinter->library);
    else
        hinter->library = nullptr;
    return hinter;
}
