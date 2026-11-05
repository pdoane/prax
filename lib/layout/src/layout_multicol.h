// Multi-column layout: the content of a multi-column container, laid out across its columns.
//
// css-multicol-1 - https://drafts.csswg.org/css-multicol-1/
//
// A multi-column container's shell (layout_block.h) finds its edges and content box as any block container's. Its
// content is a run of column sets and spanners. A column set holds the children between two spanners, laid out at the
// used column width as an anonymous block that establishes a block formatting context, one column at a time: each
// column is a fragmentainer, and its content resumes from the break token of the column before
// (layout_fragmentation.h).

#pragma once

#include "layout/src/layout_flow.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Lays out the content of the multi-column container that `in` describes, and appends its columns and spanners to the
// scratch children, at their logical offsets in its border box. The container establishes a block formatting context,
// so no margin collapses through its edges.
LayoutFlowOutcome layoutMulticolContent(LayoutState& state, const LayoutFlowInput& in);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
