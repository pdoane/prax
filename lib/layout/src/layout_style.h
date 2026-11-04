// Style records: the tables in which a tree interns groups and records, and the data that layout derives from fonts.

#pragma once

#include "pr/assert.h"
#include "pr/hash_index.h"
#include "pr/layout_style.h"
#include "pr/mem.h"
#include "pr/raw_array.h"
#include "pr/string_view.h"
#include "pr/text_font.h"

#include "layout/src/layout_background.h"
#include "layout/src/layout_effects.h"
#include "layout/src/layout_grid_template.h"
#include "layout/src/layout_index.h"
#include "layout/src/layout_unit.h"
#include "layout/src/layout_value.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The number of entries that a table's block holds.
constexpr uint32_t kLayoutTableBlock = 256;

// A table of values of one kind, which interns them or holds them as they were created. Entries live in blocks, so an
// entry never moves while it lives, and an index names its entry until the entry is freed. Index 0 holds the kind's
// default and is never freed. An interned entry is in `index`, by its value's hash, and a created one is not.
template <typename T>
struct LayoutTable {
    RawArray<T*> blocks;
    RawArray<uint32_t> references;     // by index: the records naming a group, or the nodes and callers naming a record
    RawArray<uint32_t> hashes;         // by index, for an interned entry
    RawArray<bool> indexed;            // by index: true for an interned entry
    RawArray<uint32_t> free;           // freed indices, which interning reuses
    HashIndex index;

    // Indices whose references may have fallen to none: each entry created since the last collect, and each entry whose
    // references fell to none since. Every live entry with no references is listed, so collecting reads the list and
    // not the table. An index may be listed more than once, or may have gained references since.
    RawArray<uint32_t> candidates;
};

// Returns the entry at `index` of the table.
template <typename T>
const T& layoutTableAt(const LayoutTable<T>& table, uint32_t index) {
    PR_ASSERT(index < table.references.count);
    return table.blocks.data[index / kLayoutTableBlock][index % kLayoutTableBlock];
}

// A face's metrics at a font group's size: its ascent, descent and line gap, each rounded to the nearest target pixel,
// so that lines and their baselines fall on target pixels, and its baselines.
struct LayoutFaceMetrics {
    PrLayoutUnit ascent;            // rounded to target pixels
    PrLayoutUnit descent;
    PrLayoutUnit lineGap;

    // Unrounded: its x-height, and its central, ideographic-under, hanging and math baselines below its alphabetic
    // baseline (css-inline-3/#baseline-types), as the face gives or synthesizes them (prFontMetrics). A face with no
    // ideographic baselines has them at its rounded ascent and descent, with its central baseline midway between.
    PrLayoutUnit xHeight;
    PrLayoutUnit central;
    PrLayoutUnit ideographic;
    PrLayoutUnit hanging;
    PrLayoutUnit math;

    // Unrounded: the average advance of its characters and the largest advance of its glyphs, which size a box in
    // characters (PrLayoutBoxStyle::intrinsicCharacters).
    PrLayoutUnit averageAdvance;
    PrLayoutUnit maxAdvance;

    bool vertical;                  // true if the face has vertical metrics, which upright glyphs advance by
};

// No face: a cluster that no face of its group sets, or a group with no face that has a font.
constexpr LayoutFaceIndex kLayoutFaceNone = 0xffff;

// The faces a font group sets its text in (pr/text_font.h), and each face's metrics at the group's size, as layout
// rounds them.
struct LayoutFontFaces {
    PrTextFont* PR_NULLABLE font;           // the group's text font, or the tree's default; found on first use
    PrLayoutUnit size;                      // its text font's size, which layout reads when it reads the group's size
    RawArray<LayoutFaceMetrics> metrics;    // by LayoutFaceIndex, one for each face

    // The first available face (prTextFontFirst) and its metrics. The group's strut and the lengths it gives are based
    // on this face, or on shares of the size if the group has none.
    LayoutFaceIndex firstFace;      // or kLayoutFaceNone
    LayoutFaceMetrics first;

    uint32_t generation;                    // the styles' metrics generation of the metrics, or 0 for none
    uint64_t revision;                      // the text font's revision when the faces were found (prTextFontRevision)
};

