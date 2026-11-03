// A cache of the coverage of glyphs and small paths on pages of 8-bit alpha.
//
// Pages fill with shelves, which are rows of entries of about the same height. Each shelf fills from the left. An entry
// is keyed by its resource's id, the part of the resource that is drawn, how it is filled or stroked, its scale, and
// its position within a pixel to a quarter pixel. It keeps a texel of padding on each side so that linear sampling
// never reads a neighbor. When every page is full, the atlas empties a page that holds no entry found since
// prDrawAtlasNext.

#include "pr/draw_shared_atlas.h"

#include "pr/alloc.h"
#include "pr/array.h"
#include "pr/draw_glyph.h"
#include "pr/draw_lower.h"
#include "pr/float.h"
#include "pr/font.h"
#include "pr/hash.h"
#include "pr/hash_index.h"
#include "pr/mem.h"
#include "pr/path_polyline.h"
#include "pr/path_rasterizer.h"
#include "pr/path_stroke.h"
#include "pr/vec.h"

namespace pr_impl {

namespace {

// The number of positions within a pixel, along each axis, at which an entry is rasterized: those at which a glyph is
// drawn (prDrawGlyphPlace).
constexpr float kPhases = float(kPrDrawGlyphPhases);

// The largest side length, in target pixels, of a path's coverage drawn from the atlas.
constexpr float kMaxPathExtent = 128;

// The index that means no page.
constexpr uint32_t kNoPage = 0xffffffffu;

// A row of a page that entries fill from the left.
struct Shelf {
    uint32_t y;
    uint32_t height;
    uint32_t x;
};

struct Page {
    uint8_t* texels;
    Array<Shelf> shelves;
    uint32_t top;               // the row where the next shelf starts
    uint64_t lastFound;         // the generation in which an entry on the page was last found
    uint32_t dirtyLeft;         // texels changed since they were last taken; empty if right <= left
    uint32_t dirtyTop;
    uint32_t dirtyRight;
    uint32_t dirtyBottom;
};

// The kind of thing whose coverage an entry holds.
enum {
    EntryKind_Glyph,
    EntryKind_Fill,
    EntryKind_Stroke,
};
typedef uint8_t EntryKind;

// An entry's identity. A field that does not apply to its kind is zero.
PR_NO_PADDING_BEGIN
struct Key {
    uint64_t resource;          // the font's id, or the path's id
    uint32_t glyph;             // the glyph's index in its font
    EntryKind kind;
    PrPathFillRule rule;        // how a filled path's contours wind
    uint8_t phaseX;             // the translation's position within a target pixel, in quarters
    uint8_t phaseY;

    // The transform's scale along each axis.
    float scaleX;
    float scaleY;

