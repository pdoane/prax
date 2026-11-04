// Itemization: an inline formatting context's text divided into shaping runs, before it is shaped.
//
// css-text-3 - https://drafts.csswg.org/css-text-3/
//
// Layout passes its items to lib/text's itemization (pr/text_itemize.h) as pieces, one piece per item. Itemization runs
// over the whole text of each segment, across nodes and inline boxes, so that the decision for a range can read the
// neighboring text whether or not markup ends between them. Setting clusters in faces may add faces to a font group,
// and itemization measures those faces.

#pragma once

#include "layout/src/layout_shaping.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Returns true if a typographic character unit is set upright on a line of a container styled by `container`
// (prTextIsUpright). The unit's first code point is `codePoint`, its resolved script is `script`, and it is styled by
// `font`. A unit can be upright only in a vertical typographic mode, and the text-orientation of `font` decides whether
// it is.
bool layoutIsUpright(const PrLayoutFontStyle& container, const PrLayoutFontStyle& font, uint32_t codePoint, PrScript script);

// Returns true if text styled by the font groups `a` and `b` shapes identically, which is the case if the groups have
// the same faces, size and language, ask for the same features, such as small capitals, and have the same spacing.
// Other properties of the groups, such as color, do not affect glyphs.
bool layoutSameShapingStyle(const LayoutStyles* styles, uint32_t a, uint32_t b);

// Divides the text of `t` into shaping runs (LayoutShapingRun) and replaces `t.runs` with them. Each run records its
// script, orientation, bidi level and face. Also resolves `t.levels`, `t.paragraphs`, `t.marks` and `t.splits`
// (layoutBidiFindSplits). A paragraph ends at the bounds of a segment and at each forced break. Returns true if any
// bidi level was resolved.
bool layoutItemize(LayoutStyles* styles, PrTextShaper* PR_NULLABLE shaper, LayoutInlineShaping& t, LayoutShapingScratch& s);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
