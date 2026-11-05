// Multi-column layout: the content of a multi-column container, laid out across its columns.

#include "layout/src/layout_multicol.h"

#include "layout/src/layout_block.h"
#include "layout/src/layout_box.h"

namespace pr_impl {

namespace {

// The used number and width of a container's columns, and the gap between them (css-multicol-1/#pseudo-algorithm).
struct Columns {
    uint32_t count;
    PrLayoutUnit width;
    PrLayoutUnit gap;
};

// Returns the columns of the container `node` in a content box `available` wide. A gap of normal is 1em
// (css-multicol-1/#column-gap).
Columns columnsOf(LayoutState& state, LayoutNodeIndex node, PrLayoutUnit available) {
    PrLayoutStyleIndex style = layoutTreeNode(state.tree, node).style;
    const PrLayoutFragmentationStyle& fragmentation = layoutFragmentationStyle(state.styles, style);
    PrLayoutLength gapLength = layoutFlexStyle(state.styles, style).columnGap;
    PrLayoutUnit gap;
    if (layoutLengthIsFixed(gapLength)) {
        gap = layoutGapResolve(state.styles, gapLength, available);
    } else {
        PrLayoutGroupIndex font = layoutStyle(state.styles, style).groups[PrLayoutGroup_Font];
        gap = layoutFontFacesFind(state.styles, font).size;
    }
    available = layoutUnitMax(kLayoutUnitZero, available);
    uint32_t count = fragmentation.columnCount;
    if (!layoutLengthIsAuto(fragmentation.columnWidth)) {
        PrLayoutUnit width = layoutUnitMax(layoutUnit(1),
            layoutResolve(state.styles, fragmentation.columnWidth, available));
        int64_t fits = (int64_t(available.raw) + gap.raw) / (int64_t(width.raw) + gap.raw);
        uint32_t most = uint32_t(fits < 1 ? 1 : fits > kPrLayoutMaxColumns ? kPrLayoutMaxColumns : fits);
        count = count == 0 ? most : (count < most ? count : most);
    }
    if (count == 0)
        count = 1;
    PrLayoutUnit width = layoutUnitMax(kLayoutUnitZero, (available - gap * int32_t(count - 1)) / int32_t(count));
    return Columns{count, width, gap};
}

// Returns true if the child `child` of a multi-column container is a spanner: a block-level box in flow whose
// column-span is all (css-multicol-1/#column-span).
bool isSpanner(const LayoutState& state, LayoutNodeIndex child) {
    const LayoutTreeNode& n = layoutTreeNode(state.tree, child);
    if (n.kind == PrLayoutNodeKind_Text || n.kind == PrLayoutNodeKind_LineBreak ||
            !layoutChildKindIsBlock(layoutChildKind(state.tree, child)))
        return false;
    const PrLayoutBoxStyle& box = layoutBoxStyle(state.styles, n.style);
    if (box.floating != PrLayoutFloat_None || layoutPositionIsOutOfFlow(box.position))
        return false;
    return layoutFragmentationStyle(state.styles, n.style).columnSpan == PrLayoutColumnSpan_All;
}

// Lays out the run of children from `first` through `last` of the container that `in` describes as one column `width`
// wide, from the token `resume`. The column is an anonymous block that establishes a block formatting context, laid out
// in `fragmentainer` if it is not null. Returns the column's content, with one reference. Sets `*token` to where the
// column resumes in the next column, a reference or null, and sets `baselines` to its first and last baselines from its
// block-start edge.
LayoutFragment* layOutColumn(LayoutState& state, const LayoutFlowInput& in, LayoutNodeIndex first, LayoutNodeIndex last,
        PrLayoutUnit width, const LayoutFragmentainer* PR_NULLABLE fragmentainer,
        const LayoutBreakToken* PR_NULLABLE resume, const LayoutBreakToken* PR_NULLABLE* token,
        LayoutBaseline* baselines) {
    LayoutSpace thread = *in.content;
    thread.available = width;
    thread.percentInline = width;
    thread.containing.inlineSize = width;
    thread.bfc = nullptr;
    thread.marker = LayoutLineExtent{};
    LayoutFragmentation fragmentation{fragmentainer, resume};
    thread.fragmentation = fragmentainer != nullptr ? &fragmentation : nullptr;
    // The column's children are laid out in its content box, which holds no fragmentainer: only the column's own flow
    // breaks, and gives its children theirs.
    LayoutSpace inner = thread;
    inner.fragmentation = nullptr;
    LayoutMode mode = thread.mode;
    const PrLayoutBoxStyle& anonymous = layoutAnonymousBoxStyle();
    LayoutLogicalBox logical = layoutLogicalBox(anonymous, mode);
    LayoutEdges none{};
    LayoutFlowInput flow{
        .node = in.node,
        .runFirst = first,
        .runLast = last,
        .box = &anonymous,
        .space = &thread,
        .content = &inner,
        .children = in.children,
        .origin = kLayoutLogicalOffsetZero,
        .inlineSize = width,
        .sizing = &logical,
        .inner = &none,
        .outer = &none,
        .axis = LayoutBlockAxis{.size = layoutUnit(-1), .stretch = layoutUnit(-1)},
    };
    RawArray<LayoutLogicalChild>& children = state.scratch->children;
    uint32_t mark = children.count;
    LayoutFlowOutcome outcome = layoutFlowContent(state, flow);
    PR_ASSERT(outcome.stopAt == kLayoutOffsetNone);
    LayoutFragment* fragment = layoutFragmentBuild(children, mark, LayoutFragmentKind_Box, kPrLayoutNodeNone, mode,
        LayoutLogicalSize{width, outcome.blockSize});
    fragment->flags |= LayoutFragmentFlag_Column;
    layoutFragmentSetOverflow(state.tree, fragment);
    baselines[LayoutBaselineEdge_First] = outcome.first;
    baselines[LayoutBaselineEdge_Last] = outcome.last;
    *token = outcome.token;
    return fragment;
}

// The columns of a column set laid out at one height: each column's content, its baselines, and the block size that the
// content left over after the column count needs.
struct ColumnSet {
    RawArray<LayoutFragment*> columns;
    RawArray<LayoutBaseline> baselines;     // by LayoutBaselineEdge within each column
    PrLayoutUnit leftover;
    PrLayoutUnit tallest;                   // the tallest column's content
    PrLayoutUnit shortage;                  // the least growth that lets a break push less (LayoutState)
};

// Releases the columns of a set.
void resetSet(ColumnSet& set) {
    for (uint32_t i = 0; i < set.columns.count; ++i)
        layoutFragmentRelease(set.columns.data[i]);
    set.columns.reset();
    set.baselines.reset();
}

// Lays out the run of children from `first` through `last` as columns `blockSize` tall, one after another, the first
// from `start` and each other resuming from the token of the one before, into `set`. Columns past `count` are laid out
// too, up to `limit` columns, and their content's block size is the set's leftover. If `rest` is not null, sets it to
// where the run resumes after the last column, a reference or null. `atTop` is false if content comes before the set in
// an outer fragmentainer, so that a column's content that does not fit goes to the next column, and past the last to
// the next outer fragmentainer, rather than overflow the column.
void layOutSet(LayoutState& state, const LayoutFlowInput& in, LayoutNodeIndex first, LayoutNodeIndex last,
        PrLayoutUnit width, PrLayoutUnit blockSize, uint32_t count, ColumnSet& set, bool atTop = true,
        const LayoutBreakToken* PR_NULLABLE start = nullptr, uint32_t limit = kPrLayoutMaxColumns,
        const LayoutBreakToken* PR_NULLABLE* PR_NULLABLE rest = nullptr) {
    resetSet(set);
    set.leftover = kLayoutUnitZero;
    set.tallest = kLayoutUnitZero;
    state.fragmentShortage = PrLayoutUnit{INT32_MAX};
    const LayoutBreakToken* resume = start;
    if (resume != nullptr)
        layoutBreakTokenRetain(resume);
    for (uint32_t i = 0; i < limit; ++i) {
        LayoutFragmentainer column{
            .blockSize = blockSize,
            .end = blockSize,
            .kind = LayoutFragmentainerKind_Column,
            .atTop = atTop,
        };
        const LayoutBreakToken* token = nullptr;
        LayoutBaseline baselines[LayoutBaselineEdge_Count]{};
        LayoutFragment* fragment = layOutColumn(state, in, first, last, width, &column, resume, &token, baselines);
        layoutBreakTokenRelease(resume);
        resume = token;
        PrLayoutUnit content = layoutLogicalSize(in.content->mode, fragment->extent).blockSize;
        if (i >= count)
            set.leftover += content;
        set.tallest = layoutUnitMax(set.tallest, content);
        set.columns.push(fragment);
        set.baselines.push(baselines[LayoutBaselineEdge_First]);
        set.baselines.push(baselines[LayoutBaselineEdge_Last]);
        if (resume == nullptr)
            break;
    }
    if (rest != nullptr)
        *rest = resume;
    else
        layoutBreakTokenRelease(resume);
    set.shortage = state.fragmentShortage;
}

// Returns a column `width` wide and `blockSize` tall that holds the content `content`, and takes the content.
LayoutFragment* columnOf(LayoutState& state, LayoutFragment* content, LayoutMode mode, PrLayoutUnit width,
        PrLayoutUnit blockSize) {
    LayoutLogicalSize size{width, blockSize};
    content->extent = layoutPhysicalSize(mode, size);
    layoutFragmentSetOverflow(state.tree, content);
    return content;
}

} // namespace

LayoutFlowOutcome layoutMulticolContent(LayoutState& state, const LayoutFlowInput& in) {
    layoutStackingAddFragmentationRoot(state.tree, in.node);
    const LayoutSpace& content = *in.content;
    LayoutMode mode = content.mode;
    Columns columns = columnsOf(state, in.node, content.containing.inlineSize);
    const PrLayoutFragmentationStyle& fragmentation =
        layoutFragmentationStyle(state.styles, layoutTreeNode(state.tree, in.node).style);
    // The height of the content box, which a column set fills if it does not balance, or the most that its content box
    // allows.
    PrLayoutUnit height = layoutUnit(-1);
    if (layoutIsDefinite(in.axis.size) && !in.axis.growsToContent)
        height = layoutUnitMax(kLayoutUnitZero, in.axis.size - in.axis.edges);
    PrLayoutUnit cap = layoutIsDefinite(height) ? height : content.blockLimit;
    // In a fragmentation context, the container's content is a fragmentainer's worth of rows of columns, from where its
    // token resumes: the run of children whose first is the token's child, from the token's child token in that run's
    // first column (css-multicol-1/#cf). The room is what the outer fragmentainer has left below the content box's top.
    const LayoutFragmentainer* outer = layoutSpaceFragmentainer(*in.space);
    const LayoutBreakToken* outerResume = layoutSpaceResume(*in.space);
    PrLayoutUnit outerRoom = layoutUnit(-1);
    if (outer != nullptr)
        outerRoom = layoutUnitMax(layoutUnitPx(1), outer->end - in.origin.blockOffset);
    LayoutBreakToken* token = nullptr;
    const LayoutBreakToken* runResume = outerResume != nullptr ? outerResume->childToken : nullptr;

    // The runs of children between spanners. The container's outside marker is in no run.
    RawArray<LayoutLogicalChild>& children = state.scratch->children;
    const LayoutTreeNode& container = layoutTreeNode(state.tree, in.node);
    LayoutNodeIndex first = container.first;
    if (first != kPrLayoutNodeNone && layoutNodeMarker(state.tree, in.node) == first)
        first = layoutTreeNode(state.tree, first).next;
    // A token with no child resumes only the rest of the container's own block size, after all of its content.
    if (outerResume != nullptr)
        first = outerResume->child;
    // A container whose content box starts at or past the outer fragmentainer's end places none of its content there.
    // If something comes before it, the break goes before the container (LayoutBreakToken::atStart).
    if (outer != nullptr && outerResume == nullptr && !outer->atTop && first != kPrLayoutNodeNone &&
            outer->end <= in.origin.blockOffset) {
        token = layoutBreakTokenCreate(in.node, kLayoutUnitZero, first, nullptr);
        token->atStart = true;
    }
    PrLayoutUnit cursor = kLayoutUnitZero;
    LayoutNodeIndex runFirst = kPrLayoutNodeNone;
    LayoutNodeIndex runLast = kPrLayoutNodeNone;
    // The container's first baseline is the first that its column sets have, in the column that holds it, and its last
    // baseline is the last that they have (css-multicol-1/#the-multi-column-model).
    LayoutBaseline firstBaseline{};
    LayoutBaseline lastBaseline{};
    auto set = [&](bool last) {
        if (runFirst == kPrLayoutNodeNone)
            return;
        PrLayoutUnit room = layoutIsDefinite(height) ? layoutUnitMax(kLayoutUnitZero, height - cursor) : cap;
        bool fills = last && fragmentation.columnFill == PrLayoutColumnFill_Auto && layoutIsDefinite(height);
        PrLayoutUnit blockSize = room;
        ColumnSet laid{};
        const LayoutBreakToken* start = runResume;
        runResume = nullptr;
        bool balances = !fills;
        if (outer != nullptr) {
            // The set's columns are at most as tall as the room left in the outer fragmentainer. If its content does
            // not fit in them, the columns fill that room and the container breaks after them. Otherwise the set is the
            // last in its context, and balances the content left, which is all in its columns.
            PrLayoutUnit left = layoutUnitMax(layoutUnitPx(1), outerRoom - cursor);
            room = layoutIsDefinite(room) ? layoutUnitMin(room, left) : left;
            bool atTop = outer->atTop && cursor == kLayoutUnitZero;
            const LayoutBreakToken* rest = nullptr;
            layOutSet(state, in, runFirst, runLast, columns.width, room, columns.count, laid, atTop, start,
                columns.count, &rest);
            blockSize = room;
            bool placedNone = rest != nullptr && !atTop && cursor == kLayoutUnitZero && outerResume == nullptr;
            for (uint32_t i = 0; i < laid.columns.count && placedNone; ++i)
                placedNone = laid.columns.data[i]->childCount == 0;
            if (placedNone) {
                // Every column pushed its content on, so the container places none of its content here, and the break
                // goes before it (LayoutBreakToken::atStart).
                layoutBreakTokenRelease(rest);
                resetSet(laid);
                token = layoutBreakTokenCreate(in.node, kLayoutUnitZero, runFirst, nullptr);
                token->atStart = true;
                runFirst = kPrLayoutNodeNone;
                return;
            }
            if (rest != nullptr) {
                token = layoutBreakTokenCreate(in.node, kLayoutUnitZero, runFirst, rest);
                balances = false;
            } else if (balances) {
                PrLayoutUnit total = kLayoutUnitZero;
                for (uint32_t i = 0; i < laid.columns.count; ++i)
                    total += layoutLogicalSize(mode, laid.columns.data[i]->extent).blockSize;
                blockSize = layoutUnitMin(room, layoutUnitMax(layoutUnitPx(1),
                    (total + layoutUnit(int32_t(columns.count) - 1)) / int32_t(columns.count)));
                for (uint32_t attempt = 0; attempt < 16 && blockSize < room; ++attempt) {
                    layOutSet(state, in, runFirst, runLast, columns.width, blockSize, columns.count, laid, atTop, start);
                    bool overflows = laid.tallest > blockSize;
                    if (laid.columns.count <= columns.count && !overflows)
                        break;
                    PrLayoutUnit grown =
                        overflows ? laid.tallest : blockSize + layoutUnitMax(layoutUnit(1), laid.shortage);
                    if (laid.shortage == PrLayoutUnit{INT32_MAX} && !overflows)
                        grown = blockSize + layoutUnitMax(layoutUnit(1), laid.leftover / int32_t(columns.count));
                    blockSize = layoutUnitMin(grown, room);
                    if (blockSize == room)
                        layOutSet(state, in, runFirst, runLast, columns.width, blockSize, columns.count, laid, atTop, start);
                }
            }
        } else if (fills) {
            layOutSet(state, in, runFirst, runLast, columns.width, blockSize, columns.count, laid);
        } else if (balances) {
            // A balanced set starts at the height that shares its content's unfragmented height among its columns, and
            // grows by the share of what is left over, a bounded number of times.
            const LayoutBreakToken* none = nullptr;
            LayoutBaseline ignored[LayoutBaselineEdge_Count]{};
            LayoutFragment* whole = layOutColumn(state, in, runFirst, runLast, columns.width, nullptr, nullptr, &none, ignored);
            PrLayoutUnit total = layoutLogicalSize(mode, whole->extent).blockSize;
            layoutFragmentRelease(whole);
            blockSize = layoutUnitMax(layoutUnitPx(1), (total + layoutUnit(int32_t(columns.count) - 1)) /
                int32_t(columns.count));
            if (layoutIsDefinite(room))
                blockSize = layoutUnitMin(blockSize, layoutUnitMax(room, layoutUnitPx(1)));
            // Each attempt grows the height by the least that pushes less to the next column, or to the tallest
            // monolithic content, which overflows a shorter column.
            for (uint32_t attempt = 0; attempt < 16; ++attempt) {
                layOutSet(state, in, runFirst, runLast, columns.width, blockSize, columns.count, laid);
                bool overflows = laid.tallest > blockSize;
                if ((laid.columns.count <= columns.count && !overflows) || (layoutIsDefinite(room) && blockSize >= room))
                    break;
                PrLayoutUnit grown = overflows ? laid.tallest : blockSize + layoutUnitMax(layoutUnit(1), laid.shortage);
                if (laid.shortage == PrLayoutUnit{INT32_MAX} && !overflows)
                    grown = blockSize + layoutUnitMax(layoutUnit(1), laid.leftover / int32_t(columns.count));
                blockSize = layoutIsDefinite(room) ? layoutUnitMin(grown, room) : grown;
            }
        }
        runFirst = kPrLayoutNodeNone;
        for (uint32_t i = 0; i < laid.columns.count; ++i) {
            PrLayoutUnit moved = in.origin.blockOffset + cursor;
            const LayoutBaseline& first = laid.baselines.data[2 * i];
            const LayoutBaseline& lastLine = laid.baselines.data[2 * i + 1];
            if (first.present && !firstBaseline.present)
                firstBaseline = layoutBaselineMoved(first, moved);
            if (lastLine.present)
                lastBaseline = layoutBaselineMoved(lastLine, moved);
            LayoutFragment* column = columnOf(state, laid.columns.data[i], mode, columns.width, blockSize);
            LayoutLogicalOffset at{(columns.width + columns.gap) * int32_t(i), cursor};
            children.push(LayoutLogicalChild{column, in.origin + at});
        }
        laid.columns.reset();
        laid.baselines.reset();
        cursor += blockSize;
    };
    LayoutNodeIndex end = kPrLayoutNodeNone;
    for (LayoutNodeIndex child = first; child != end && token == nullptr; child =
            layoutTreeNode(state.tree, child).next) {
        if (!isSpanner(state, child)) {
            if (runFirst == kPrLayoutNodeNone)
                runFirst = child;
            runLast = child;
            continue;
        }
        set(false);
        if (token != nullptr)
            break;
        // A spanner is laid out across the content box, in a block formatting context of its own, between the column
        // sets before and after it. In a fragmentation context it is monolithic, and goes to the next fragmentainer if
        // it does not fit after what comes before it.
        LayoutSpace space = content;
        space.bfc = nullptr;
        space.marker = LayoutLineExtent{};
        LayoutResult result = layoutBlock(state, child, space);
        PrLayoutUnit spanned =
            layoutStrutSize(result.start) + layoutLogicalSize(mode, result.fragment->extent).blockSize;
        if (outer != nullptr && cursor > kLayoutUnitZero && cursor + spanned > outerRoom) {
            layoutFragmentRelease(result.fragment);
            token = layoutBreakTokenCreate(in.node, kLayoutUnitZero, child, nullptr);
            break;
        }
        cursor += layoutStrutSize(result.start);
        children.push(LayoutLogicalChild{
            result.fragment, in.origin + LayoutLogicalOffset{result.marginInlineStart, cursor},
        });
        PrLayoutUnit at = in.origin.blockOffset + cursor;
        if ((result.flags & LayoutResultFlag_FirstBaseline) != 0 && !firstBaseline.present)
            firstBaseline = layoutBaselineMoved(layoutFragmentBaseline(result.fragment, LayoutBaselineEdge_First), at);
        if ((result.flags & LayoutResultFlag_LastBaseline) != 0)
            lastBaseline = layoutBaselineMoved(layoutFragmentBaseline(result.fragment, LayoutBaselineEdge_Last), at);
        cursor += layoutLogicalSize(mode, result.fragment->extent).blockSize + layoutStrutSize(result.end);
    }
    if (token == nullptr)
        set(true);

    LayoutFlowOutcome outcome{};
    outcome.token = token;
    outcome.stopAt = kLayoutOffsetNone;
    outcome.top = kLayoutOffsetNone;
    outcome.contentBlock = cursor;
    outcome.first = firstBaseline;
    outcome.last = lastBaseline;
    PrLayoutUnit size = cursor + in.axis.edges;
    outcome.blockSize = layoutContentBlockSize(state.styles, *in.box, *in.sizing, in.axis, in.space->percentBlock,
        in.clamps, size, size);
    return outcome;
}

} // namespace pr_impl
