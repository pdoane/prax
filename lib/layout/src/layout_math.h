// Math layout: the content of math boxes (mathml-core/#layout-algorithms), laid out as TeX lays out formulas where
// MathML Core is looser than TeXbook Appendix G and the OpenType MATH table's intended use.
//
// mathml-core - https://w3c.github.io/mathml-core/
// mathml4 - https://w3c.github.io/mathml/
// opentype-math - https://learn.microsoft.com/en-us/typography/opentype/spec/math
//
// A math box (PrLayoutDisplay_BlockMath, PrLayoutDisplay_InlineMath) is laid out by its shell as any block-level box
// is, and its content by the algorithm that its math group names. Its items are its child boxes and its runs of text.
// Each item is laid out through its own cache, in a space whose stretch size constraint (LayoutSpace::stretch) is part
// of the cache key. A token's content is laid out in flow, and its math layout then sizes its box to the ink of its
// glyphs, or draws its operator stretched. A math box's fragment holds its math metrics, which its math parent reads,
// and the rules and glyphs that it draws beside its children (LayoutFragmentFlag_Math).

#pragma once

#include "layout/src/layout_space.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// What the shell of a math box gives its math layout, in the box's own writing mode.
struct LayoutMathInput {
    LayoutNodeIndex node;
    const LayoutSpace* content;             // the content box, with its mode and the fallback that its children read
    LayoutLogicalOffset origin;             // the start corner of the content box in the border box

    // The content box's inline and block sizes if the box's style decides them, or negative.
    PrLayoutUnit contentInline;
    PrLayoutUnit contentBlock;

    // The size that the inline-axis percentages of the children resolve against. It is the content box's inline size if
    // the box's style decides it, or else what the box's own percentages resolve against, or negative for none.
    PrLayoutUnit percentInline;

    const LayoutMathStretch* PR_NULLABLE stretch;

    // A token: the first of its lines among the scratch children, and its first baseline from the block-start edge of
    // its border box, or kLayoutBaselineNone.
    uint32_t mark;
    PrLayoutUnit baseline;
};

// What the math layout of a box gives its shell. It holds the inline and block sizes of the math content box, the
// position of its alphabetic baseline from its block-start edge, and its math metrics, whose ink is measured from that
// baseline and whose attachment from its inline-start edge. The content box places the math content box at `shift`
// along the inline axis. The box's paints and glyphs are those of the scratch from `paints` and `glyphs` on, positioned
// from the start corner of the content box.
struct LayoutMathOutcome {
    PrLayoutUnit inlineSize;
    PrLayoutUnit blockSize;
    PrLayoutUnit baseline;
    PrLayoutUnit shift;
    LayoutMathMetrics metrics;
    uint32_t paints;
    uint32_t glyphs;
    bool sized;                     // false if its line's block metrics stand, as for a token that is not only text
    bool inlineSized;               // true if `inlineSize` replaces the inline size that its flow content decided
};

// Lays out the children of the math box `input.node`, which is not a token, and places their fragments among the
// scratch children.
LayoutMathOutcome layoutMathContent(LayoutState& state, const LayoutMathInput& input);

// Measures the token `input.node` by the ink of the line that its flow content laid out among the scratch children from
// `input.mark`, and moves that line so that its baseline is the outcome's. An operator that stretches, or that is drawn
// large, replaces its line with the glyphs that it draws.
LayoutMathOutcome layoutMathToken(LayoutState& state, const LayoutMathInput& input);

// Returns the fragment of a math box made from `built`, which it takes, with the metrics and paints of `outcome`. The
// box's content box starts at `origin` in its border box, whose inline size is `inlineSize`, in `mode`.
LayoutFragment* layoutMathFinish(LayoutState& state, LayoutFragment* built, const LayoutMathOutcome& outcome,
    LayoutLogicalOffset origin, PrLayoutUnit inlineSize, LayoutMode mode);

// Drops the paints of `outcome` from the scratch, for a box that makes no fragment.
void layoutMathDiscard(LayoutState& state, const LayoutMathOutcome& outcome);

// Returns the intrinsic sizes of the math box `node`, which is not a token. Both are the inline size of its math
// content as its layout finds it, with its edges. Math content does not wrap, and a stretched operator's size is the
// size that it is drawn at.
LayoutIntrinsicSizes layoutMathIntrinsic(LayoutState& state, LayoutNodeIndex node, const LayoutIntrinsicInput& input);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
