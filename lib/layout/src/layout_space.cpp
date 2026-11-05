// Layout's protocol: what a box is laid out and measured in, what it gives back, and what every step of a layout reads.

#include "layout/src/layout_space.h"

#include "pr/alloc.h"

#include "layout/src/layout_flex.h"
#include "layout/src/layout_grid.h"
#include "layout/src/layout_inline.h"
#include "layout/src/layout_shaping.h"
#include "layout/src/layout_table.h"

namespace pr_impl {

LayoutScratch* layoutScratchCreate(void) {
    LayoutScratch* scratch = allocZeroed<LayoutScratch>();
    scratch->glyphs = prTextGlyphBufferCreate();
    scratch->shaping = allocZeroed<LayoutShapingScratch>();
    return scratch;
}

void layoutScratchDestroy(LayoutScratch* PR_NULLABLE scratch) {
    if (scratch == nullptr)
        return;
    scratch->children.reset();
    scratch->pending.reset();
    scratch->held.reset();
    scratch->inlineBlocks.reset();
    scratch->shapings.reset();
    for (uint32_t i = 0; i < scratch->handoffs.count; ++i)
        layoutFragmentRelease(scratch->handoffs.data[i].result.fragment);
    scratch->handoffs.reset();
    scratch->handoffIndex.reset();
    scratch->memos.reset();
    scratch->broken.reset();
    scratch->memoIndex.reset();
    scratch->intrinsicMemos.reset();
    scratch->intrinsicMemoIndex.reset();
    scratch->bands.reset();
    scratch->bandIndex.reset();
    prTextGlyphBufferDestroy(scratch->glyphs);
    layoutShapingScratchReset(*scratch->shaping);
    prMemoryFree(scratch->shaping);
    layoutInlineScratchDestroy(scratch->inlines);
    layoutFlexScratchDestroy(scratch->flex);
    layoutTableScratchDestroy(scratch->table);
    layoutGridScratchDestroy(scratch->grid);
    prMemoryFree(scratch);
}

} // namespace pr_impl
