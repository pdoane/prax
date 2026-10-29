// Style records: the tables that intern groups and records, and what layout derives from fonts.

#include "pr/layout_style.h"
#include "layout/src/layout_style.h"

#include "pr/alloc.h"
#include "pr/ascii.h"
#include "pr/bitset.h"
#include "pr/float.h"
#include "pr/hash.h"
#include "pr/locale_common.h"
#include "pr/mem.h"
#include "pr/string_ops.h"
#include "pr/text.h"
#include "pr/utf8.h"

#include "layout/src/layout_background.h"
#include "layout/src/layout_box.h"
#include "layout/src/layout_tree.h"

namespace pr_impl {

namespace {

// The seed that interned values are hashed with.
constexpr uint64_t kLayoutSeed = 0x2f0b1c3a5d7e9f61ull;

// The reference count of a freed entry, which no live entry reaches.
constexpr uint32_t kLayoutFreed = 0xffffffffu;

// A slot of the table, at `index`, which the table has room for.
template <typename T>
T& tableSlot(LayoutTable<T>& table, uint32_t index) {
    return table.blocks.data[index / kLayoutTableBlock][index % kLayoutTableBlock];
}

// Returns the hash that a table keeps for `value`.
template <typename T>
uint32_t tableHash(const T& value) {
    return uint32_t(hashBytes(&value, sizeof(T), kLayoutSeed));
}

// A record is four 8-byte words of group indices and one more index, which products mix, where the bytes' general hash
// would loop.
static_assert(sizeof(PrLayoutStyle) == 36);
template <>
uint32_t tableHash(const PrLayoutStyle& value) {
    constexpr uint64_t kMix = 0x9e3779b97f4a7c15ull;
    uint64_t words[4];
    uint32_t last = 0;
    memCopy(words, &value, sizeof words);
    memCopy(&last, value.groups + 8, sizeof last);
    return uint32_t(
        hashFold((words[0] * kMix) ^ words[1], (words[2] ^ kMix) + (words[3] + uint64_t(last) * kMix) * kMix));
}

// Adds an entry of `value` with no references, which the next collect frees unless something names it. An interned
// entry goes in the index by `hash`.
template <typename T>
uint32_t tableAdd(LayoutTable<T>& table, const T& value, uint32_t hash, bool interned) {
    uint32_t at = 0;
    if (table.free.count != 0) {
        at = table.free.last();
        table.free.pop();
    } else {
        at = table.references.count;
        if (at % kLayoutTableBlock == 0)
            table.blocks.push(static_cast<T*>(prMemoryAlloc(sizeof(T) * kLayoutTableBlock)));
        table.references.push(0);
        table.hashes.push(0);
        table.indexed.push(false);
    }
    memCopy(&tableSlot(table, at), &value, sizeof(T));
    table.references.data[at] = 0;
    table.hashes.data[at] = hash;
    table.indexed.data[at] = interned;
    if (interned)
        table.index.insert(hash, at);
    table.candidates.push(at);
    return at;
}

template <typename T>
uint32_t tableIntern(LayoutTable<T>& table, const T& value, bool* added = nullptr) {
    uint32_t hash = tableHash(value);
    uint32_t found = table.index.find(hash, [&](uint32_t at) {
        return memEquals(&layoutTableAt(table, at), &value, sizeof(T));
    });
    if (found != kHashMissing)
        return found;
    if (added != nullptr)
        *added = true;
    return tableAdd(table, value, hash, true);
}

// Interns the kind's default at index 0, where a permanent reference keeps it.
template <typename T>
void tableInit(LayoutTable<T>& table, const T& value) {
    uint32_t at = tableIntern(table, value);
    PR_ASSERT(at == 0);
    table.references.data[at] = 1;
    table.candidates.clear();
}

// Adds `delta` to the references of the entry at `index`, and lists the entry for the next collect if that leaves it
// with none.
template <typename T>
void tableCount(LayoutTable<T>& table, uint32_t index, uint32_t delta) {
    PR_ASSERT(table.references.data[index] != kLayoutFreed && (delta == 1 || table.references.data[index] != 0));
    table.references.data[index] += delta;
    if (table.references.data[index] == 0)
        table.candidates.push(index);
}

template <typename T>
void tableFree(LayoutTable<T>& table, uint32_t index) {
    PR_ASSERT(index != 0 && table.references.data[index] == 0);
    if (table.indexed.data[index])
        table.index.erase(table.hashes.data[index], index);
    table.references.data[index] = kLayoutFreed;
    table.free.push(index);
}

template <typename T>
void tableReset(LayoutTable<T>& table) {
    for (uint32_t i = 0; i < table.blocks.count; ++i)
        prMemoryFree(table.blocks.data[i]);
    table.blocks.reset();
    table.references.reset();
    table.hashes.reset();
    table.indexed.reset();
    table.free.reset();
    table.candidates.reset();
    table.index.reset();
}

// Counts a text font of `set` into or out of the sets that the styles' text fonts belong to. A text font counted in may
// be behind its set, so the next layout brings the text fonts up to date.
void useFontSet(LayoutStyles* c, PrFontSet* set, uint32_t delta) {
    uint32_t at = 0;
    while (at < c->fontSets.count && c->fontSets.data[at].set != set)
        ++at;
    if (delta == 1) {
        if (at == c->fontSets.count)
            c->fontSets.push(LayoutFontSetUse{.set = set, .fonts = 0, .version = 0});
        c->fontSets.data[at].fonts += 1;
        c->fontJoined = true;
        return;
    }
    PR_ASSERT(at < c->fontSets.count);
    c->fontSets.data[at].fonts -= 1;
    if (c->fontSets.data[at].fonts == 0) {
        c->fontSets.data[at] = c->fontSets.last();
        c->fontSets.pop();
    }
}

// Forgets a group's faces and their metrics, which the group's next use finds again.
void releaseFaces(LayoutFontFaces& faces) {
    faces.font = nullptr;
    faces.metrics.clear();
    faces.generation = 0;
}

// Adds `delta` to the references of the group of `kind` at `index`.
void countGroup(LayoutStyles* c, PrLayoutGroup kind, PrLayoutGroupIndex index, uint32_t delta) {
    switch (kind) {
        case PrLayoutGroup_Font:
            tableCount(c->font, index, delta);
            break;
        case PrLayoutGroup_Color:
            tableCount(c->color, index, delta);
            break;
        case PrLayoutGroup_Box:
            tableCount(c->box, index, delta);
            break;
        case PrLayoutGroup_Decoration:
            tableCount(c->decoration, index, delta);
            break;
        case PrLayoutGroup_Effect:
            tableCount(c->effects, index, delta);
            break;
        case PrLayoutGroup_Flex:
            tableCount(c->flex, index, delta);
            break;
        case PrLayoutGroup_Table:
            tableCount(c->table, index, delta);
            break;
        case PrLayoutGroup_Compositing:
            tableCount(c->compositing, index, delta);
            break;
        case PrLayoutGroup_Grid:
            tableCount(c->grid, index, delta);
            break;
        default:
            PR_ASSERT(false);
    }
}

// Adds `delta` to the references of the groups that a record names.
void countGroups(LayoutStyles* c, const PrLayoutStyle& style, uint32_t delta) {
    static_assert(PrLayoutGroup_Count == 9, "count every group of a record");
    tableCount(c->font, style.groups[PrLayoutGroup_Font], delta);
    tableCount(c->color, style.groups[PrLayoutGroup_Color], delta);
    tableCount(c->box, style.groups[PrLayoutGroup_Box], delta);
    tableCount(c->decoration, style.groups[PrLayoutGroup_Decoration], delta);
    tableCount(c->effects, style.groups[PrLayoutGroup_Effect], delta);
    tableCount(c->flex, style.groups[PrLayoutGroup_Flex], delta);
    tableCount(c->table, style.groups[PrLayoutGroup_Table], delta);
    tableCount(c->compositing, style.groups[PrLayoutGroup_Compositing], delta);
    tableCount(c->grid, style.groups[PrLayoutGroup_Grid], delta);
}

// Adds `delta` to the references of the compounds that a group's words name, and checks in debug builds that each word
// is of a kind that its type accepts.
void countCompounds(LayoutStyles* c, const PrLayoutLength& length, uint32_t delta) {
    PR_ASSERT(prLayoutWordKind(length.bits) != PrLayoutWordKind_Number
              && prLayoutWordKind(length.bits) != PrLayoutWordKind_Integer
              && (prLayoutWordKind(length.bits) != PrLayoutWordKind_Keyword || layoutLengthIsAuto(length)
                  || layoutLengthIsNone(length) || layoutLengthIsStretch(length)
                  || layoutLengthIsContentBased(length)));
    layoutCompoundsCount(&c->compounds, length.bits, delta);
}
void countCompounds(LayoutStyles* c, const PrLayoutBoxStyle& box, uint32_t delta) {
    const PrLayoutLength* sizes[] = {&box.width, &box.height, &box.minWidth, &box.minHeight, &box.maxWidth,
                                     &box.maxHeight};
    for (const PrLayoutLength* length : sizes)
        countCompounds(c, *length, delta);
    for (uint32_t side = 0; side < PrRectSide_Count; ++side) {
        countCompounds(c, box.margin[side], delta);
        countCompounds(c, box.padding[side], delta);
        countCompounds(c, box.inset[side], delta);
    }
    PR_ASSERT(prLayoutWordKind(box.baselineShift.bits) != PrLayoutWordKind_Number
              && prLayoutWordKind(box.baselineShift.bits) != PrLayoutWordKind_Integer
              && (prLayoutWordKind(box.baselineShift.bits) != PrLayoutWordKind_Keyword
                  || layoutBaselineShiftIsKeyword(box.baselineShift)));
    PR_ASSERT(box.alignmentBaseline <= PrLayoutAlignmentBaseline_Hanging
              && box.baselineSource <= PrLayoutBaselineSource_Last);
    layoutCompoundsCount(&c->compounds, box.baselineShift.bits, delta);
}
void countCompounds(LayoutStyles* c, const PrLayoutFontStyle& font, uint32_t delta) {
    countCompounds(c, font.textIndent, delta);
    // The group holds its text font and its feature set in the same way as its compounds.
    if (font.font != nullptr) {
        useFontSet(c, prTextFontSet(font.font), delta);
        if (delta == 1)
            prTextFontRetain(font.font);
        else
            prTextFontRelease(font.font);
    }
    if (font.features != nullptr) {
        if (delta == 1)
            prTextFeatureSetRetain(font.features);
        else
            prTextFeatureSetRelease(font.features);
    }
}
void countCompounds(LayoutStyles* c, const PrLayoutDecorationStyle& decoration, uint32_t delta) {
    countCompounds(c, decoration.objectPositionX, delta);
    countCompounds(c, decoration.objectPositionY, delta);
    for (PrLayoutLength length : decoration.radii)
        countCompounds(c, length, delta);
    // The group holds its background layers in the same way as its compounds.
    layoutBackgroundsCount(&c->backgrounds, decoration.backgroundLayers, delta);
}
void countCompounds(LayoutStyles* c, const PrLayoutEffectStyle& effects, uint32_t delta) {
    countCompounds(c, effects.originX, delta);
    countCompounds(c, effects.originY, delta);
    countCompounds(c, effects.perspectiveOriginX, delta);
    countCompounds(c, effects.perspectiveOriginY, delta);
}
void countCompounds(LayoutStyles* c, const PrLayoutCompositingStyle& compositing, uint32_t delta) {
    // The group holds its shapes, masks and filter lists in the same way as its compounds.
    layoutEffectListsCount(&c->effectLists, &c->compounds, compositing, delta);
    if (compositing.blendMode != PrDrawBlendMode_Normal || compositing.composite != PrDrawComposite_SourceOver)
        c->blendingGroups += delta;
}
void countCompounds(LayoutStyles* c, const PrLayoutFlexStyle& flex, uint32_t delta) {
    // A basis may be content, and a gap may be normal; no other length may be either.
    PR_ASSERT(flex.basis.bits == prLayoutLengthContent().bits || layoutLengthIsAuto(flex.basis)
              || layoutLengthIsContentBased(flex.basis) || prLayoutWordKind(flex.basis.bits) < PrLayoutWordKind_Number
              || prLayoutWordKind(flex.basis.bits) == PrLayoutWordKind_Compound);
    layoutCompoundsCount(&c->compounds, flex.basis.bits, delta);
    const PrLayoutLength gaps[] = {flex.rowGap, flex.columnGap};
    for (PrLayoutLength gap : gaps) {
        PR_ASSERT(gap.bits == prLayoutLengthNormal().bits || prLayoutWordKind(gap.bits) < PrLayoutWordKind_Number
                  || prLayoutWordKind(gap.bits) == PrLayoutWordKind_Compound);
        layoutCompoundsCount(&c->compounds, gap.bits, delta);
    }
}

void countCompounds(LayoutStyles* c, const PrLayoutGridStyle& grid, uint32_t delta) {
    // The group holds its track lists and areas in the same way as its compounds.
    layoutGridTemplatesCount(&c->grids, grid, delta);
}

// Returns true if a table group's values are in their ranges.
bool isValid(const PrLayoutTableStyle& table) {
    return table.borderSpacing[0] >= kLayoutUnitZero && table.borderSpacing[1] >= kLayoutUnitZero
           && table.columnSpan >= 1 && table.columnSpan <= kPrLayoutTableMaxColumnSpan
           && table.rowSpan <= kPrLayoutTableMaxRowSpan && table.borderCollapse <= PrLayoutBorderCollapse_Collapse
           && table.captionSide <= PrLayoutCaptionSide_Bottom && table.tableLayout <= PrLayoutTableLayout_Fixed;
}

// Interns a group whose words may name compounds. An entry that it adds holds references to them.
template <typename T>
uint32_t internNaming(LayoutStyles* c, LayoutTable<T>& table, const T& value, bool* added = nullptr) {
    bool made = false;
    uint32_t at = tableIntern(table, value, &made);
    if (made)
        countCompounds(c, value, 1);
    if (added != nullptr)
        *added = made;
    return at;
}

// Creates a group whose words may name compounds, which holds references to them.
template <typename T>
uint32_t createNaming(LayoutStyles* c, LayoutTable<T>& table, const T& value) {
    uint32_t at = tableAdd(table, value, 0, false);
    countCompounds(c, value, 1);
    return at;
}

// Frees the entries of the table that nothing names, calling `freeing` with each first. Only a candidate can be such an
// entry. A candidate listed again is already freed by then, and one that gained references since is kept.
template <typename T, typename Freeing>
void collect(LayoutTable<T>& table, Freeing freeing) {
    for (uint32_t k = 0; k < table.candidates.count; ++k) {
        uint32_t i = table.candidates.data[k];
        if (table.references.data[i] == 0) {
            freeing(i);
            tableFree(table, i);
        }
    }
    table.candidates.clear();
}

// Returns true if every live entry of the table with no references is a candidate.
template <typename T>
bool covered(const LayoutTable<T>& table) {
    return layoutCandidatesCover(table.candidates, table.references.count,
                                 [&](uint32_t i) { return table.references.data[i] == 0; });
}

// Sets the tree's target pixels per px, and the units of one target pixel if they are whole. Writes the box group at
// `index` as layout reads it (LayoutStyles::usedBox), making room for it.
void writeUsedBox(LayoutStyles* styles, uint32_t index) {
    while (styles->usedBox.count * kLayoutTableBlock <= index)
        styles->usedBox.push(
            static_cast<PrLayoutBoxStyle*>(prMemoryAlloc(sizeof(PrLayoutBoxStyle) * kLayoutTableBlock)));
    PrLayoutBoxStyle& used = styles->usedBox.data[index / kLayoutTableBlock][index % kLayoutTableBlock];
    used = layoutTableAt(styles->box, index);
    for (PrLayoutUnit& width : used.border)
        width = layoutBorderWidth(styles, width);
}

void setTargetPixelRatio(LayoutStyles* styles, float ratio) {
    styles->targetPixelRatio = ratio;
    float units = float(kPrLayoutUnitsPerPx) / ratio;
    styles->targetPixelUnits = units == floorF32(units) && units >= 1 ? int32_t(units) : 0;
}

// Returns the metrics of `font` at `size` px, at the tree's target pixel ratio. Positions along y are measured down
// from the alphabetic baseline, as layout measures them.
LayoutFaceMetrics faceMetrics(const LayoutStyles* styles, const PrFont* font, float size) {
    PrFontMetrics metrics = prFontMetrics(font);
    LayoutFaceMetrics face{};
    face.ascent = layoutRoundToTargetPixel(styles, metrics.ascent * size);
    face.descent = layoutRoundToTargetPixel(styles, metrics.descent * size);
    face.lineGap = layoutRoundToTargetPixel(styles, metrics.lineGap * size);
    face.xHeight = prLayoutUnitFromPx(metrics.xHeight * size);
    face.central = prLayoutUnitFromPx(metrics.centralBaseline * size);
    face.ideographic = prLayoutUnitFromPx(metrics.ideographicUnder * size);
    face.hanging = prLayoutUnitFromPx(metrics.hangingBaseline * size);
    face.math = prLayoutUnitFromPx(metrics.mathBaseline * size);
    face.averageAdvance = prLayoutUnitFromPx(metrics.averageAdvance * size);
    face.maxAdvance = prLayoutUnitFromPx(metrics.maxAdvance * size);
    face.vertical = (metrics.fromFont & PrFontMetricFlag_Vertical) != 0;
    if ((metrics.fromFont & PrFontMetricFlag_Ideographic) == 0) {
        // Synthesized from the ascent and descent, as layout rounds them, so that a face's central baseline is the
        // middle of its content area.
        face.central = (face.descent - face.ascent) / 2;
        face.ideographic = face.descent;
    }
    return face;
}

// Finds the metrics of the group's faces that have none yet, at `size` px. A group with no text font has no faces.
void measureNewFaces(const LayoutStyles* styles, LayoutFontFaces& faces, float size) {
    if (faces.font == nullptr)
        return;
    uint32_t count = prTextFontMatchCount(faces.font);
    while (faces.metrics.count < count)
        faces.metrics.push(faceMetrics(styles, prTextFontMatchedFont(faces.font, faces.metrics.count), size));
}

// Finds the group's size and the metrics of its faces and of its first available face, or shares of the size if it has
// none.
void measureFaces(LayoutStyles* styles, LayoutFontFaces& faces) {
    faces.generation = styles->metricsGeneration;
    faces.metrics.clear();
    // A group with no text font sets no text: it has no faces and size 0, and its metrics are at its line's edge.
    if (faces.font == nullptr) {
        faces.size = kLayoutUnitZero;
        faces.firstFace = kLayoutFaceNone;
        faces.first = LayoutFaceMetrics{};
        faces.revision = 0;
        return;
    }
    faces.size = prLayoutUnitFromPx(prTextFontStyle(faces.font).size);
    float size = prLayoutUnitToPx(faces.size);
    uint32_t first = prTextFontFirst(faces.font);
    measureNewFaces(styles, faces, size);
    faces.firstFace = first != kPrTextMatchNone ? LayoutFaceIndex(first) : kLayoutFaceNone;
    if (first != kPrTextMatchNone) {
        faces.first = faces.metrics.data[first];
    } else {
        LayoutFaceMetrics& m = faces.first;
        m.ascent = layoutRoundToTargetPixel(styles, size * 0.8f);
        m.descent = layoutRoundToTargetPixel(styles, size * 0.2f);
        m.lineGap = kLayoutUnitZero;
        m.xHeight = prLayoutUnitFromPx(size * 0.5f);
        m.central = -(m.ascent - m.descent) / 2;
        m.ideographic = m.descent;
        m.hanging = -prLayoutUnitFromPx(size * 0.6f);
        m.math = -m.xHeight / 2;
        m.averageAdvance = prLayoutUnitFromPx(size * 0.5f);
        m.maxAdvance = m.averageAdvance;
    }
    faces.revision = prTextFontRevision(faces.font);
}

// Interns a record that the table may not hold. A record new to the table names its groups, and its traits are found.
// Counts a record that the table added into the groups it names, and finds its traits.
void addedRecord(LayoutStyles* c, const PrLayoutStyle& style, PrLayoutStyleIndex index) {
    countGroups(c, style, 1);
    while (c->traits.count <= index)
        c->traits.push(LayoutStyleTraits{});
    c->traits.data[index] = layoutStyleTraitsFromIndex(c, index);
}

PR_NOINLINE PrLayoutStyleIndex internRecord(LayoutStyles* c, const PrLayoutStyle& style) {
    bool added = false;
    uint32_t index = tableIntern(c->style, style, &added);
    if (added)
        addedRecord(c, style, index);
    return index;
}

} // namespace

PrLayoutUnit layoutRoundToTargetPixel(const LayoutStyles* styles, float px) {
    double ratio = double(styles->targetPixelRatio);
    return prLayoutUnitFromPxFloorDouble(roundF64(double(px) * ratio) / ratio);
}

PrLayoutUnit layoutFloorToFractionalTargetPixel(const LayoutStyles* styles, PrLayoutUnit unit) {
    double ratio = double(styles->targetPixelRatio);
    return prLayoutUnitFromPxFloorDouble(floorF64(prLayoutUnitToPxDouble(unit) * ratio) / ratio);
}

PrLayoutUnit layoutSnapAnyBorderWidth(const LayoutStyles* styles, PrLayoutUnit width) {
    if (width.raw == 0)
        return width;
    int64_t magnitude = width.raw < 0 ? -int64_t(width.raw) : int64_t(width.raw);
    int64_t snapped = 0;
    int32_t step = styles->targetPixelUnits;
    if (step != 0) {
        snapped = magnitude < step ? step : magnitude - magnitude % step;
    } else {
        double ratio = double(styles->targetPixelRatio);
        double pixels = floorF64(double(magnitude + 1) * ratio / double(kPrLayoutUnitsPerPx));
        snapped = prLayoutUnitFromPxFloorDouble(maxF64(pixels, 1) / ratio).raw;
    }
    return prLayoutUnitClamped(width.raw < 0 ? -snapped : snapped);
}

const LayoutFontFaces& layoutFontFacesFind(LayoutStyles* styles, PrLayoutGroupIndex group) {
    while (styles->faces.count <= group)
        styles->faces.push(LayoutFontFaces{});
    LayoutFontFaces& faces = styles->faces.data[group];
    if (faces.generation == styles->metricsGeneration)
        return faces;
    const PrLayoutFontStyle& font = layoutTableAt(styles->font, group);
    if (faces.font == nullptr)
        faces.font = layoutTextFont(styles, font);
    measureFaces(styles, faces);
    return faces;
}

LayoutFaceIndex layoutFaceFor(LayoutStyles* styles, PrLayoutGroupIndex group, PrStringView cluster, PrScript script) {
    layoutFontFacesFind(styles, group);
    LayoutFontFaces& faces = styles->faces.data[group];
    // Text in a group with no text font, in a tree with no default font, is set in no face.
    if (faces.font == nullptr)
        return kLayoutFaceNone;
    uint32_t face = prTextFontMatch(faces.font, cluster, script);
    measureNewFaces(styles, faces, prLayoutUnitToPx(faces.size));
    return face != kPrTextMatchNone ? LayoutFaceIndex(face) : kLayoutFaceNone;
}

void layoutFontFacesMeasureNew(LayoutStyles* styles, PrLayoutGroupIndex group) {
    LayoutFontFaces& faces = styles->faces.data[group];
    measureNewFaces(styles, faces, prLayoutUnitToPx(faces.size));
}

bool layoutSyncFonts(LayoutStyles* styles, Array<uint64_t>* changed) {
    // A text font changes only with its set, and one that joined may be behind its set.
    bool moved = styles->fontJoined;
    for (uint32_t i = 0; i < styles->fontSets.count; ++i) {
        LayoutFontSetUse& use = styles->fontSets.data[i];
        uint64_t version = prFontSetVersion(use.set);
        moved = moved || version != use.version;
        use.version = version;
    }
    changed->clear();
    if (!moved)
        return false;
    styles->fontJoined = false;
    // Groups share text fonts, and later updates of a text font find it up to date. Each group therefore compares the
    // revision at which it found its faces, whichever update raised it.
    if (styles->defaultFont != nullptr)
        prTextFontUpdate(styles->defaultFont);
    for (uint32_t i = 1; i < styles->font.references.count; ++i) {
        if (styles->font.references.data[i] == kLayoutFreed)
            continue;
        PrTextFont* PR_NULLABLE font = layoutTableAt(styles->font, i).font;
        if (font != nullptr)
            prTextFontUpdate(font);
    }
    bool any = false;
    for (uint32_t i = 0; i < styles->faces.count; ++i) {
        LayoutFontFaces& faces = styles->faces.data[i];
        if (faces.font == nullptr || faces.generation == 0 || faces.revision == prTextFontRevision(faces.font))
            continue;
        if (!any) {
            changed->resize(uint32_t(bitsetWords(styles->faces.count)));
            memZero(changed->data, changed->count * sizeof(uint64_t));
        }
        bitsetSet(changed->data, i);
        faces.generation = 0;
        any = true;
    }
    return any;
}

void layoutStyleRetain(LayoutStyles* styles, PrLayoutStyleIndex style) {
    tableCount(styles->style, style, 1);
}

void layoutStyleRelease(LayoutStyles* styles, PrLayoutStyleIndex style) {
    tableCount(styles->style, style, 0xffffffffu);
}

void layoutFontRetain(LayoutStyles* styles, PrLayoutGroupIndex font) {
    tableCount(styles->font, font, 1);
}

void layoutFontRelease(LayoutStyles* styles, PrLayoutGroupIndex font) {
    tableCount(styles->font, font, 0xffffffffu);
}

LayoutStyles* layoutStylesCreate(PrTextFont* PR_NULLABLE font, PrTextShaper* PR_NULLABLE shaper,
                                 float targetPixelRatio) {
    LayoutStyles* c = allocZeroed<LayoutStyles>();
    if (font != nullptr) {
        prTextFontRetain(font);
        useFontSet(c, prTextFontSet(font), 1);
    }
    c->defaultFont = font;
    c->shaper = shaper;
    c->metricsGeneration = 1;
    PR_ASSERT(targetPixelRatio > 0 && isFiniteF32(targetPixelRatio));
    setTargetPixelRatio(c, targetPixelRatio);
    PrLayoutFontStyle fontGroup = prLayoutFontStyleDefault();
    PrLayoutColorStyle color = prLayoutColorStyleDefault();
    PrLayoutBoxStyle box = prLayoutBoxStyleDefault();
    PrLayoutDecorationStyle decoration = prLayoutDecorationStyleDefault();
    PrLayoutEffectStyle effects = prLayoutEffectStyleDefault();
    PrLayoutFlexStyle flex = prLayoutFlexStyleDefault();
    PrLayoutTableStyle table = prLayoutTableStyleDefault();
    PrLayoutCompositingStyle compositing = prLayoutCompositingStyleDefault();
    PrLayoutGridStyle grid = prLayoutGridStyleDefault();
    PrLayoutStyle style = prLayoutStyleDefault();
    tableInit(c->font, fontGroup);
    tableInit(c->color, color);
    tableInit(c->box, box);
    writeUsedBox(c, 0);
    tableInit(c->decoration, decoration);
    tableInit(c->effects, effects);
    tableInit(c->flex, flex);
    tableInit(c->table, table);
    tableInit(c->compositing, compositing);
    tableInit(c->grid, grid);
    tableInit(c->style, style);
    layoutBackgroundsInit(&c->backgrounds);
    layoutEffectListsInit(&c->effectLists);
    layoutGridTemplatesInit(&c->grids);
    c->traits.push(layoutStyleTraitsFromIndex(c, 0));
    return c;
}

void layoutStylesDestroy(LayoutStyles* PR_NULLABLE styles) {
    LayoutStyles* c = styles;
    if (c == nullptr)
        return;
    for (uint32_t i = 0; i < c->faces.count; ++i) {
        releaseFaces(c->faces.data[i]);
        c->faces.data[i].metrics.reset();
    }
    c->faces.reset();
    for (uint32_t i = 1; i < c->font.references.count; ++i) {
        if (c->font.references.data[i] == kLayoutFreed)
            continue;
        prTextFontRelease(layoutTableAt(c->font, i).font);
        prTextFeatureSetRelease(layoutTableAt(c->font, i).features);
    }
    prTextFontRelease(c->defaultFont);
    tableReset(c->font);
    tableReset(c->color);
    tableReset(c->box);
    for (uint32_t i = 0; i < c->usedBox.count; ++i)
        prMemoryFree(c->usedBox.data[i]);
    c->usedBox.reset();
    tableReset(c->decoration);
    tableReset(c->effects);
    tableReset(c->flex);
    tableReset(c->table);
    for (uint32_t i = 1; i < c->compositing.references.count; ++i) {
        const PrLayoutCompositingStyle& compositing = layoutTableAt(c->compositing, i);
        if (c->compositing.references.data[i] != kLayoutFreed && compositing.clipKind == PrLayoutClipKind_Source
            && compositing.clipSource.functions != nullptr && compositing.clipSource.functions->release != nullptr)
            compositing.clipSource.functions->release(compositing.clipSource.context);
    }
    tableReset(c->compositing);
    tableReset(c->grid);
    tableReset(c->style);
    c->traits.reset();
    layoutBackgroundsReset(&c->backgrounds);
    layoutEffectListsReset(&c->effectLists);
    layoutGridTemplatesReset(&c->grids);
    layoutCompoundsReset(&c->compounds);
    c->fontSets.reset();
    prMemoryFree(c);
}

void layoutStylesSetTargetPixelRatio(LayoutStyles* styles, float ratio) {
    PR_ASSERT(ratio > 0 && isFiniteF32(ratio));
    if (ratio == styles->targetPixelRatio)
        return;
    setTargetPixelRatio(styles, ratio);
    ++styles->metricsGeneration;
    for (uint32_t i = 0; i < styles->box.references.count; ++i) {
        if (styles->box.references.data[i] != kLayoutFreed)
            writeUsedBox(styles, i);
    }
}

void layoutStylesCollect(LayoutStyles* styles) {
    LayoutStyles* c = styles;
    collect(c->style, [&](uint32_t i) { countGroups(c, layoutTableAt(c->style, i), 0xffffffffu); });
    collect(c->font, [&](uint32_t i) {
        countCompounds(c, layoutTableAt(c->font, i), 0xffffffffu);
        if (i < c->faces.count)
            releaseFaces(c->faces.data[i]);
    });
    collect(c->color, [](uint32_t) {});
    collect(c->box, [&](uint32_t i) { countCompounds(c, layoutTableAt(c->box, i), 0xffffffffu); });
    collect(c->decoration, [&](uint32_t i) { countCompounds(c, layoutTableAt(c->decoration, i), 0xffffffffu); });
    collect(c->effects, [&](uint32_t i) { countCompounds(c, layoutTableAt(c->effects, i), 0xffffffffu); });
    collect(c->flex, [&](uint32_t i) { countCompounds(c, layoutTableAt(c->flex, i), 0xffffffffu); });
    collect(c->table, [](uint32_t) {});
    collect(c->compositing, [&](uint32_t i) { countCompounds(c, layoutTableAt(c->compositing, i), 0xffffffffu); });
    collect(c->grid, [&](uint32_t i) { countCompounds(c, layoutTableAt(c->grid, i), 0xffffffffu); });
    if (c->backgrounds.candidates.count != 0)
        layoutBackgroundsCollect(&c->backgrounds, &c->compounds);
    layoutEffectListsCollect(&c->effectLists, &c->compounds);
    if (c->grids.tracks.candidates.count != 0 || c->grids.areas.candidates.count != 0)
        layoutGridTemplatesCollect(&c->grids, &c->compounds);
    layoutCompoundsCollect(&c->compounds);
}

bool layoutStylesVerify(const LayoutStyles* styles) {
    const LayoutStyles* c = styles;
    return covered(c->style) && covered(c->font) && covered(c->color) && covered(c->box) && covered(c->decoration)
           && covered(c->effects) && covered(c->flex) && covered(c->table) && covered(c->compositing)
           && covered(c->grid) && layoutBackgroundsVerify(&c->backgrounds) && layoutEffectListsVerify(&c->effectLists)
           && layoutGridTemplatesVerify(&c->grids)
           && layoutCompoundsVerify(&c->compounds);
}

} // namespace pr_impl

