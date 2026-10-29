// Flow layout: the content of a block container, laid out in normal flow.

#include "layout/src/layout_flow.h"

#include "layout/src/layout_block.h"
#include "layout/src/layout_cache.h"
#include "layout/src/layout_float.h"
#include "layout/src/layout_inline.h"
#include "layout/src/layout_line_align.h"
#include "layout/src/layout_position.h"

namespace pr_impl {

namespace {

// ---------------------------------------------------------------------------------------------------------------------
// Containers

// A block container whose children are being laid out, and its position in its block formatting context.
struct Container {
    LayoutNodeIndex node;             // the box, or the parent of the content of an anonymous box

    // An anonymous box: the first and last node of the siblings it wraps. Otherwise kPrLayoutNodeNone, and the content
    // is the box's children.
    LayoutNodeIndex runFirst;
    LayoutNodeIndex runLast;

    const LayoutSpace* content;       // its content box, the containing block of its children; with no `bfc`
    LayoutOuterMode children;         // the mode of its content, which its children are laid out in
    LayoutLogicalOffset origin;       // inline-start, block-start corner of its content box in its border box
    PrLayoutUnit edgeStart;           // its block-start border and padding
    bool inContext;                   // true if it is in flow in a context it does not establish
    bool startCollapses;              // true if its block-start margin collapses with its first child's
    bool clears;                      // true if it has clear of its own, not only a floor of a box it collapses into
    LayoutContainingBlock containing; // its content box among the floats of its context

    // In a context: the margins before it and its own margin, which the margins of its first children join while its
    // block-start margin collapses with theirs.
    LayoutMarginChain chain;

    // The position in the context of a block-level child in flow, set for each such child, and the space the child is
    // laid out in, which refers to that position.
    LayoutBfc child;
    LayoutSpace inFlow;

    // The outside markers that the first line of its flow holds: those of its space, and its own if it is a list item.
    LayoutLineExtent marker;

    // In a context: whether its block-end margin collapses with its last child's, and whether it would if its block
    // size were auto. The size of its content, which is its automatic minimum, then ends before the pending margins.
    bool endCollapses;
    bool contentEndCollapses;
};

// The state of the block-start edge of a container as its children are laid out.
enum {
    Edge_Open,                      // its margins collapse with what follows, and where they resolve is not known
    Edge_Located,                   // a restarted layout says where they resolve, but they still collapse
    Edge_Closed,                    // they have resolved, and its content is past them
    Edge_Stopped,                   // they resolve with floats waiting, so its layout stops or starts over there
};
typedef uint8_t Edge;

// A baseline of the flow of a container, measured from the block-start edge of its border box. It is either the
// baseline of a child, or a baseline of the container's own lines, or the block-end margin edge of a child that is a
// scroll container. The fragment of a child records its baseline with its set, and the baseline is read from there only
// when the container exports its baselines, so that taking one costs little.
struct FlowBaseline {
    const LayoutFragment* PR_NULLABLE child;
    PrLayoutUnit at;                // block-start edge of the child's border box, or else the baseline position
    uint32_t source;                // if not a child's: its set's source, a font group or kLayoutBaselineMarginEdge
    bool present;
};

// The content of a container laid out so far, and the position of the container in its context.
struct Flow {
    Edge edge;
    PrLayoutUnit top;               // block-start edge of its border box in the context, once located
    PrLayoutUnit contentTop;        // block-start edge of its content box in the context, once located
    bool cleared;                   // true if clearance put its border box at `top`

    // Where a layout that stopped found the margins resolve. `restart` is true if the container lays its children out
    // again with the margins resolved there, because the floats waiting for them are its own. Otherwise its parent
    // does.
    PrLayoutUnit stopAt;
    bool restart;

    PrLayoutUnit cursor;            // the block offset in the content box past the last in-flow content
    LayoutStrut pending;            // margins adjoining the end of the content
    LayoutStrut escaped;            // margins that collapsed through the container's block-start edge
    bool content;                   // true if some in-flow content was placed, past which margins cannot collapse
    bool sealed;                    // true if `pending` follows a cleared box that collapsed through, and stays inside
    bool blocks;                    // true once a block-level box in flow was reached: no later line is the first
                                    // formatted
    FlowBaseline first;             // the baseline of the first in-flow line
    FlowBaseline last;              // the baseline of the last in-flow line