// A font set that the default font or a font group's text font belongs to, and the version of the set with which the
// styles last brought their text fonts up to date (prFontSetVersion).
struct LayoutFontSetUse {
    PrFontSet* set;
    uint32_t fonts;                 // the default font and the font groups whose text fonts are of the set
    uint64_t version;
};

// What layout derives from a style record that walks read for every node it styles. The traits are found once, when the
// record is interned (layoutStyleTraitsFromIndex), so that a node takes them when its record is set and a change can
// compare them.
struct LayoutStyleTraits {
    uint8_t childKind;              // the LayoutChildKind of an element styled by it that is not the root
    bool vertical;                  // its writing mode is vertical
    bool positioned;                // its position is absolute or fixed and its display not none
    bool mayPaintWhole;             // a box styled by it may paint whole, block-level or an inline box
    bool mayStack;                  // a box styled by it that is not the root may make a stacking context
    uint8_t children;               // the LayoutChildren of an element styled by it
    bool justifies;                 // its justify-self is neither auto nor normal
    bool justifiesItems;            // its justify-items is not normal
    bool composites;                // its opacity is below one, or `layer`, or its compositing group makes a group
    bool transforms;                // its effects group has a transform other than the identity (layoutHasTransform)
    bool fixedBackground;           // a background layer of its decoration is fixed to the viewport

    // A table's row group, row or cell styled by it may paint whole, and then paints the track backgrounds behind its
    // cells itself.
    bool wholeTablePart;

    bool multiColumn;               // true if a box of the record is a multi-column container (css-multicol-1)
};

// The interned style records and groups of a tree, and the faces and metrics that layout caches for their fonts.
struct LayoutStyles {
    LayoutTable<PrLayoutFontStyle> font;
    LayoutTable<PrLayoutColorStyle> color;
    LayoutTable<PrLayoutBoxStyle> box;

    // The box groups as layout reads them, by PrLayoutGroupIndex in blocks as `box` keeps them: each a copy of its
    // group with its border widths snapped as border widths at the target pixel ratio. Snapping once, as a group is
    // added or the ratio changes, keeps it off layout's reads, and `box` keeps the values that interning compares.
    RawArray<PrLayoutBoxStyle*> usedBox;
    LayoutTable<PrLayoutDecorationStyle> decoration;
    LayoutTable<PrLayoutEffectStyle> effects;
    LayoutTable<PrLayoutFlexStyle> flex;
    LayoutTable<PrLayoutTableStyle> table;
    LayoutTable<PrLayoutCompositingStyle> compositing;
    LayoutTable<PrLayoutGridStyle> grid;
    LayoutTable<PrLayoutMathStyle> math;
    LayoutTable<PrLayoutStyle> style;

    // By PrLayoutStyleIndex, set as each record is interned.
    RawArray<LayoutStyleTraits> traits;

    LayoutCompounds compounds;      // the compounds that the groups' words name
    LayoutBackgrounds backgrounds;  // the background layers that the decoration groups name
    LayoutEffectLists effectLists;  // the shapes, masks and filter lists that the compositing groups name
    LayoutGridTemplates grids;      // the track lists, template areas and names that the grid groups name

    // The number of live compositing groups that blend. If there are none, layoutIsolatesBlending returns false at
    // once.
    uint32_t blendingGroups;

    // The number of live box groups whose position is sticky, and of live records whose traits have wholeTablePart. A
    // tree with none of either pays nothing to look for sticky boxes or for the table parts that paint their tracks.
    uint32_t stickyGroups;
    uint32_t wholeTableParts;

    // The number of live box groups whose display is a page-margin box. A tree with none pays nothing to tell a change
    // in a margin box from a change in its parent's content.
    uint32_t pageMarginGroups;

    // The number of live table groups whose break-before or break-after forces a break, or that name a page, whose
    // change between siblings forces one. If there are none, layoutHasForcedBreakInside returns false at once.
    uint32_t forcedBreakGroups;

    // The number of live records of a math box or of a box that lays out its children by math layout. While there are
    // none, block layout runs a copy without math layout's checks.
    uint32_t mathRecords;

    RawArray<LayoutFontFaces> faces; // by PrLayoutGroupIndex, made on first use

