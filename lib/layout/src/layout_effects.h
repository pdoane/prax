// Effects: the shapes, masks and filter lists that a tree interns for its compositing groups, and how a box's clip
// path, mask, filters and blending apply around its content.
//
// css-masking-1 - https://drafts.fxtf.org/css-masking-1/
// css-shapes-1 - https://drafts.csswg.org/css-shapes-1/
// filter-effects-1 - https://drafts.fxtf.org/filter-effects-1/
// filter-effects-2 - https://drafts.fxtf.org/filter-effects-2/
// compositing-1 - https://drafts.fxtf.org/compositing-1/
//
// A box's effects are resolved once its box is laid out, in the space of its border box from its top left, into what
// lib/draw applies around a group: a clip, a mask, filters and a blend. What they read of the box is its boxes and its
// overflow, so a scene resolves them where it places the box's layer.

#pragma once

#include "pr/array.h"
#include "pr/draw.h"
#include "pr/draw_filter.h"
#include "pr/hash_index.h"
#include "pr/layout_style.h"
#include "pr/path.h"

#include "layout/src/layout_background.h"
#include "layout/src/layout_fragment.h"
#include "layout/src/layout_index.h"
#include "layout/src/layout_value.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

struct LayoutFragment;
struct LayoutStyles;
struct LayoutTree;

// ---------------------------------------------------------------------------------------------------------------------
// Interning

// The kinds of entry that the effect lists intern.
enum {
    LayoutEffectKind_Shape,
    LayoutEffectKind_Mask,
    LayoutEffectKind_Filter,
};
typedef uint8_t LayoutEffectKind;

// An interned shape, mask or filter list, with the storage it owns: a polygon's points and a path for a shape, the
// layers and their images' gradient stops and filter items for a mask, and the items for a filter list. `filterCount`
// counts a filter list's items.
struct LayoutEffectEntry {
    bool live;                      // false while the entry is free, and for the none entry at index 0
    PrLayoutShape shape;
    PrLayoutMask mask;
    PrLayoutFilter* PR_NULLABLE filters;
    uint32_t filterCount;
    PrLayoutColorStop* PR_NULLABLE stops;
    Array<uint8_t> key;             // the bytes that equal values share
    uint32_t hash;
    uint32_t references;            // number of compositing groups that name it
};

// One kind's interned entries. Index 0 is none, which is never freed. An index names its entry until the entry is
// freed.
struct LayoutEffectTable {
    Array<LayoutEffectEntry> entries;
    Array<uint32_t> free;           // freed indices, which interning reuses
    HashIndex index;
    Array<uint32_t> candidates;     // indices whose references may have fallen to none (LayoutTable::candidates)
};

// The shapes, masks and filter lists of a tree's styles, by LayoutEffectKind.
struct LayoutEffectLists {
    LayoutEffectTable tables[3];
};

// Prepares the tables with none at index 0 of each.
void layoutEffectListsInit(LayoutEffectLists* lists);

// Interns a shape, a mask or a filter list, as prLayoutShapeIntern, prLayoutMaskIntern and prLayoutFilterIntern define.
// An entry that they add holds references to the compounds that its lengths name, and to its images, lists, painters
// and sources.
PrLayoutShapeIndex layoutEffectListsInternShape(LayoutEffectLists* lists, LayoutCompounds* compounds,
                                                const PrLayoutShape& shape);
PrLayoutMaskIndex layoutEffectListsInternMask(LayoutEffectLists* lists, LayoutCompounds* compounds,
                                              const PrLayoutMask& mask);
PrLayoutFilterIndex layoutEffectListsInternFilter(LayoutEffectLists* lists, LayoutCompounds* compounds,
                                                  const PrLayoutFilter* filters, uint32_t count);

// Returns the entry of `kind` at `index`.
inline const LayoutEffectEntry& layoutEffectAt(const LayoutEffectLists* lists, LayoutEffectKind kind, uint32_t index) {
    PR_ASSERT(index < lists->tables[kind].entries.count);
    return lists->tables[kind].entries.data[index];
}