    // How a stroke's outline is made from its path. The fields hold the hash of the dash lengths and width profile, the
    // width, joins, caps and dashes, the direction of a contour of zero length, whether the stroke is measured in
    // target pixels, and where it lies across the path.
    uint64_t pattern;
    float width;
    float miterLimit;
    float dashOffset;
    PrVec2 zeroLengthDirection;
    uint32_t dashCount;
    uint32_t profileCount;
    PrPathJoin join;
    PrPathCap startCap;
    PrPathCap endCap;
    PrPathCap dashCap;
    uint8_t nonScaling;
    PrDrawStrokeAlign align;
    uint8_t _pad[6];
};
PR_NO_PADDING_END

// An entry on a page. The box of its texels lies at (left, top) from the entry's origin, in target pixels.
struct Entry {
    Key key;
    uint32_t page;
    uint32_t x;
    uint32_t y;
    uint32_t width;             // zero for an entry with no texels
    uint32_t height;
    int32_t left;
    int32_t top;
};

struct DrawAtlas {
    PrDrawAtlasOptions options;
    uint64_t generation;        // advanced by prDrawAtlasNext
    uint64_t rasterizations;
    uint64_t clears;
    Array<Page> pages;
    Array<Entry> entries;
    HashIndex entryIndex;
    PrPathRasterizer* rasterizer;
    PrPath* shape;
    PrPathPolyline outline;         // an entry's coverage in target pixels, from its origin at its phase
    PrPathPolyline aligned;         // an aligned stroke's parts (prDrawLowerStroke)
    PrPathPolyline region;
};

uint32_t hashOf(const Key& key) {
    return uint32_t(prHashBytes(&key, sizeof(key), 0));
}

void markDirty(Page& page, uint32_t left, uint32_t top, uint32_t right, uint32_t bottom) {
    if (page.dirtyRight <= page.dirtyLeft) {
        page.dirtyLeft = left;
        page.dirtyTop = top;
        page.dirtyRight = right;
        page.dirtyBottom = bottom;
        return;
    }
    page.dirtyLeft = left < page.dirtyLeft ? left : page.dirtyLeft;
    page.dirtyTop = top < page.dirtyTop ? top : page.dirtyTop;
    page.dirtyRight = right > page.dirtyRight ? right : page.dirtyRight;
    page.dirtyBottom = bottom > page.dirtyBottom ? bottom : page.dirtyBottom;
}

// Returns the number of rows at the top of a page that its shelves do not use.
uint32_t keptRows(const DrawAtlas* a, uint32_t index) {
    return index == 0 ? a->options.solidSize : 0;
}

uint32_t addPage(DrawAtlas* a) {
    uint32_t size = a->options.size;
    Page page{};
    page.texels = static_cast<uint8_t*>(prMemoryAllocZeroed(size_t(size) * size));
    page.shelves = Array<Shelf>{};
    uint32_t index = a->pages.count;
    uint32_t solid = keptRows(a, index);
    for (uint32_t y = 0; y < solid; ++y)
        prMemFill(page.texels + size_t(y) * size, 0xff, solid);
    page.top = solid;
    markDirty(page, 0, 0, size, size);
    a->pages.push(page);
    return a->pages.count - 1;
}

// Empties the page and forgets its entries.
void clearPage(DrawAtlas* a, uint32_t index) {
    ++a->clears;
    Page& page = a->pages.data[index];
    uint32_t size = a->options.size;
    uint32_t keep = keptRows(a, index);
    prMemFill(page.texels + size_t(keep) * size, 0, size_t(size - keep) * size);
    page.shelves.clear();
    page.top = keep;
    markDirty(page, 0, keep, size, size);
    uint32_t kept = 0;
    for (uint32_t i = 0; i < a->entries.count; ++i) {
        if (a->entries.data[i].page != index)
            a->entries.data[kept++] = a->entries.data[i];
    }
    a->entries.count = kept;
    a->entryIndex.clear();
    for (uint32_t i = 0; i < a->entries.count; ++i)
        a->entryIndex.insert(hashOf(a->entries.data[i].key), i);
}

// Places a box on a page's shelves. Returns false if the box does not fit.
bool placeOn(const DrawAtlas* a, Page& page, uint32_t width, uint32_t height, uint32_t* x, uint32_t* y) {
    uint32_t size = a->options.size;
    for (uint32_t i = 0; i < page.shelves.count; ++i) {
        Shelf& shelf = page.shelves.data[i];
        if (height <= shelf.height && height * 4 >= shelf.height * 3 && shelf.x + width <= size) {
            *x = shelf.x;
            *y = shelf.y;
            shelf.x += width;
            return true;
        }
    }
    if (page.top + height > size || width > size)
        return false;
    page.shelves.push(Shelf{page.top, height, width});
    *x = 0;
    *y = page.top;
    page.top += height;
    return true;
}

// Places a box, adding or emptying a page if no page has room. Returns false if every page holds an entry found since
// prDrawAtlasNext.
bool placeBox(DrawAtlas* a, uint32_t width, uint32_t height, uint32_t* page, uint32_t* x, uint32_t* y) {
    for (uint32_t i = 0; i < a->pages.count; ++i) {
        if (placeOn(a, a->pages.data[i], width, height, x, y)) {
            *page = i;
            return true;
        }
    }
    uint32_t reclaimed = kNoPage;
    if (a->pages.count < a->options.maxPages) {
        reclaimed = addPage(a);
    } else {
        for (uint32_t i = 0; i < a->pages.count; ++i) {
            if (a->pages.data[i].lastFound != a->generation) {
                clearPage(a, i);
                reclaimed = i;
                break;
            }
        }
    }
    if (reclaimed == kNoPage || !placeOn(a, a->pages.data[reclaimed], width, height, x, y))
        return false;
    *page = reclaimed;
    return true;
}

// Returns the entry for the key, and marks its page as found. Returns null if the atlas has no entry for the key.
const Entry* PR_NULLABLE lookUp(DrawAtlas* a, const Key& key, uint32_t hash) {
    uint32_t found = a->entryIndex.find(hash, [&](uint32_t i) {
        return prMemEquals(&a->entries.data[i].key, &key, sizeof(key));
    });
    if (found == kHashMissing)
        return nullptr;
    const Entry& e = a->entries.data[found];
    if (e.width != 0)
        a->pages.data[e.page].lastFound = a->generation;
    return &e;
}

// Adds the key's entry, rasterizing `a->outline` by `rule` on a page. Returns null if no page has room.
const Entry* PR_NULLABLE add(DrawAtlas* a, const Key& key, uint32_t hash, PrPathFillRule rule) {
    Entry e{};
    e.key = key;
    if (a->outline.pointCount != 0) {
        PrRect bounds = prPathPolylineBounds(&a->outline);
        int32_t left = int32_t(prF32Floor(bounds.left));
        int32_t top = int32_t(prF32Floor(bounds.top));
        int32_t right = int32_t(prF32Ceil(bounds.right));
        int32_t bottom = int32_t(prF32Ceil(bounds.bottom));
        if (right > left && bottom > top) {
            uint32_t width = uint32_t(right - left);
            uint32_t height = uint32_t(bottom - top);
            uint32_t page = 0;
            uint32_t x = 0;
            uint32_t y = 0;
            if (!placeBox(a, width + 2, height + 2, &page, &x, &y))
                return nullptr;
            Page& p = a->pages.data[page];
            uint32_t size = a->options.size;
            PrPixels mask{p.texels + size_t(y + 1) * size + x + 1, size, {width, height}};
            prPathRasterizerFill(a->rasterizer, &a->outline, rule, PrPathCoverage_Area, PrIVec2{left, top}, mask);
            ++a->rasterizations;
            markDirty(p, x, y, x + width + 2, y + height + 2);
            p.lastFound = a->generation;
            e.page = page;
            e.x = x + 1;
            e.y = y + 1;
            e.width = width;
            e.height = height;
            e.left = left;
            e.top = top;
        }
    }
    a->entryIndex.insert(hash, a->entries.count);
    a->entries.push(e);
    return &a->entries.last();
}

// Computes the pixel that contains a point, and the point's phase within that pixel to a quarter pixel. Returns false
// if the point is not finite.
bool phaseOf(PrVec2 point, PrVec2* pixel, uint8_t* phaseX, uint8_t* phaseY) {
    if (!prF32IsFinite(point.x) || !prF32IsFinite(point.y))
        return false;
    float column = prF32Floor(point.x);
    float x = prF32Round((point.x - column) * kPhases);
    if (x >= kPhases) {
        x = 0;
        column += 1;
    }
    float row = prF32Floor(point.y);
    float y = prF32Round((point.y - row) * kPhases);
    if (y >= kPhases) {
        y = 0;
        row += 1;
    }
    *pixel = PrVec2{column, row};
    *phaseX = uint8_t(x);
    *phaseY = uint8_t(y);
    return true;
}

// Returns the entry drawn from the atlas. Its texels are placed relative to the pixel that contains the entry's origin.
PrDrawAtlasEntry entryOf(const Entry& e, PrVec2 pixel) {
    return PrDrawAtlasEntry{
        e.page, {e.x, e.y}, {e.width, e.height},
        {int32_t(pixel.x) + e.left, int32_t(pixel.y) + e.top},
    };
}

// Returns true if the path has both an open contour and a closed contour. An aligned stroke of such a path composites
// the two kinds of contour separately.
bool mixesContours(const PrPath* path) {
    const PrPathVerb* verbs = prPathVerbs(path);
    uint32_t count = prPathVerbCount(path);
    bool open = false;
    bool closed = false;
    for (uint32_t i = 0; i < count; ++i) {
        if (verbs[i] != PrPathVerb_Move)
            continue;
        uint32_t end = i + 1;
        while (end < count && verbs[end] != PrPathVerb_Move && verbs[end] != PrPathVerb_Close)
            ++end;
        if (end == i + 1)
            continue;
        if (end < count && verbs[end] == PrPathVerb_Close)
            closed = true;
        else
            open = true;
    }
    return open && closed;
}

// Returns the hash of a stroke's dashes and width profile, or zero if it has neither.
uint64_t patternOf(const PrPathStroke& stroke) {
    uint64_t hash = 0;
    if (stroke.dashCount != 0)
        hash = prHashBytes(stroke.dashes, sizeof(float) * stroke.dashCount, hash + 1);
    if (stroke.profileCount != 0)
        hash = prHashBytes(stroke.profile, sizeof(PrPathWidthPoint) * stroke.profileCount, hash + 2);
    return hash;
}

void destroyPages(DrawAtlas* a) {
    for (uint32_t i = 0; i < a->pages.count; ++i) {
        prMemoryFree(a->pages.data[i].texels);
        a->pages.data[i].shelves.reset();
    }
    a->pages.clear();
    a->entries.clear();
    a->entryIndex.clear();
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrDrawAtlas : DrawAtlas {};

PrDrawAtlasOptions prDrawAtlasOptionsDefault(void) {
    return PrDrawAtlasOptions{1024, 4, 0, 0.05f};
}

PrDrawAtlas* prDrawAtlasCreate(const PrDrawAtlasOptions* PR_NULLABLE options) {
    PrDrawAtlas* a = allocZeroed<PrDrawAtlas>();
    a->options = options != nullptr ? *options : prDrawAtlasOptionsDefault();
    a->rasterizer = prPathRasterizerCreate();
    a->shape = prPathCreate();
    return a;
}

void prDrawAtlasDestroy(PrDrawAtlas* PR_NULLABLE atlas) {
    DrawAtlas* a = atlas;
    if (a == nullptr)
        return;
    destroyPages(a);
    a->pages.reset();
    a->entries.reset();
    a->entryIndex.reset();
    prPathRasterizerDestroy(a->rasterizer);
    prPathDestroy(a->shape);
    prPathPolylineReset(&a->outline);
    prPathPolylineReset(&a->aligned);
    prPathPolylineReset(&a->region);
    prMemoryFree(a);
}

void prDrawAtlasClear(PrDrawAtlas* atlas) {
    destroyPages(atlas);
}

void prDrawAtlasNext(PrDrawAtlas* atlas) {
    ++atlas->generation;
    // The solid block stays in place for every draw.
    if (atlas->options.solidSize != 0) {
        if (atlas->pages.count == 0)
            addPage(atlas);
        atlas->pages.data[0].lastFound = atlas->generation;
    }
}
void prDrawAtlasKeepPage(PrDrawAtlas* atlas, uint32_t page) {
    if (page < atlas->pages.count)
        atlas->pages.data[page].lastFound = atlas->generation;
}

bool prDrawAtlasGlyph(PrDrawAtlas* atlas, const PrFontGlyphRun* run, uint32_t index, PrAffine2 transform,
        PrDrawAtlasEntry* out) {
    // The atlas holds a glyph at its phase (prDrawGlyphPlace).
    PrDrawGlyphPlacement placement = prDrawGlyphPlace(run, index, transform);
    if (!placement.phased)
        return false;
    const PrAffine2& glyph = placement.glyph;
    PrVec2 drawn = placement.origin;
    PrVec2 pixel{prF32Floor(drawn.x), prF32Floor(drawn.y)};
    Key key{};
    key.phaseX = uint8_t((drawn.x - pixel.x) * kPhases);
    key.phaseY = uint8_t((drawn.y - pixel.y) * kPhases);
    key.resource = prFontId(run->font);
    key.glyph = run->glyphs[index].id;
    key.scaleX = glyph.a;
    key.scaleY = glyph.d;
    key.kind = EntryKind_Glyph;
    uint32_t hash = hashOf(key);
    const Entry* e = lookUp(atlas, key, hash);
    if (e == nullptr) {
        // The glyph is outlined at its own origin and placed at the phase, so that its coverage is the same however far
        // from the target's origin the glyph that first asks for it lies.
        PrFontGlyph alone = run->glyphs[index];
        alone.position = PrVec2{0, 0};
        PrFontGlyphRun single = *run;
        single.glyphs = &alone;
        single.glyphCount = 1;
        single.transforms = run->transforms != nullptr ? &run->transforms[index] : nullptr;
        PrAffine2 place = transform;
        place.e = float(key.phaseX) / kPhases;
        place.f = float(key.phaseY) / kPhases;
        prPathClear(atlas->shape);
        prPathPolylineClear(&atlas->outline);
        if (prDrawGlyphPath(&single, 0, &place, atlas->shape))
            prPathFlatten(atlas->shape, place, atlas->options.tolerance, PrPathFlattening_Balanced, &atlas->outline);
        e = add(atlas, key, hash, PrPathFillRule_NonZero);
        if (e == nullptr)
            return false;
    }
    *out = entryOf(*e, pixel);
    return true;
}

bool prDrawAtlasPath(PrDrawAtlas* atlas, const PrPath* path, PrPathFillRule rule,
        const PrDrawStroke* PR_NULLABLE stroke, PrAffine2 transform, PrDrawAtlasEntry* out) {
    if (transform.b != 0 || transform.c != 0 || !(transform.a > 0) || !(transform.d > 0))
        return false;
    if (stroke != nullptr && !(stroke->shape.width > 0))
        return false;
    // The size of the coverage is judged before stroking, from the path's bounds and the stroke's reach.
    PrRect bounds = prPathBounds(path);
    if (prRectIsEmpty(bounds) && stroke == nullptr)
        return false;
    float reach = 0;
    if (stroke != nullptr) {
        reach = prPathStrokeReach(&stroke->shape) * (stroke->align == PrDrawStrokeAlign_Center ? 1 : 2);
        if (!stroke->nonScaling)
            reach *= prF32Max(transform.a, transform.d);
    }
    float width = (bounds.right - bounds.left) * transform.a + 2 * reach;
    float height = (bounds.bottom - bounds.top) * transform.d + 2 * reach;
    if (!(width <= kMaxPathExtent) || !(height <= kMaxPathExtent))
        return false;
    if (stroke != nullptr && stroke->align != PrDrawStrokeAlign_Center && mixesContours(path))
        return false;
    PrVec2 pixel{};
    Key key{};
    if (!phaseOf(PrVec2{transform.e, transform.f}, &pixel, &key.phaseX, &key.phaseY))
        return false;
    key.resource = prPathId(path);
    key.scaleX = transform.a;
    key.scaleY = transform.d;
    if (stroke != nullptr) {
        const PrPathStroke& shape = stroke->shape;
        key.kind = EntryKind_Stroke;
        key.pattern = patternOf(shape);
        key.width = shape.width;
        key.miterLimit = shape.miterLimit;
        key.dashOffset = shape.dashCount != 0 ? shape.dashOffset : 0;
        key.zeroLengthDirection = shape.zeroLengthDirection;
        key.dashCount = shape.dashCount;
        key.profileCount = shape.profileCount;
        key.join = shape.join;
        key.startCap = shape.startCap;
        key.endCap = shape.endCap;
        key.dashCap = shape.dashCap;
        key.nonScaling = stroke->nonScaling ? 1 : 0;
        key.align = stroke->align;
    } else {
        key.kind = EntryKind_Fill;
        key.rule = rule;
    }
    uint32_t hash = hashOf(key);
    const Entry* e = lookUp(atlas, key, hash);
    if (e == nullptr) {
        // The transform's scale, with the origin at the phase.
        PrAffine2 place = transform;
        place.e = float(key.phaseX) / kPhases;
        place.f = float(key.phaseY) / kPhases;
        float tolerance = atlas->options.tolerance;
        prPathPolylineClear(&atlas->outline);
        if (stroke == nullptr) {
            prPathFlatten(path, place, tolerance, PrPathFlattening_Balanced, &atlas->outline);
        } else {
            prPathPolylineClear(&atlas->aligned);
            prPathPolylineClear(&atlas->region);
            prDrawLowerStroke(path, stroke, place, tolerance, &atlas->outline, &atlas->aligned, &atlas->region);
            if (atlas->aligned.pointCount != 0) {
                // Cuts the aligned outline to the region's interior or exterior. The path's contours are all closed, so
                // the centered outline is empty.
                bool inside = stroke->align == PrDrawStrokeAlign_Inside;
                PrPathPolylineOp op = inside ? PrPathPolylineOp_Intersect : PrPathPolylineOp_Difference;
                prPathPolylineClear(&atlas->outline);
                prPathPolylineCombine(&atlas->aligned, PrPathFillRule_NonZero, &atlas->region, PrPathFillRule_NonZero,
                    op, &atlas->outline);
            }
            rule = PrPathFillRule_NonZero;
        }
        e = add(atlas, key, hash, rule);
        if (e == nullptr)
            return false;
    }
    *out = entryOf(*e, pixel);
    return true;
}

PrDrawAtlasInfo prDrawAtlasInfo(const PrDrawAtlas* atlas) {
    return PrDrawAtlasInfo{atlas->entries.count, atlas->rasterizations, atlas->clears};
}

uint32_t prDrawAtlasPageCount(const PrDrawAtlas* atlas) {
    return atlas->pages.count;
}

PrPixelView prDrawAtlasPageTexels(const PrDrawAtlas* atlas, uint32_t page) {
    uint32_t size = atlas->options.size;
    return PrPixelView{atlas->pages.data[page].texels, size, {size, size}};
}

bool prDrawAtlasTakeChanges(PrDrawAtlas* atlas, uint32_t page, PrIRect* out) {
    Page& p = atlas->pages.data[page];
    if (p.dirtyRight <= p.dirtyLeft)
        return false;
    *out = PrIRect{int32_t(p.dirtyLeft), int32_t(p.dirtyTop), int32_t(p.dirtyRight), int32_t(p.dirtyBottom)};
    p.dirtyLeft = p.dirtyRight = 0;
    return true;
}