    LayoutLineSides adjoining;      // the floats adjoining its margins while they collapse, placed or not
    LayoutLineSides unplaced;       // the adjoining floats not yet placed
};

// Returns true if the container's block-start margin collapses with what follows.
bool collapsing(const Flow& flow) {
    return flow.edge == Edge_Open || flow.edge == Edge_Located;
}

// Returns where the pending margins, together with `more`, end. While the margins of the container collapse with them,
// they end through the chain. Otherwise they end past its content.
PrLayoutUnit nextEdge(const Container& c, const Flow& flow, LayoutStrut more = LayoutStrut{}) {
    if (collapsing(flow))
        return layoutMarginChainEnd(c.chain, flow.pending + more);
    return flow.contentTop + flow.cursor + layoutStrutSize(flow.pending + more);
}

// Stops the layout of the container because the margins before it resolve at `at`. The container starts its layout over
// if the waiting floats are its own. Otherwise its parent starts it over.
void stop(const Container& c, Flow& flow, PrLayoutUnit at) {
    flow.edge = Edge_Stopped;
    flow.stopAt = at;
    flow.restart = c.chain.adjoining == 0;
}

// Finds the position of the border box of the container. Its margins resolve at `end` if no earlier layout said where
// they resolve. Returns false if floats wait for the margins, in which case its layout stops.
bool locate(const Container& c, Flow& flow, PrLayoutUnit end) {
    PR_ASSERT(flow.edge != Edge_Stopped);
    if (flow.edge != Edge_Open)
        return true;
    LayoutResolution resolution = layoutMarginChainResolve(c.chain, end, (c.chain.adjoining | flow.unplaced) != 0);
    if (resolution.waits) {
        stop(c, flow, resolution.at);
        return false;
    }
    flow.edge = Edge_Located;
    flow.top = resolution.top;
    flow.contentTop = resolution.top + c.edgeStart;
    flow.cleared = resolution.cleared;
    return true;
}

// Ends the collapsing of the margins of the container at `end`. The pending margins escape through its block-start
// edge. Returns false if its layout stops.
bool close(const Container& c, Flow& flow, PrLayoutUnit end) {
    PR_ASSERT(collapsing(flow));
    if (!locate(c, flow, end))
        return false;
    flow.escaped += flow.pending;
    flow.pending = LayoutStrut{};
    flow.edge = Edge_Closed;
    return true;
}

// Returns a flow at the start of the content of the container. Its margins collapse if its block-start edge lets them,
// and are otherwise resolved. If the container establishes its context, there is nothing to collapse.
Flow startFlow(const Container& c) {
    Flow flow{};
    flow.stopAt = kLayoutOffsetNone;
    flow.edge = Edge_Closed;
    if (!c.inContext)
        return flow;
    flow.edge = Edge_Open;
    if (!c.startCollapses) {
        // Its block-start border or padding resolves the margins before it.
        if (locate(c, flow, layoutMarginChainEnd(c.chain)))
            flow.edge = Edge_Closed;
    } else if (c.chain.resolved != kLayoutOffsetNone) {
        locate(c, flow, c.chain.resolved);
    }
    return flow;
}

// Appends the fragment of a child at `at` in the content box, and takes the baselines of the child if it has them. The
// child's first baseline becomes the first baseline of the flow if the flow has none yet. The child's last baseline
// becomes the last baseline of the flow.
//
// A scroll container gives its parent its block-end margin edge as its last baseline, whether or not it has lines. Its
// baseline-source does not change this, because baseline-source applies only to inline-level boxes
// (css-align-3/#baseline-export).
//
// A table gives its parent no last baseline. The baseline of an inline-block is the baseline of its last line box in
// normal flow (CSS2/visudet.html#propdef-vertical-align), and the line boxes inside a table's cells are not in the
// parent's flow. The engine applies this to the last baseline of every block container, not only an inline-block's.
//
// Relative positioning moves neither baseline.
void pushChild(LayoutState& state, const Container& c, Flow& flow, const LayoutResult& result, LayoutLogicalOffset at,
               LayoutLogicalOffset relative) {
    state.scratch->children.push(LayoutLogicalChild{result.fragment, c.origin + at + relative});
    PrLayoutUnit by = c.origin.blockOffset + at.blockOffset;
    if ((result.flags & LayoutResultFlag_FirstBaseline) != 0 && !flow.first.present)
        flow.first = FlowBaseline{result.fragment, by, kLayoutBaselineSynthesized, true};
    if ((result.flags & LayoutResultFlag_Scrolls) == 0) {
        bool table = (result.fragment->flags & LayoutFragmentFlag_Table) != 0;
        if ((result.flags & LayoutResultFlag_LastBaseline) != 0 && !table)
            flow.last = FlowBaseline{result.fragment, by, kLayoutBaselineSynthesized, true};
        return;
    }
    const PrLayoutBoxStyle& box = layoutBoxStyle(state.styles, layoutTreeNode(state.tree, result.fragment->node).style);
    LayoutMode mode = c.content->mode;
    LayoutEdges e = layoutEdgesFromBox(state.styles, layoutLogicalBox(box, mode), c.content->containing.inlineSize);
    PrLayoutUnit edge = layoutLogicalSize(mode, result.fragment->extent).blockSize + e.margin[LayoutSide_BlockEnd];
    flow.last = FlowBaseline{nullptr, by + edge, kLayoutBaselineMarginEdge, true};
}

// Returns a baseline of the lines of a container, `offset` from the block-start edge of its border box. Its set comes
// from the font of the container, which is the font of the root inline box. The content of the container is in its own
// block flow whenever it has a baseline to export.
FlowBaseline lineBaseline(const Container& c, PrLayoutUnit offset) {
    return FlowBaseline{nullptr, offset, c.children.font, true};
}

// Returns the outside markers that the next child in flow holds on its first line. The container's markers go to its
// first line, which is in the first child that has a first baseline.
LayoutLineExtent markerFor(const Container& c, const Flow& flow) {
    return flow.first.present ? LayoutLineExtent{} : c.marker;
}

// ---------------------------------------------------------------------------------------------------------------------
// Children

// Places the result of a block-level child in the flow, as normal flow places it. The child goes past the pending
// margins and its own block-start margins, and its block-end margins become pending after it. Records what the child
// read of the context from that position, in the context that childContext last gave. Relative positioning moves no
// baseline. Returns false if the layout of the container stops when its margins resolve.
PR_FORCE_INLINE bool placeBlock(LayoutState& state, Container& c, Flow& flow, const LayoutResult& result,
                                LayoutLogicalOffset relative) {
    PrLayoutUnit size = layoutLogicalSize(c.content->mode, result.fragment->extent).blockSize;
    PrLayoutUnit offset = kLayoutUnitZero;
    if (result.cleared) {
        // Clearance ends the margins before the child, which resolve without the child's own margin
        // (CSS2/#flow-control).
        if (collapsing(flow) && !close(c, flow, nextEdge(c, flow)))
            return false;
        offset = result.top - flow.contentTop;
        if ((result.flags & LayoutResultFlag_CollapsedThrough) != 0) {
            // The margins after the child collapse with its own, as though they began with its block-start margins
            // above it.
            flow.cursor = offset - layoutStrutSize(result.start);
            flow.pending = result.start + result.end;
            flow.sealed = true;
        } else {
            flow.cursor = offset + size;
            flow.pending = result.end;
            flow.sealed = false;
        }
        flow.content = true;
    } else if ((result.flags & LayoutResultFlag_CollapsedThrough) != 0) {
        // Its border box is past the margins before it and its block-start margins, but not its block-end margin.
        offset = collapsing(flow) ? kLayoutUnitZero : flow.cursor + layoutStrutSize(flow.pending + result.start);
        flow.pending += result.start + result.end;
    } else {
        flow.pending += result.start;
        if (collapsing(flow)) {
            if (!close(c, flow, nextEdge(c, flow)))
                return false;
            offset = flow.cursor;
        } else {
            offset = flow.cursor + layoutStrutSize(flow.pending);
        }
        flow.cursor = offset + size;
        flow.pending = result.end;
        flow.content = true;
        flow.sealed = false;
    }
    // What the child read is recorded relative to its border box, which is either past the content or where the margins
    // before it end.
    if (result.readLo.raw < result.readHi.raw) {
        const LayoutMarginChain& chain = c.child.chain;
        PrLayoutUnit top = flow.edge != Edge_Open             ? flow.contentTop + offset
                           : chain.resolved != kLayoutOffsetNone ? chain.resolved
                                                               : layoutMarginChainEnd(chain, result.start);
        layoutReadsRange(*c.containing.reads, result.readLo + top, result.readHi + top);
    }
    pushChild(state, c, flow, result, LayoutLogicalOffset{result.marginInlineStart, offset}, relative);
    return true;
}

// Returns the position in the context of a block-level child in flow. It comes after the pending margins, which
// collapse with the margins of the container while those still collapse, and below the floats on `clears`.
LayoutBfc& childContext(Container& c, const Flow& flow, LayoutLineSides clears) {
    LayoutMarginChain& chain = c.child.chain;
    if (collapsing(flow)) {
        chain = c.chain;
        chain.strut += flow.pending;
        chain.adjoining = LayoutLineSides(c.chain.adjoining | flow.adjoining);
    } else {
        chain = layoutMarginChainAt(flow.contentTop + flow.cursor);
        chain.strut = flow.pending;
    }
    if (clears != 0)
        layoutMarginChainClear(chain, layoutExclusionsClearance(c.containing, clears, kLayoutOffsetNone));
    return c.child;
}

// Lays out a float among the block-level children. The float is placed at the point where layout reaches it or, if the
// margins of the container have not resolved, waits for them (CSS2/#float-position).
PR_NOINLINE void floatChild(LayoutState& state, const Container& c, Flow& flow, uint32_t child,
                            const PrLayoutBoxStyle& box) {
    LayoutMode mode = c.content->mode;
    LayoutAtomic f = layoutFloat(state, child, *c.content);
    LayoutLineSides side = layoutFloatSide(box, mode.direction);
    if (flow.edge == Edge_Open) {
        flow.unplaced = LayoutLineSides(flow.unplaced | side);
        flow.adjoining = LayoutLineSides(flow.adjoining | side);
        layoutFragmentRelease(f.fragment);
        return;
    }
    PrLayoutUnit top = flow.edge == Edge_Located ? flow.contentTop : nextEdge(c, flow);
    LayoutExclusion placed = layoutFloatPlace(box, f, c.containing, mode, top);
    LayoutLogicalOffset at = layoutFloatOffset(f, placed, c.containing, mode, flow.contentTop);
    state.scratch->children.push(LayoutLogicalChild{f.fragment, c.origin + at});
    if (collapsing(flow))
        flow.adjoining = LayoutLineSides(flow.adjoining | side);
}

// Places the placeholder of the box out of flow `child` among the block-level children, where a block-level box with no
// margins of its own would be placed, ignoring floats and clearance (css-position-3/#staticpos-rect). The placeholder
// goes at the inline start of the content box, past the margins before it. Those margins collapse past it, and it takes
// no room.
PR_NOINLINE void outOfFlowChild(LayoutState& state, const Container& c, const Flow& flow, uint32_t child) {
    PrLayoutUnit offset = collapsing(flow) ? kLayoutUnitZero : flow.cursor + layoutStrutSize(flow.pending);
    LayoutFragment* placeholder =
        layoutFragmentCreatePlaceholder(child, layoutStaticEdges(c.content->mode), layoutIsFixed(state.tree, child));
    state.scratch->children.push(
        LayoutLogicalChild{placeholder, c.origin + LayoutLogicalOffset{kLayoutUnitZero, offset}});
}

// Lays out a block-level child in flow that does not avoid floats. Returns false if the layout of the container stops.
PR_FORCE_INLINE bool flowChild(LayoutState& state, Container& c, Flow& flow, uint32_t child,
                               const PrLayoutBoxStyle& box) {
    LayoutLineSides clears = layoutClearSides(box, c.content->mode.direction);
    // A child that clears floats adjoining its position has clearance. The margins before it resolve without its own
    // margin, and the floats are placed there.
    bool clearsAdjoining = collapsing(flow) && (clears & (c.chain.adjoining | flow.adjoining)) != 0;
    if (clearsAdjoining && !close(c, flow, nextEdge(c, flow)))
        return false;
    LayoutBfc& bfc = childContext(c, flow, clears);
    bfc.chain.clearsAdjoining = bfc.chain.clearsAdjoining || clearsAdjoining;
    c.inFlow.marker = markerFor(c, flow);
    LayoutResult result = layoutBlock(state, child, c.inFlow);
    if (layoutResultStopped(result)) {
        stop(c, flow, result.resolution);
        return false;
    }
    if (result.unplaced != 0) {
        if (!collapsing(flow)) {
            // It collapsed through while holding floats, which go at the position of its border box.
            bfc.chain.resolved = layoutMarginChainEnd(bfc.chain, result.start);
            layoutFragmentRelease(result.fragment);
            result = layoutBlock(state, child, c.inFlow);
            PR_ASSERT(result.unplaced == 0 && !layoutResultStopped(result));
        } else {
            flow.unplaced = LayoutLineSides(flow.unplaced | result.unplaced);
            flow.adjoining = LayoutLineSides(flow.adjoining | result.unplaced);
        }
    }
    if (!placeBlock(state, c, flow, result, layoutRelativeOffset(state.styles, box, *c.content))) {
        layoutFragmentRelease(result.fragment);
        return false;
    }
    return true;
}

// Lays out a block-level child that avoids floats, placed beside them (CSS2/#floats), or the anonymous table that wraps
// the run of table parts from `child` through `runLast` if that is not kPrLayoutNodeNone. While the margins of the
// container collapse, they resolve with the block-start margin of the child if the child fits beside the floats
// adjoining them. If it does not fit, they resolve without that margin, and the margin then takes no room, as with
// clearance. Returns false if the layout of the container stops.
PR_NOINLINE bool avoiderChild(LayoutState& state, Container& c, Flow& flow, uint32_t child,
                              const PrLayoutBoxStyle& box, LayoutNodeIndex runLast = kPrLayoutNodeNone) {
    LayoutMode mode = c.content->mode;
    LayoutEdges e = layoutEdgesFromBox(state.styles, layoutLogicalBox(box, mode), c.content->containing.inlineSize);
    LayoutStrut marginStart = layoutStrut(e.margin[LayoutSide_BlockStart]);
    LayoutLineSides adjoining = LayoutLineSides(c.chain.adjoining | flow.adjoining);
    LayoutLineSides clears = layoutClearSides(box, mode.direction);
    // Where the margins before the child end without its own margin, and with it.
    PrLayoutUnit without = nextEdge(c, flow);
    PrLayoutUnit with = nextEdge(c, flow, marginStart);
    PrLayoutUnit top = with;
    bool absorbed = false;          // true if the margins before it resolved without its own, which takes no room
    if (collapsing(flow) && (clears & adjoining) != 0) {
        // The child clears floats adjoining its position, and so has clearance.
        if (!close(c, flow, without))
            return false;
        absorbed = true;
        top = flow.contentTop;
    }
    PrLayoutUnit floor = clears != 0 ? layoutExclusionsClearance(c.containing, clears, with) : kLayoutOffsetNone;
    if (!absorbed && floor != kLayoutOffsetNone && with.raw <= floor.raw) {
        // Its position is not past the floats it clears, so the child has clearance.
        if (collapsing(flow) && !close(c, flow, without))
            return false;
        absorbed = true;
    }
    if (!absorbed && collapsing(flow)) {
        if (c.chain.resolved != kLayoutOffsetNone && c.chain.resolved == without && without != with) {
            // This is a restarted layout, because the first layout found that the child did not fit beside the
            // adjoining floats with its margin.
            if (!close(c, flow, without))
                return false;
            absorbed = true;
        } else {
            // Its margin collapses into the margins of the container, and the child starts there.
            flow.pending += marginStart;
            if (!close(c, flow, with))
                return false;
        }
        top = flow.contentTop;
    }
    if (floor != kLayoutOffsetNone)
        top = layoutUnitMax(top, floor);
    // A child that holds outside markers on its first line is laid out with them, in a space of its own.
    LayoutSpace marked{};
    const LayoutSpace* content = c.content;
    if (!layoutLineExtentIsEmpty(markerFor(c, flow))) {
        marked = *c.content;
        marked.marker = markerFor(c, flow);
        content = &marked;
    }
    LayoutAvoider placed = layoutAvoiderPlace(state, child, runLast, *content, c.containing, top, e.margin);
    if (adjoining != 0 && !absorbed && c.chain.resolved != kLayoutOffsetNone && placed.top != top && without != with) {
        // The child does not fit beside the floats at the position its margin put them, so the margins resolve without
        // its margin.
        layoutFragmentRelease(placed.result.fragment);
        stop(c, flow, without);
        return false;
    }
    PR_ASSERT(flow.edge == Edge_Closed);
    const LayoutResult& result = placed.result;
    LayoutLogicalSize size = layoutLogicalSize(mode, result.fragment->extent);
    PrLayoutUnit offset = placed.top - flow.contentTop;
    LayoutLogicalOffset at{layoutInlineStartFromLeft(c.containing, mode.direction, placed.left,
                                                     size.inlineSize), offset};
    flow.cursor = offset + size.blockSize;
    flow.pending = result.end;
    flow.content = true;
    flow.sealed = false;
    pushChild(state, c, flow, result, at, layoutRelativeOffset(state.styles, box, *c.content));
    return true;
}

// Lays out segment `segment` of `run` as an inline formatting context. It is either alone, as the content of the
// container, or in an anonymous block box among block-level children (css-display-4/#anonymous).
PR_NOINLINE void inlineSegment(LayoutState& state, Container& c, Flow& flow, LayoutInlineRun& run, uint32_t segment,
                               bool alone, bool firstFormatted) {
    PrLayoutUnit end = nextEdge(c, flow);
    LayoutInlineBfc bfc{.containing = c.containing,
                        .top = end,
                        .topFinal = true,
                        .mayResolve = true,
                        .firstFormatted = firstFormatted,
                        .marker = markerFor(c, flow),
                        .reuses = alone && !c.inContext};
    if (flow.edge == Edge_Open) {
        // Its first line would resolve the margins of the container, either there or at the floor it clears.
        bfc.top = layoutMarginChainResolve(c.chain, end, false).top;
        bfc.topFinal = false;
        bfc.mayResolve = (c.chain.adjoining | flow.unplaced) == 0;
    } else if (flow.edge == Edge_Located) {
        bfc.top = flow.contentTop;
    }
    Array<LayoutLogicalChild>& children = state.scratch->children;
    uint32_t mark = children.count;
    LayoutInlineResult lines = layoutInlineSegment(state, run, segment, bfc);
    if (lines.stopped) {
        bool located = locate(c, flow, end);
        PR_ASSERT(!located);
        (void)located;
        return;
    }
    flow.unplaced = LayoutLineSides(flow.unplaced | lines.unplaced);
    if (collapsing(flow))
        flow.adjoining = LayoutLineSides(flow.adjoining | lines.unplaced | lines.placed);
    if (alone) {
        if (lines.hasLines) {
            if (collapsing(flow) && !close(c, flow, end)) {
                PR_ASSERT(false);
                return;
            }
            flow.content = true;
            flow.cursor = lines.blockSize;
            if (lines.hasBaselines) {
                flow.first = lineBaseline(c, c.origin.blockOffset + lines.firstBaseline);
                flow.last = lineBaseline(c, c.origin.blockOffset + lines.lastBaseline);
            }
        }
        for (uint32_t i = mark; i < children.count; ++i)
            children.data[i].offset += c.origin;
        return;
    }
    if (children.count == mark)
        return;
    LayoutFragment* anonymous = layoutFragmentBuild(children, mark, LayoutFragmentKind_Box, kPrLayoutNodeNone,
                                                    c.content->mode,
                                                    LayoutLogicalSize{c.content->containing.inlineSize,
                                                                      lines.blockSize});
    ++state.stats->boxFragmentsMade;
    layoutFragmentSetOverflow(state.tree, anonymous);
    LayoutResult result{};
    result.fragment = anonymous;
    if (lines.hasBaselines) {
        result.flags |= LayoutResultFlag_FirstBaseline | LayoutResultFlag_LastBaseline;
        layoutFragmentSetBaselines(anonymous, layoutBaselineAt(lines.firstBaseline, c.children.font),
                                   layoutBaselineAt(lines.lastBaseline, c.children.font));
    }
    if (!lines.hasLines)
        result.flags |= LayoutResultFlag_CollapsedThrough;
    if (!placeBlock(state, c, flow, result, kLayoutLogicalOffsetZero))
        layoutFragmentRelease(anonymous);
}

// Wraps the block-level box `block`, the last of the scratch children, in fragments for the inline boxes around it up
// to the container (CSS2/#anonymous-block-level), innermost first. Each fragment is a row that spans the content box of
// the container, is as tall as the border box of the block, and paints nothing.
//
// The block keeps its position, and keeps its relative offset within the rows. The rows sit where the border box of the
// block would be without that offset. Each row is moved by the relative offset of its inline box, together with the row
// around it (CSS2/#relative-positioning). Each row reaches as far as what it holds.
PR_NOINLINE void wrapInRows(LayoutState& state, const Container& c, uint32_t block, const PrLayoutBoxStyle& box) {
    Array<LayoutLogicalChild>& children = state.scratch->children;
    LayoutLogicalChild placed = children.last();
    children.pop();
    LayoutMode mode = c.content->mode;
    LayoutLogicalOffset relative = layoutRelativeOffset(state.styles, box, *c.content);
    LayoutLogicalOffset row{c.origin.inlineOffset, placed.offset.blockOffset - relative.blockOffset};
    LayoutLogicalSize size{c.content->containing.inlineSize,
                           layoutLogicalSize(mode, placed.fragment->extent).blockSize};
    LayoutLogicalChild inner{placed.fragment, placed.offset - row};
    for (uint32_t at = layoutTreeNode(state.tree, block).parent; at != c.node;
         at = layoutTreeNode(state.tree, at).parent) {
        children.push(inner);
        LayoutFragment* around = layoutFragmentBuild(children, children.count - 1, LayoutFragmentKind_Around, at, mode,
                                                     size);
        layoutFragmentSetOverflow(state.tree, around);
        const PrLayoutBoxStyle& inlineBox = layoutBoxStyle(state.styles, layoutTreeNode(state.tree, at).style);
        inner = LayoutLogicalChild{around, layoutRelativeOffset(state.styles, inlineBox, *c.content)};
    }
    children.push(LayoutLogicalChild{inner.fragment, row + inner.offset});
}

// Lays out the run of inline-level children from `first` through `last`. Each segment of the run is laid out as an
// inline formatting context. Between segments, the block-level boxes in flow inside its inline boxes are laid out as
// block-level children wrapped in the rows of their inline boxes (CSS2/#anonymous-block-level). A run with one segment
// is alone if `alone` says so.
PR_NOINLINE void inlineChildren(LayoutState& state, Container& c, Flow& flow, uint32_t first, uint32_t last,
                                bool alone) {
    LayoutInlineRun run = layoutInlineBegin(state, c.node, first, last, *c.content);
    uint32_t segments = layoutInlineSegmentCount(run);
    for (uint32_t k = 0; k < segments && flow.edge != Edge_Stopped; ++k) {
        inlineSegment(state, c, flow, run, k, alone && segments == 1, k == 0 && !flow.blocks);
        if (k + 1 == segments || flow.edge == Edge_Stopped)
            continue;
        uint32_t block = layoutInlineBlockAfter(run, k);
        const PrLayoutBoxStyle& box = layoutBoxStyle(state.styles, layoutTreeNode(state.tree, block).style);
        flow.blocks = true;
        bool placed = layoutAvoidsFloats(state.tree, block, box, c.children) ? avoiderChild(state, c, flow, block, box)
                                                                              : flowChild(state, c, flow, block, box);
        if (placed)
            wrapInRows(state, c, block, box);
    }
    layoutInlineEnd(state, run);
}

// Lays out the children of the container and appends their fragments to the scratch children, until all are laid out or
// the layout stops.
void layoutChildren(LayoutState& state, Container& c, Flow& flow) {
    auto blocks = [&](uint32_t child, const PrLayoutBoxStyle& box) {
        if (layoutPositionIsOutOfFlow(box.position)) {
            if (flow.edge != Edge_Stopped)
                outOfFlowChild(state, c, flow, child);
            return;
        }
        if (box.floating == PrLayoutFloat_None)
            flow.blocks = true;
        if (flow.edge == Edge_Stopped)
            return;
        if (box.floating != PrLayoutFloat_None)
            floatChild(state, c, flow, child, box);
        else if (layoutAvoidsFloats(state.tree, child, box, c.children))
            avoiderChild(state, c, flow, child, box);
        else
            flowChild(state, c, flow, child, box);
    };
    auto inlines = [&](uint32_t first, uint32_t last, bool alone) {
        if (flow.edge != Edge_Stopped)
            inlineChildren(state, c, flow, first, last, alone);
    };
    // An anonymous table is a block-level box that avoids floats.
    auto tables = [&](uint32_t first, uint32_t last) {
        flow.blocks = true;
        if (flow.edge != Edge_Stopped)
            avoiderChild(state, c, flow, first, layoutAnonymousTableStyle(), last);
    };
    if (c.runFirst != kPrLayoutNodeNone)
        layoutEachChildIn(state.tree, c.runFirst, c.runLast, blocks, inlines, tables);
    else
        layoutEachChild(state.tree, c.node, blocks, inlines, tables);
}

// ---------------------------------------------------------------------------------------------------------------------
// Outside markers

// The outside marker of a list item, laid out apart from the item's flow (PrLayoutDisplay_Marker).
struct Marker {
    const LayoutFragment* PR_NULLABLE fragment; // a reference that the item holds; null if it has no marker
    PrLayoutUnit baseline;                      // its alphabetic baseline from the block-start edge of its border box
    PrLayoutUnit inlineEnd;                     // its inline-end margin
    LayoutLineExtent extent;                    // its margin box about that baseline, in the frame of a line
};

// Lays out the outside marker `node` of the container in the content box of the container, as an atomic inline is laid
// out. A marker with no baseline has the line-under edge of its margin box as its baseline (CSS2/#leading).
PR_NOINLINE Marker layOutMarker(LayoutState& state, const Container& c, LayoutNodeIndex node) {
    LayoutMode mode = c.content->mode;
    const PrLayoutBoxStyle& box = layoutBoxStyle(state.styles, layoutTreeNode(state.tree, node).style);
    LayoutEdges e = layoutEdgesFromBox(state.styles, layoutLogicalBox(box, mode), c.content->containing.inlineSize);
    LayoutResult result = layoutBlock(state, node, *c.content);
    PrLayoutUnit blockSize = layoutLogicalSize(mode, result.fragment->extent).blockSize;
    bool overIsStart = layoutLineOverIsBlockStart(mode);
    LayoutBaseline first = layoutFragmentBaseline(result.fragment, LayoutBaselineEdge_First);
    Marker marker{.fragment = result.fragment, .inlineEnd = e.margin[LayoutSide_InlineEnd]};
    if (first.present)
        marker.baseline = first.offset;
    else
        marker.baseline = overIsStart ? blockSize + e.margin[LayoutSide_BlockEnd]
                                      : kLayoutUnitZero - e.margin[LayoutSide_BlockStart];
    PrLayoutUnit before = marker.baseline + e.margin[LayoutSide_BlockStart];
    PrLayoutUnit after = blockSize - marker.baseline + e.margin[LayoutSide_BlockEnd];
    marker.extent = overIsStart ? LayoutLineExtent{before, after} : LayoutLineExtent{after, before};
    return marker;
}

// Returns the extent of a line that holds only the outside markers of the container: the strut of its text, with the
// markers aligned on its alphabetic baseline, as layoutLines aligns them. Sets `*alphabetic` to that baseline from the
// block-start edge of the line.
LayoutLineExtent markerLineOf(LayoutState& state, const Container& c, PrLayoutUnit* alphabetic) {
    PrLayoutStyleIndex style = layoutTreeNode(state.tree, c.node).style;
    LayoutLineExtent extent = layoutTextExtent(state, style);
    PrLayoutUnit dominant = layoutDominantOffset(state, style);
    layoutLineExtend(extent, LayoutLineExtent{c.marker.above + dominant, c.marker.below - dominant});
    *alphabetic = layoutLineOverIsBlockStart(c.content->mode) ? extent.above - dominant : extent.below + dominant;
    return extent;
}

// Lays out a line for the outside markers of a list item that has no in-flow content: no block-level box in flow, and
// no line. The line resolves the margins before it, as any line does, and gives the flow its baselines. Its floats stay
// where they are. The layout of the container may stop where the margins resolve.
PR_NOINLINE void markerLine(LayoutState& state, const Container& c, Flow& flow) {
    if (collapsing(flow) && !close(c, flow, nextEdge(c, flow)))
        return;
    PrLayoutUnit alphabetic = kLayoutUnitZero;
    LayoutLineExtent extent = markerLineOf(state, c, &alphabetic);
    PrLayoutUnit top = flow.cursor + layoutStrutSize(flow.pending);
    flow.pending = LayoutStrut{};
    flow.cursor = top + extent.above + extent.below;
    flow.content = true;
    flow.first = lineBaseline(c, c.origin.blockOffset + top + alphabetic);
    flow.last = flow.first;
}

// Puts the outside marker of the container among the scratch children from `mark`, first, with its baseline on `first`,
// the first baseline of the container's flow. A container with no first baseline puts it as if on a line at the
// block-start edge of its content box.
//
// Its inline-end margin edge is at the inline-start edge of the border box, moved by as far as the floats placed before
// the container, the first `floats` of the context's, push a line at the marker past the content box's start. A list
// beside a float so keeps its markers beside its lines. The floats in the container do not move the marker.
void placeMarker(LayoutState& state, const Container& c, const Flow& flow, const Marker& marker,
                 const LayoutBaseline& first, uint32_t floats, uint32_t mark) {
    PrLayoutUnit baseline = first.offset;
    if (!first.present) {
        PrLayoutUnit alphabetic = kLayoutUnitZero;
        markerLineOf(state, c, &alphabetic);
        baseline = c.origin.blockOffset + alphabetic;
    }
    LayoutMode mode = c.content->mode;
    LayoutLogicalSize size = layoutLogicalSize(mode, marker.fragment->extent);
    LayoutLogicalOffset at{kLayoutUnitZero - size.inlineSize - marker.inlineEnd, baseline - marker.baseline};
    if (c.inContext && flow.edge != Edge_Open && floats != 0) {
        LayoutSpan span =
            layoutExclusionsAvailableAmong(c.containing, flow.top + at.blockOffset, size.blockSize, floats);
        PrLayoutUnit moved = mode.direction == PrTextDirection_Ltr ? span.left - c.containing.lineLeft
                                                                   : c.containing.lineRight - span.right;
        at.inlineOffset += layoutUnitMax(kLayoutUnitZero, moved);
    }
    Array<LayoutLogicalChild>& children = state.scratch->children;
    children.push(LayoutLogicalChild{});
    for (uint32_t i = children.count - 1; i > mark; --i)
        children.data[i] = children.data[i - 1];
    children.data[mark] = LayoutLogicalChild{marker.fragment, at};
}

// The end of the content of a container, once its children are laid out.
struct Ending {
    PrLayoutUnit contentBlock;      // the end of its in-flow content in its content box
    PrLayoutUnit blockSize;         // the block size of its border box, clamped
    bool collapsedThrough;
};

// Ends the content of the container. The pending margins either collapse through its block-end edge or stay inside. A
// context root grows to contain its floats (CSS2/#root-height). The shell's clamp then gives the block size
// (layoutContentBlockSize). If its margins still collapse, they resolve at its end, unless it collapses through, and
// its layout may stop there.
Ending endFlow(const LayoutState& state, const LayoutFlowInput& in, const Container& c, Flow& flow) {
    const LayoutBlockAxis& axis = in.axis;
    Ending ending{};
    ending.contentBlock = flow.cursor;
    if (flow.content && c.endCollapses && !flow.sealed) {
        // The margins pending collapse through its block-end edge.
    } else if (!flow.content && c.startCollapses) {
        // The margins of the children it holds collapse through its block-start edge.
        flow.escaped += flow.pending;
        flow.pending = LayoutStrut{};
    } else {
        ending.contentBlock += layoutStrutSize(flow.pending);
    }
    bool fixed = layoutIsDefinite(axis.size);
    PrLayoutUnit lowest = c.containing.exclusions->mark.lowestAll;
    if (!c.inContext && (!fixed || axis.growsToContent) && lowest != kLayoutOffsetNone)
        ending.contentBlock = layoutUnitMax(ending.contentBlock, lowest);
    // A size from the box's aspect ratio grows to its content, which then ends before the margins that would collapse
    // through its block-end edge if its block size were auto.
    PrLayoutUnit contentSize = ending.contentBlock + axis.edges;
    PrLayoutUnit grown = c.contentEndCollapses ? flow.cursor + axis.edges : contentSize;
    ending.blockSize = layoutContentBlockSize(state.styles, *in.box, *in.sizing, axis, in.space->percentBlock,
                                              in.clamps, contentSize, grown);
    ending.collapsedThrough =
        !flow.content && c.startCollapses && c.endCollapses && ending.blockSize == kLayoutUnitZero;
    PrLayoutUnit end = layoutMarginChainEnd(c.chain, flow.escaped);
    if (ending.collapsedThrough) {
        // A box that collapses through and has clearance has its border box at the floor.
        if (c.clears && flow.edge == Edge_Open && layoutMarginChainResolve(c.chain, end, false).cleared)
            locate(c, flow, end);
    } else if (collapsing(flow)) {
        // The end of a box that does not collapse through resolves the margins that collapse with its own.
        if (locate(c, flow, end))
            flow.edge = Edge_Closed;
    }
    return ending;
}

// Returns a baseline of the flow with its set, read from the child that gave it, or from the flow's own record.
LayoutBaseline setOf(const FlowBaseline& flow, LayoutBaselineEdge edge) {
    if (flow.child != nullptr)
        return layoutBaselineMoved(layoutFragmentBaseline(flow.child, edge), flow.at);
    if (!flow.present)
        return LayoutBaseline{.source = kLayoutBaselineSynthesized};
    return layoutBaselineAt(flow.at, flow.source);
}

// Returns how far the content of the block container of `c` moves along its block axis. The content box is `free`
// larger than the content, whose block-end edge is at `contentEnd` from the block-start edge of the border box.
//
// A table cell moves its content by its vertical-align (css-tables-3/#row-layout). Bottom moves the content down by all
// of the free space, and middle by half of it. Baseline moves the content so that the cell's first baseline, `first`,
// lies on the baseline that the cell's row gives it. A cell that has no first baseline aligns the block-end edge of its
// content instead. An empty cell is not moved, because its table gives it no row baseline (layoutTableContent).
//
// Any other block container moves its content by its align-content (css-align-3/#align-block), but only if it
// establishes its own formatting context. A distributed alignment falls back to its single-subject alignment, and a
// baseline alignment falls back to start. Content that does not fit is not moved.
PR_NOINLINE PrLayoutUnit contentShift(const LayoutState& state, const Container& c, const LayoutSpace& space,
                                      const PrLayoutBoxStyle& box, PrLayoutUnit free, const LayoutBaseline& first,
                                      PrLayoutUnit contentEnd) {
    if (box.display == PrLayoutDisplay_TableCell) {
        switch (layoutCellAlignFromBox(box)) {
            case LayoutCellAlign_Middle: return layoutUnitMax(kLayoutUnitZero, free / 2);
            case LayoutCellAlign_Bottom: return layoutUnitMax(kLayoutUnitZero, free);
            case LayoutCellAlign_Baseline: {
                if (space.cell == nullptr || !space.cell->aligned)
                    return kLayoutUnitZero;
                PrLayoutUnit own = first.present ? first.offset : contentEnd;
                return layoutUnitMax(kLayoutUnitZero, space.cell->baseline - own);
            }
            default: return kLayoutUnitZero;
        }
    }
    // A box in its parent's context has placed its floats there, where its content would leave them behind.
    if (free <= kLayoutUnitZero || c.runFirst != kPrLayoutNodeNone || c.inContext)
        return kLayoutUnitZero;
    const PrLayoutFlexStyle& flex = layoutFlexStyle(state.styles, layoutTreeNode(state.tree, c.node).style);
    switch (PrLayoutAlignment(flex.alignContent & kPrLayoutAlignmentValueMask)) {
        case PrLayoutAlignment_Center:
        case PrLayoutAlignment_SpaceAround:
        case PrLayoutAlignment_SpaceEvenly: return free / 2;
        case PrLayoutAlignment_End:
        case PrLayoutAlignment_FlexEnd: return free;
        default: return kLayoutUnitZero;
    }
}

} // namespace

LayoutFlowOutcome layoutFlowContent(LayoutState& state, const LayoutFlowInput& input) {
    const LayoutSpace& space = *input.space;
    const LayoutEdges& e = *input.outer;
    const LayoutEdges& inner = *input.inner;
    Container c{};
    c.node = input.node;
    c.runFirst = input.runFirst;
    c.runLast = input.runLast;
    c.children = input.children;
    c.content = input.content;
    c.edgeStart = layoutEdgesContent(inner, LayoutSide_BlockStart);
    c.origin = input.origin;
    PrLayoutUnit contentInline = c.content->containing.inlineSize;

    // A box that establishes a block formatting context keeps the margins of its children and its floats inside it
    // (CSS2/#block-formatting). For a box in flow in the context of its parent, the margins of its children collapse
    // with its own.
    PR_ASSERT((space.bfc != nullptr) == (input.reads != nullptr));
    LayoutExclusions exclusions;
    layoutExclusionsInit(exclusions);
    LayoutReads ownReads = layoutReadsBegin(exclusions);
    c.inContext = space.bfc != nullptr;
    if (c.inContext) {
        const LayoutBfc& bfc = *space.bfc;
        PrLayoutUnit left = layoutLineLeftFromInlineStart(bfc.containing, space.mode.direction,
                                                          e.margin[LayoutSide_InlineStart], input.inlineSize);
        PrLayoutUnit lineLeftEdge = layoutEdgesContent(inner, layoutLineLeft(c.content->mode));
        c.containing = LayoutContainingBlock{bfc.containing.exclusions, input.reads, left + lineLeftEdge,
                                             left + lineLeftEdge + contentInline};
        c.chain = bfc.chain;
        c.chain.strut += layoutStrut(e.margin[LayoutSide_BlockStart]);
        c.clears = input.box->clear != PrLayoutClear_None;
        c.startCollapses = c.edgeStart == kLayoutUnitZero;
        c.contentEndCollapses = layoutEdgesContent(inner, LayoutSide_BlockEnd) == kLayoutUnitZero;
        c.endCollapses = c.contentEndCollapses && !layoutIsDefinite(input.axis.size);
    } else {
        c.containing = LayoutContainingBlock{&exclusions, &ownReads, kLayoutUnitZero, contentInline};
    }
    c.child.containing = c.containing;
    c.inFlow = *c.content;
    c.inFlow.bfc = &c.child;

    // A list item lays out its outside marker apart from its flow, and gives the marker to the first line of its flow,
    // along with the markers of its space.
    c.marker = space.marker;
    Marker marker{};
    if (input.marked) {
        LayoutNodeIndex markerNode = layoutNodeMarker(state.tree, input.node);
        if (markerNode != kPrLayoutNodeNone) {
            marker = layOutMarker(state, c, markerNode);
            layoutLineExtend(c.marker, marker.extent);
        }
    }

    // Lays out the children. If floats wait for their margins, they are laid out again once the margins resolve.
    Array<LayoutLogicalChild>& children = state.scratch->children;
    uint32_t mark = children.count;
    LayoutExclusionMark exclusionMark = c.containing.exclusions->mark;
    Flow flow{};
    Ending ending{};
    for (uint32_t attempt = 0;; ++attempt) {
        flow = startFlow(c);
        if (flow.edge != Edge_Stopped)
            layoutChildren(state, c, flow);
        if (flow.edge != Edge_Stopped && marker.fragment != nullptr && !flow.first.present && !flow.blocks)
            markerLine(state, c, flow);
        if (flow.edge != Edge_Stopped)
            ending = endFlow(state, input, c, flow);
        if (flow.edge != Edge_Stopped || !flow.restart)
            break;
        layoutFragmentDiscard(children, mark);
        layoutExclusionsCut(*c.containing.exclusions, exclusionMark);
        *c.containing.reads = layoutReadsBegin(*c.containing.exclusions);
        c.chain.resolved = flow.stopAt;
        ++state.stats->blocksRestarted;
        PR_ASSERT(attempt < 8);
    }
    layoutExclusionsReset(exclusions);
    LayoutFlowOutcome outcome{};
    outcome.top = kLayoutOffsetNone;
    outcome.stopAt = kLayoutOffsetNone;
    if (flow.edge == Edge_Stopped) {
        layoutFragmentRelease(marker.fragment);
        layoutFragmentDiscard(children, mark);
        layoutExclusionsCut(*c.containing.exclusions, exclusionMark);
        outcome.stopAt = flow.stopAt;
        return outcome;
    }
    outcome.first = setOf(flow.first, LayoutBaselineEdge_First);
    outcome.last = setOf(flow.last, LayoutBaselineEdge_Last);
    // Content in a block size that its style or its container fixes is aligned in it.
    if (input.fixedBlock) {
        PrLayoutUnit free = ending.blockSize - input.axis.edges - ending.contentBlock;
        outcome.shift = contentShift(state, c, space, *input.box, free, outcome.first,
                                     c.origin.blockOffset + ending.contentBlock);
    }
    if (outcome.shift != kLayoutUnitZero) {
        for (uint32_t i = mark; i < children.count; ++i)
            children.data[i].offset.blockOffset += outcome.shift;
        if (outcome.first.present)
            outcome.first = layoutBaselineMoved(outcome.first, outcome.shift);
        if (outcome.last.present)
            outcome.last = layoutBaselineMoved(outcome.last, outcome.shift);
    }
    // The marker is among the children that the fragment is built from.
    if (marker.fragment != nullptr)
        placeMarker(state, c, flow, marker, outcome.first, exclusionMark.count, mark);
    outcome.contentBlock = ending.contentBlock;
    outcome.blockSize = ending.blockSize;
    outcome.collapsedThrough = ending.collapsedThrough;
    outcome.start = flow.escaped;
    if (flow.content && c.endCollapses && !flow.sealed)
        outcome.end = flow.pending;
    if (c.inContext && flow.edge != Edge_Open) {
        outcome.top = flow.top;
        outcome.cleared = flow.cleared;
    }
    if (ending.collapsedThrough && flow.edge == Edge_Open)
        outcome.unplaced = flow.unplaced;
    return outcome;
}

} // namespace pr_impl