// Adds `delta` to the references of the entries that a compositing group names, and checks in debug builds that each of
// its lengths is of a kind that its type accepts. A group that names a source holds a reference to its context.
void layoutEffectListsCount(LayoutEffectLists* lists, LayoutCompounds* compounds,
                            const PrLayoutCompositingStyle& compositing, uint32_t delta);

// Frees the entries that no group names, of those listed as candidates, and releases what they hold.
void layoutEffectListsCollect(LayoutEffectLists* lists, LayoutCompounds* compounds);

// Returns true if every live entry that no group names is listed as a candidate.
bool layoutEffectListsVerify(const LayoutEffectLists* lists);

// Frees every entry and the tables' memory.
void layoutEffectListsReset(LayoutEffectLists* lists);

// ---------------------------------------------------------------------------------------------------------------------
// Questions

// Returns true if a compositing group makes a group of its box, and so a stacking context of its own: a clip path, a
// mask, a filter, a backdrop filter, a blend mode, isolation, or a backdrop root that will-change asks for. A clip
// rectangle makes none: it clips as a container (PrLayoutBoxStyle::clipped).
bool layoutCompositingGroups(const PrLayoutCompositingStyle& compositing);

// Returns true if the record `style` blends its box with what lies beneath it: a blend mode other than normal, or
// plus-lighter (compositing-1/#mix-blend-mode).
bool layoutStyleBlends(const LayoutStyles* styles, PrLayoutStyleIndex style);

// Returns true if the box of `node` is a stacking context that its style gives no layer, and its list holds a box that
// blends. Every stacking context is an isolated group (compositing-1/#csscompositingrules_CSS), so such a box takes a
// layer to isolate the box that blends. A stacking context in which nothing blends takes no layer for this.
bool layoutIsolatesBlending(const LayoutTree* tree, LayoutNodeIndex node);

// Returns true if a decoration group gives some corner of the border box a radius (PrLayoutDecorationStyle::radii).
bool layoutHasCorners(const PrLayoutDecorationStyle& decoration);

// Returns the shape of the border box `border`, with the corners of `decoration` fitted to it
// (css-backgrounds-3/#corner-overlap). A corner at a side in `broken`, a mask of PrRectSide bits at which a fragment
// breaks, is square before the others are fitted, since slicing cuts the box there (css-break-4/#break-decoration).
PrCornerRect layoutBorderShape(const LayoutStyles* styles, const PrLayoutDecorationStyle& decoration, PrRect border,
                               uint8_t broken = 0);

// Returns true if the container `fragment` clips its content to curves, and writes its clip's corners, by PrRectCorner,
// to `out`. `clip` is the clip that the container's walk gives it. The clip is curved at the corners of the padding box
// (css-backgrounds-3/#corner-clipping), so only where it is the padding box: in both axes, and not cut by a clip
// rectangle.
bool layoutClipCorners(const LayoutTree* tree, const LayoutFragment* fragment, PrLayoutRect clip,
                       PrCorner out[PR_NONNULL PrRectCorner_Count]);

// Returns true if the point `at`, in the same space as `border`, is inside the border box `border` of the box styled by
// `style`, within the curves of its corners.
bool layoutBorderContains(const LayoutStyles* styles, PrLayoutStyleIndex style, PrLayoutRect border, PrLayoutVec at);

// Returns the clip rectangle of the box of `node` whose border box is `border`, in the same space, with its auto edges
// at the border box's edges (css-masking-1/#clip-property). The box must be clipped (PrLayoutBoxStyle::clipped).
PrLayoutRect layoutClipRect(const LayoutTree* tree, LayoutNodeIndex node, PrLayoutRect border);

// Returns true if a compositing group makes its box a backdrop root (filter-effects-2/#BackdropRoot): a filter, a mask,
// a clip path, a backdrop filter or a blend mode. Isolation and a clip rectangle make none.
bool layoutCompositingIsBackdropRoot(const PrLayoutCompositingStyle& compositing);