    // By PrLayoutGroupIndex, the feature set that the text of a font group with math state is shaped with
    // (layoutShapingFeatures), or null. Each set is made on first use, and the styles hold a reference to it until its
    // group is freed.
    RawArray<const PrTextFeatureSet*> mathFeatures;
    float targetPixelRatio;          // target pixels per px, which font metrics and leading snap to
    int32_t targetPixelUnits;        // the units in a target pixel if whole, and 0 otherwise

    // What text is shaped with. Only text needs it (PrLayoutTreeDesc).
    PrTextShaper* PR_NULLABLE shaper;

    // The text font of a font group that names none, or null. The styles hold a reference to it.
    PrTextFont* PR_NULLABLE defaultFont;

    // Raised from 1 whenever the basis on which every group's font metrics are found changes. A tree laid out at
    // another generation keeps nothing.
    uint32_t metricsGeneration;

    // The sets that the text fonts belong to, and whether a text font joined since the last layoutSyncFonts. A text
    // font changes only with its set, and one that joined may be behind its set, so bringing the text fonts up to date
    // reads them only if a set's version moved or a text font joined. If nothing changed, it costs one comparison per
    // set.
    RawArray<LayoutFontSetUse> fontSets;
    bool fontJoined;
};

// Returns `px` rounded to the nearest target pixel, in units.
PrLayoutUnit layoutRoundToTargetPixel(const LayoutStyles* styles, float px);

// Returns `unit` rounded down to a whole target pixel, if a target pixel is not a whole number of units.
PrLayoutUnit layoutFloorToFractionalTargetPixel(const LayoutStyles* styles, PrLayoutUnit unit);

// Returns `unit` rounded down to a whole target pixel.
inline PrLayoutUnit layoutFloorToTargetPixel(const LayoutStyles* styles, PrLayoutUnit unit) {
    int32_t step = styles->targetPixelUnits;
    if (step == 0)
        return layoutFloorToFractionalTargetPixel(styles, unit);
    int32_t below = unit.raw % step;
    return PrLayoutUnit{unit.raw - (below < 0 ? below + step : below)};
}

// Returns `width` snapped as a border width at the target pixel ratio (css-values-4/#snap-as-a-line-width): a magnitude
// above zero and under one target pixel is one, and a greater one is rounded down to whole target pixels, the sign
// kept. If a target pixel is not a whole number of units, a magnitude within a unit below a whole target pixel reaches
// it, since a whole target pixel truncates to units. Most widths are zero or positive at a ratio whose target pixel is
// a whole number of units, which is inline; layoutSnapAnyBorderWidth handles every width.
PrLayoutUnit layoutSnapAnyBorderWidth(const LayoutStyles* styles, PrLayoutUnit width);
inline PrLayoutUnit layoutSnapBorderWidth(const LayoutStyles* styles, PrLayoutUnit width) {
    // Most widths are zero, or positive at a ratio whose target pixel is a whole number of units.
    int32_t step = styles->targetPixelUnits;
    if (width.raw == 0)
        return width;
    if (step == 0 || width.raw < 0)
        return layoutSnapAnyBorderWidth(styles, width);
    return PrLayoutUnit{width.raw < step ? step : width.raw - width.raw % step};
}

// Returns a border's or an outline's `width`, snapped as a border width. A negative width is zero.
inline PrLayoutUnit layoutBorderWidth(const LayoutStyles* styles, PrLayoutUnit width) {
    return layoutSnapBorderWidth(styles, layoutUnitMax(width, kLayoutUnitZero));
}

// Returns the units that a fixed length resolves to against `base` (layoutLengthResolve).
inline PrLayoutUnit layoutResolve(const LayoutStyles* styles, PrLayoutLength length, PrLayoutUnit base) {
    return layoutLengthResolve(&styles->compounds, length, base);
}

// Returns the gap `gap` resolved against `base`, the content box's size in its axis. A percentage of an indefinite base
// resolves against zero, and normal is zero in a flex or grid container (css-gaps-1/#gap-percent,
// css-gaps-1/#column-row-gap).
inline PrLayoutUnit layoutGapResolve(const LayoutStyles* styles, PrLayoutLength gap, PrLayoutUnit base) {
    if (!layoutLengthIsFixed(gap))
        return kLayoutUnitZero;
    return layoutUnitMax(kLayoutUnitZero, layoutResolve(styles, gap, layoutUnitMax(base, kLayoutUnitZero)));
}

