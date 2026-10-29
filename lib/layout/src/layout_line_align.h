// Line alignment: where each box on a line sits in the block axis, and how far the line reaches about its baseline
// (css-inline-3/#transverse-alignment).
//
// CSS2 - https://drafts.csswg.org/css2/
// css-inline-3 - https://drafts.csswg.org/css-inline-3/
//
// The extent and shift of each inline box are measured about its dominant baseline, which is also the baseline that its
// text's glyphs are set on. The extent and shift of an atomic inline are measured about its alphabetic baseline. A box
// attaches to its parent by its alignment baseline, which by default is the parent's dominant baseline
// (css-inline-3/#alignment-baseline-property). Each item of a run aligns in the inline box around it, or in the root
// inline box. The local shift of an item depends on styles and on the layouts of the atomic inlines, so the aligner
// finds it once per run and sums the shifts down the tree. One case depends on the line: an inline box on lines of
// normal height that aligns by its leading box and whose text uses a fallback face. The aligner finds the shifts again
// for each line that holds such a box.

#pragma once

#include "layout/src/layout_line.h"
#include "layout/src/layout_shaping.h"
#include "layout/src/layout_space.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The `parent` and `subtree` of an item that the root inline box holds (CSS2/#aligned-subtree).
constexpr uint32_t kLayoutInlineRoot = UINT32_MAX;

// How an item aligns on a line.
struct LayoutInlineAlign {
    LayoutLineExtent extent;        // its reach about its own baseline: its leading box, or its margin box
    PrLayoutUnit local;             // its baseline below the parent's dominant one; zero for text and subtree roots
    PrLayoutUnit shift;             // its baseline below the subtree root's: the sum of the local shifts
    uint32_t parent;                // the Open item it aligns in, its own Open item for a Close, or kLayoutInlineRoot
    uint32_t subtree;               // the item of the box at the root of its aligned subtree, or kLayoutInlineRoot

    // True for an Open item whose local shift depends on the line. This is the case if its lines have normal height, it
    // aligns by its leading box, and its own text uses a face other than its first.
    bool lineDependent;

    bool fallbackText;              // an Open item: some of its own text uses a face other than its first
};

// How an aligned subtree aligns with the line box (css-inline-3/#baseline-shift-property).
enum {
    LayoutInlineSubtreeKind_Top,
    LayoutInlineSubtreeKind_Center,
    LayoutInlineSubtreeKind_Bottom,
};
typedef uint8_t LayoutInlineSubtreeKind;

// An aligned subtree on the line, rooted at a box whose baseline shift is top, center or bottom.
struct LayoutInlineSubtree {
    LayoutItemIndex item;           // the Open or Atomic item of its root
    LayoutInlineSubtreeKind kind;
    LayoutLineExtent extent;        // about the baseline of its root
    PrLayoutUnit offset;            // its root's baseline below the line's baseline
};

// The alignment of a run's items and of the most recently aligned line.
struct LayoutLineAligner {
    Array<LayoutInlineAlign> align;         // by LayoutItemIndex
    Array<uint32_t> boxes;                  // scratch: the Open items around the item being aligned
    Array<LayoutInlineSubtree> subtrees;    // the line's subtrees, in the order the line meets their roots
    Array<uint32_t> subtreeAt;              // by LayoutItemIndex: the index in `subtrees` of the subtree it roots
    Array<LayoutLineExtent> onLine;         // by LayoutItemIndex: a line-dependent box's leading box on the line
    PrLayoutStyleIndex containerStyle;      // the record of the run's container, which styles the root inline box
    bool lineDependent;                     // true if some Open item is line-dependent
    bool subtreeRoots;                      // true if some item roots an aligned subtree
};

// Frees the memory that an aligner holds.
void layoutLineAlignerReset(LayoutLineAligner& aligner);

// Returns the dominant baseline of a box whose font group is `font` (css-inline-3/#dominant-baseline-property). Auto
// resolves to central in a vertical typographic mode unless text-orientation is sideways, and to alphabetic otherwise.
inline PrLayoutAlignmentBaseline layoutDominantBaseline(const PrLayoutFontStyle& font) {
    static_assert(PrLayoutAlignmentBaseline_Hanging - PrLayoutAlignmentBaseline_Baseline
                      == PrLayoutDominantBaseline_Hanging - PrLayoutDominantBaseline_Auto,
                  "the named baselines are in one order");
    if (font.dominantBaseline != PrLayoutDominantBaseline_Auto)
        return PrLayoutAlignmentBaseline(font.dominantBaseline - PrLayoutDominantBaseline_Auto
                                         + PrLayoutAlignmentBaseline_Baseline);
    bool vertical = layoutIsVerticalTypographic(LayoutMode{font.writingMode, font.direction});
    return vertical && font.textOrientation != PrLayoutTextOrientation_Sideways ? PrLayoutAlignmentBaseline_Central
                                                                                : PrLayoutAlignmentBaseline_Alphabetic;
}