using namespace pr_impl;

PrLayoutStyle prLayoutStyleDefault(void) {
    PrLayoutStyle style{};
    return style;
}

uint32_t prLayoutStyleIntern(PrLayoutTree* tree, const PrLayoutStyle* style) {
    LayoutStyles* c = tree->styles;
    // A component library interns its records each frame, so a record that the table holds is found inline.
    uint32_t found = c->style.index.find(tableHash(*style), [&](uint32_t at) {
        return memEquals(&layoutTableAt(c->style, at), style, sizeof *style);
    });
    return found != kHashMissing ? found : internRecord(c, *style);
}

PrLayoutStyleIndex prLayoutStyleCreate(PrLayoutTree* tree, const PrLayoutStyle* style) {
    LayoutStyles* c = tree->styles;
    uint32_t index = tableAdd(c->style, *style, 0, false);
    addedRecord(c, *style, index);
    return index;
}

PrLayoutStyle prLayoutStyleRecord(const PrLayoutTree* tree, uint32_t index) {
    return layoutTableAt(tree->styles->style, index);
}

void prLayoutStyleRetain(PrLayoutTree* tree, uint32_t index) {
    layoutStyleRetain(tree->styles, index);
}

void prLayoutStyleRelease(PrLayoutTree* tree, uint32_t index) {
    layoutStyleRelease(tree->styles, index);
}

