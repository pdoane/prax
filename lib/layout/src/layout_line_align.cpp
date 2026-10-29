// Line alignment: where each box on a line sits in the block axis, and how far the line reaches about its baseline
// (css-inline-3/#transverse-alignment).

#include "layout/src/layout_line_align.h"

#include "layout/src/layout_sizing.h"

namespace pr_impl {

namespace {

// Returns the line height of text styled by `style`. A normal line height is the normal line height of the text's first
// face.
PrLayoutUnit lineHeightOf(LayoutState& state, PrLayoutStyleIndex style) {
    const LayoutFontFaces& faces = layoutFontFaces(state.styles, style);
    const PrLayoutFontStyle& font = layoutFontStyle(state.styles, style);
    return layoutLineHeight(font.lineHeight, faces.size,
                            faces.first.ascent + faces.first.descent + faces.first.lineGap);
}

// Returns how far a face's text reaches about the face's `name` baseline on lines of normal height.
LayoutLineExtent normalExtent(const LayoutState& state, const LayoutFaceMetrics& face,
                              PrLayoutAlignmentBaseline name) {
    PrLayoutUnit above = face.ascent + layoutFloorToTargetPixel(state.styles, face.lineGap / 2);
    PrLayoutUnit at = layoutFaceBaseline(face, name);
    return LayoutLineExtent{above + at, face.ascent + face.descent + face.lineGap - above - at};
}

// Returns how far an atomic inline reaches about its baseline: its margin box (CSS2/#line-height).
LayoutLineExtent atomicExtent(const LayoutAtomic& atomic, LayoutMode mode) {
    PrLayoutUnit blockSize = layoutLogicalSize(mode, atomic.fragment->extent).blockSize;
    return LayoutLineExtent{atomic.margin[LayoutSide_BlockStart] + atomic.baseline.offset,
                            blockSize + atomic.margin[LayoutSide_BlockEnd] - atomic.baseline.offset};
}

// Returns the `name` baseline of a parent styled by `parent`, below the parent's dominant baseline, from the parent's
// first face (layoutFaceBaseline).
PrLayoutUnit parentBaseline(LayoutState& state, uint32_t parent, PrLayoutAlignmentBaseline name) {
    const LayoutFaceMetrics& face = layoutFontFaces(state.styles, parent).first;
    return layoutFaceBaseline(face, name)
           - layoutFaceBaseline(face, layoutDominantBaseline(layoutFontStyle(state.styles, parent)));
}

// Returns the baseline that a box styled by `box` aligns by in a parent whose dominant baseline is `dominant`
// (css-inline-3/#alignment-baseline-property): its alignment-baseline, or the parent's dominant baseline for baseline.
PrLayoutAlignmentBaseline alignmentName(const PrLayoutBoxStyle& box, PrLayoutAlignmentBaseline dominant) {
    return box.alignmentBaseline == PrLayoutAlignmentBaseline_Baseline ? dominant : box.alignmentBaseline;
}

// Returns the `name` baseline of a box that reaches `extent` about a baseline, measured below that baseline and found
// from the box's edges. Text-top and text-bottom are the top and bottom of its leading box or margin box, and middle is
// their midpoint (CSS2). Any other name returns `fallback`.
PrLayoutUnit edgeBaseline(PrLayoutAlignmentBaseline name, LayoutLineExtent extent, PrLayoutUnit fallback) {
    switch (name) {
        case PrLayoutAlignmentBaseline_TextTop: return -extent.above;
        case PrLayoutAlignmentBaseline_TextBottom: return extent.below;
        case PrLayoutAlignmentBaseline_Middle: return (extent.below - extent.above) / 2;
        default: return fallback;
    }
}

// Returns the `name` baseline of an atomic inline, below the alphabetic baseline that it aligns by, from the baseline
// set of that alphabetic baseline (css-inline-3/#baseline-synthesis-box). A set from a font is clamped to the range
// that a scroll container clamped the alphabetic baseline to. A set at a margin edge places every baseline at that
// edge. Otherwise the set is synthesized from the margin box.
PrLayoutUnit atomicBaseline(LayoutState& state, const LayoutAtomic& atomic, PrLayoutAlignmentBaseline name,
                            LayoutLineExtent extent) {
    const LayoutBaseline& baseline = atomic.baseline;
    if (name == PrLayoutAlignmentBaseline_Alphabetic)
        return kLayoutUnitZero;
    if (baseline.source == kLayoutBaselineMarginEdge)
        return edgeBaseline(name, extent, kLayoutUnitZero);
    if (baseline.source == kLayoutBaselineSynthesized) {
        PrLayoutUnit middle = (extent.below - extent.above) / 2;
        switch (name) {
            case PrLayoutAlignmentBaseline_Ideographic: return extent.below;
            case PrLayoutAlignmentBaseline_Hanging: return -extent.above;
            case PrLayoutAlignmentBaseline_Central:
            case PrLayoutAlignmentBaseline_Mathematical: return middle;
            default: return edgeBaseline(name, extent, kLayoutUnitZero);
        }
    }
    if (name == PrLayoutAlignmentBaseline_TextTop || name == PrLayoutAlignmentBaseline_TextBottom
        || name == PrLayoutAlignmentBaseline_Middle)
        return edgeBaseline(name, extent, kLayoutUnitZero);
    PrLayoutUnit at =
        baseline.unclamped + layoutFaceBaseline(layoutFontFacesOfGroup(state.styles, baseline.source).first, name);
    if (baseline.clamped)
        at = layoutUnitMin(layoutUnitMax(at, baseline.lo), baseline.hi);
    return at - baseline.offset;
}

// Returns true if a box with the box style `box` and the record `style` sits on the dominant baseline of its parent,
// styled by `parent`, with no shift. This is the case if the box has no baseline shift and aligns by the parent's
// dominant baseline, and that baseline is also the one that the box's position is measured about: its dominant baseline
// for an inline box, or its alphabetic baseline for an atomic inline.
bool onParentBaseline(const LayoutState& state, const PrLayoutBoxStyle& box, PrLayoutStyleIndex style, uint32_t parent,
                      bool open) {
    if (box.baselineShift.bits != prLayoutBaselineShiftPx(0).bits)
        return false;
    PrLayoutAlignmentBaseline dominant = layoutDominantBaseline(layoutFontStyle(state.styles, parent));
    PrLayoutAlignmentBaseline own =
        open ? layoutDominantBaseline(layoutFontStyle(state.styles, style)) : PrLayoutAlignmentBaseline_Alphabetic;
    return alignmentName(box, dominant) == dominant && own == dominant;
}

// Returns true if a box styled by `box` roots an aligned subtree: its baseline shift is top, center or bottom.
bool rootsSubtree(const PrLayoutBoxStyle& box) {
    if (prLayoutWordKind(box.baselineShift.bits) != PrLayoutWordKind_Keyword)
        return false;
    PrLayoutKeyword keyword = prLayoutWordKeywordValue(box.baselineShift.bits);
    return keyword == PrLayoutKeyword_Top || keyword == PrLayoutKeyword_Center || keyword == PrLayoutKeyword_Bottom;
}

// Returns the offset, below the dominant baseline of its parent styled by `parent`, at which a box with the box style
// `box` and the record `style` places the baseline that its position is measured about
// (css-inline-3/#transverse-alignment). The box's alignment baseline, `name`, is aligned to the parent's baseline of
// the same name. `own` is the box's `name` baseline, below the baseline that its position is measured about. The
// baseline shift then moves the box. The engine chooses the offsets for sub and super
// (css-inline-3/#valdef-baseline-shift-sub). A percentage raise is a percentage of the box's own line height.
PrLayoutUnit localShift(LayoutState& state, const PrLayoutBoxStyle& box, PrLayoutStyleIndex style, uint32_t parent,
                        PrLayoutAlignmentBaseline name, PrLayoutUnit own) {
    PrLayoutUnit shift = parentBaseline(state, parent, name) - own;
    PrLayoutBaselineShift by = box.baselineShift;
    if (!layoutBaselineShiftIsKeyword(by))
        return shift - layoutBaselineShiftRaise(&state.styles->compounds, by, lineHeightOf(state, style));
    PrLayoutUnit size = layoutFontFaces(state.styles, parent).size;
    switch (prLayoutWordKeywordValue(by.bits)) {
        case PrLayoutKeyword_Sub: return shift + size / 5 + layoutUnitPx(1);
        case PrLayoutKeyword_Super: return shift - (size / 3 + layoutUnitPx(1));
        default: return shift;
    }
}

// Returns the local shift of an inline box styled by `style` in a parent styled by `parent`. The box's leading box
// reaches `extent` about its dominant baseline.
PrLayoutUnit boxShift(LayoutState& state, PrLayoutStyleIndex style, uint32_t parent, LayoutLineExtent extent) {
    const PrLayoutBoxStyle& box = layoutBoxStyle(state.styles, style);
    PrLayoutAlignmentBaseline name =
        alignmentName(box, layoutDominantBaseline(layoutFontStyle(state.styles, parent)));
    const LayoutFaceMetrics& face = layoutFontFaces(state.styles, style).first;
    PrLayoutUnit fromFont =
        layoutFaceBaseline(face, name)
        - layoutFaceBaseline(face, layoutDominantBaseline(layoutFontStyle(state.styles, style)));
    return localShift(state, box, style, parent, name, edgeBaseline(name, extent, fromFont));
}

// Returns the local shift of an atomic inline with the box style `box` and the record `style`, laid out as `atomic`, in
// a parent styled by `parent`. The atomic inline's margin box reaches `extent` about its alphabetic baseline.
PrLayoutUnit atomicShift(LayoutState& state, const PrLayoutBoxStyle& box, PrLayoutStyleIndex style, uint32_t parent,
                         const LayoutAtomic& atomic, LayoutLineExtent extent) {
    PrLayoutAlignmentBaseline name =
        alignmentName(box, layoutDominantBaseline(layoutFontStyle(state.styles, parent)));
    return localShift(state, box, style, parent, name, atomicBaseline(state, atomic, name, extent));
}

// Returns true if an inline box styled by `style` aligns by its leading box. This is the case if its lines have normal
// height and its edges give its alignment baseline. Only an explicit alignment-baseline names an edge, because no
// dominant baseline that alignment-baseline defers to is an edge.
bool alignsByLeadingBox(const LayoutState& state, PrLayoutStyleIndex style) {
    PrLayoutAlignmentBaseline name = layoutBoxStyle(state.styles, style).alignmentBaseline;
    return layoutLineHeightIsNormal(layoutFontStyle(state.styles, style).lineHeight)
           && (name == PrLayoutAlignmentBaseline_TextTop || name == PrLayoutAlignmentBaseline_TextBottom
               || name == PrLayoutAlignmentBaseline_Middle);
}

// Returns the record that styles the parent `parent` of an item, which may be the root inline box.
PrLayoutStyleIndex parentStyleOf(const LayoutLineAligner& a, const PrLayoutStyleIndex* styles, uint32_t parent) {
    return parent != kLayoutInlineRoot ? styles[parent] : a.containerStyle;
}

// Finds the shifts of the line's items and of the boxes that it carries, for a line on which some box is
// line-dependent. First finds the leading box of each line-dependent box from the faces that its own text uses on the
// line, then that box's local shift, and then each item's shift from its parent's shift, parents first.
void shiftLine(LayoutState& state, LayoutLineAligner& a, const LayoutInlineShaping& t, const PrLayoutStyleIndex* styles,
               const LayoutLineContent& line) {
    auto dependent = [&](LayoutItemIndex item) {
        return t.items.data[item].kind == LayoutInlineItemKind_Open && a.align.data[item].lineDependent;
    };
    for (uint32_t i = 0; i < line.carried.count; ++i) {
        LayoutItemIndex item = line.carried.data[i];
        if (dependent(item))
            a.onLine.data[item] = a.align.data[item].extent;
    }
    for (uint32_t i = 0; i < line.items.count; ++i) {
        const LayoutLineItem& on = line.items.data[i];
        const LayoutInlineItem& it = t.items.data[on.item];
        if (dependent(on.item)) {
            a.onLine.data[on.item] = a.align.data[on.item].extent;
        } else if (it.kind == LayoutInlineItemKind_Text && on.to > on.from) {
            uint32_t parent = a.align.data[on.item].parent;
            if (parent != kLayoutInlineRoot && dependent(parent))
                a.onLine.data[parent] =
                    layoutTextOnLine(state, t, it, styles[on.item], on.from, on.to, a.onLine.data[parent]);
        }
    }
    auto sum = [&](LayoutItemIndex item) {
        LayoutInlineAlign& align = a.align.data[item];
        if (t.items.data[item].kind == LayoutInlineItemKind_Close) {
            align.shift = a.align.data[align.parent].shift;
            return;
        }
        if (dependent(item))
            align.local = boxShift(state, styles[item], parentStyleOf(a, styles, align.parent), a.onLine.data[item]);
        if (align.subtree == item) {
            align.shift = kLayoutUnitZero;
            return;
        }
        PrLayoutUnit base = align.parent != kLayoutInlineRoot ? a.align.data[align.parent].shift : kLayoutUnitZero;
        align.shift = base + align.local;
    };
    for (uint32_t i = 0; i < line.carried.count; ++i)
        sum(line.carried.data[i]);
    for (uint32_t i = 0; i < line.items.count; ++i)
        sum(line.items.data[i].item);
}

// Returns the aligned subtree that `item` roots on the line, and adds it if the line has not met it yet.
LayoutInlineSubtree& subtreeOf(const LayoutState& state, LayoutLineAligner& a, const PrLayoutStyleIndex* styles,
                               LayoutItemIndex item) {
    uint32_t at = a.subtreeAt.data[item];
    if (at < a.subtrees.count && a.subtrees.data[at].item == item)
        return a.subtrees.data[at];
    a.subtreeAt.data[item] = a.subtrees.count;
    PrLayoutKeyword keyword = prLayoutWordKeywordValue(layoutBoxStyle(state.styles, styles[item]).baselineShift.bits);
    LayoutInlineSubtreeKind kind = keyword == PrLayoutKeyword_Top      ? LayoutInlineSubtreeKind_Top
                                   : keyword == PrLayoutKeyword_Center ? LayoutInlineSubtreeKind_Center
                                                                       : LayoutInlineSubtreeKind_Bottom;
    PrLayoutUnit least = layoutUnit(kLayoutUnitMin);
    a.subtrees.push(LayoutInlineSubtree{item, kind, LayoutLineExtent{least, least}, kLayoutUnitZero});
    return a.subtrees.last();
}

} // namespace

void layoutLineAlignerReset(LayoutLineAligner& aligner) {
    aligner.align.reset();
    aligner.boxes.reset();
    aligner.subtrees.reset();
    aligner.subtreeAt.reset();
    aligner.onLine.reset();
}

PrLayoutUnit layoutFaceBaseline(const LayoutFaceMetrics& face, PrLayoutAlignmentBaseline name) {
    switch (name) {
        case PrLayoutAlignmentBaseline_TextTop: return -face.ascent;
        case PrLayoutAlignmentBaseline_TextBottom: return face.descent;
        case PrLayoutAlignmentBaseline_Middle: return -face.xHeight / 2;
        case PrLayoutAlignmentBaseline_Central: return face.central;
        case PrLayoutAlignmentBaseline_Ideographic: return face.ideographic;
        case PrLayoutAlignmentBaseline_Hanging: return face.hanging;
        case PrLayoutAlignmentBaseline_Mathematical: return face.math;
        default: return kLayoutUnitZero;
    }
}

PrLayoutUnit layoutDominantOffset(LayoutState& state, PrLayoutStyleIndex style) {
    return layoutFaceBaseline(layoutFontFaces(state.styles, style).first,
                              layoutDominantBaseline(layoutFontStyle(state.styles, style)));
}

LayoutLineExtent layoutTextExtent(LayoutState& state, PrLayoutStyleIndex style) {
    const LayoutFontFaces& faces = layoutFontFaces(state.styles, style);
    PrLayoutUnit content = faces.first.ascent + faces.first.descent;
    PrLayoutUnit height = lineHeightOf(state, style);
    PrLayoutUnit above = faces.first.ascent + layoutFloorToTargetPixel(state.styles, (height - content) / 2);
    PrLayoutUnit dominant = layoutDominantOffset(state, style);
    return LayoutLineExtent{above + dominant, height - above - dominant};
}

PR_NOINLINE LayoutLineExtent layoutTextOnLineSearch(LayoutState& state, const LayoutInlineShaping& t,
                                                    const LayoutInlineItem& item, PrLayoutStyleIndex style,
                                                    uint32_t from, uint32_t to, LayoutLineExtent extent) {
    if (!layoutLineHeightIsNormal(layoutFontStyle(state.styles, style).lineHeight))
        return extent;
    PrLayoutAlignmentBaseline dominant = layoutDominantBaseline(layoutFontStyle(state.styles, style));
    for (uint32_t g = item.glyphStart; g < item.glyphEnd; ++g) {
        const LayoutInlineGlyph& glyph = t.glyphs.data[g];
        // A run's face is among the faces of the run's own font group, which has the same shaping style as the item.
        const LayoutShapingRun& run = t.runs.data[glyph.run];
        const LayoutFontFaces& faces = layoutFontFacesOfGroup(state.styles, run.font);
        if (glyph.cluster >= from && glyph.cluster < to && run.face != 0)
            layoutLineExtend(extent, normalExtent(state, faces.metrics.data[run.face], dominant));
    }
    return extent;
}

void layoutAlignItems(LayoutState& state, LayoutLineAligner& aligner, const LayoutInlineShaping& t,
                      const PrLayoutStyleIndex* styles, const LayoutAtomic* atomics, PrLayoutStyleIndex containerStyle,
                      LayoutLineExtent strut, LayoutMode mode) {
    // A text run aligns with the box around it. A Close item aligns with its Open item. A box that roots an aligned
    // subtree is placed by the line box alone, so its own alignment baseline moves nothing.
    aligner.align.resize(t.items.count);
    aligner.subtreeAt.resize(t.items.count);
    aligner.onLine.resize(t.items.count);
    aligner.boxes.clear();
    aligner.containerStyle = containerStyle;
    aligner.lineDependent = false;
    aligner.subtreeRoots = false;
    for (uint32_t i = 0; i < t.items.count; ++i) {
        const LayoutInlineItem& item = t.items.data[i];
        bool inBox = aligner.boxes.count != 0;
        uint32_t parent = inBox ? aligner.boxes.last() : kLayoutInlineRoot;
        LayoutInlineAlign& align = aligner.align.data[i];
        align = LayoutInlineAlign{};
        align.parent = parent;
        align.extent = strut;
        align.subtree = inBox ? aligner.align.data[parent].subtree : kLayoutInlineRoot;
        align.shift = inBox ? aligner.align.data[parent].shift : kLayoutUnitZero;
        switch (item.kind) {
            case LayoutInlineItemKind_Text:
                if (inBox) {
                    align.extent = aligner.align.data[parent].extent;
                    aligner.align.data[parent].fallbackText = aligner.align.data[parent].fallbackText || item.fallback;
                }
                // Text with a style record of its own, such as text whose properties inherit from an element that
                // generates no box, also reaches its own leading box. An inline box with that record, sitting on the
                // parent box's baseline, would reach the same extent (css-display-4/#valdef-display-contents).
                if (styles[i] != parentStyleOf(aligner, styles, parent))
                    layoutLineExtend(align.extent, layoutTextExtent(state, styles[i]));
                break;
            case LayoutInlineItemKind_Close: {
                LayoutInlineAlign& open = aligner.align.data[parent];
                open.lineDependent = open.fallbackText && open.subtree != parent && alignsByLeadingBox(state,
                                                                                                       styles[i]);
                aligner.lineDependent = aligner.lineDependent || open.lineDependent;
                align = open;
                align.parent = parent;
                aligner.boxes.pop();
                break;
            }
            case LayoutInlineItemKind_Open:
            case LayoutInlineItemKind_Atomic: {
                bool open = item.kind == LayoutInlineItemKind_Open;
                align.extent = open ? layoutTextExtent(state, styles[i]) : atomicExtent(atomics[i], mode);
                const PrLayoutBoxStyle& box = layoutInlineItemBox(state.styles, item, styles[i]);
                uint32_t parentStyle = parentStyleOf(aligner, styles, parent);
                if (rootsSubtree(box)) {
                    align.subtree = i;
                    align.shift = kLayoutUnitZero;
                    aligner.subtreeRoots = true;
                } else if (!onParentBaseline(state, box, styles[i], parentStyle, open)) {
                    align.local = open ? boxShift(state, styles[i], parentStyle, align.extent)
                                       : atomicShift(state, box, styles[i], parentStyle, atomics[i], align.extent);
                    align.shift += align.local;
                }
                if (open)
                    aligner.boxes.push(i);
                break;
            }
            default:
                break;
        }
    }
}

LayoutLineExtent layoutAlignLine(LayoutState& state, LayoutLineAligner& aligner, const LayoutInlineShaping& t,
                                 const PrLayoutStyleIndex* styles, const LayoutLineContent& line,
                                 LayoutLineExtent strut) {
    if (aligner.lineDependent)
        shiftLine(state, aligner, t, styles, line);
    LayoutLineExtent root = strut;
    aligner.subtrees.clear();
    auto merge = [&](LayoutItemIndex item, LayoutLineExtent extent) {
        const LayoutInlineAlign& align = aligner.align.data[item];
        LayoutLineExtent& into =
            align.subtree == kLayoutInlineRoot ? root : subtreeOf(state, aligner, styles, align.subtree).extent;
        layoutLineExtend(into, LayoutLineExtent{extent.above - align.shift, extent.below + align.shift});
    };
    // A line-dependent box reaches its leading box on this line.
    auto own = [&](LayoutItemIndex item) {
        bool dependent = aligner.lineDependent && aligner.align.data[item].lineDependent;
        return dependent ? aligner.onLine.data[item] : aligner.align.data[item].extent;
    };
    for (uint32_t i = 0; i < line.carried.count; ++i)
        merge(line.carried.data[i], own(line.carried.data[i]));
    for (uint32_t i = 0; i < line.items.count; ++i) {
        const LayoutLineItem& on = line.items.data[i];
        const LayoutInlineItem& it = t.items.data[on.item];
        if (it.kind == LayoutInlineItemKind_Open || it.kind == LayoutInlineItemKind_Atomic)
            merge(on.item, own(on.item));
        else if (it.kind == LayoutInlineItemKind_Text && on.to > on.from)
            merge(on.item,
                  layoutTextOnLine(state, t, it, styles[on.item], on.from, on.to, aligner.align.data[on.item].extent));
    }
    LayoutLineExtent extent = root;
    for (uint32_t i = 0; i < aligner.subtrees.count; ++i) {
        const LayoutInlineSubtree& subtree = aligner.subtrees.data[i];
        PrLayoutUnit taller = subtree.extent.above + subtree.extent.below - (extent.above + extent.below);
        if (taller > kLayoutUnitZero)
            (subtree.kind == LayoutInlineSubtreeKind_Top ? extent.below : extent.above) += taller;
    }
    // A top subtree hangs from the line's top, a bottom subtree hangs from the line's bottom, and a center subtree is
    // centered on the line.
    for (uint32_t i = 0; i < aligner.subtrees.count; ++i) {
        LayoutInlineSubtree& subtree = aligner.subtrees.data[i];
        switch (subtree.kind) {
            case LayoutInlineSubtreeKind_Top: subtree.offset = subtree.extent.above - extent.above; break;
            case LayoutInlineSubtreeKind_Bottom: subtree.offset = extent.below - subtree.extent.below; break;
            default:
                subtree.offset = (extent.below - extent.above - subtree.extent.below + subtree.extent.above) / 2;
                break;
        }
    }
    return extent;
}

} // namespace pr_impl