// Returns the `name` baseline of a face, below its alphabetic baseline (css-inline-3/#baseline-types). Text-top and
// text-bottom are the top and bottom of the face's content area, and middle is half its x-height above its alphabetic
// baseline (CSS2). Baseline is the alphabetic baseline.
PrLayoutUnit layoutFaceBaseline(const LayoutFaceMetrics& face, PrLayoutAlignmentBaseline name);

// Returns the dominant baseline of text styled by `style`, from its first face, below its alphabetic baseline.
PrLayoutUnit layoutDominantOffset(LayoutState& state, PrLayoutStyleIndex style);

// Returns how far text styled by `style` reaches about its dominant baseline in its first face: its ascent and descent,
// with the leading of its line height split between them (css-inline-3/#inline-height). The half above rounds down to a
// whole target pixel. Either value may be negative if the line height is less than the content height.
LayoutLineExtent layoutTextExtent(LayoutState& state, PrLayoutStyleIndex style);

// Returns how far the glyphs of `item`, styled by `style` and shaped from bytes [from, to), reach about the item's
// dominant baseline. `extent` is the reach of the item's style. On lines of normal height, the result also holds the
// normal extent of each face that the glyphs use. That extent is measured about the face's baseline with the same name
// as the dominant baseline, and that baseline sits on the item's dominant baseline. Most text uses only its first face,
// which needs no search.
LayoutLineExtent layoutTextOnLineSearch(LayoutState& state, const LayoutInlineShaping& t, const LayoutInlineItem& item,
                                        PrLayoutStyleIndex style, uint32_t from, uint32_t to, LayoutLineExtent extent);
inline LayoutLineExtent layoutTextOnLine(LayoutState& state, const LayoutInlineShaping& t, const LayoutInlineItem& item,
                                         PrLayoutStyleIndex style, uint32_t from, uint32_t to,
                                         LayoutLineExtent extent) {
    if (!item.fallback)
        return extent;
    return layoutTextOnLineSearch(state, t, item, style, from, to, extent);
}

// Finds how each item of the run aligns. `styles` and `atomics` are indexed by item. The root inline box is styled by
// `containerStyle` and reaches `strut`. Atomic inlines align in `mode`.
void layoutAlignItems(LayoutState& state, LayoutLineAligner& aligner, const LayoutInlineShaping& t,
                      const PrLayoutStyleIndex* styles, const LayoutAtomic* atomics, PrLayoutStyleIndex containerStyle,
                      LayoutLineExtent strut, LayoutMode mode);

// Returns true if each item sits at its own shift on every line, because no item roots an aligned subtree and no box is
// line-dependent. The line then reaches as far as its items reach about their shifts, which the caller finds as it
// places them.
inline bool layoutAlignsByShift(const LayoutLineAligner& aligner) {
    return !aligner.subtreeRoots && !aligner.lineDependent;
}

// Aligns `line` and returns how far it reaches about its baseline. The subtree of the root inline box reaches at least
// `strut`. Then each subtree at top, center or bottom grows the line if it is taller, in the order the line meets it:
// below the baseline for top, and above it for center and bottom. CSS2/#line-height does not say on which side.
LayoutLineExtent layoutAlignLine(LayoutState& state, LayoutLineAligner& aligner, const LayoutInlineShaping& t,
                                 const PrLayoutStyleIndex* styles, const LayoutLineContent& line,
                                 LayoutLineExtent strut);

// Returns the baseline of `item` below the line's baseline, on the line that layoutAlignLine aligned last, or on any
// line if layoutAlignsByShift returns true.
inline PrLayoutUnit layoutAlignedBaseline(const LayoutLineAligner& aligner, LayoutItemIndex item) {
    const LayoutInlineAlign& align = aligner.align.data[item];
    if (align.subtree == kLayoutInlineRoot)
        return align.shift;
    uint32_t at = aligner.subtreeAt.data[align.subtree];
    PR_ASSERT(at < aligner.subtrees.count && aligner.subtrees.data[at].item == align.subtree);
    return aligner.subtrees.data[at].offset + align.shift;
}

} // namespace pr_impl

PR_ASSUME_NONNULL_END