void prLayoutGroupRetain(PrLayoutTree* tree, PrLayoutGroup kind, PrLayoutGroupIndex index) {
    countGroup(tree->styles, kind, index, 1);
}

void prLayoutGroupRelease(PrLayoutTree* tree, PrLayoutGroup kind, PrLayoutGroupIndex index) {
    countGroup(tree->styles, kind, index, 0xffffffffu);
}

uint32_t prLayoutGroupReferences(const PrLayoutTree* tree, PrLayoutGroup kind, PrLayoutGroupIndex index) {
    const LayoutStyles* c = tree->styles;
    switch (kind) {
        case PrLayoutGroup_Font:
            return c->font.references.data[index];
        case PrLayoutGroup_Color:
            return c->color.references.data[index];
        case PrLayoutGroup_Box:
            return c->box.references.data[index];
        case PrLayoutGroup_Decoration:
            return c->decoration.references.data[index];
        case PrLayoutGroup_Effect:
            return c->effects.references.data[index];
        case PrLayoutGroup_Flex:
            return c->flex.references.data[index];
        case PrLayoutGroup_Table:
            return c->table.references.data[index];
        case PrLayoutGroup_Compositing:
            return c->compositing.references.data[index];
        case PrLayoutGroup_Grid:
            return c->grid.references.data[index];
        default:
            PR_ASSERT(false);
            return 0;
    }
}