// Appends the nodes of a filter function to `builder` (prLayoutFilterAppend).
PrDrawFilterInput layoutFilterAppend(const PrLayoutFilter& filter, PrDrawFilterInput input, PrImageEdgeMode edge,
                                     PrDrawFilterBuilder* builder);

// Appends the outline of `shape` in the reference box `box` to `out` (prLayoutShapePath), with its compounds read from
// `compounds`, if it names any.
void layoutShapePath(const LayoutCompounds* PR_NULLABLE compounds, const PrLayoutShape& shape, PrRect box,
                     PrPath* out);

// ---------------------------------------------------------------------------------------------------------------------
// Resolving a box's effects

// A box's effects, resolved in the space of its border box from its top left, as lib/draw applies them. The clip is
// pushed around a group of the content, which the filter, the backdrop, the mask, the opacity and the blend apply to.
// The resolved effects hold references to their lists, paths and filters.
struct LayoutResolvedEffects {
    bool hidden;                        // the box draws nothing, as an empty clip path asks

    // The clip rectangle of a box with a layer, used if `clipsRect` is set. It clips the filter's result
    // (css-masking-1/#placement). The box's clip as a container applies inside the group, and does not clip that
    // result.
    bool clipsRect;
    PrRect clipRect;

    // The clip path: its outline, or the coverage that a source drew over `coverageArea`, or neither.
    PrPath* PR_NULLABLE clipPath;
    PrPathFillRule clipRule;
    PrDrawList* PR_NULLABLE coverage;
    PrRect coverageArea;

    // The mask, whose values in `maskArea` mask by `maskMode`, and which is zero outside it. Null for none.
    PrDrawList* PR_NULLABLE mask;
    PrRect maskArea;
    PrDrawMaskMode maskMode;
    PrColorSpace maskSpace;

    const PrDrawFilter* PR_NULLABLE filter;
    PrRect filterRegion;            // the region of a filter element that ends the list, or empty for no limit

    // The backdrop filter, and the corner rectangle of the border box that it shows within.
    const PrDrawFilter* PR_NULLABLE backdrop;
    PrCornerRect backdropShape;

    PrDrawBlendMode blendMode;
    PrDrawComposite composite;
    bool isolated;

    // The box is a backdrop root (layoutCompositingIsBackdropRoot). Its content then needs a group, which a clip path
    // alone does not push.
    bool backdropRoot;
};

// Resolves the effects of the box of `node`, whose fragment is `fragment`. For an inline box, `fragment` is its block
// container's, and the box opens on the line `opening`. The inline box's border box is the bounds of its parts there.
// The box's ink overflow, `ink`, bounds what an unclipped mask layer covers. Returns false, leaving `out` empty, if its
// compositing group is the default.
bool layoutEffectsResolve(const LayoutTree* tree, LayoutNodeIndex node, const LayoutFragment* fragment,
                          LayoutPieceAt opening, PrRect ink, LayoutResolvedEffects* out);

// Releases what the resolved effects hold, and leaves them empty.
void layoutEffectsReset(LayoutResolvedEffects* effects);

// What layoutEffectsPush pushed, which layoutEffectsPop pops: clips, and the groups inside them.
struct LayoutEffectsPushed {
    uint8_t clips;
    uint8_t groups;
};

// Pushes the effects onto `list`, with `opacity` on the group of the content, which is isolated
// (compositing-1/#isolation). A box without an effect that needs a group gets no group unless `isolate` is set, as it
// is for a box in which something blends. The effects of a box that draws nothing must not be pushed. The caller skips
// such a box.
LayoutEffectsPushed layoutEffectsPush(const LayoutResolvedEffects& effects, float opacity, bool isolate,
                                      PrDrawList* list);
void layoutEffectsPop(LayoutEffectsPushed pushed, PrDrawList* list);

// Returns true if the point `at`, in the border box space of the box of `node` whose fragment is `fragment`, is inside
// its clip path and its clip rectangle, which hit testing respects (css-masking-1/#clipping-paths). A mask does not
// limit where a box is hit.
bool layoutEffectsContain(const LayoutTree* tree, LayoutNodeIndex node, const LayoutFragment* fragment, PrVec2 at);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