// Returns the text font of the font group `font`: its own, or the tree's default, or null if the tree has none.
inline PrTextFont* PR_NULLABLE layoutTextFont(const LayoutStyles* styles, const PrLayoutFontStyle& font) {
    return font.font != nullptr ? font.font : styles->defaultFont;
}

// Returns the record `style`.
inline const PrLayoutStyle& layoutStyle(const LayoutStyles* styles, PrLayoutStyleIndex style) {
    return layoutTableAt(styles->style, style);
}

// Returns the traits of the record `style`, or each of its groups.
inline const LayoutStyleTraits& layoutStyleTraits(const LayoutStyles* styles, PrLayoutStyleIndex style) {
    PR_ASSERT(style < styles->traits.count);
    return styles->traits.data[style];
}
inline const PrLayoutFontStyle& layoutFontStyle(const LayoutStyles* styles, PrLayoutStyleIndex style) {
    return layoutTableAt(styles->font, layoutStyle(styles, style).groups[PrLayoutGroup_Font]);
}
inline const PrLayoutColorStyle& layoutColorStyle(const LayoutStyles* styles, PrLayoutStyleIndex style) {
    return layoutTableAt(styles->color, layoutStyle(styles, style).groups[PrLayoutGroup_Color]);
}
inline const PrLayoutBoxStyle& layoutBoxStyle(const LayoutStyles* styles, PrLayoutStyleIndex style) {
    uint32_t index = layoutStyle(styles, style).groups[PrLayoutGroup_Box];
    return styles->usedBox.data[index / kLayoutTableBlock][index % kLayoutTableBlock];
}
inline const PrLayoutDecorationStyle& layoutDecorationStyle(const LayoutStyles* styles, PrLayoutStyleIndex style) {
    return layoutTableAt(styles->decoration, layoutStyle(styles, style).groups[PrLayoutGroup_Decoration]);
}
inline const PrLayoutEffectStyle& layoutEffectStyle(const LayoutStyles* styles, PrLayoutStyleIndex style) {
    return layoutTableAt(styles->effects, layoutStyle(styles, style).groups[PrLayoutGroup_Effect]);
}
inline const PrLayoutFlexStyle& layoutFlexStyle(const LayoutStyles* styles, PrLayoutStyleIndex style) {
    return layoutTableAt(styles->flex, layoutStyle(styles, style).groups[PrLayoutGroup_Flex]);
}
inline const PrLayoutTableStyle& layoutTableStyle(const LayoutStyles* styles, PrLayoutStyleIndex style) {
    return layoutTableAt(styles->table, layoutStyle(styles, style).groups[PrLayoutGroup_Table]);
}
inline const PrLayoutCompositingStyle& layoutCompositingStyle(const LayoutStyles* styles, PrLayoutStyleIndex style) {
    return layoutTableAt(styles->compositing, layoutStyle(styles, style).groups[PrLayoutGroup_Compositing]);
}
inline const PrLayoutGridStyle& layoutGridStyle(const LayoutStyles* styles, PrLayoutStyleIndex style) {
    return layoutTableAt(styles->grid, layoutStyle(styles, style).groups[PrLayoutGroup_Grid]);
}
inline const PrLayoutMathStyle& layoutMathStyle(const LayoutStyles* styles, PrLayoutStyleIndex style) {
    return layoutTableAt(styles->math, layoutStyle(styles, style).groups[PrLayoutGroup_Math]);
}
inline const PrLayoutFragmentationStyle& layoutFragmentationStyle(const LayoutStyles* styles, PrLayoutStyleIndex style) {
    return layoutTableStyle(styles, style).fragmentation;
}

// Returns true if a box of the record `style` is a multi-column container (LayoutStyleTraits::multiColumn). A record of
// the default table group, as most are, has no columns, so the check reads its traits only for the others.
inline bool layoutHasColumns(const LayoutStyles* styles, PrLayoutStyleIndex style) {
    return layoutStyle(styles, style).groups[PrLayoutGroup_Table] != 0 && layoutStyleTraits(styles, style).multiColumn;
}

// The identity box transform.
constexpr PrLayoutTransform kLayoutTransformIdentity{
    .matrix = {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}},
};

// Returns true if the effects group has a transform other than the identity.
inline bool layoutHasTransform(const PrLayoutEffectStyle& effects) {
    return !prMemEquals(&effects.transform, &kLayoutTransformIdentity, sizeof(PrLayoutTransform));
}