PrLayoutFontStyle prLayoutFontStyleDefault(void) {
    PrLayoutFontStyle font{};
    font.lineHeight = prLayoutLineHeightNormal();
    font.textIndent = prLayoutLengthPx(0);
    font.textAlignLast = PrLayoutTextAlign_Auto;
    return font;
}

uint32_t prLayoutFontStyleIntern(PrLayoutTree* tree, const PrLayoutFontStyle* font) {
    return internNaming(tree->styles, tree->styles->font, *font);
}

uint32_t prLayoutFontStyleCreate(PrLayoutTree* tree, const PrLayoutFontStyle* font) {
    return createNaming(tree->styles, tree->styles->font, *font);
}

PrLayoutFontStyle prLayoutFontStyleRecord(const PrLayoutTree* tree, uint32_t index) {
    return layoutTableAt(tree->styles->font, index);
}

PrLayoutColorStyle prLayoutColorStyleDefault(void) {
    PrLayoutColorStyle color{};
    color.color = PR_COLOR16(0, 0, 0, 1);
    color.visibility = PrLayoutVisibility_Visible;
    return color;
}

uint32_t prLayoutColorStyleIntern(PrLayoutTree* tree, const PrLayoutColorStyle* color) {
    return tableIntern(tree->styles->color, *color);
}

