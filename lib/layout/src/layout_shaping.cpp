// Shaping: an inline formatting context's items, and the parts of its text that do not depend on its width: its text
// with white space collapsed and transformed, its break opportunities and its glyphs.

#include "layout/src/layout_shaping.h"

#include "pr/alloc.h"
#include "pr/mem.h"
#include "pr/utf8.h"

#include "layout/src/layout_box.h"
#include "layout/src/layout_itemize.h"
#include "layout/src/layout_table.h"

namespace pr_impl {

namespace {

// ---------------------------------------------------------------------------------------------------------------------
// Collecting

// Appends a byte of collected text, what it is as white space, and the offset in its node's text that it comes from.
void pushByte(LayoutInlineShaping& t, char c, PrTextSpaceFlags spaces, uint32_t source = 0) {
    t.text.push(c);
    t.spaces.push(spaces);
    t.sources.push(source);
}

// Returns the offset in the text at which the segment being collected starts. White space collapsing does not reach
// before it.
uint32_t segmentStart(const LayoutInlineShaping& t) {
    return t.segments.last().byteStart;
}

// Removes a collapsible space at the end of the segment so far, along with the items that then end past the text
// (css-text-3/#white-space-phase-1: a collapsible space before a preserved segment break).
void dropCollapsibleSpace(LayoutInlineShaping& t) {
    if (t.text.count == segmentStart(t) || (t.spaces.last() & PrTextSpaceFlag_Collapsible) == 0)
        return;
    uint32_t end = t.text.count - 1;
    t.text.pop();
    t.spaces.pop();
    t.sources.pop();
    for (uint32_t i = t.items.count; i-- > 0;) {
        LayoutInlineItem& item = t.items.data[i];
        if (item.start <= end && item.end <= end)
            break;
        item.start = item.start < end ? item.start : end;
        item.end = item.end < end ? item.end : end;
    }
    while (t.items.count != 0 && t.items.last().kind == LayoutInlineItemKind_Text &&
            t.items.last().start == t.items.last().end)
        t.items.pop();
}

// Appends the bytes of text styled by `font`, with its white space collapsed or preserved as the style says
// (css-text-4/#white-space-phase-1). `space` says whether the text so far ends in a collapsible space, after which a
// following collapsible space is removed, and is set if the text now does. A carriage return is a space
// (css-text-3/#white-space-processing).
void collectText(LayoutInlineShaping& t, PrStringView text, const PrLayoutFontStyle& font, bool& space) {
    PrLayoutWhiteSpaceCollapse collapse = font.whiteSpaceCollapse;
    bool discard = collapse == PrLayoutWhiteSpaceCollapse_Discard;
    bool keepSpaces = collapse == PrLayoutWhiteSpaceCollapse_Preserve ||
        collapse == PrLayoutWhiteSpaceCollapse_PreserveSpaces || collapse == PrLayoutWhiteSpaceCollapse_BreakSpaces;
    bool keepBreaks = collapse == PrLayoutWhiteSpaceCollapse_Preserve ||
        collapse == PrLayoutWhiteSpaceCollapse_PreserveBreaks || collapse == PrLayoutWhiteSpaceCollapse_BreakSpaces;
    // Preserved spaces at the end of a line hang if the line may wrap, except with break-spaces, which wraps after
    // them.
    bool wraps = font.textWrapMode == PrLayoutTextWrapMode_Wrap;
    PrTextSpaceFlags wrapping = wraps ? PrTextSpaceFlag_Wraps : PrTextSpaceFlags(0);
    PrTextSpaceFlags preserved =
        PrTextSpaceFlags(wrapping | (wraps && collapse != PrLayoutWhiteSpaceCollapse_BreakSpaces ? PrTextSpaceFlag_Hangs : 0));
    for (size_t i = 0; i < text.size; ++i) {
        char c = text.data[i] == '\r' || text.data[i] == '\f' ? ' ' : text.data[i];
        uint32_t source = uint32_t(i);
        if (c == '\n' && keepBreaks) {
            // Collapsible spaces around a preserved segment break are removed.
            dropCollapsibleSpace(t);
            pushByte(t, '\n', 0, source);
            space = true;
        } else if (c == ' ' || c == '\t' || c == '\n') {
            if (discard)
                continue;
            if (keepSpaces) {
                // A segment break that is not preserved becomes a space here, and that space is preserved.
                bool tab = c == '\t';
                pushByte(t, tab ? '\t' : ' ', PrTextSpaceFlags(preserved | (tab ? PrTextSpaceFlag_Tab : 0)), source);
                t.tabs = t.tabs || tab;
                space = false;
            } else if (!space) {
                pushByte(t, ' ', PrTextSpaceFlag_Collapsible | wrapping, source);
                space = true;
            } else if (t.spaces.count > segmentStart(t)) {
                // A space collapsed into the space before it shares that space's opportunity.
                t.spaces.last() = PrTextSpaceFlags(t.spaces.last() | wrapping);
            }
        } else {
            pushByte(t, c, 0, source);
            space = false;
        }
    }
}

// Returns the font group of the record `style`.
uint32_t fontOf(const LayoutStyles* styles, PrLayoutStyleIndex style) {
    return layoutStyle(styles, style).groups[PrLayoutGroup_Font];
}

// Returns the font group that an item of `node` is set in, in the records of `t`: those of the first formatted line if
// `t` is lined (layoutNodeLineStyle), and the nodes' own otherwise.
uint32_t itemFontOf(const LayoutState& state, const LayoutInlineShaping& t, LayoutNodeIndex node) {
    return fontOf(state.styles, t.lined ? layoutNodeLineStyle(state.tree, node) : layoutNodeStyle(state.tree, node));
}

// The text that stands for an atomic inline, U+FFFC OBJECT REPLACEMENT CHARACTER, around which lines break as UAX14
// says.
constexpr char kAtomicText[] = "\xEF\xBF\xBC";

// Returns true if `node`, a node of an inline formatting context, is laid out by its own cache rather than collected
// into the context's items: a float, a block-level box in flow, a box out of flow, an atomic inline, or a table part,
// which the anonymous inline table around it lays out.
bool laidOutApart(const LayoutState& state, LayoutNodeIndex node) {
    LayoutChildKind kind = layoutChildKind(state.tree, node);
    return kind == LayoutChildKind_Float || layoutChildKindIsBlock(kind) || kind == LayoutChildKind_OutOfFlow ||
        (kind == LayoutChildKind_Inline && layoutIsAtomicInline(state.tree, node));
}

// Returns the item kind that a node laid out apart is collected as.
LayoutInlineItemKind apartKind(const LayoutState& state, LayoutNodeIndex node) {
    switch (layoutChildKind(state.tree, node)) {
        case LayoutChildKind_Float: return LayoutInlineItemKind_Float;
        case LayoutChildKind_Block: return LayoutInlineItemKind_Block;
        case LayoutChildKind_OutOfFlow: return LayoutInlineItemKind_OutOfFlow;
        default: return LayoutInlineItemKind_Atomic;
    }
}

// Returns true if the layout of `node`, a node of an inline formatting context, reads and clears its own change marks,
// because it is laid out apart by its own cache.
bool keepsMarks(const LayoutState& state, LayoutNodeIndex node) {
    return layoutTreeNode(state.tree, node).kind != PrLayoutNodeKind_Text && laidOutApart(state, node);
}

// Returns true if the edge of the inline box styled by `style` at `side`, in its own writing mode `mode`, interrupts
// shaping between the text on either side of it (css-text-3/#boundary-shaping). This is the case if the box has a
// non-zero inline-axis margin, border or padding there, as specified, has a vertical alignment other than the initial
// one, or isolates its content.
bool breaksShaping(const LayoutStyles* styles, PrLayoutStyleIndex style, LayoutMode mode, LayoutSide side) {
    const PrLayoutBoxStyle& box = layoutBoxStyle(styles, style);
    LayoutLogicalBox logical = layoutLogicalBox(box, mode);
    auto nonZero = [](PrLayoutLength length) {
        return length.bits != prLayoutLengthPx(0).bits && length.bits != prLayoutLengthPercent(0).bits;
    };
    bool aligned = box.alignmentBaseline == PrLayoutAlignmentBaseline_Baseline &&
        box.baselineShift.bits == prLayoutBaselineShiftPx(0).bits;
    bool isolates = box.unicodeBidi == PrTextBidi_Isolate || box.unicodeBidi == PrTextBidi_IsolateOverride ||
        box.unicodeBidi == PrTextBidi_Plaintext;
    return nonZero(logical.margin[side]) || logical.border[side] != kLayoutUnitZero || nonZero(logical.padding[side]) ||
        !aligned || isolates;
}

// Appends the items of `node` and its descendants, collapsing white space as each style says (collectText), and leaves
// the edges of the boxes to refreshItems. `space` says whether the text so far ends in a collapsible space, after which
// a following space is removed. Returns the last sibling that it collected, which is `node` unless `node` starts a run
// of table parts.
LayoutNodeIndex collect(LayoutState& state, LayoutInlineShaping& t, LayoutNodeIndex node, bool& space) {
    const LayoutTreeNode& n = layoutTreeNode(state.tree, node);
    // The context is laid out whole, so what changed in it is laid out now. The exception is a node laid out apart,
    // whose own layout reads its marks.
    auto take = [&](LayoutNodeIndex taken) {
        if (state.reuse && !keepsMarks(state, taken))
            state.tree->nodes.data[taken].dirty = 0;
        t.nodes.push(taken);
    };
    take(node);
    if (n.kind == PrLayoutNodeKind_LineBreak) {
        if (layoutBoxStyle(state.styles, n.style).display == PrLayoutDisplay_None)
            return node;
        // A line feed. Collapsible spaces around it are removed, as around a preserved segment break.
        dropCollapsibleSpace(t);
        LayoutInlineItem item{};
        item.kind = LayoutInlineItemKind_Break;
        item.node = node;
        item.font = itemFontOf(state, t, node);
        item.start = t.text.count;
        pushByte(t, '\n', 0);
        item.end = t.text.count;
        t.items.push(item);
        space = true;
        return node;
    }
    if (n.kind == PrLayoutNodeKind_Text) {
        PrStringView text = layoutNodeText(state.tree, node);
        LayoutInlineItem item{};
        item.kind = LayoutInlineItemKind_Text;
        item.node = node;
        PrLayoutStyleIndex style = layoutNodeStyle(state.tree, node);
        item.font = itemFontOf(state, t, node);
        item.start = t.text.count;
        collectText(t, text, layoutFontStyle(state.styles, style), space);
        item.end = t.text.count;
        if (item.end != item.start)
            t.items.push(item);
        return node;
    }
    LayoutChildKind kind = layoutChildKind(state.tree, node);
    if (kind == LayoutChildKind_None)
        return node;
    // A run of table parts in an inline box is wrapped in an anonymous inline table, an atomic inline. The white space
    // between its parts is in the table, not in the text.
    if (kind == LayoutChildKind_TablePart) {
        LayoutInlineItem atomic{};
        atomic.kind = LayoutInlineItemKind_Atomic;
        atomic.wraps = true;
        atomic.node = node;
        atomic.runLast = layoutTablePartRunLast(state.tree, node, kPrLayoutNodeNone);
        atomic.font = itemFontOf(state, t, n.parent);
        atomic.start = t.text.count;
        for (uint32_t i = 0; i + 1 < sizeof(kAtomicText); ++i)
            pushByte(t, kAtomicText[i], 0);
        atomic.end = t.text.count;
        t.items.push(atomic);
        for (LayoutNodeIndex c = node; c != atomic.runLast;) {
            c = layoutTreeNode(state.tree, c).next;
            take(c);
        }
        space = false;
        return atomic.runLast;
    }
    // A block-level box ends the segment, and the next segment starts after it, with its leading collapsible spaces
    // removed as at the start of a line.
    if (layoutChildKindIsBlock(kind)) {
        dropCollapsibleSpace(t);
        LayoutInlineSegment& before = t.segments.last();
        before.byteEnd = t.text.count;
        before.itemEnd = t.items.count;
        LayoutInlineItem item{};
        item.kind = LayoutInlineItemKind_Block;
        item.node = node;
        item.font = itemFontOf(state, t, node);
        item.start = t.text.count;
        item.end = t.text.count;
        t.items.push(item);
        t.segments.push(LayoutInlineSegment{.byteStart = t.text.count, .itemStart = t.items.count});
        space = true;
        return node;
    }
    // A float or a box out of flow adds no text, and the spaces around it collapse as though it were not there, in the
    // same way as around an empty inline box.
    if (kind == LayoutChildKind_Float || kind == LayoutChildKind_OutOfFlow) {
        LayoutInlineItem item{};
        item.kind = kind == LayoutChildKind_Float ? LayoutInlineItemKind_Float : LayoutInlineItemKind_OutOfFlow;
        item.node = node;
        item.font = itemFontOf(state, t, node);
        item.start = t.text.count;
        item.end = t.text.count;
        t.items.push(item);
        return node;
    }
    if (layoutIsAtomicInline(state.tree, node)) {
        LayoutInlineItem atomic{};
        atomic.kind = LayoutInlineItemKind_Atomic;
        atomic.node = node;
        if (n.kind == PrLayoutNodeKind_Replaced)
            atomic.unicodeBidi = layoutBoxStyle(state.styles, n.style).unicodeBidi;
        atomic.font = itemFontOf(state, t, node);
        atomic.start = t.text.count;
        for (uint32_t i = 0; i + 1 < sizeof(kAtomicText); ++i)
            pushByte(t, kAtomicText[i], 0);
        atomic.end = t.text.count;
        t.items.push(atomic);
        space = false;
        return node;
    }
    LayoutMode mode = layoutOwnMode(state.tree, node);
    LayoutInlineItem open{};
    open.kind = LayoutInlineItemKind_Open;
    open.node = node;
    open.font = itemFontOf(state, t, node);
    open.start = t.text.count;
    open.breaksShaping = breaksShaping(state.styles, n.style, mode, LayoutSide_InlineStart);
    open.unicodeBidi = layoutBoxStyle(state.styles, n.style).unicodeBidi;
    t.items.push(open);
    for (uint32_t c = n.first; c != kPrLayoutNodeNone; c = layoutTreeNode(state.tree, c).next)
        c = collect(state, t, c, space);
    LayoutInlineItem close = open;
    close.kind = LayoutInlineItemKind_Close;
    close.start = t.text.count;
    close.breaksShaping = breaksShaping(state.styles, n.style, mode, LayoutSide_InlineEnd);
    t.items.push(close);
    return node;
}

// Returns the font group of the record of the container that holds `first`. The text runs directly in the container are
// set in this group.
uint32_t containerFontOf(const LayoutState& state, uint32_t first) {
    uint32_t container = layoutTreeNode(state.tree, first).parent;
    return container != kPrLayoutNodeNone ? fontOf(state.styles, layoutTreeNode(state.tree, container).style) : 0;
}

// Returns the font group of the record of the container that holds `first` for the first formatted line, or of its own
// record if it has none for that line.
uint32_t containerLineFontOf(const LayoutState& state, uint32_t first) {
    uint32_t container = layoutTreeNode(state.tree, first).parent;
    return container != kPrLayoutNodeNone ? fontOf(state.styles, layoutNodeLineStyle(state.tree, container)) : 0;
}

// Returns the unicode-bidi of the container that holds `first`, which its paragraphs' levels read.
PrTextBidi containerBidiOf(const LayoutState& state, uint32_t first) {
    uint32_t container = layoutTreeNode(state.tree, first).parent;
    if (container == kPrLayoutNodeNone)
        return PrTextBidi_Normal;
    return layoutBoxStyle(state.styles, layoutTreeNode(state.tree, container).style).unicodeBidi;
}

// Releases the font groups `t` holds.
void releaseFonts(LayoutInlineShaping& t) {
    for (uint32_t i = 0; i < t.fonts.count; ++i)
        layoutFontRelease(t.styles, t.fonts.data[i]);
    t.fonts.clear();
}

// Holds a reference to each font group that `t` names, replacing the references it held.
void holdFonts(LayoutStyles* styles, LayoutInlineShaping& t) {
    releaseFonts(t);
    t.styles = styles;
    auto hold = [&](PrLayoutGroupIndex font) {
        if (t.fonts.find(font) != kArrayMissing)
            return;
        layoutFontRetain(styles, font);
        t.fonts.push(font);
    };
    hold(t.containerFont);
    for (uint32_t i = 0; i < t.items.count; ++i)
        hold(t.items.data[i].font);
}

// Returns the text-transform of the text item `item`, and the locale of its text, from its own record.
PrTextTransform transformOf(const LayoutState& state, const LayoutInlineItem& item, PrLocale* locale) {
    const PrLayoutFontStyle& font = layoutFontStyle(state.styles, layoutNodeStyle(state.tree, item.node));
    *locale = font.font != nullptr ? prTextFontLocale(font.font) : 0;
    return font.textTransform;
}

// Transforms the collected text of each text item that has a text-transform, after white space collapsing and before
// trimming and shaping (css-text-4/#text-transform-order), and moves the items and segments to the bytes they hold
// after it. The transforms read the text of the item's segment around it as context, so a word continues across inline
// boxes. A byte that a transform changes is no longer white space, so a space made full-width neither collapses nor
// hangs.
void transformText(const LayoutState& state, LayoutInlineShaping& t) {
    uint32_t firstItem = 0;
    PrLocale locale = 0;
    while (firstItem < t.items.count && (t.items.data[firstItem].kind != LayoutInlineItemKind_Text ||
            transformOf(state, t.items.data[firstItem], &locale) == PrTextTransform_None))
        ++firstItem;
    if (firstItem == t.items.count)
        return;
    Array<char> text = t.text;
    Array<PrTextSpaceFlags> spaces = t.spaces;
    Array<uint32_t> sources = t.sources;
    t.text = Array<char>{};
    t.spaces = Array<PrTextSpaceFlags>{};
    t.sources = Array<uint32_t>{};
    t.text.reserve(text.count);
    t.spaces.reserve(text.count);
    t.sources.reserve(text.count);
    // By byte of the text before the transforms, and one past the last: the byte it moved to.
    Array<uint32_t> moved{};
    moved.resize(text.count + 1);
    uint32_t copied = 0;
    auto copyTo = [&](uint32_t end) {
        for (; copied < end; ++copied) {
            moved.data[copied] = t.text.count;
            t.text.push(text.data[copied]);
            t.spaces.push(spaces.data[copied]);
            t.sources.push(sources.data[copied]);
        }
    };
    for (uint32_t s = 0; s < t.segments.count; ++s) {
        const LayoutInlineSegment& segment = t.segments.data[s];
        PrStringView context{text.data + segment.byteStart, segment.byteEnd - segment.byteStart};
        for (uint32_t i = segment.itemStart > firstItem ? segment.itemStart : firstItem; i < segment.itemEnd; ++i) {
            const LayoutInlineItem& item = t.items.data[i];
            if (item.kind != LayoutInlineItemKind_Text)
                continue;
            PrTextTransformRun run{
                .text = context,
                .start = item.start - segment.byteStart,
                .end = item.end - segment.byteStart,
                .transform = transformOf(state, item, &locale),
                .locale = locale,
            };
            if (run.transform == PrTextTransform_None)
                continue;
            copyTo(item.start);
            for (size_t at = run.start; at < run.end;) {
                uint32_t codePoint = 0;
                uint32_t length = prUtf8Decode(context, at, &codePoint);
                uint32_t mapped[kPrTextTransformMaxCodePoints];
                uint32_t count = prTextTransformAt(&run, at, mapped);
                if (count == 1 && mapped[0] == codePoint) {
                    copyTo(copied + length);
                } else {
                    for (uint32_t k = 0; k < length; ++k)
                        moved.data[copied + k] = t.text.count;
                    for (uint32_t k = 0; k < count; ++k) {
                        char bytes[kPrUtf8MaxBytes];
                        uint32_t written = prUtf8Encode(mapped[k], bytes);
                        t.text.append(bytes, written);
                        for (uint32_t b = 0; b < written; ++b) {
                            t.spaces.push(0);
                            t.sources.push(sources.data[copied]);
                        }
                    }
                    copied += length;
                }
                at += length;
            }
        }
    }
    copyTo(text.count);
    moved.data[text.count] = t.text.count;
    for (uint32_t i = 0; i < t.items.count; ++i) {
        t.items.data[i].start = moved.data[t.items.data[i].start];
        t.items.data[i].end = moved.data[t.items.data[i].end];
    }
    for (uint32_t s = 0; s < t.segments.count; ++s) {
        t.segments.data[s].byteStart = moved.data[t.segments.data[s].byteStart];
        t.segments.data[s].byteEnd = moved.data[t.segments.data[s].byteEnd];
    }
    text.reset();
    spaces.reset();
    sources.reset();
    moved.reset();
}

// Collects the context from `first` through `last` into `t` from scratch, in the records that `t.lined` says.
void collectItems(LayoutState& state, LayoutInlineShaping& t, uint32_t first, uint32_t last) {
    t.collected = state.reuse ? ++state.tree->cache.collections : 0;
    t.containerFont = t.lined ? containerLineFontOf(state, first) : containerFontOf(state, first);
    t.containerLineFont = containerLineFontOf(state, first);
    t.containerBidi = containerBidiOf(state, first);
    t.last = last;
    t.nodes.clear();
    t.text.clear();
    t.spaces.clear();
    t.sources.clear();
    t.tabs = false;
    t.items.clear();
    t.segments.clear();
    t.segments.push(LayoutInlineSegment{});
    bool space = true;
    for (uint32_t c = first;; c = layoutTreeNode(state.tree, c).next) {
        c = collect(state, t, c, space);
        if (c == last)
            break;
    }
    t.segments.last().byteEnd = t.text.count;
    t.segments.last().itemEnd = t.items.count;
    transformText(state, t);
    holdFonts(state.styles, t);
}

// Returns true if the container of the context that `t` collected, or an element in it, has a record for the first
// formatted line, so that the context has a shaping for that line.
bool hasLineStyles(const LayoutState& state, const LayoutInlineShaping& t) {
    if (state.tree->firstLines.count == 0)
        return false;
    LayoutNodeIndex container = layoutTreeNode(state.tree, t.nodes.data[0]).parent;
    if (container != kPrLayoutNodeNone && layoutTreeNode(state.tree, container).listed)
        return true;
    for (uint32_t i = 0; i < t.nodes.count; ++i) {
        const LayoutTreeNode& n = layoutTreeNode(state.tree, t.nodes.data[i]);
        if (n.listed && n.kind != PrLayoutNodeKind_Text)
            return true;
    }
    return false;
}

// Collects the context from `first` through `last` into `t` from scratch, and into its shaping for the first formatted
// line if it has one (LayoutInlineShaping::firstLine).
void collectContext(LayoutState& state, LayoutInlineShaping& t, uint32_t first, uint32_t last) {
    t.lined = false;
    collectItems(state, t, first, last);
    if (!hasLineStyles(state, t)) {
        layoutShapingDestroy(t.firstLine);
        t.firstLine = nullptr;
        return;
    }
    if (t.firstLine == nullptr)
        t.firstLine = allocZeroed<LayoutInlineShaping>();
    t.firstLine->lined = true;
    collectItems(state, *t.firstLine, first, last);
}

// Clears the change records of the nodes of a current shaping, as collecting does.
void clearChanges(LayoutState& state, const LayoutInlineShaping& t) {
    for (uint32_t i = 0; i < t.nodes.count; ++i) {
        LayoutNodeIndex node = t.nodes.data[i];
        if (!keepsMarks(state, node))
            state.tree->nodes.data[node].dirty = 0;
    }
}

// Returns true if the cached `shaping` matches what collecting the context from `first` through `last` would give: the
// same nodes, in order, in a container whose font has not changed, with the same nodes laid out apart, each of the same
// kind. A change to a collected node marks that node itself. Marks of changes below a node therefore leave the shaping
// current. Only a change inside a node laid out apart leaves such marks without marking the node itself. Any change to
// a node laid out apart that keeps its kind also leaves the shaping current. A run of table parts must also hold the
// same nodes, with no change to those between its parts.
bool isCurrent(const LayoutState& state, const LayoutInlineShaping& shaping, uint32_t first, uint32_t last) {
    if (shaping.containerFont != containerFontOf(state, first) ||
            shaping.containerLineFont != containerLineFontOf(state, first))
        return false;
    if (shaping.containerBidi != containerBidiOf(state, first))
        return false;
    uint32_t at = 0;
    LayoutItemIndex item = 0;              // the next atomic inline's item is at or past it
    bool same = true;
    // Visits `node` as collect does, and returns the last sibling that collect took with it.
    auto visit = [&](auto& recurse, LayoutNodeIndex node) -> LayoutNodeIndex {
        const LayoutTreeNode& n = layoutTreeNode(state.tree, node);
        same = same && at < shaping.nodes.count && shaping.nodes.data[at] == node;
        ++at;
        if (!same)
            return node;
        bool none =
            n.kind != PrLayoutNodeKind_Text && layoutBoxStyle(state.styles, n.style).display == PrLayoutDisplay_None;
        if (!none && n.kind != PrLayoutNodeKind_Text && laidOutApart(state, node)) {
            // It was also collected as a node laid out apart, of the same kind.
            LayoutInlineItemKind kind = apartKind(state, node);
            auto apart = [&](LayoutInlineItemKind k) {
                return k == LayoutInlineItemKind_Atomic || k == LayoutInlineItemKind_Float ||
                    k == LayoutInlineItemKind_Block || k == LayoutInlineItemKind_OutOfFlow;
            };
            while (item < shaping.items.count && !apart(shaping.items.data[item].kind))
                ++item;
            same = item < shaping.items.count && shaping.items.data[item].node == node && shaping.items.data[item].kind == kind;
            if (!same || !shaping.items.data[item].wraps) {
                ++item;
                return node;
            }
            LayoutNodeIndex runLast = shaping.items.data[item++].runLast;
            same = runLast == layoutTablePartRunLast(state.tree, node, kPrLayoutNodeNone);
            for (LayoutNodeIndex c = node; same && c != runLast;) {
                c = layoutTreeNode(state.tree, c).next;
                same = at < shaping.nodes.count && shaping.nodes.data[at] == c &&
                    (layoutChildKind(state.tree, c) == LayoutChildKind_TablePart ||
                        (layoutTreeNode(state.tree, c).dirty & LayoutDirtyFlag_Self) == 0);
                ++at;
            }
            return runLast;
        }
        same = (n.dirty & LayoutDirtyFlag_Self) == 0;
        if (!same || none || n.kind == PrLayoutNodeKind_Text)
            return node;
        for (uint32_t c = n.first; c != kPrLayoutNodeNone && same; c = layoutTreeNode(state.tree, c).next)
            c = recurse(recurse, c);
        return node;
    };
    for (uint32_t c = first; same; c = layoutTreeNode(state.tree, c).next) {
        c = visit(visit, c);
        if (c == last)
            break;
    }
    return same && at == shaping.nodes.count;
}

// Returns the context's cached shaping, creating it if there is none and `state` reuses results.
LayoutInlineShaping* PR_NULLABLE shapingOf(LayoutState& state, uint32_t first) {
    if (!state.reuse)
        return nullptr;
    Array<LayoutInlineShaping* PR_NULLABLE>& shapings = state.tree->shapings;
    while (shapings.count <= first)
        shapings.push(nullptr);
    if (shapings.data[first] == nullptr)
        shapings.data[first] = allocZeroed<LayoutInlineShaping>();
    return shapings.data[first];
}

// ---------------------------------------------------------------------------------------------------------------------
// Shaping

// Returns the advance of `text` shaped alone in `font` at `size` px, drawn at `ratio` target pixels per px, in
// `locale`. The advance is down the line if `upright` is true, in a face of `metrics`, in the same way that a shaped
// run advances it.
float advanceOf(PrTextShaper* PR_NULLABLE shaper, PrTextGlyphBuffer* buffer, const PrFont* font, float size,
        float ratio, PrLocale locale, PrStringView text, bool upright, const LayoutFaceMetrics& metrics) {
    PR_ASSERT(shaper != nullptr); // text needs a shaper (PrLayoutTreeDesc)
    PrTextRun run{};
    run.font = font;
    run.text = text;
    run.axis = upright ? PrTextAxis_Vertical : PrTextAxis_Horizontal;
    run.locale = locale;
    run.pixelsPerEm = size * ratio;
    if (prTextShaperShape(shaper, &run, buffer) != PrStatus_Ok)
        return 0;
    float advance = 0;
    const PrTextGlyph* glyphs = prTextGlyphBufferGlyphs(buffer);
    for (uint32_t g = 0; g < prTextGlyphBufferGlyphCount(buffer); ++g) {
        float along = (upright ? glyphs[g].advance.y : glyphs[g].advance.x) * size;
        if (upright && !metrics.vertical && along != 0)
            along = prLayoutUnitToPx(metrics.ascent + metrics.descent);
        advance += along;
    }
    return advance;
}

// Sets the tab stops of text with preserved tabs from the first face of the container's font, which the context has
// already matched, in the orientation in which the container sets its spaces and digits.
void measureTabs(LayoutStyles* styles, PrTextShaper* PR_NULLABLE shaper, PrTextGlyphBuffer* buffer, LayoutInlineShaping& t) {
    t.tabSize = 0;
    t.tabMinimum = 0;
    const LayoutFontFaces& faces = layoutFontFacesOfGroup(styles, t.containerFont);
    if (!t.tabs || faces.firstFace == kLayoutFaceNone)
        return;
    layoutFontFacesMeasureNew(styles, t.containerFont);
    const PrFont* font = layoutFaceFont(faces, faces.firstFace);
    const PrLayoutFontStyle& group = layoutTableAt(styles->font, t.containerFont);
    float size = prLayoutUnitToPx(faces.size);
    PrLocale locale = prTextFontLocale(faces.font);
    const LayoutFaceMetrics& metrics = faces.metrics.data[faces.firstFace];
    bool spaceUpright = layoutIsUpright(group, group, ' ', PrScript_Zzzz);
    bool digitUpright = layoutIsUpright(group, group, '0', PrScript_Zzzz);
    float ratio = styles->targetPixelRatio;
    t.tabSize = 8 * advanceOf(shaper, buffer, font, size, ratio, locale, " ", spaceUpright, metrics);
    t.tabMinimum = advanceOf(shaper, buffer, font, size, ratio, locale, "0", digitUpright, metrics) / 2;
}

// Has lib/text itemize, shape and find the break opportunities of the shaping's text, and gives each text item the
// glyphs whose clusters start in its bytes. Doing so matches and measures in `styles` the faces that its clusters need
// (layoutItemize). Returns true if it resolved bidi levels. A context of only atomic inlines shapes no run and needs no
// shaper.
bool shape(LayoutStyles* styles, PrTextShaper* PR_NULLABLE shaper, PrTextGlyphBuffer* buffer, LayoutInlineShaping& t,
        LayoutShapingScratch& s) {
    measureTabs(styles, shaper, buffer, t);
    bool leveled = layoutItemize(styles, shaper, t, s);
    // Runs are in text order and their glyphs in ascending cluster order, so each item's glyphs are consecutive. The
    // items, in text order, therefore take their glyphs and first runs in one pass.
    uint32_t g = 0;
    uint32_t r = 0;
    for (uint32_t i = 0; i < t.items.count; ++i) {
        LayoutInlineItem& item = t.items.data[i];
        item.fallback = false;
        item.glyphStart = item.glyphEnd = g;
        while (r < t.runs.count && t.runs.data[r].end <= item.start)
            ++r;
        item.runStart = item.runEnd = r;
        if (item.kind != LayoutInlineItemKind_Text)
            continue;
        while (item.runEnd < t.runs.count && t.runs.data[item.runEnd].start < item.end)
            ++item.runEnd;
        item.glyphStart = t.info.glyphAt[item.start];
        item.glyphEnd = g = t.info.glyphAt[item.end];
        // Glyphs are in text order, so a run's glyphs among the item's glyphs are where their ranges overlap.
        for (uint32_t k = item.runStart; k < item.runEnd && !item.fallback; ++k) {
            const LayoutShapingRun& run = t.runs.data[k];
            bool meets = run.glyphStart < item.glyphEnd && item.glyphStart < run.glyphEnd;
            item.fallback = meets && run.face != layoutFontFacesOfGroup(styles, run.font).firstFace;
        }
    }
    return leveled;
}

// Shapes the text that a shaping collected.
void shapeText(LayoutState& state, LayoutInlineShaping& t) {
    if (shape(state.styles, state.styles->shaper, state.scratch->glyphs, t, *state.scratch->shaping))
        ++state.stats->inlinesLeveled;
    if (t.firstLine != nullptr)
        shapeText(state, *t.firstLine);
}

// ---------------------------------------------------------------------------------------------------------------------
// Shaping ahead

// Collects the context from `first` through `last` into the shaping that `first` caches if that shaping is not current,
// and appends the shaping to `jobs`.
void prepareContext(LayoutState& state, uint32_t first, uint32_t last, Array<LayoutInlineShaping*>& jobs) {
    LayoutInlineShaping* cached = shapingOf(state, first);
    if (cached == nullptr || (cached->nodes.count != 0 && isCurrent(state, *cached, first, last)))
        return;
    collectContext(state, *cached, first, last);
    jobs.push(cached);
}

void prepareContainer(LayoutState& state, LayoutNodeIndex node, Array<LayoutInlineShaping*>& jobs);

// Calls prepareContainer with each changed node laid out apart (an inline-block, a float, or a block-level box in an
// inline box) among the inline-level siblings from `first` through `last` and in the inline boxes among them. A change
// marks its ancestors, so the walk enters only the marked inline boxes.
void prepareAtomics(LayoutState& state, uint32_t first, uint32_t last, Array<LayoutInlineShaping*>& jobs) {
    for (uint32_t c = first;; c = layoutTreeNode(state.tree, c).next) {
        const LayoutTreeNode& n = layoutTreeNode(state.tree, c);
        if (n.dirty != 0 && layoutKindIsElement(n.kind)) {
            PrLayoutDisplay display = layoutBoxStyle(state.styles, n.style).display;
            if (laidOutApart(state, c))
                prepareContainer(state, c, jobs);
            else if (display != PrLayoutDisplay_None && n.first != kPrLayoutNodeNone)
                prepareAtomics(state, n.first, n.last, jobs);
        }
        if (c == last)
            break;
    }
}

// Collects the contexts of the changed parts in the run of table parts from `first` through `last`, which an anonymous
// table wraps.
void prepareParts(LayoutState& state, LayoutNodeIndex first, LayoutNodeIndex last, Array<LayoutInlineShaping*>& jobs) {
    for (uint32_t c = first;; c = layoutTreeNode(state.tree, c).next) {
        const LayoutTreeNode& n = layoutTreeNode(state.tree, c);
        if (n.dirty != 0 && layoutChildKind(state.tree, c) == LayoutChildKind_TablePart)
            prepareContainer(state, c, jobs);
        if (c == last)
            break;
    }
}

// Collects the inline formatting contexts whose shaping is not current in the run of siblings from `first` through
// `last`, and those of the changed boxes laid out apart in the run. The run is the content of a block container or of
// an anonymous box.
void prepareRun(LayoutState& state, LayoutNodeIndex first, LayoutNodeIndex last, Array<LayoutInlineShaping*>& jobs) {
    layoutEachChildIn(state.tree, first, last, [&](uint32_t child, const PrLayoutBoxStyle&) {
        if (layoutTreeNode(state.tree, child).dirty != 0)
            prepareContainer(state, child, jobs);
    }, [&](uint32_t runFirst, uint32_t runLast, bool) {
            // This runs before the context is collected, because collecting clears the marks of the inline boxes that
            // lead to the changed nodes.
        prepareAtomics(state, runFirst, runLast, jobs);
        prepareContext(state, runFirst, runLast, jobs);
    }, [&](uint32_t runFirst, uint32_t runLast) { prepareParts(state, runFirst, runLast, jobs); });
}

// Collects the inline formatting contexts in the block container, flex or grid container or table `node` whose shaping
// is not current, and those of the changed block containers, inline-blocks, floats, outside markers, captions and cells
// in it.
void prepareContainer(LayoutState& state, LayoutNodeIndex node, Array<LayoutInlineShaping*>& jobs) {
    LayoutChildren children = layoutTreeNode(state.tree, node).children;
    if (children == LayoutChildren_Table) {
        layoutEachTableBox(state.tree, node, [&](uint32_t child) {
            if (layoutTreeNode(state.tree, child).dirty != 0)
                prepareContainer(state, child, jobs);
        }, [&](uint32_t first, uint32_t last) { prepareRun(state, first, last, jobs); });
        return;
    }
    if (layoutChildrenAreItems(children)) {
        layoutEachItem(state.tree, node, [&](uint32_t child, const PrLayoutBoxStyle&) {
            if (layoutTreeNode(state.tree, child).dirty != 0)
                prepareContainer(state, child, jobs);
        }, [&](uint32_t first, uint32_t last) { prepareContext(state, first, last, jobs); });
        return;
    }
    LayoutNodeIndex marker = layoutNodeMarker(state.tree, node);
    if (marker != kPrLayoutNodeNone && layoutTreeNode(state.tree, marker).dirty != 0)
        prepareContainer(state, marker, jobs);
    layoutEachChild(state.tree, node, [&](uint32_t child, const PrLayoutBoxStyle&) {
        if (layoutTreeNode(state.tree, child).dirty != 0)
            prepareContainer(state, child, jobs);
    }, [&](uint32_t first, uint32_t last, bool) {
            // This runs before the context is collected, because collecting clears the marks of the inline boxes that
            // lead to the changed nodes.
        prepareAtomics(state, first, last, jobs);
        prepareContext(state, first, last, jobs);
    }, [&](uint32_t first, uint32_t last) { prepareParts(state, first, last, jobs); });
}

} // namespace

void layoutShapingReset(LayoutInlineShaping& shaping) {
    layoutShapingDestroy(shaping.firstLine);
    shaping.firstLine = nullptr;
    releaseFonts(shaping);
    shaping.fonts.reset();
    shaping.runs.reset();
    shaping.splits.reset();
    prTextShapingDestroy(shaping.shaped);
    shaping.shaped = nullptr;
    shaping.nodes.reset();
    shaping.text.reset();
    shaping.spaces.reset();
    shaping.sources.reset();
    shaping.items.reset();
    shaping.segments.reset();
}

void layoutShapingDestroy(LayoutInlineShaping* PR_NULLABLE shaping) {
    if (shaping == nullptr)
        return;
    layoutShapingReset(*shaping);
    prMemoryFree(shaping);
}

void layoutShapingScratchReset(LayoutShapingScratch& scratch) {
    prTextShapingScratchDestroy(scratch.text);
    scratch.text = nullptr;
    scratch.pieces.reset();
    scratch.classes.reset();
    scratch.bidiLevels.reset();
    scratch.bidiOpen.reset();
    scratch.unitStarts.reset();
    scratch.unitLevels.reset();
    scratch.unitOrder.reset();
    scratch.unitVisual.reset();
    scratch.unitRuns.reset();
    scratch.boxBytes.reset();
    scratch.positions.reset();
    scratch.lineLevels.reset();
}

const LayoutInlineShaping& layoutShaping(LayoutState& state, LayoutInlineShaping& fresh, uint32_t first, uint32_t last) {
    LayoutInlineShaping* cached = shapingOf(state, first);
    LayoutInlineShaping& t = cached != nullptr ? *cached : fresh;
    if (cached != nullptr && cached->nodes.count != 0 && isCurrent(state, *cached, first, last)) {
        clearChanges(state, t);
        return t;
    }
    collectContext(state, t, first, last);
    shapeText(state, t);
    ++state.stats->inlinesShaped;
    return t;
}

void layoutShapeAhead(LayoutState& state, uint32_t root) {
    if (!state.reuse || root == kPrLayoutNodeNone || layoutTreeNode(state.tree, root).dirty == 0)
        return;
    Array<LayoutInlineShaping*>& jobs = state.scratch->shapings;
    jobs.clear();
    prepareContainer(state, root, jobs);
    for (uint32_t i = 0; i < jobs.count; ++i)
        shapeText(state, *jobs.data[i]);
    state.stats->inlinesShaped += jobs.count;
}

} // namespace pr_impl