// Matches the faces of the font group at `group` if it has none, and finds their metrics if they are not of the styles'
// generation.
const LayoutFontFaces& layoutFontFacesFind(LayoutStyles* styles, PrLayoutGroupIndex group);

// Returns the index, among the faces of the font group `group`, of the face that sets the grapheme cluster `cluster`,
// whose script itemization resolved as `script` (prTextFontMatch). Finds the metrics of a face that joins the text
// font. Returns kLayoutFaceNone if the group has no face.
LayoutFaceIndex layoutFaceFor(LayoutStyles* styles, PrLayoutGroupIndex group, PrStringView cluster, PrScript script);

// Finds the metrics of the faces that joined the text font of the group `group` since they were last found, as setting
// clusters in it adds them (prTextFontMatch).
void layoutFontFacesMeasureNew(LayoutStyles* styles, PrLayoutGroupIndex group);

// Returns the font of face `face` of a group's faces.
inline const PrFont* layoutFaceFont(const LayoutFontFaces& faces, uint32_t face) {
    PR_ASSERT(faces.font != nullptr);
    return prTextFontMatchedFont(faces.font, face);
}

// Brings the groups' text fonts up to date with their sets (prTextFontUpdate). Sets in `changed`, a bit set indexed by
// PrLayoutGroupIndex that it sizes to the groups that have faces, the groups whose text fonts' answers changed. Their
// face indices are no longer valid, and their faces are found again at their next use. Returns true if any changed.
bool layoutSyncFonts(LayoutStyles* styles, RawArray<uint64_t>* changed);

// Returns the faces and line extent of the font group at `group`, or of the group of the record `style`, matched on
// first use.
inline const LayoutFontFaces& layoutFontFacesOfGroup(LayoutStyles* styles, PrLayoutGroupIndex group) {
    if (group < styles->faces.count && styles->faces.data[group].generation == styles->metricsGeneration)
        return styles->faces.data[group];
    return layoutFontFacesFind(styles, group);
}
inline const LayoutFontFaces& layoutFontFaces(LayoutStyles* styles, PrLayoutStyleIndex style) {
    return layoutFontFacesOfGroup(styles, layoutStyle(styles, style).groups[PrLayoutGroup_Font]);
}

// Returns the feature set that the text of the font group `group` is shaped with. It holds the script forms of the
// group's math depth (ssty) and the dotless forms (dtls) if the group asks for them (pr/font_math.h), followed by the
// group's own features, which win.
const PrTextFeatureSet* PR_NULLABLE layoutShapingFeatures(LayoutStyles* styles, PrLayoutGroupIndex group);

// Retains or releases a node's reference to the record `style`.
void layoutStyleRetain(LayoutStyles* styles, PrLayoutStyleIndex style);
void layoutStyleRelease(LayoutStyles* styles, PrLayoutStyleIndex style);

// Retains or releases a reference to the font group `font` from layout state other than a record, such as a shaping
// that holds its faces' indices. The reference keeps the group, and its faces, alive through layoutStylesCollect.
void layoutFontRetain(LayoutStyles* styles, PrLayoutGroupIndex font);
void layoutFontRelease(LayoutStyles* styles, PrLayoutGroupIndex font);

// Creates styles whose tables hold only their defaults, or frees them. The styles hold a reference to `font` and shape
// with `shaper` (PrLayoutTreeDesc).
LayoutStyles* layoutStylesCreate(PrTextFont* PR_NULLABLE font, PrTextShaper* PR_NULLABLE shaper,
    float targetPixelRatio);
void layoutStylesDestroy(LayoutStyles* PR_NULLABLE styles);

// Sets the target pixels per px that font metrics and leading snap to. A new ratio raises the metrics generation.
void layoutStylesSetTargetPixelRatio(LayoutStyles* styles, float ratio);

// Frees the records, groups and compounds that no node names and no reference holds. What was interned since the last
// collect lives until this one. Their indices may be reused. Reads only the entries listed as candidates.
void layoutStylesCollect(LayoutStyles* styles);

// Returns true if every live entry with no references is listed for the next collect (LayoutTable::candidates).
bool layoutStylesVerify(const LayoutStyles* styles);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