uint32_t prLayoutColorStyleCreate(PrLayoutTree* tree, const PrLayoutColorStyle* color) {
    return tableAdd(tree->styles->color, *color, 0, false);
}

PrLayoutColorStyle prLayoutColorStyleRecord(const PrLayoutTree* tree, uint32_t index) {
    return layoutTableAt(tree->styles->color, index);
}

bool prLayoutDisplayIsInlineLevel(PrLayoutDisplay display) {
    return display == PrLayoutDisplay_Inline || display == PrLayoutDisplay_InlineBlock
           || display == PrLayoutDisplay_InlineFlex || display == PrLayoutDisplay_InlineGrid
           || display == PrLayoutDisplay_InlineTable;
}

bool prLayoutDisplayIsInlineBox(PrLayoutDisplay display) {
    return display == PrLayoutDisplay_Inline;
}

bool prLayoutDisplayIsLayoutInternal(PrLayoutDisplay display) {
    return display >= PrLayoutDisplay_TableRowGroup && display <= PrLayoutDisplay_TableCaption;
}

PrLayoutBoxStyle prLayoutBoxStyleDefault(void) {
    PrLayoutBoxStyle box{};
    box.width = prLayoutLengthAuto();
    box.height = prLayoutLengthAuto();
    box.minWidth = prLayoutLengthAuto();
    box.minHeight = prLayoutLengthAuto();
    box.maxWidth = prLayoutLengthNone();
    box.maxHeight = prLayoutLengthNone();
    box.containIntrinsicWidth = prLayoutLengthNone();
    box.containIntrinsicHeight = prLayoutLengthNone();
    for (uint32_t side = 0; side < PrRectSide_Count; ++side) {
        box.margin[side] = prLayoutLengthPx(0);
        box.padding[side] = prLayoutLengthPx(0);
        box.inset[side] = prLayoutLengthAuto();
    }
    box.display = PrLayoutDisplay_Inline;
    box.boxSizing = PrLayoutBoxSizing_ContentBox;
    box.overflowX = PrLayoutOverflow_Visible;
    box.overflowY = PrLayoutOverflow_Visible;
    box.position = PrLayoutPosition_Static;
    box.baselineShift = prLayoutBaselineShiftPx(0);
    return box;
}

uint32_t prLayoutBoxStyleIntern(PrLayoutTree* tree, const PrLayoutBoxStyle* box) {
    bool added = false;
    uint32_t index = internNaming(tree->styles, tree->styles->box, *box, &added);
    if (added)
        writeUsedBox(tree->styles, index);
    return index;
}

uint32_t prLayoutBoxStyleCreate(PrLayoutTree* tree, const PrLayoutBoxStyle* box) {
    uint32_t index = createNaming(tree->styles, tree->styles->box, *box);
    writeUsedBox(tree->styles, index);
    return index;
}

PrLayoutBoxStyle prLayoutBoxStyleRecord(const PrLayoutTree* tree, uint32_t index) {
    return layoutTableAt(tree->styles->box, index);
}

PrLayoutDecorationStyle prLayoutDecorationStyleDefault(void) {
    PrLayoutDecorationStyle decoration{};
    for (uint32_t side = 0; side < PrRectSide_Count; ++side) {
        decoration.borderColor[side] = PR_COLOR16(0, 0, 0, 1);
        decoration.borderStyle[side] = PrDrawBorderStyle_None;
    }
    decoration.objectPositionX = prLayoutLengthPercent(50);
    decoration.objectPositionY = prLayoutLengthPercent(50);
    for (PrLayoutLength& length : decoration.radii)
        length = prLayoutLengthPx(0);
    for (float& shape : decoration.cornerShapes)
        shape = 1;
    decoration.objectFit = PrLayoutObjectFit_Fill;
    return decoration;
}

uint32_t prLayoutDecorationStyleIntern(PrLayoutTree* tree, const PrLayoutDecorationStyle* decoration) {
    return internNaming(tree->styles, tree->styles->decoration, *decoration);
}

uint32_t prLayoutDecorationStyleCreate(PrLayoutTree* tree, const PrLayoutDecorationStyle* decoration) {
    return createNaming(tree->styles, tree->styles->decoration, *decoration);
}

PrLayoutDecorationStyle prLayoutDecorationStyleRecord(const PrLayoutTree* tree, uint32_t index) {
    return layoutTableAt(tree->styles->decoration, index);
}

PrDrawFilterInput prLayoutFilterAppend(const PrLayoutFilter* filter, PrDrawFilterInput input, PrImageEdgeMode edge,
                                       PrDrawFilterBuilder* builder) {
    return layoutFilterAppend(*filter, input, edge, builder);
}

PrLayoutFilterIndex prLayoutFilterIntern(PrLayoutTree* tree, const PrLayoutFilter* filters, uint32_t count) {
    return layoutEffectListsInternFilter(&tree->styles->effectLists, &tree->styles->compounds, filters, count);
}

PrLayoutNaturalSize prLayoutNaturalSizeNone(void) {
    PrLayoutNaturalSize size{};
    size.width = layoutUnit(-1);
    size.height = layoutUnit(-1);
    size.ratio = 0;
    return size;
}

void prLayoutPainterRetain(PrLayoutPainter painter) {
    if (painter.retain != nullptr)
        painter.retain(painter.context);
}

void prLayoutPainterRelease(PrLayoutPainter painter) {
    if (painter.release != nullptr)
        painter.release(painter.context);
}

PrLayoutGradient prLayoutGradientDefault(void) {
    PrLayoutGradient gradient{};
    gradient.kind = PrLayoutGradientKind_Linear;
    gradient.space = PrColorSpace_Oklab;
    gradient.hueMethod = PrColorHueMethod_Shorter;
    gradient.angle = 180;
    gradient.shape = PrLayoutRadialShape_Ellipse;
    gradient.extent = PrLayoutRadialExtent_FarthestCorner;
    gradient.radiusX = prLayoutLengthAuto();
    gradient.radiusY = prLayoutLengthAuto();
    gradient.centerX = prLayoutLengthPercent(50);
    gradient.centerY = prLayoutLengthPercent(50);
    return gradient;
}

void prLayoutGradientDraw(const PrLayoutGradient* gradient, PrRect box, PrRect rect, PrDrawList* list) {
    layoutBackgroundDrawGradient(nullptr, *gradient, box, rect, PrDrawEdges_Antialiased, list);
}

PrLayoutBackgroundLayer prLayoutBackgroundLayerDefault(void) {
    PrLayoutBackgroundLayer layer{};
    layer.kind = PrLayoutImageKind_None;
    layer.repeatX = PrLayoutBackgroundRepeat_Repeat;
    layer.repeatY = PrLayoutBackgroundRepeat_Repeat;
    layer.attachment = PrLayoutBackgroundAttachment_Scroll;
    layer.origin = PrLayoutBackgroundBox_PaddingBox;
    layer.clip = PrLayoutBackgroundBox_BorderBox;
    layer.size = PrLayoutBackgroundSize_Lengths;
    layer.blendMode = PrDrawBlendMode_Normal;
    layer.composite = PrDrawComposite_SourceOver;
    layer.sizeX = prLayoutLengthAuto();
    layer.sizeY = prLayoutLengthAuto();
    layer.positionX = prLayoutLengthPercent(0);
    layer.positionY = prLayoutLengthPercent(0);
    layer.natural = prLayoutNaturalSizeNone();
    return layer;
}

PrLayoutBackgroundIndex prLayoutBackgroundIntern(PrLayoutTree* tree, const PrLayoutBackgroundLayer* layers,
                                                 uint32_t count) {
    return layoutBackgroundsIntern(&tree->styles->backgrounds, &tree->styles->compounds, layers, count);
}

uint32_t prLayoutBackgroundLayerCount(const PrLayoutTree* tree, PrLayoutBackgroundIndex index) {
    return layoutBackgroundAt(&tree->styles->backgrounds, index).count;
}

PrLayoutBackgroundLayer prLayoutBackgroundLayer(const PrLayoutTree* tree, PrLayoutBackgroundIndex index,
                                                uint32_t layer) {
    const LayoutBackground& b = layoutBackgroundAt(&tree->styles->backgrounds, index);
    PR_ASSERT(layer < b.count);
    return b.layers[layer];
}

PrLayoutTransform prLayoutTransformIdentity(void) {
    return kLayoutTransformIdentity;
}

PrLayoutTransform prLayoutTransformFromAffine2(PrAffine2 transform) {
    return PrLayoutTransform{.matrix = prMatrix4FromAffine2(transform)};
}

bool prLayoutTransformIsIdentity(const PrLayoutTransform* transform) {
    return memEquals(transform, &kLayoutTransformIdentity, sizeof(PrLayoutTransform));
}

PrMatrix4 prLayoutTransformResolve(const PrLayoutTransform* transform, PrVec2 extent) {
    PrMatrix4 m = transform->matrix;
    for (uint32_t row = 0; row < 4; ++row)
        m.m[row][3] += extent.x * transform->perWidth[row] + extent.y * transform->perHeight[row];
    return m;
}

PrLayoutEffectStyle prLayoutEffectStyleDefault(void) {
    PrLayoutEffectStyle effects{};
    effects.transform = prLayoutTransformIdentity();
    effects.originX = prLayoutLengthPercent(50);
    effects.originY = prLayoutLengthPercent(50);
    effects.perspectiveOriginX = prLayoutLengthPercent(50);
    effects.perspectiveOriginY = prLayoutLengthPercent(50);
    effects.opacity = 1;
    return effects;
}

uint32_t prLayoutEffectStyleIntern(PrLayoutTree* tree, const PrLayoutEffectStyle* effects) {
    return internNaming(tree->styles, tree->styles->effects, *effects);
}

uint32_t prLayoutEffectStyleCreate(PrLayoutTree* tree, const PrLayoutEffectStyle* effects) {
    return createNaming(tree->styles, tree->styles->effects, *effects);
}

PrLayoutEffectStyle prLayoutEffectStyleRecord(const PrLayoutTree* tree, uint32_t index) {
    return layoutTableAt(tree->styles->effects, index);
}

PrLayoutShape prLayoutShapeDefault(void) {
    PrLayoutShape shape{};
    shape.kind = PrLayoutShapeKind_Inset;
    shape.extentX = PrLayoutRadialExtent_ClosestSide;
    shape.extentY = PrLayoutRadialExtent_ClosestSide;
    shape.centerX = prLayoutLengthPercent(50);
    shape.centerY = prLayoutLengthPercent(50);
    return shape;
}

void prLayoutShapePath(const PrLayoutShape* shape, const PrLayoutTree* PR_NULLABLE tree, PrRect box, PrPath* out) {
    layoutShapePath(tree != nullptr ? &tree->styles->compounds : nullptr, *shape, box, out);
}

PrLayoutShapeIndex prLayoutShapeIntern(PrLayoutTree* tree, const PrLayoutShape* shape) {
    return layoutEffectListsInternShape(&tree->styles->effectLists, &tree->styles->compounds, *shape);
}

PrLayoutMaskLayer prLayoutMaskLayerDefault(void) {
    PrLayoutMaskLayer layer{};
    layer.image = prLayoutBackgroundLayerDefault();
    layer.image.origin = PrLayoutBackgroundBox_BorderBox;
    layer.mode = PrLayoutMaskMode_MatchSource;
    layer.composite = PrDrawComposite_SourceOver;
    return layer;
}

PrLayoutMaskBorder prLayoutMaskBorderDefault(void) {
    PrLayoutMaskBorder border{};
    border.image = prLayoutBackgroundLayerDefault();
    for (uint32_t side = 0; side < PrRectSide_Count; ++side) {
        border.slice[side] = prLayoutLengthPx(0);
        border.widthKind[side] = PrLayoutBorderImageKind_Auto;
        border.width[side] = prLayoutLengthPx(0);
        border.outsetKind[side] = PrLayoutBorderImageKind_Length;
        border.outset[side] = prLayoutLengthPx(0);
    }
    border.horizontal = PrDrawSliceMode_Stretch;
    border.vertical = PrDrawSliceMode_Stretch;
    border.mode = PrDrawMaskMode_Alpha;
    return border;
}

PrLayoutMaskIndex prLayoutMaskIntern(PrLayoutTree* tree, const PrLayoutMask* mask) {
    return layoutEffectListsInternMask(&tree->styles->effectLists, &tree->styles->compounds, *mask);
}

PrLayoutCompositingStyle prLayoutCompositingStyleDefault(void) {
    PrLayoutCompositingStyle c{};
    for (PrLayoutLength& length : c.clip)
        length = prLayoutLengthAuto();
    c.blendMode = PrDrawBlendMode_Normal;
    c.composite = PrDrawComposite_SourceOver;
    return c;
}

uint32_t prLayoutCompositingStyleIntern(PrLayoutTree* tree, const PrLayoutCompositingStyle* compositing) {
    return internNaming(tree->styles, tree->styles->compositing, *compositing);
}

uint32_t prLayoutCompositingStyleCreate(PrLayoutTree* tree, const PrLayoutCompositingStyle* compositing) {
    return createNaming(tree->styles, tree->styles->compositing, *compositing);
}

PrLayoutCompositingStyle prLayoutCompositingStyleRecord(const PrLayoutTree* tree, uint32_t index) {
    return layoutTableAt(tree->styles->compositing, index);
}

PrLayoutFlexStyle prLayoutFlexStyleDefault(void) {
    PrLayoutFlexStyle flex{};
    flex.shrink = 1;
    flex.basis = prLayoutLengthAuto();
    flex.rowGap = prLayoutLengthNormal();
    flex.columnGap = prLayoutLengthNormal();
    flex.justifyContent = PrLayoutAlignment_Normal;
    flex.alignItems = PrLayoutAlignment_Normal;
    flex.alignContent = PrLayoutAlignment_Normal;
    flex.alignSelf = PrLayoutAlignment_Auto;
    flex.justifySelf = PrLayoutAlignment_Auto;
    flex.justifyItems = PrLayoutAlignment_Normal;
    return flex;
}

uint32_t prLayoutFlexStyleIntern(PrLayoutTree* tree, const PrLayoutFlexStyle* flex) {
    return internNaming(tree->styles, tree->styles->flex, *flex);
}

uint32_t prLayoutFlexStyleCreate(PrLayoutTree* tree, const PrLayoutFlexStyle* flex) {
    return createNaming(tree->styles, tree->styles->flex, *flex);
}

PrLayoutFlexStyle prLayoutFlexStyleRecord(const PrLayoutTree* tree, uint32_t index) {
    return layoutTableAt(tree->styles->flex, index);
}

PrLayoutTableStyle prLayoutTableStyleDefault(void) {
    PrLayoutTableStyle table{};
    table.columnSpan = 1;
    table.rowSpan = 1;
    return table;
}

uint32_t prLayoutTableStyleIntern(PrLayoutTree* tree, const PrLayoutTableStyle* table) {
    PR_ASSERT(isValid(*table));
    return tableIntern(tree->styles->table, *table);
}

uint32_t prLayoutTableStyleCreate(PrLayoutTree* tree, const PrLayoutTableStyle* table) {
    PR_ASSERT(isValid(*table));
    return tableAdd(tree->styles->table, *table, 0, false);
}

PrLayoutTableStyle prLayoutTableStyleRecord(const PrLayoutTree* tree, uint32_t index) {
    return layoutTableAt(tree->styles->table, index);
}

PrLayoutGridName prLayoutGridNameIntern(PrLayoutTree* tree, PrStringView name) {
    return layoutGridTemplatesInternName(&tree->styles->grids, name);
}

PrLayoutGridTracksIndex prLayoutGridTracksIntern(PrLayoutTree* tree, const PrLayoutGridTrackList* list) {
    return layoutGridTemplatesInternTracks(&tree->styles->grids, &tree->styles->compounds, *list);
}

PrLayoutGridAreasIndex prLayoutGridAreasIntern(PrLayoutTree* tree, const PrLayoutGridAreas* areas) {
    return layoutGridTemplatesInternAreas(&tree->styles->grids, *areas);
}

PrLayoutGridStyle prLayoutGridStyleDefault(void) {
    PrLayoutGridStyle grid{};
    return grid;
}

uint32_t prLayoutGridStyleIntern(PrLayoutTree* tree, const PrLayoutGridStyle* grid) {
    return internNaming(tree->styles, tree->styles->grid, *grid);
}

uint32_t prLayoutGridStyleCreate(PrLayoutTree* tree, const PrLayoutGridStyle* grid) {
    return createNaming(tree->styles, tree->styles->grid, *grid);
}

PrLayoutGridStyle prLayoutGridStyleRecord(const PrLayoutTree* tree, uint32_t index) {
    return layoutTableAt(tree->styles->grid, index);
}

PrLayoutLength prLayoutLengthIntern(PrLayoutTree* tree, const PrLayoutCalcNode* nodes, uint32_t count) {
    return layoutCompoundsIntern(&tree->styles->compounds, nodes, count);
}

PrLayoutUnit prLayoutLengthResolve(const PrLayoutTree* tree, PrLayoutLength length, PrLayoutUnit base) {
    return layoutResolve(tree->styles, length, base);
}
