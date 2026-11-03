// Flex layout: the items of a flex container, sized and placed along its line, and what each layout of them costs.
// Words are inline-blocks 20px tall of the widths given, in a font of size 0, with no white space between them, so that
// lines break between any two and are only as tall as their words.

#include "pr/layout_style.h"
#include "pr/layout_tree.h"

#include "layout/tests/layout_fixture.h"

#include "doctest/doctest.h"

#include <cmath>
#include <utility>
#include <vector>

namespace pr_impl {

namespace {

// A tree whose root is in a font of size 0, with helpers for the boxes and words that flex cases are made of.
struct Stage {
    Fixture f;
    PrLayoutFontStyle font = prLayoutFontStyleDefault();
    PrLayoutNode root = kPrLayoutNodeNone;

    Stage() {
        font.font = textFontOf(0);
        root = add(kPrLayoutNodeNone, block());
    }

    // Creates an element under `parent`, with a flex group if given.
    PrLayoutNode add(PrLayoutNode parent, const PrLayoutBoxStyle& box,
            const PrLayoutFlexStyle& flex = prLayoutFlexStyleDefault()) {
        return f.element(parent, box, nullptr, nullptr, &font, &flex);
    }
    PrLayoutNode add(PrLayoutNode parent, const std::pair<PrLayoutBoxStyle, PrLayoutFlexStyle>& styled) {
        return add(parent, styled.first, styled.second);
    }

    // Returns the record of `styled`.
    uint32_t style(const std::pair<PrLayoutBoxStyle, PrLayoutFlexStyle>& styled) {
        return f.style(styled.first, nullptr, nullptr, &font, &styled.second);
    }

    // Creates words of the widths given, each 20px tall.
    void words(PrLayoutNode parent, std::vector<float> widths) {
        for (float width : widths) {
            PrLayoutBoxStyle box = block(20);
            box.display = PrLayoutDisplay_InlineBlock;
            box.width = prLayoutLengthPx(width);
            add(parent, box);
        }
    }

    void layout(float width = 600, float height = 400) {
        f.layout(px(width, height));
        CHECK(verified(f.tree));
    }

    PrLayoutTreeStats stats() { return prLayoutTreeStats(f.tree); }

    // Returns a box's border box, or one of its edges or sizes, relative to the root's top left.
    PrRect at(PrLayoutNode node) { return f.bounds(node); }
    float left(PrLayoutNode node) { return at(node).left; }
    float top(PrLayoutNode node) { return at(node).top; }
    float width(PrLayoutNode node) { return at(node).right - at(node).left; }
    float height(PrLayoutNode node) { return at(node).bottom - at(node).top; }
};

// A box and its flex group.
typedef std::pair<PrLayoutBoxStyle, PrLayoutFlexStyle> Styled;

// Returns a flex container of `direction`, `width` wide if given.
Styled flexBox(PrLayoutFlexDirection direction = PrLayoutFlexDirection_Row, float width = -1) {
    Styled styled{block(), prLayoutFlexStyleDefault()};
    styled.first.display = PrLayoutDisplay_Flex;
    styled.second.direction = direction;
    if (width >= 0)
        styled.first.width = prLayoutLengthPx(width);
    return styled;
}

// Returns a block item that grows by `grow` and shrinks by `shrink` from `basis`.
Styled flexItem(float grow, float shrink, PrLayoutLength basis) {
    Styled styled{block(), prLayoutFlexStyleDefault()};
    styled.second.grow = grow;
    styled.second.shrink = shrink;
    styled.second.basis = basis;
    return styled;
}

// A stage whose flex containers hold Ahem at 10px, so that each character of text is 10px square.
struct TextStage : Stage {
    PrLayoutFontStyle ahem = prLayoutFontStyleDefault();

    TextStage() { ahem.font = textFontOf(10, {f.family("Ahem.ttf", "Ahem")}); }

    // Creates a flex container styled by `styled` under `parent`, in Ahem.
    PrLayoutNode container(PrLayoutNode parent, const Styled& styled) {
        return f.element(parent, styled.first, nullptr, nullptr, &ahem, &styled.second);
    }

    PrLayoutNode text(PrLayoutNode parent, const char* value, PrLayoutNode before = kPrLayoutNodeNone) {
        PrLayoutNode node = prLayoutNodeCreateText(f.tree, kPrLayoutNodeNone, value);
        prLayoutTreeInsert(f.tree, parent, node, before);
        return node;
    }

    // Creates a block item `width` wide and `height` tall.
    PrLayoutNode box(PrLayoutNode parent, float width, float height, PrLayoutNode before = kPrLayoutNodeNone) {
        PrLayoutBoxStyle style = block(height);
        style.width = prLayoutLengthPx(width);
        PrLayoutNode node = prLayoutNodeCreateElement(f.tree, kPrLayoutNodeNone, f.style(style, nullptr, nullptr, &ahem));
        prLayoutTreeInsert(f.tree, parent, node, before);
        return node;
    }
};

// Lays out a row of a text item beside a 60px box that stretches it, with the item's content made by `fill`, in `mode`.
// Returns the stats of its first layout, in which the item is measured at its own height and laid out at the line's
// height.
template <typename Fill>
PrLayoutTreeStats stretchedLines(PrLayoutWritingMode mode, Fill fill) {
    TextStage s;
    s.ahem.writingMode = mode;
    Styled row = flexBox(PrLayoutFlexDirection_Row);
    if (mode == PrLayoutWritingMode_HorizontalTb)
        row.first.width = prLayoutLengthPx(300);
    else
        row.first.height = prLayoutLengthPx(300);
    PrLayoutNode r = s.container(s.root, row);
    Styled item{block(), prLayoutFlexStyleDefault()};
    if (mode == PrLayoutWritingMode_HorizontalTb)
        item.first.width = prLayoutLengthPx(40);
    else
        item.first.height = prLayoutLengthPx(40);
    PrLayoutNode a = s.container(r, item);
    fill(s, a);
    PrLayoutBoxStyle tall = block();
    tall.width = prLayoutLengthPx(60);
    tall.height = prLayoutLengthPx(60);
    s.f.element(r, tall, nullptr, nullptr, &s.ahem);
    s.layout();
    PrLayoutTreeStats stats = s.stats();
    // Nothing changed: nothing is laid out, and the item's lines are still its fragment's.
    s.layout();
    CHECK(s.stats().linesLaidOut == 0);
    return stats;
}

// Returns the physical side of the block-start edge in `mode`.
PrRectSide blockStartOf(PrLayoutWritingMode mode) {
    if (mode == PrLayoutWritingMode_HorizontalTb)
        return PrRectSide_Top;
    return mode == PrLayoutWritingMode_VerticalRl ? PrRectSide_Right : PrRectSide_Left;
}

// Creates an inline box under `parent`, relatively positioned by `offset` from its block-start edge if given.
PrLayoutNode span(TextStage& s, PrLayoutNode parent, PrLayoutLength offset = prLayoutLengthAuto()) {
    PrLayoutBoxStyle box = inlineBox();
    if (offset.bits != prLayoutLengthAuto().bits) {
        box.position = PrLayoutPosition_Relative;
        box.inset[blockStartOf(s.ahem.writingMode)] = offset;
    }
    PrLayoutNode node = prLayoutNodeCreateElement(s.f.tree, parent, s.f.style(box, nullptr, nullptr, &s.ahem));
    return node;
}

// The kinds of chain that a sweep builds. Each level is a container of two items, one of which is the next level, so
// that the number of nodes grows by a constant per level.
enum {
    Chain_Columns,          // single-line columns of auto height, their items stretched
    Chain_Alternating,      // rows and columns in turn
    Chain_Wrapped,          // columns, each level's chain item a block wrapping the next column
    Chain_Percentages,      // rows of auto height whose chain item's child is half its stretched height
    Chain_Fitted,           // columns whose items are at flex-start, so that each level's width is its content's
    Chain_Orthogonal,       // columns, each in the other writing mode from its parent's, so that each is an orthogonal
                            // item, main-inline in its parent
    Chain_WrappedColumns,   // multi-line columns, each level's leaf wider than the next level, which its line stretches
};
typedef uint8_t Chain;

// The work of a cold layout of a chain `depth` levels deep, and of a layout after its deepest leaf changes.
struct Sweep {
    uint32_t nodes;
    PrLayoutTreeStats cold;
    PrLayoutTreeStats change;
};

Sweep sweep(Chain chain, uint32_t depth) {
    Stage s;
    PrLayoutNode parent = s.root;
    PrLayoutNode leaf = kPrLayoutNodeNone;
    uint32_t nodes = 1;
    for (uint32_t level = 0; level < depth; ++level) {
        bool row = chain == Chain_Percentages || (chain == Chain_Alternating && level % 2 == 1);
        Styled box = flexBox(row ? PrLayoutFlexDirection_Row : PrLayoutFlexDirection_Column);
        if (chain == Chain_Fitted)
            box.second.alignItems = PrLayoutAlignment_FlexStart;
        if (chain == Chain_WrappedColumns)
            box.second.wrap = PrLayoutFlexWrap_Wrap;
        PrLayoutFontStyle font = s.font;
        if (chain == Chain_Orthogonal && level % 2 == 1)
            font.writingMode = PrLayoutWritingMode_VerticalRl;
        PrLayoutNode container = s.f.element(parent, box.first, nullptr, nullptr, &font, &box.second);
        ++nodes;
        PrLayoutBoxStyle leafBox = block(10);
        if (chain == Chain_WrappedColumns)
            leafBox.width = prLayoutLengthPx(float(10 * (depth - level)));
        leaf = s.add(container, leafBox);
        ++nodes;
        if (chain == Chain_Wrapped) {
            parent = s.add(container, block());
            ++nodes;
        } else if (chain == Chain_Percentages) {
            PrLayoutBoxStyle half = block();
            half.height = prLayoutLengthPercent(50);
            PrLayoutNode item = s.add(container, block());
            parent = s.add(item, half);
            nodes += 2;
        } else {
            parent = container;
        }
    }
    s.layout();
    Sweep out{nodes, s.stats(), {}};
    prLayoutNodeSetStyle(s.f.tree, leaf, s.style(Styled{block(12), prLayoutFlexStyleDefault()}));
    s.layout();
    out.change = s.stats();
    return out;
}

// Returns true if `work` at 32 levels is within 15% of linear growth from `work` at 16, per node.
template <typename Of>
bool linear(Chain chain, Of work) {
    Sweep half = sweep(chain, 16);
    Sweep whole = sweep(chain, 32);
    double perHalf = double(work(half)) / double(half.nodes);
    double perWhole = double(work(whole)) / double(whole.nodes);
    CAPTURE(work(half));
    CAPTURE(work(whole));
    return perWhole <= perHalf * 1.15 + 0.01;
}

// Creates flex items `width` wide that neither grow nor shrink, each holding a word, under `parent`.
std::vector<PrLayoutNode> fixedItems(Stage& s, PrLayoutNode parent, std::vector<float> widths) {
    std::vector<PrLayoutNode> out;
    for (float width : widths) {
        Styled item = flexItem(0, 0, prLayoutLengthAuto());
        item.first.width = prLayoutLengthPx(width);
        item.first.height = prLayoutLengthPx(20);
        out.push_back(s.add(parent, item));
    }
    return out;
}

// Lays out a row `width` wide whose free space justify-content places, and returns the lefts of its items of `widths`,
// relative to its own left.
std::vector<float> justified(PrLayoutAlignment justify, std::vector<float> widths,
        PrLayoutFlexDirection direction = PrLayoutFlexDirection_Row, float width = 100,
        PrTextDirection inlineDirection = PrTextDirection_Ltr) {
    Stage s;
    Styled row = flexBox(direction, width);
    row.second.justifyContent = justify;
    PrLayoutFontStyle font = s.font;
    font.direction = inlineDirection;
    PrLayoutNode r = s.f.element(s.root, row.first, nullptr, nullptr, &font, &row.second);
    std::vector<PrLayoutNode> items = fixedItems(s, r, widths);
    s.layout();
    std::vector<float> lefts;
    for (PrLayoutNode item : items)
        lefts.push_back(s.left(item) - s.left(r));
    return lefts;
}

PrLayoutAlignment safe(PrLayoutAlignment value) {
    return PrLayoutAlignment(value | PrLayoutAlignmentFlag_Safe);
}

// Returns the width of a row sized to `keyword` holding items of `styles`, each holding words of the widths given.
float intrinsicWidth(PrLayoutLength keyword, std::vector<std::pair<Styled, std::vector<float>>> items,
        PrLayoutLength gap = prLayoutLengthNormal()) {
    Stage s;
    Styled row = flexBox(PrLayoutFlexDirection_Row);
    row.first.width = keyword;
    row.second.columnGap = gap;
    PrLayoutNode r = s.add(s.root, row);
    for (auto& [styled, words] : items) {
        PrLayoutNode item = s.add(r, styled);
        s.words(item, words);
    }
    s.layout();
    return s.width(r);
}

// The left edges of a row's items from the row's, and the row's right edge, the row `width` wide holding `items`.
std::vector<float> flexedEdges(Stage& s, PrLayoutNode r, const std::vector<PrLayoutNode>& items) {
    std::vector<float> edges;
    for (PrLayoutNode item : items)
        edges.push_back(s.left(item) - s.left(r));
    edges.push_back(s.at(items.back()).right - s.left(r));
    return edges;
}

// A wrapping row `width` by `height`, auto if `height` is negative, aligning its lines by `alignContent` and holding
// items 40 wide and `itemHeight` tall, auto if negative.
struct Wrapped {
    Stage s;
    PrLayoutNode container = kPrLayoutNodeNone;
    std::vector<PrLayoutNode> items;

    Wrapped(uint32_t count, float width, float height, PrLayoutAlignment alignContent = PrLayoutAlignment_Normal,
            PrLayoutFlexWrap wrap = PrLayoutFlexWrap_Wrap, float itemHeight = 20, float itemWidth = 40) {
        Styled row = flexBox(PrLayoutFlexDirection_Row, width);
        if (height >= 0)
            row.first.height = prLayoutLengthPx(height);
        row.second.wrap = wrap;
        row.second.alignContent = alignContent;
        container = s.add(s.root, row);
        for (uint32_t i = 0; i < count; ++i) {
            Styled item = flexItem(0, 1, prLayoutLengthAuto());
            item.first.width = prLayoutLengthPx(itemWidth);
            if (itemHeight >= 0)
                item.first.height = prLayoutLengthPx(itemHeight);
            items.push_back(s.add(container, item));
        }
    }

    float top(uint32_t i) { return s.top(items[i]) - s.top(container); }
    float left(uint32_t i) { return s.left(items[i]) - s.left(container); }
};

// Returns the top of a container of `styled`, holding items 20 wide of heights `heights` and no baselines, relative to
// the top of an outer row aligned by baseline whose other item is a 20px box, which also has no baseline.
float baselineTop(Styled styled, std::vector<float> heights) {
    Stage s;
    Styled outer = flexBox(PrLayoutFlexDirection_Row, 300);
    outer.second.alignItems = PrLayoutAlignment_Baseline;
    PrLayoutNode o = s.add(s.root, outer);
    PrLayoutBoxStyle box = block(20);
    box.width = prLayoutLengthPx(20);
    s.add(o, box);
    PrLayoutNode inner = s.add(o, styled);
    for (float height : heights) {
        PrLayoutBoxStyle item = block(height);
        item.width = prLayoutLengthPx(20);
        s.add(inner, item);
    }
    s.layout();
    return s.top(inner) - s.top(o);
}

// Returns the offset, from its container's border box, of an absolute 20px box styled by `child`, in a relative flex
// container 100px square styled by `styled` and holding nothing else.
PrVec2 staticOffset(Styled styled, Styled child = flexItem(0, 1, prLayoutLengthAuto()),
        PrTextDirection direction = PrTextDirection_Ltr) {
    Stage s;
    styled.first.width = prLayoutLengthPx(100);
    styled.first.height = prLayoutLengthPx(100);
    styled.first.position = PrLayoutPosition_Relative;
    PrLayoutFontStyle font = s.font;
    font.direction = direction;
    PrLayoutNode c = s.f.element(s.root, styled.first, nullptr, nullptr, &font, &styled.second);
    child.first.position = PrLayoutPosition_Absolute;
    if (child.first.width.bits == prLayoutLengthAuto().bits)
        child.first.width = prLayoutLengthPx(20);
    child.first.height = prLayoutLengthPx(20);
    PrLayoutNode a = s.f.element(c, child.first, nullptr, nullptr, &font, &child.second);
    s.layout();
    return PrVec2{s.left(a) - s.left(c), s.top(a) - s.top(c)};
}

// Returns a flex item `width` wide and `height` tall, collapsed if `collapsed` is true.
Styled collapsible(float width, float height, bool collapsed) {
    Styled item = flexItem(0, 0, prLayoutLengthAuto());
    item.first.width = prLayoutLengthPx(width);
    item.first.height = prLayoutLengthPx(height);
    item.first.collapsed = collapsed;
    return item;
}

uint32_t boxCount(Stage& s, PrLayoutNode node) {
    return prLayoutNodeBoxes(s.f.tree, node, PrLayoutCoordinates_Viewport, nullptr, 0);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a row's items flex to its width, a shrunk item stops at its content's minimum, and the line stretches them") {
    Stage s;
    PrLayoutNode r = s.add(s.root, flexBox(PrLayoutFlexDirection_Row, 300));
    PrLayoutNode a = s.add(r, flexItem(1, 1, prLayoutLengthPx(0)));
    s.words(a, {40, 40, 40});
    Styled fixed = flexItem(0, 0, prLayoutLengthAuto());
    fixed.first.width = prLayoutLengthPx(50);
    fixed.second.alignSelf = PrLayoutAlignment_Center;
    PrLayoutNode b = s.add(r, fixed);
    s.words(b, {30, 20});
    PrLayoutNode below = s.add(b, block(10));
    PrLayoutNode c = s.add(r, flexItem(0, 1, prLayoutLengthAuto()));
    s.words(c, std::vector<float>(14, 20));
    s.words(c, {60});
    s.layout();

    // A is at its automatic minimum, its widest word. B keeps its width, and C takes what is left.
    CHECK(s.width(a) == 40.0f);
    CHECK(s.width(b) == 50.0f);
    CHECK(s.width(c) == 210.0f);
    CHECK(s.left(b) == 40.0f);
    CHECK(s.left(c) == 90.0f);
    // The line is as tall as A's three lines. A and C stretch to it, and B is centred in it.
    CHECK(s.height(r) == 60.0f);
    CHECK(s.height(a) == 60.0f);
    CHECK(s.height(c) == 60.0f);
    CHECK(s.height(b) == 30.0f);
    CHECK(s.top(b) == 15.0f);
    // Each item is measured once. B's final layout takes the layout that its measurement made, and A and C are laid out
    // stretched.
    CHECK(s.stats().measuresComputed == 3);
    CHECK(s.stats().measuresReplaced == 0);
    CHECK(s.stats().handoffsTaken == 1);
    CHECK(s.stats().flexLaidOut == 1);
    // The freeze loop takes two rounds: nothing clamps C, and the second round finds every item frozen.
    CHECK(s.stats().flexItemsFlexed == 3);
    // Fragments are made of the root, R, A and C twice each, B, B's block, the anonymous block around B's words, and
    // the 20 words once each. The tree is published whole: the root and R, R's three items, B's block, and the words,
    // each a box of a node of its own.
    CHECK(s.stats().boxFragmentsMade == 29);
    CHECK(s.stats().childrenPublished == 2 + 3 + 1 + 20);

    // Nothing changed: the layout reaches nothing.
    s.layout();
    CHECK(s.stats().blocksLaidOut == 0);
    CHECK(s.stats().measuresComputed == 0);
    CHECK(s.stats().childrenPublished == 0);

    // B grows: its measurement and layout are made again, and A's and C's are taken from their caches.
    PrLayoutNode grown = s.add(b, block(20));
    s.layout();
    CHECK(s.height(b) == 50.0f);
    CHECK(s.top(b) == 5.0f);
    CHECK(s.stats().measuresComputed == 1);
    CHECK(s.stats().measuresReused == 2);
    CHECK(s.stats().handoffsTaken == 1);
    (void)grown;
    (void)below;

    // R narrows: C shrinks to 150, three lines, and A and B keep their results.
    prLayoutNodeSetStyle(s.f.tree, r, s.style(flexBox(PrLayoutFlexDirection_Row, 240)));
    s.layout();
    CHECK(s.width(c) == 150.0f);
    CHECK(s.height(c) == 60.0f);
    CHECK(s.stats().measuresComputed == 1);
    CHECK(s.stats().measuresReused == 2);
}

TEST_CASE("a row of definite height lays its stretched items out once at its height, and measures the others") {
    Stage s;
    Styled row = flexBox(PrLayoutFlexDirection_Row, 300);
    row.first.height = prLayoutLengthPx(100);
    PrLayoutNode r = s.add(s.root, row);
    PrLayoutNode a = s.add(r, flexItem(1, 1, prLayoutLengthPx(0)));
    s.words(a, {40, 40, 40});
    PrLayoutBoxStyle half = block();
    half.height = prLayoutLengthPercent(50);
    PrLayoutNode inner = s.add(a, half);
    Styled centred = flexItem(0, 0, prLayoutLengthAuto());
    centred.second.alignSelf = PrLayoutAlignment_Center;
    PrLayoutNode b = s.add(r, centred);
    s.words(b, {30});
    s.layout();

    // A is stretched to the row's height, which is definite for its child's percentage. B is centred at its content's
    // height.
    CHECK(s.height(a) == 100.0f);
    CHECK(s.height(inner) == 50.0f);
    CHECK(s.height(b) == 20.0f);
    CHECK(s.top(b) == 40.0f);
    // Only B is measured, and its final layout takes the layout that its measurement made.
    CHECK(s.stats().measuresComputed == 1);
    CHECK(s.stats().handoffsTaken == 1);
}

TEST_CASE("a column's items take their content's size as a basis, and a definite basis makes a percentage resolve") {
    Stage s;
    PrLayoutNode k = s.add(s.root, flexBox(PrLayoutFlexDirection_Column, 200));
    // P's basis is its content, 80, its max-height is 50, and its minimum is the lesser of its content and its height.
    Styled p = flexItem(0, 1, prLayoutLengthContent());
    p.first.height = prLayoutLengthPx(20);
    p.first.maxHeight = prLayoutLengthPx(50);
    PrLayoutNode pNode = s.add(k, p);
    PrLayoutNode inner = s.add(pNode, block(80));
    // Q grows from zero. Its minimum is its content's: Q1, half its height, and Q2, an orthogonal line 120 long.
    PrLayoutNode q = s.add(k, flexItem(1, 1, prLayoutLengthPx(0)));
    PrLayoutBoxStyle half = block();
    half.height = prLayoutLengthPercent(50);
    PrLayoutNode q1 = s.add(q, half);
    PrLayoutFontStyle vertical = s.font;
    vertical.writingMode = PrLayoutWritingMode_VerticalRl;
    PrLayoutNode q2 = s.f.element(q, block(), nullptr, nullptr, &vertical);
    PrLayoutBoxStyle word = block();
    word.display = PrLayoutDisplay_InlineBlock;
    word.width = prLayoutLengthPx(20);
    word.height = prLayoutLengthPx(120);
    s.f.element(q2, word, nullptr, nullptr, &vertical);
    s.layout();

    CHECK(s.height(pNode) == 50.0f);
    CHECK(s.height(inner) == 80.0f);
    CHECK(s.top(q) == 50.0f);
    CHECK(s.height(q) == 120.0f);
    CHECK(s.height(k) == 170.0f);
    // Q's flexed size is definite, because its basis is definite. Q1's percentage resolves against it, and Q2 sits
    // below.
    CHECK(s.height(q1) == 60.0f);
    CHECK(s.width(q2) == 20.0f);
    CHECK(s.height(q2) == 120.0f);
    CHECK(s.top(q2) == 110.0f);
    // P and Q are each measured once, because the base size's measurement also answers the content suggestion.
    CHECK(s.stats().measuresComputed == 2);

    // P's block grows: P is measured again, and Q's measurement and layout are taken from its caches.
    prLayoutNodeSetStyle(s.f.tree, inner, s.f.style(block(90), nullptr, nullptr, &s.font));
    s.layout();
    CHECK(s.height(pNode) == 50.0f);
    CHECK(s.top(q) == 50.0f);
    CHECK(s.stats().measuresComputed == 1);
    CHECK(s.stats().measuresReused >= 1);
}

TEST_CASE("an item sized to stretch fills its container's cross size less its margins, as alignment would") {
    Stage s;
    PrLayoutNode column = s.add(s.root, flexBox(PrLayoutFlexDirection_Column, 200));
    // A, at width: stretch, fills the column. B also has margins of 10, which the stretch leaves. C is aligned to the
    // start, which a size of stretch overrides.
    PrLayoutBoxStyle a = block(10);
    a.width = prLayoutLengthStretch();
    PrLayoutNode aNode = s.add(column, a);
    PrLayoutBoxStyle b = a;
    b.margin[PrRectSide_Left] = b.margin[PrRectSide_Right] = prLayoutLengthPx(10);
    PrLayoutNode bNode = s.add(column, b);
    Styled c{a, prLayoutFlexStyleDefault()};
    c.second.alignSelf = PrLayoutAlignment_FlexStart;
    PrLayoutNode cNode = s.add(column, c);
    // E, a replaced element with no natural size, fills the column too.
    PrLayoutNode eNode = replaced(s.f, column, a, PrLayoutNaturalSize{});
    // In a row of definite height, D at height: stretch fills the row.
    Styled row = flexBox(PrLayoutFlexDirection_Row, 200);
    row.first.height = prLayoutLengthPx(40);
    PrLayoutNode rowNode = s.add(s.root, row);
    PrLayoutBoxStyle d = block();
    d.width = prLayoutLengthPx(30);
    d.height = prLayoutLengthStretch();
    PrLayoutNode dNode = s.add(rowNode, d);
    s.layout();

    CHECK(s.width(aNode) == 200.0f);
    CHECK(s.width(bNode) == 180.0f);
    CHECK(s.left(bNode) == 10.0f);
    CHECK(s.width(cNode) == 200.0f);
    CHECK(s.width(eNode) == 200.0f);
    CHECK(s.height(dNode) == 40.0f);
}

TEST_CASE("a row's baseline is its first item's at its stretched size, and a baseline-aligned line aligns by it") {
    Stage s;
    Styled outer = flexBox(PrLayoutFlexDirection_Row, 300);
    outer.second.alignItems = PrLayoutAlignment_Baseline;
    PrLayoutNode o = s.add(s.root, outer);
    PrLayoutNode i = s.add(o, flexBox(PrLayoutFlexDirection_Row));
    PrLayoutBoxStyle narrow = block();
    narrow.width = prLayoutLengthPx(50);
    PrLayoutNode sNode = s.add(i, narrow);
    PrLayoutBoxStyle half = block();
    half.height = prLayoutLengthPercent(50);
    PrLayoutNode t = s.add(sNode, half);
    PrLayoutNode line = s.add(sNode, block());
    s.words(line, {30});
    PrLayoutBoxStyle tall = block(60);
    tall.width = prLayoutLengthPx(50);
    s.add(i, tall);
    PrLayoutNode v = s.add(o, block());
    s.words(v, {40});
    s.layout();

    // S is stretched to I's line, 60, in which T is 30 with the word below it. I's baseline is 50, and V's is 20.
    CHECK(s.height(sNode) == 60.0f);
    CHECK(s.height(t) == 30.0f);
    CHECK(s.top(i) == 0.0f);
    CHECK(s.top(v) == 30.0f);
    CHECK(s.height(o) == 60.0f);
    // I is solved for its measurement, and laid out once.
    CHECK(s.stats().flexSolved >= 1);
    CHECK(s.stats().flexLaidOut == 2);

    // V's word widens: I's measurement and layout are taken from its caches.
    s.words(v, {20});
    s.layout();
    CHECK(s.top(v) == 30.0f);
    CHECK(s.stats().flexSolved == 0);
    CHECK(s.stats().flexLaidOut == 1);
}

TEST_CASE("each run of text in a flex container is an anonymous item, and a run of white space is none") {
    TextStage s;
    PrLayoutNode r = s.container(s.root, flexBox(PrLayoutFlexDirection_Row, 300));
    PrLayoutNode a = s.text(r, "XXXX");
    PrLayoutNode b = s.box(r, 30, 40);
    s.text(r, " \n\t ");
    PrLayoutNode c = s.box(r, 20, 10);
    PrLayoutNode d = s.text(r, "XX");
    s.layout();

    // The text before B is 40 wide. The white space between B and C makes no item, and the last text follows C.
    CHECK(s.left(a) == 0.0f);
    CHECK(s.width(a) == 40.0f);
    CHECK(s.left(b) == 40.0f);
    CHECK(s.left(c) == 70.0f);
    CHECK(s.left(d) == 90.0f);
    CHECK(s.width(d) == 20.0f);
    // The line is B's height. The anonymous items stretch to it, and their text sits on their first line.
    CHECK(s.height(r) == 40.0f);
    CHECK(s.top(a) == 0.0f);
    CHECK(s.f.hit(PrVec2{45, 5}) == b);
    CHECK(s.f.hit(PrVec2{95, 5}) == d);

    // A text item shrinks to its widest word, and wraps.
    PrLayoutNode narrow = s.container(s.root, flexBox(PrLayoutFlexDirection_Row, 50));
    PrLayoutNode words = s.text(narrow, "XXX XX XX");
    s.box(narrow, 30, 10);
    s.layout();
    CHECK(s.width(words) == 30.0f);
    CHECK(s.height(words) == 30.0f);
    CHECK(s.height(narrow) == 30.0f);
}

TEST_CASE("a child out of flow ends a run of text, and a column stacks its text items") {
    TextStage s;
    PrLayoutNode r = s.container(s.root, flexBox(PrLayoutFlexDirection_Row, 300));
    PrLayoutNode a = s.text(r, "XX");
    PrLayoutBoxStyle out = block(10);
    out.position = PrLayoutPosition_Absolute;
    out.width = prLayoutLengthPx(10);
    prLayoutNodeCreateElement(s.f.tree, r, s.f.style(out, nullptr, nullptr, &s.ahem));
    PrLayoutNode b = s.text(r, "XXX");
    PrLayoutNode c = s.box(r, 10, 10);
    s.layout();
    CHECK(s.left(a) == 0.0f);
    CHECK(s.left(b) == 20.0f);
    CHECK(s.left(c) == 50.0f);

    PrLayoutNode k = s.container(s.root, flexBox(PrLayoutFlexDirection_Column, 100));
    PrLayoutNode first = s.text(k, "XX XX");
    PrLayoutNode between = s.box(k, 10, 15);
    PrLayoutNode second = s.text(k, "X");
    s.layout();
    CHECK(s.height(k) == 35.0f);
    CHECK(s.top(first) == s.top(k));
    CHECK(s.width(first) == 50.0f);
    CHECK(s.top(between) == s.top(k) + 10.0f);
    CHECK(s.top(second) == s.top(k) + 25.0f);
}

TEST_CASE("an inline-flex box's width holds its text items") {
    TextStage s;
    Styled inlineFlex = flexBox(PrLayoutFlexDirection_Row);
    inlineFlex.first.display = PrLayoutDisplay_InlineFlex;
    PrLayoutNode line = s.add(s.root, block());
    PrLayoutNode r = s.container(line, inlineFlex);
    s.text(r, "XXX XX");
    s.box(r, 30, 10);
    s.layout();
    CHECK(s.width(r) == 90.0f);
}

TEST_CASE("anonymous items follow changes to their runs") {
    TextStage s;
    PrLayoutNode r = s.container(s.root, flexBox(PrLayoutFlexDirection_Row, 300));
    PrLayoutNode a = s.text(r, "XX");
    PrLayoutNode b = s.text(r, "XXX");
    PrLayoutNode e = s.box(r, 10, 10);
    PrLayoutNode c = s.text(r, "X");
    PrLayoutNode d = s.text(r, "XX");
    s.layout();
    CHECK(s.left(e) == 50.0f);
    CHECK(s.left(d) == 70.0f);

    // The second text node of the first run changes.
    prLayoutNodeSetText(s.f.tree, b, "X");
    s.layout();
    CHECK(s.left(e) == 30.0f);

    // The first is removed, and then the last node of the second run.
    prLayoutTreeRemove(s.f.tree, a);
    s.layout();
    CHECK(s.left(e) == 10.0f);
    prLayoutTreeRemove(s.f.tree, d);
    s.layout();
    CHECK(s.width(r) == 300.0f);
    CHECK(s.width(c) == 10.0f);

    // A run of three loses its middle node, and then its nodes are reordered.
    PrLayoutNode x = s.text(r, "XXXX", c);
    PrLayoutNode y = s.text(r, "XXX");
    s.layout();
    CHECK(s.left(y) == 70.0f);
    prLayoutTreeRemove(s.f.tree, c);
    s.layout();
    CHECK(s.left(y) == 60.0f);
    CHECK(s.width(x) == 40.0f);
    prLayoutTreeRemove(s.f.tree, y);
    prLayoutTreeInsert(s.f.tree, r, y, x);
    s.layout();
    CHECK(s.left(y) == 20.0f);
    CHECK(s.left(x) == 50.0f);

    // The element between two runs is removed, and they join. It comes back, and they separate.
    prLayoutTreeRemove(s.f.tree, e);
    s.layout();
    CHECK(s.left(b) == 0.0f);
    CHECK(s.left(y) == 10.0f);
    prLayoutTreeInsert(s.f.tree, r, e, y);
    s.layout();
    CHECK(s.left(e) == 10.0f);
    CHECK(s.left(y) == 20.0f);

    // The element between them stops generating a box, and then is taken out of flow.
    PrLayoutBoxStyle none = block(10);
    none.display = PrLayoutDisplay_None;
    prLayoutNodeSetStyle(s.f.tree, e, s.f.style(none, nullptr, nullptr, &s.ahem));
    s.layout();
    CHECK(s.left(y) == 10.0f);
    PrLayoutBoxStyle out = block(10);
    out.position = PrLayoutPosition_Absolute;
    out.width = prLayoutLengthPx(10);
    prLayoutNodeSetStyle(s.f.tree, e, s.f.style(out, nullptr, nullptr, &s.ahem));
    s.layout();
    CHECK(s.left(y) == 10.0f);

    // The container's font changes, which its items inherit.
    PrLayoutFontStyle larger = s.ahem;
    larger.font = textFontOf(20, {s.f.family("Ahem.ttf", "Ahem")});
    Styled styled = flexBox(PrLayoutFlexDirection_Row, 300);
    prLayoutNodeSetStyle(s.f.tree, r, s.f.style(styled.first, nullptr, nullptr, &larger, &styled.second));
    s.layout();
    CHECK(s.width(b) == 20.0f);
    CHECK(s.left(y) == 20.0f);
}

TEST_CASE("text created last in a flex container joins the run before it, and an element created last ends none") {
    TextStage s;
    PrLayoutNode r = s.container(s.root, flexBox(PrLayoutFlexDirection_Row, 300));
    PrLayoutNode a = prLayoutNodeCreateText(s.f.tree, r, "XX");
    s.layout();
    CHECK(s.width(a) == 20.0f);

    // The new text is in A's anonymous item, on its line after A, and the item is laid out again.
    PrLayoutNode b = prLayoutNodeCreateText(s.f.tree, r, "X");
    s.layout();
    CHECK(s.left(b) == 20.0f);
    CHECK(s.top(b) == s.top(a));

    // The new element follows the run, which keeps its layout.
    PrLayoutNode e = prLayoutNodeCreateElement(s.f.tree, r, s.f.style(block(10), nullptr, nullptr, &s.ahem));
    s.layout();
    CHECK(s.left(e) == 30.0f);
    CHECK(s.stats().inlinesShaped == 0);
}

TEST_CASE("a text item keeps its layout while a sibling's change leaves its requests the same") {
    TextStage s;
    PrLayoutNode r = s.container(s.root, flexBox(PrLayoutFlexDirection_Row, 300));
    PrLayoutNode a = s.text(r, "XX XX");
    PrLayoutNode e = s.box(r, 10, 10);
    PrLayoutNode b = s.text(r, "XXX");
    s.layout();
    CHECK(s.left(b) == 60.0f);

    // E widens: the root, R and E are laid out, and the measurements and layouts of the text items are taken from their
    // caches. E's style fixes its height, so it is not measured.
    PrLayoutBoxStyle wider = block(10);
    wider.width = prLayoutLengthPx(20);
    prLayoutNodeSetStyle(s.f.tree, e, s.f.style(wider, nullptr, nullptr, &s.ahem));
    s.layout();
    CHECK(s.left(b) == 70.0f);
    CHECK(s.width(a) == 50.0f);
    CHECK(s.stats().blocksLaidOut == 3);
    CHECK(s.stats().measuresComputed == 0);
    CHECK(s.stats().measuresReused == 2);
    CHECK(s.stats().inlinesShaped == 0);

    // A's text changes: of R's items, only A is laid out again, measured at its own height and then stretched to the
    // line's height.
    prLayoutNodeSetText(s.f.tree, prLayoutNodeFirstChild(s.f.tree, r), "X");
    s.layout();
    CHECK(s.left(e) == 10.0f);
    CHECK(s.stats().blocksLaidOut == 4);
    CHECK(s.stats().measuresComputed == 1);
    // Its line is broken once, by the measurement, and the stretched layout takes that line.
    CHECK(s.stats().linesLaidOut == 1);
    CHECK(s.stats().linesReused == 1);
    CHECK(s.stats().inlinesShaped == 1);
}

TEST_CASE("a stretched item's lines are broken once, unless something on them reads its height") {
    const PrLayoutWritingMode modes[] = {
        PrLayoutWritingMode_HorizontalTb, PrLayoutWritingMode_VerticalRl,
        PrLayoutWritingMode_VerticalLr,
    };
    for (PrLayoutWritingMode mode : modes) {
        CAPTURE(int(mode));
        // Text, a fixed-size image and a span two inlines deep moved by a length: the stretched layout takes the lines
        // that the measurement broke.
        PrLayoutTreeStats text = stretchedLines(mode, [](TextStage& s, PrLayoutNode a) { s.text(a, "XX XX XX"); });
        CHECK(text.linesLaidOut == 3);
        CHECK(text.linesReused == 3);
        PrLayoutTreeStats image = stretchedLines(mode, [](TextStage& s, PrLayoutNode a) {
            s.text(a, "XX");
            PrLayoutBoxStyle box = inlineBox();
            box.width = prLayoutLengthPx(10);
            box.height = prLayoutLengthPx(10);
            replaced(s.f, a, box, natural(10, 10));
            s.text(a, "XX");
        });
        CHECK(image.linesReused == image.linesLaidOut);
        CHECK(image.linesReused != 0);
        PrLayoutTreeStats moved = stretchedLines(mode, [](TextStage& s, PrLayoutNode a) {
            PrLayoutNode outer = span(s, a, prLayoutLengthPx(10));
            s.text(span(s, outer), "XX XX");
        });
        CHECK(moved.linesReused == 2);

        // An image of a percentage height, a span moved by a percentage, a relatively positioned inline-block moved by
        // one, and a float: each is laid out again at the stretched height.
        PrLayoutTreeStats percent = stretchedLines(mode, [](TextStage& s, PrLayoutNode a) {
            s.text(a, "XX");
            PrLayoutBoxStyle box = inlineBox();
            bool horizontal = s.ahem.writingMode == PrLayoutWritingMode_HorizontalTb;
            (horizontal ? box.width : box.height) = prLayoutLengthPx(10);
            (horizontal ? box.height : box.width) = prLayoutLengthPercent(50);
            replaced(s.f, a, box, natural(10, 10));
        });
        CHECK(percent.linesReused == 0);
        PrLayoutTreeStats spanPercent = stretchedLines(mode, [](TextStage& s, PrLayoutNode a) {
            s.text(span(s, a, prLayoutLengthPercent(50)), "XX");
        });
        CHECK(spanPercent.linesReused == 0);
        PrLayoutTreeStats atomicPercent = stretchedLines(mode, [](TextStage& s, PrLayoutNode a) {
            PrLayoutBoxStyle box = block(10);
            box.display = PrLayoutDisplay_InlineBlock;
            box.width = prLayoutLengthPx(10);
            box.position = PrLayoutPosition_Relative;
            box.inset[blockStartOf(s.ahem.writingMode)] = prLayoutLengthPercent(50);
            prLayoutNodeCreateElement(s.f.tree, a, s.f.style(box, nullptr, nullptr, &s.ahem));
            s.text(a, "XX");
        });
        CHECK(atomicPercent.linesReused == 0);
        PrLayoutTreeStats floated = stretchedLines(mode, [](TextStage& s, PrLayoutNode a) {
            PrLayoutBoxStyle box = block();
            box.floating = PrLayoutFloat_Left;
            box.width = prLayoutLengthPx(10);
            box.height = prLayoutLengthPercent(50);
            prLayoutNodeCreateElement(s.f.tree, a, s.f.style(box, nullptr, nullptr, &s.ahem));
            s.text(a, "XX XX");
        });
        CHECK(floated.linesReused == 0);
    }
}

TEST_CASE("a text block's lines follow an earlier float that grows over them and shrinks back") {
    TextStage s;
    PrLayoutNode container = s.f.element(s.root, block(), nullptr, nullptr, &s.ahem);
    PrLayoutBoxStyle floatBox = block(0);
    floatBox.floating = PrLayoutFloat_Left;
    floatBox.width = prLayoutLengthPx(20);
    PrLayoutNode f = s.f.element(container, floatBox, nullptr, nullptr, &s.ahem);
    PrLayoutNode text = s.f.element(container, block(), nullptr, nullptr, &s.ahem);
    PrLayoutNode words = s.text(text, "XX XX");
    s.layout();
    CHECK(s.left(words) == 0.0f);
    floatBox.height = prLayoutLengthPx(30);
    prLayoutNodeSetStyle(s.f.tree, f, s.f.style(floatBox, nullptr, nullptr, &s.ahem));
    s.layout();
    CHECK(s.left(words) == 20.0f);
    floatBox.height = prLayoutLengthPx(0);
    prLayoutNodeSetStyle(s.f.tree, f, s.f.style(floatBox, nullptr, nullptr, &s.ahem));
    s.layout();
    CHECK(s.left(words) == 0.0f);
}

TEST_CASE("a stretched item's fixed-size image and scroll container are reused, and percentage-sized ones are not") {
    // The item's child, and how many times a layout lays it out as the item is measured and then stretched.
    auto layouts = [](bool scrolls, PrLayoutLength height) {
        TextStage s;
        PrLayoutNode r = s.container(s.root, flexBox(PrLayoutFlexDirection_Row, 300));
        PrLayoutNode a = s.container(r, Styled{block(), prLayoutFlexStyleDefault()});
        PrLayoutBoxStyle child = block();
        child.width = prLayoutLengthPx(20);
        child.height = height;
        if (scrolls) {
            child.overflowX = PrLayoutOverflow_Hidden;
            child.overflowY = PrLayoutOverflow_Hidden;
            PrLayoutNode scroller = s.f.element(a, child, nullptr, nullptr, &s.ahem);
            s.text(scroller, "XX XX");
        } else {
            replaced(s.f, a, child, natural(20, 20));
        }
        s.box(r, 10, 60);
        s.layout();
        // The root, R, A twice, the tall box, and the child once or twice.
        return s.stats().blocksLaidOut - 5;
    };
    CHECK(layouts(false, prLayoutLengthPx(20)) == 1);
    CHECK(layouts(false, prLayoutLengthPercent(50)) == 2);
    CHECK(layouts(true, prLayoutLengthPx(20)) == 1);
    CHECK(layouts(true, prLayoutLengthPercent(50)) == 2);
}

TEST_CASE("the work of laying out a chain of flex containers grows linearly with its nodes") {
    const Chain chains[] = {
        Chain_Columns,    Chain_Alternating, Chain_Wrapped,
        Chain_Percentages, Chain_Fitted,     Chain_Orthogonal, Chain_WrappedColumns,
    };
    for (Chain chain : chains) {
        CAPTURE(int(chain));
        CHECK(linear(chain, [](const Sweep& w) { return w.cold.blocksLaidOut; }));
        CHECK(linear(chain, [](const Sweep& w) { return w.cold.measuresComputed; }));
        CHECK(linear(chain, [](const Sweep& w) { return w.cold.measuresReused; }));
        CHECK(linear(chain, [](const Sweep& w) { return w.cold.flexItemsFlexed; }));
        CHECK(linear(chain, [](const Sweep& w) { return w.change.blocksLaidOut; }));
        CHECK(linear(chain, [](const Sweep& w) { return w.change.measuresComputed; }));
        // No chain asks a box for more measurements than it keeps: a container solved for its size finds no baselines,
        // which would ask its items for their used sizes.
        CHECK(sweep(chain, 32).cold.measuresReplaced == 0);
    }
}

TEST_CASE("justify-content places a line's free space, and its distributed values fall back where the items overflow") {
    using Lefts = std::vector<float>;
    CHECK(justified(PrLayoutAlignment_FlexEnd, {30, 30}) == Lefts{40, 70});
    CHECK(justified(PrLayoutAlignment_Center, {30, 30}) == Lefts{20, 50});
    CHECK(justified(PrLayoutAlignment_SpaceBetween, {30, 30}) == Lefts{0, 70});
    CHECK(justified(PrLayoutAlignment_SpaceAround, {30, 30}) == Lefts{10, 60});
    CHECK(justified(PrLayoutAlignment_SpaceEvenly, {20, 20}) == Lefts{20, 60});
    // One item: space-between acts as flex-start, and space-around centres it.
    CHECK(justified(PrLayoutAlignment_SpaceBetween, {40}) == Lefts{0});
    CHECK(justified(PrLayoutAlignment_SpaceAround, {40}) == Lefts{30});

    // Overflowing by 60: center overflows both ways, and the distributed values fall back to safe flex-start and safe
    // center, which both act as flex-start.
    CHECK(justified(PrLayoutAlignment_Center, {80, 80}) == Lefts{-30, 50});
    CHECK(justified(safe(PrLayoutAlignment_Center), {80, 80}) == Lefts{0, 80});
    CHECK(justified(PrLayoutAlignment_FlexEnd, {80, 80}) == Lefts{-60, 20});
    CHECK(justified(safe(PrLayoutAlignment_FlexEnd), {80, 80}) == Lefts{0, 80});
    CHECK(justified(PrLayoutAlignment_SpaceBetween, {80, 80}) == Lefts{0, 80});
    CHECK(justified(PrLayoutAlignment_SpaceAround, {80, 80}) == Lefts{0, 80});
    CHECK(justified(PrLayoutAlignment_SpaceEvenly, {80, 80}) == Lefts{0, 80});

    // In a reversed row, flex-start is the right, where safe alignment and the fallbacks put what overflows.
    constexpr PrLayoutFlexDirection kReverse = PrLayoutFlexDirection_RowReverse;
    CHECK(justified(PrLayoutAlignment_SpaceBetween, {80, 80}, kReverse) == Lefts{20, -60});
    CHECK(justified(PrLayoutAlignment_SpaceAround, {80, 80}, kReverse) == Lefts{20, -60});
    CHECK(justified(safe(PrLayoutAlignment_Center), {80, 80}, kReverse) == Lefts{20, -60});
    CHECK(justified(safe(PrLayoutAlignment_End), {80, 80}, kReverse) == Lefts{20, -60});
    // start is the container's inline-start, the left, whichever way its items run. left is its line-left.
    CHECK(justified(PrLayoutAlignment_Start, {30, 30}, kReverse) == Lefts{30, 0});
    CHECK(justified(PrLayoutAlignment_End, {30, 30}, kReverse) == Lefts{70, 40});
    CHECK(justified(PrLayoutAlignment_Left, {30, 30}, PrLayoutFlexDirection_Row, 100, PrTextDirection_Rtl) == Lefts{30, 0});
    CHECK(justified(PrLayoutAlignment_Right, {30, 30}, PrLayoutFlexDirection_Row, 100, PrTextDirection_Ltr) == Lefts{40, 70});
}

TEST_CASE("left and right in a column are start, and end is its block-end, but in a vertical column are physical") {
    for (PrLayoutAlignment justify : {PrLayoutAlignment_Right, PrLayoutAlignment_Left, PrLayoutAlignment_End}) {
        Stage s;
        Styled column = flexBox(PrLayoutFlexDirection_Column, 50);
        column.first.height = prLayoutLengthPx(100);
        column.second.justifyContent = justify;
        PrLayoutNode c = s.add(s.root, column);
        Styled item = flexItem(0, 0, prLayoutLengthAuto());
        item.first.height = prLayoutLengthPx(30);
        PrLayoutNode a = s.add(c, item);
        s.layout();
        CHECK(s.top(a) - s.top(c) == (justify == PrLayoutAlignment_End ? 70.0f : 0.0f));
    }
    // A vertical-rl column 100px wide runs right to left: left is its block-end, and right its block-start.
    for (PrLayoutAlignment justify : {PrLayoutAlignment_Left, PrLayoutAlignment_Right}) {
        Stage s;
        PrLayoutFontStyle vertical = s.font;
        vertical.writingMode = PrLayoutWritingMode_VerticalRl;
        Styled column = flexBox(PrLayoutFlexDirection_Column);
        column.first.width = prLayoutLengthPx(100);
        column.first.height = prLayoutLengthPx(50);
        column.second.justifyContent = justify;
        PrLayoutNode c = s.f.element(s.root, column.first, nullptr, nullptr, &vertical, &column.second);
        Styled item = flexItem(0, 0, prLayoutLengthAuto());
        item.first.width = prLayoutLengthPx(30);
        PrLayoutNode a = s.f.element(c, item.first, nullptr, nullptr, &vertical, &item.second);
        s.layout();
        CHECK(s.left(a) - s.left(c) == (justify == PrLayoutAlignment_Left ? 0.0f : 70.0f));
    }
}

TEST_CASE("gaps separate a line's items before its free space is flexed or placed") {
    // Three 20px items 10px apart, with the rest of the space between them.
    std::vector<float> spaced = [] {
        Stage s;
        Styled row = flexBox(PrLayoutFlexDirection_Row, 200);
        row.second.columnGap = prLayoutLengthPx(10);
        row.second.justifyContent = PrLayoutAlignment_SpaceBetween;
        PrLayoutNode r = s.add(s.root, row);
        std::vector<PrLayoutNode> items = fixedItems(s, r, {20, 20, 20});
        s.layout();
        return std::vector<float>{s.left(items[0]), s.left(items[1]), s.left(items[2])};
    }();
    CHECK(spaced == std::vector<float>{0, 90, 180});

    // A gap of 10% of a 200px row leaves 180 to flex. A percentage row-gap of a column of auto height is zero.
    Stage s;
    Styled row = flexBox(PrLayoutFlexDirection_Row, 200);
    row.second.columnGap = prLayoutLengthPercent(10);
    PrLayoutNode r = s.add(s.root, row);
    PrLayoutNode a = s.add(r, flexItem(1, 1, prLayoutLengthPx(0)));
    PrLayoutNode b = s.add(r, flexItem(1, 1, prLayoutLengthPx(0)));
    Styled column = flexBox(PrLayoutFlexDirection_Column, 50);
    column.second.rowGap = prLayoutLengthPercent(20);
    PrLayoutNode c = s.add(s.root, column);
    std::vector<PrLayoutNode> stacked;
    for (int i = 0; i < 2; ++i)
        stacked.push_back(s.add(c, block(48)));
    s.layout();
    CHECK(s.width(a) == 90.0f);
    CHECK(s.left(b) == 110.0f);
    CHECK(s.top(stacked[1]) - s.top(c) == 48.0f);
    CHECK(s.height(c) == 96.0f);
}

TEST_CASE("auto margins take only positive free space, in both axes") {
    Stage s;
    PrLayoutNode r = s.add(s.root, flexBox(PrLayoutFlexDirection_Row, 100));
    Styled wide = flexItem(0, 0, prLayoutLengthAuto());
    wide.first.width = prLayoutLengthPx(150);
    wide.first.height = prLayoutLengthPx(20);
    wide.first.margin[PrRectSide_Left] = prLayoutLengthAuto();
    PrLayoutNode a = s.add(r, wide);
    std::vector<PrLayoutNode> tall;
    for (int sides = 0; sides < 3; ++sides) {
        Styled row = flexBox(PrLayoutFlexDirection_Row, 30);
        row.first.height = prLayoutLengthPx(50);
        PrLayoutNode container = s.add(s.root, row);
        Styled item = flexItem(0, 0, prLayoutLengthAuto());
        item.first.width = prLayoutLengthPx(20);
        item.first.height = prLayoutLengthPx(80);
        if (sides != 1)
            item.first.margin[PrRectSide_Top] = prLayoutLengthAuto();
        if (sides != 0)
            item.first.margin[PrRectSide_Bottom] = prLayoutLengthAuto();
        tall.push_back(s.add(container, item));
        s.add(s.root, block(100));
    }
    s.layout();
    CHECK(s.left(a) == 0.0f);
    // An item that overflows its line is flush with its start, whichever cross margins are auto.
    for (PrLayoutNode item : tall)
        CHECK(s.top(item) == s.top(prLayoutNodeParent(s.f.tree, item)));
}

TEST_CASE("a scroll container's scrollable overflow holds its flex items' margin boxes") {
    // A 120 by 40 scroll container holding an item 160 wide with a margin past its end in the main or cross axis.
    auto overflow = [](PrLayoutFlexDirection direction, PrTextDirection inlineDirection, PrRectSide side) {
        Stage s;
        Styled scroller = flexBox(direction, 120);
        scroller.first.height = prLayoutLengthPx(40);
        scroller.first.overflowX = PrLayoutOverflow_Scroll;
        scroller.first.overflowY = PrLayoutOverflow_Scroll;
        PrLayoutFontStyle font = s.font;
        font.direction = inlineDirection;
        PrLayoutNode container = s.f.element(s.root, scroller.first, nullptr, nullptr, &font, &scroller.second);
        Styled item = flexItem(0, 0, prLayoutLengthAuto());
        item.first.width = prLayoutLengthPx(160);
        item.first.height = prLayoutLengthPx(20);
        item.first.margin[side] = prLayoutLengthPx(40);
        s.add(container, item);
        s.layout();
        PrLayoutScrollInfo info{};
        REQUIRE(prLayoutNodeScrollInfo(s.f.tree, container, &info));
        return PrRect{
            prLayoutUnitToPx(info.content.left), prLayoutUnitToPx(info.content.top),
            prLayoutUnitToPx(info.content.right), prLayoutUnitToPx(info.content.bottom),
        };
    };
    CHECK(overflow(PrLayoutFlexDirection_Row, PrTextDirection_Ltr, PrRectSide_Right).right == 200.0f);
    CHECK(overflow(PrLayoutFlexDirection_Row, PrTextDirection_Ltr, PrRectSide_Bottom).bottom == 60.0f);
    CHECK(overflow(PrLayoutFlexDirection_Column, PrTextDirection_Ltr, PrRectSide_Right).right == 200.0f);
    CHECK(overflow(PrLayoutFlexDirection_Column, PrTextDirection_Ltr, PrRectSide_Bottom).bottom == 60.0f);
    // In a right-to-left row, the item starts at the right and its margin runs past its left.
    CHECK(overflow(PrLayoutFlexDirection_Row, PrTextDirection_Rtl, PrRectSide_Left).left == -80.0f);
    // A reversed row or column starts at its end, which is its scroll origin, so it overflows toward its start and
    // scrolls to negative offsets there (css-overflow-3/#scroll-origin).
    CHECK(overflow(PrLayoutFlexDirection_RowReverse, PrTextDirection_Ltr, PrRectSide_Left).left == -80.0f);
    CHECK(overflow(PrLayoutFlexDirection_RowReverse, PrTextDirection_Rtl, PrRectSide_Right).right == 200.0f);
    CHECK(overflow(PrLayoutFlexDirection_ColumnReverse, PrTextDirection_Ltr, PrRectSide_Top).top == -20.0f);
}

TEST_CASE("align-self places items in the line by their own start and end, and by their last baselines") {
    Stage s;
    // A vertical-rl row 100px wide, whose cross-start is its right: a vertical-lr item's own start is the left. In a
    // 100px-wide column, an rtl item's own start is its right.
    PrLayoutFontStyle verticalRl = s.font;
    verticalRl.writingMode = PrLayoutWritingMode_VerticalRl;
    PrLayoutFontStyle verticalLr = s.font;
    verticalLr.writingMode = PrLayoutWritingMode_VerticalLr;
    PrLayoutFontStyle rtl = s.font;
    rtl.direction = PrTextDirection_Rtl;
    Styled row = flexBox(PrLayoutFlexDirection_Row, 100);
    row.first.height = prLayoutLengthPx(50);
    Styled column = flexBox(PrLayoutFlexDirection_Column, 100);
    PrLayoutNode placed[4];
    PrLayoutNode containers[4];
    for (int i = 0; i < 4; ++i) {
        bool inRow = i % 2 == 0;
        if (inRow)
            containers[i] = s.f.element(s.root, row.first, nullptr, nullptr, &verticalRl, &row.second);
        else
            containers[i] = s.add(s.root, column);
        Styled item = flexItem(0, 0, prLayoutLengthAuto());
        item.first.width = prLayoutLengthPx(30);
        item.first.height = prLayoutLengthPx(20);
        item.second.alignSelf = i < 2 ? PrLayoutAlignment_SelfStart : PrLayoutAlignment_SelfEnd;
        placed[i] = s.f.element(containers[i], item.first, nullptr, nullptr, inRow ? &verticalLr : &rtl, &item.second);
    }
    // A row 50px tall holding an 80px item at end and at safe end.
    PrLayoutNode overflowing[2];
    for (int i = 0; i < 2; ++i) {
        Styled shallow = flexBox(PrLayoutFlexDirection_Row, 30);
        shallow.first.height = prLayoutLengthPx(50);
        PrLayoutNode container = s.add(s.root, shallow);
        Styled item = flexItem(0, 0, prLayoutLengthAuto());
        item.first.width = prLayoutLengthPx(20);
        item.first.height = prLayoutLengthPx(80);
        item.second.alignSelf = i == 0 ? PrLayoutAlignment_End : safe(PrLayoutAlignment_End);
        overflowing[i] = s.add(container, item);
        s.add(s.root, block(50));
    }
    // A row 100px tall aligning a word and two lines of words by their last baselines, which meet at its bottom.
    Styled last = flexBox(PrLayoutFlexDirection_Row);
    last.first.height = prLayoutLengthPx(100);
    last.second.alignItems = PrLayoutAlignment_LastBaseline;
    PrLayoutNode lastRow = s.add(s.root, last);
    Styled forty = flexItem(0, 1, prLayoutLengthAuto());
    forty.first.width = prLayoutLengthPx(40);
    PrLayoutNode one = s.add(lastRow, forty);
    s.words(one, {20});
    PrLayoutNode two = s.add(lastRow, forty);
    s.words(two, {40, 40});
    // A column has no baselines in its cross axis: first baseline falls back to self-start, last to self-end.
    PrLayoutNode fallback = s.add(s.root, flexBox(PrLayoutFlexDirection_Column, 100));
    Styled first = flexItem(0, 0, prLayoutLengthAuto());
    first.first.width = prLayoutLengthPx(30);
    first.second.alignSelf = PrLayoutAlignment_Baseline;
    PrLayoutNode firstItem = s.add(fallback, first);
    s.words(firstItem, {20});
    first.second.alignSelf = PrLayoutAlignment_LastBaseline;
    PrLayoutNode lastItem = s.add(fallback, first);
    s.words(lastItem, {20});
    s.layout();

    CHECK(s.left(placed[0]) - s.left(containers[0]) == 0.0f);
    CHECK(s.left(placed[1]) - s.left(containers[1]) == 70.0f);
    CHECK(s.left(placed[2]) - s.left(containers[2]) == 70.0f);
    CHECK(s.left(placed[3]) - s.left(containers[3]) == 0.0f);
    CHECK(s.top(overflowing[0]) - s.top(prLayoutNodeParent(s.f.tree, overflowing[0])) == -30.0f);
    CHECK(s.top(overflowing[1]) - s.top(prLayoutNodeParent(s.f.tree, overflowing[1])) == 0.0f);
    CHECK(s.top(one) - s.top(lastRow) == 80.0f);
    CHECK(s.top(two) - s.top(lastRow) == 60.0f);
    CHECK(s.left(firstItem) - s.left(fallback) == 0.0f);
    CHECK(s.left(lastItem) - s.left(fallback) == 70.0f);
}

TEST_CASE("a row's intrinsic sizes sum its items' contributions, which their flexibility caps and floors") {
    PrLayoutLength maxContent = prLayoutLengthMaxContent();
    PrLayoutLength minContent = prLayoutLengthMinContent();
    std::vector<float> five(5, 20);
    // I1: flexible items contribute their max-content sizes.
    CHECK(intrinsicWidth(maxContent, {
        {flexItem(1, 1, prLayoutLengthPx(0)), five},
        {flexItem(2, 1, prLayoutLengthPx(0)), five},
    }) == 200.0f);
    // I2: an item that cannot grow is capped by its base, and the base of an item with an auto basis is its content.
    Styled capped = flexItem(0, 1, prLayoutLengthPx(200));
    CHECK(intrinsicWidth(maxContent, {
        {capped, {20, 20, 10}}, {flexItem(0, 0, prLayoutLengthAuto()), {20, 20, 20, 20,}},
    }) == 130.0f);
    // I3: a maximum clamps the contribution.
    Styled clamped = flexItem(0, 0, prLayoutLengthAuto());
    clamped.first.maxWidth = prLayoutLengthPx(60);
    CHECK(intrinsicWidth(maxContent, {{clamped, five}, {flexItem(0, 0, prLayoutLengthAuto()), {20, 20}}}) == 100.0f);
    // I4: min-content contributions.
    CHECK(intrinsicWidth(minContent, {
        {flexItem(1, 1, prLayoutLengthPx(100)), five},
        {flexItem(1, 1, prLayoutLengthPx(100)), {40, 40, 40}},
    }) == 60.0f);
    // I6: a single line's gaps.
    Styled fixed = flexItem(0, 0, prLayoutLengthAuto());
    CHECK(intrinsicWidth(maxContent, {{fixed, {30}}, {fixed, {30}}, {fixed, {30}}}, prLayoutLengthPx(10)) == 110.0f);
    // I10: a preferred size is the contribution, whatever the content's.
    Styled preferred = flexItem(1, 1, prLayoutLengthAuto());
    preferred.first.width = prLayoutLengthPx(50);
    CHECK(intrinsicWidth(maxContent, {{preferred, five}}) == 50.0f);
    // I12: an item that cannot shrink is floored by its base.
    CHECK(intrinsicWidth(maxContent, {{flexItem(0, 0, prLayoutLengthPx(200)), {20, 20, 10}}}) == 200.0f);
    // I11: a preferred size above the content's min-content size is the item's min-content contribution. A preferred
    // size below it is the contribution if the item cannot grow, because the item's automatic minimum is capped by its
    // preferred size.
    Styled unlimited = flexItem(0, 1, prLayoutLengthAuto());
    unlimited.first.width = prLayoutLengthPx(50);
    unlimited.first.minWidth = prLayoutLengthPx(0);
    CHECK(intrinsicWidth(minContent, {{unlimited, {20, 20}}}) == 50.0f);
    Styled narrow = flexItem(0, 0, prLayoutLengthAuto());
    narrow.first.width = prLayoutLengthPx(10);
    CHECK(intrinsicWidth(minContent, {{narrow, {20, 20}}}) == 10.0f);
    // I15: an inflexible item of a content basis is sized under the constraint, its min-content size.
    CHECK(intrinsicWidth(minContent, {{flexItem(0, 0, prLayoutLengthAuto()), {20, 30}}}) == 30.0f);
    // A multi-line row's min-content size is its largest contribution, with no gap.
    Stage s;
    Styled wrapping = flexBox(PrLayoutFlexDirection_Row);
    wrapping.first.width = minContent;
    wrapping.second.wrap = PrLayoutFlexWrap_Wrap;
    wrapping.second.columnGap = prLayoutLengthPx(100);
    PrLayoutNode r = s.add(s.root, wrapping);
    for (int i = 0; i < 2; ++i)
        s.words(s.add(r, flexItem(0, 1, prLayoutLengthAuto())), {40});
    s.layout();
    CHECK(s.width(r) == 40.0f);
}

TEST_CASE("flex layout is logical: rows and columns in rtl and the vertical modes") {
    Stage s;
    // An rtl row: the first item at the right.
    PrLayoutFontStyle rtl = s.font;
    rtl.direction = PrTextDirection_Rtl;
    Styled row = flexBox(PrLayoutFlexDirection_Row, 300);
    PrLayoutNode r = s.f.element(s.root, row.first, nullptr, nullptr, &rtl, &row.second);
    std::vector<PrLayoutNode> rtlItems = fixedItems(s, r, {50, 50});
    // A vertical-rl row 300px tall: items run down, and flex-start in the cross axis is the right.
    PrLayoutFontStyle verticalRl = s.font;
    verticalRl.writingMode = PrLayoutWritingMode_VerticalRl;
    Styled vertical = flexBox(PrLayoutFlexDirection_Row);
    vertical.first.width = prLayoutLengthPx(100);
    vertical.first.height = prLayoutLengthPx(300);
    vertical.second.alignItems = PrLayoutAlignment_FlexStart;
    PrLayoutNode v = s.f.element(s.root, vertical.first, nullptr, nullptr, &verticalRl, &vertical.second);
    Styled grows = flexItem(1, 1, prLayoutLengthPx(0));
    grows.first.width = prLayoutLengthPx(20);
    PrLayoutNode down[2];
    for (PrLayoutNode& item : down)
        item = s.f.element(v, grows.first, nullptr, nullptr, &verticalRl, &grows.second);
    // A vertical-lr column: items run left to right.
    PrLayoutFontStyle verticalLr = s.font;
    verticalLr.writingMode = PrLayoutWritingMode_VerticalLr;
    Styled across = flexBox(PrLayoutFlexDirection_Column);
    across.first.height = prLayoutLengthPx(100);
    PrLayoutNode c = s.f.element(s.root, across.first, nullptr, nullptr, &verticalLr, &across.second);
    Styled fifty = flexItem(0, 0, prLayoutLengthAuto());
    fifty.first.width = prLayoutLengthPx(50);
    PrLayoutNode right[2];
    for (PrLayoutNode& item : right)
        item = s.f.element(c, fifty.first, nullptr, nullptr, &verticalLr, &fifty.second);
    s.layout();

    CHECK(s.left(rtlItems[0]) - s.left(r) == 250.0f);
    CHECK(s.left(rtlItems[1]) - s.left(r) == 200.0f);
    CHECK(s.height(down[0]) == 150.0f);
    CHECK(s.top(down[1]) - s.top(v) == 150.0f);
    CHECK(s.left(down[0]) - s.left(v) == 80.0f);
    CHECK(s.left(right[0]) - s.left(c) == 0.0f);
    CHECK(s.left(right[1]) - s.left(c) == 50.0f);
    CHECK(s.width(c) == 100.0f);
}

TEST_CASE("what a container reads of its item marks the container, and the item is laid out where its request changes") {
    Stage s;
    PrLayoutNode r = s.add(s.root, flexBox(PrLayoutFlexDirection_Row, 300));
    Styled itemStyle = flexItem(0, 1, prLayoutLengthAuto());
    itemStyle.first.width = prLayoutLengthPx(100);
    PrLayoutNode a = s.add(r, itemStyle);
    s.words(a, {40, 40});
    PrLayoutNode b = s.add(r, itemStyle);
    s.words(b, {40, 40});
    s.layout();

    // A's order moves it after B: neither's request changes, so neither is laid out; the root and R are, the root
    // taking R's other siblings from the cache.
    Styled moved = itemStyle;
    moved.second.order = 1;
    prLayoutNodeSetStyle(s.f.tree, a, s.style(moved));
    s.layout();
    CHECK(s.left(b) == 0.0f);
    CHECK(s.left(a) == 100.0f);
    CHECK(s.stats().blocksLaidOut == 2);
    CHECK(s.stats().measuresComputed == 0);

    // A grows: its width changes, so it is measured at it and laid out stretched, and B is not laid out.
    moved.second.grow = 1;
    prLayoutNodeSetStyle(s.f.tree, a, s.style(moved));
    s.layout();
    CHECK(s.width(a) == 200.0f);
    CHECK(s.stats().blocksLaidOut == 2 + 2);

    // B's align-self changes from stretch to center: its measurement is a hit, and its final layout, at its own height,
    // is the one item laid out.
    Styled centred = itemStyle;
    centred.second.alignSelf = PrLayoutAlignment_Center;
    prLayoutNodeSetStyle(s.f.tree, b, s.style(centred));
    s.layout();
    CHECK(s.stats().blocksLaidOut == 2 + 1);
    CHECK(s.stats().measuresComputed == 0);

    // The container's own flex properties are its own layout's input.
    Styled justified = flexBox(PrLayoutFlexDirection_Row, 300);
    justified.second.justifyContent = PrLayoutAlignment_FlexEnd;
    prLayoutNodeSetStyle(s.f.tree, r, s.style(justified));
    s.layout();
    CHECK(s.left(b) == 0.0f);
    CHECK(s.stats().blocksLaidOut == 2);
}

TEST_CASE("flexed sizes that divide unevenly sum to the line, in order, and come back as they were") {
    Stage s;
    PrLayoutNode r = s.add(s.root, flexBox(PrLayoutFlexDirection_Row, 100));
    std::vector<PrLayoutNode> items;
    for (float grow : {1.0f, 2.0f, 1.0f})
        items.push_back(s.add(r, flexItem(grow, 1, prLayoutLengthPx(0))));
    s.layout();
    // 25, 50 and 25 of 100 divide evenly. At 99px, a quarter is 24.75px, which is not a whole number of units.
    std::vector<float> even = flexedEdges(s, r, items);
    CHECK(even == std::vector<float>{0, 25, 75, 100});
    prLayoutNodeSetStyle(s.f.tree, r, s.style(flexBox(PrLayoutFlexDirection_Row, 99.3f)));
    s.layout();
    std::vector<float> uneven = flexedEdges(s, r, items);
    // Each edge is within a unit of the exact sums, and the last item ends at the row's end.
    float unit = 1.0f / float(kPrLayoutUnitsPerPx);
    const float exact[] = {0, 99.3f / 4, 99.3f * 3 / 4};
    for (size_t i = 0; i < 3; ++i)
        CHECK(std::abs(uneven[i] - exact[i]) <= unit);
    CHECK(std::abs(uneven[3] - 99.3f) <= unit / 2);
    CHECK(uneven[0] < uneven[1]);
    CHECK(uneven[1] < uneven[2]);
    // The row resized away and back has its items where they were.
    prLayoutNodeSetStyle(s.f.tree, r, s.style(flexBox(PrLayoutFlexDirection_Row, 100)));
    s.layout();
    CHECK(flexedEdges(s, r, items) == even);

    // Shrinking unevenly: three items of 70px basis shrinking 1, 1 and 2 by 110px of 100px.
    Stage t;
    PrLayoutNode shrinking = t.add(t.root, flexBox(PrLayoutFlexDirection_Row, 100));
    std::vector<PrLayoutNode> shrunk;
    for (float shrink : {1.0f, 1.0f, 2.0f}) {
        Styled item = flexItem(0, shrink, prLayoutLengthPx(70));
        item.first.minWidth = prLayoutLengthPx(0);
        shrunk.push_back(t.add(shrinking, item));
    }
    t.layout();
    std::vector<float> edges = flexedEdges(t, shrinking, shrunk);
    CHECK(std::abs(edges[1] - 42.5f) <= unit);
    CHECK(std::abs(edges[2] - 85.0f) <= unit);
    CHECK(edges[3] == 100.0f);
}

TEST_CASE("the freeze loop freezes the items that violate their limits, and redistributes the rest") {
    // A with a max of 30, B with a min of 120, and C with neither, each flex 1 1 0 in 200px. The violations total
    // positive, so only B freezes, at 120. Then A is over its max and freezes at 30, and C takes the 50 that is left.
    Stage s;
    PrLayoutNode r = s.add(s.root, flexBox(PrLayoutFlexDirection_Row, 200));
    Styled a = flexItem(1, 1, prLayoutLengthPx(0));
    a.first.maxWidth = prLayoutLengthPx(30);
    Styled b = flexItem(1, 1, prLayoutLengthPx(0));
    b.first.minWidth = prLayoutLengthPx(120);
    std::vector<PrLayoutNode> items{s.add(r, a), s.add(r, b), s.add(r, flexItem(1, 1, prLayoutLengthPx(0)))};
    s.layout();
    CHECK(flexedEdges(s, r, items) == std::vector<float>{0, 30, 150, 200});
    // Three rounds of three items: B frozen, then A, then C with no violation.
    CHECK(s.stats().flexItemsFlexed == 9);

    // Items whose maximums are each below the share of the free space left once the narrower ones froze, but no other:
    // one item freezes per round, from the narrowest.
    Stage t;
    PrLayoutNode line = t.add(t.root, flexBox(PrLayoutFlexDirection_Row, 400));
    std::vector<PrLayoutNode> capped;
    for (float max : {10.0f, 85.0f, 100.0f, 102.0f}) {
        Styled item = flexItem(1, 1, prLayoutLengthPx(0));
        item.first.maxWidth = prLayoutLengthPx(max);
        capped.push_back(t.add(line, item));
    }
    capped.push_back(t.add(line, flexItem(1, 1, prLayoutLengthPx(0))));
    t.layout();
    CHECK(flexedEdges(t, line, capped) == std::vector<float>{0, 10, 95, 195, 297, 400});
    // Five rounds of five items, each freezing one.
    CHECK(t.stats().flexItemsFlexed == 25);
}

TEST_CASE("an orthogonal item is main-block in a row and main-inline in a column") {
    Stage s;
    PrLayoutFontStyle vertical = s.font;
    vertical.writingMode = PrLayoutWritingMode_VerticalRl;
    auto item = [&](PrLayoutNode parent, const Styled& styled) {
        PrLayoutNode node = s.f.element(parent, styled.first, nullptr, nullptr, &vertical, &styled.second);
        s.words(node, std::vector<float>(6, 20));
        return node;
    };
    Styled initial = flexItem(0, 1, prLayoutLengthAuto());
    // In an auto-height row beside a 60px item, its main size is its line's 20, and it stretches to the line, which its
    // words fill, 120 long in the fallback.
    PrLayoutNode beside = s.add(s.root, flexBox(PrLayoutFlexDirection_Row, 300));
    PrLayoutNode o1 = item(beside, initial);
    Styled tall = flexItem(0, 0, prLayoutLengthAuto());
    tall.first.width = prLayoutLengthPx(50);
    tall.first.height = prLayoutLengthPx(60);
    s.add(beside, tall);
    // In a row 50px tall, at flex-start: its words fit the row's height, three lines of two words.
    Styled shallow = flexBox(PrLayoutFlexDirection_Row, 300);
    shallow.first.height = prLayoutLengthPx(50);
    shallow.second.alignItems = PrLayoutAlignment_FlexStart;
    PrLayoutNode o2 = item(s.add(s.root, shallow), initial);
    // In a 100px column of auto height, stretched: the item is main-inline, its height is its line's, and its width is
    // the column's.
    PrLayoutNode o4 = item(s.add(s.root, flexBox(PrLayoutFlexDirection_Column, 100)), initial);
    // In a 100px column 60px tall, from a basis of 0 with no minimum: its words fall in three lines of 60.
    Styled column = flexBox(PrLayoutFlexDirection_Column, 100);
    column.first.height = prLayoutLengthPx(60);
    Styled flexible = flexItem(1, 1, prLayoutLengthPx(0));
    flexible.first.minHeight = prLayoutLengthPx(0);
    PrLayoutNode o5 = item(s.add(s.root, column), flexible);
    s.layout();

    CHECK(s.width(o1) == 20.0f);
    CHECK(s.height(o1) == 120.0f);
    CHECK(s.width(o2) == 60.0f);
    CHECK(s.height(o2) == 50.0f);
    CHECK(s.width(o4) == 100.0f);
    CHECK(s.height(o4) == 120.0f);
    CHECK(s.width(o5) == 100.0f);
    CHECK(s.height(o5) == 60.0f);
}

TEST_CASE("a column's item is measured at its cross size, and its lines are broken once") {
    Stage s;
    PrLayoutNode c = s.add(s.root, flexBox(PrLayoutFlexDirection_Column, 200));
    Styled centred = flexItem(0, 1, prLayoutLengthAuto());
    centred.first.width = prLayoutLengthPx(80);
    centred.second.alignSelf = PrLayoutAlignment_Center;
    PrLayoutNode a = s.add(c, centred);
    s.words(a, std::vector<float>(6, 20));
    Styled percent = flexItem(0, 1, prLayoutLengthAuto());
    percent.first.width = prLayoutLengthPercent(30);
    PrLayoutNode b = s.add(c, percent);
    s.words(b, std::vector<float>(6, 20));
    s.layout();
    CHECK(s.left(a) - s.left(c) == 60.0f);
    CHECK(s.width(a) == 80.0f);
    CHECK(s.height(a) == 40.0f);
    CHECK(s.width(b) == 60.0f);
    CHECK(s.height(b) == 40.0f);
    // Each is measured at its width for its content's height, and laid out at that height on the same lines.
    CHECK(s.stats().linesBroken == 4);
    CHECK(s.stats().linesReused == 4);
}

TEST_CASE("a replaced item of auto width in a column takes its width from its flexed height") {
    Stage s;
    Styled column = flexBox(PrLayoutFlexDirection_Column);
    column.first.height = prLayoutLengthPx(100);
    column.second.alignItems = PrLayoutAlignment_FlexStart;
    PrLayoutNode c = s.add(s.root, column);
    Styled grows = flexItem(1, 1, prLayoutLengthAuto());
    grows.first.minHeight = prLayoutLengthPx(0);
    PrLayoutNode image = prLayoutNodeCreateReplaced(s.f.tree, c, s.style(grows));
    prLayoutNodeSetNaturalSize(s.f.tree, image, natural(80, 40));
    // Stretched, its width is the column's, whatever its height.
    Styled stretched = flexBox(PrLayoutFlexDirection_Column, 50);
    stretched.first.height = prLayoutLengthPx(100);
    PrLayoutNode d = s.add(s.root, stretched);
    PrLayoutNode wide = prLayoutNodeCreateReplaced(s.f.tree, d, s.style(grows));
    prLayoutNodeSetNaturalSize(s.f.tree, wide, natural(80, 40));
    s.layout();
    CHECK(s.width(image) == 200.0f);
    CHECK(s.height(image) == 100.0f);
    CHECK(s.width(wide) == 50.0f);
    CHECK(s.height(wide) == 100.0f);
}

TEST_CASE("a multi-line row collects its items into lines, which wrap-reverse stacks from the cross axis's end") {
    // Three 40px items in 100px: two on a line, then one, on lines 20 tall.
    Wrapped w(3, 100, -1);
    w.s.layout();
    CHECK(w.left(2) == 0.0f);
    CHECK(w.top(2) == 20.0f);
    CHECK(w.s.height(w.container) == 40.0f);
    Wrapped reverse(3, 100, -1, PrLayoutAlignment_Normal, PrLayoutFlexWrap_WrapReverse);
    reverse.s.layout();
    CHECK(reverse.top(0) == 20.0f);
    CHECK(reverse.top(2) == 0.0f);
    // In a 100px-tall wrap-reverse row, flex-start packs the lines at the bottom.
    Wrapped packed(3, 100, 100, PrLayoutAlignment_FlexStart, PrLayoutFlexWrap_WrapReverse);
    packed.s.layout();
    CHECK(packed.top(0) == 80.0f);
    CHECK(packed.top(2) == 60.0f);
    // A max-height breaks nothing in a row, and clamps its height.
    Styled capped = flexBox(PrLayoutFlexDirection_Row, 100);
    capped.first.maxHeight = prLayoutLengthPx(30);
    capped.second.wrap = PrLayoutFlexWrap_Wrap;
    Stage s;
    PrLayoutNode r = s.add(s.root, capped);
    std::vector<PrLayoutNode> items = fixedItems(s, r, {40, 40, 40});
    s.layout();
    CHECK(s.height(r) == 30.0f);
    CHECK(s.top(items[2]) - s.top(r) == 20.0f);
}

TEST_CASE("align-content centers a block container's content, past its start only if the alignment is unsafe") {
    for (bool unsafe : {false, true}) {
        CAPTURE(unsafe);
        Stage s;
        Styled box{block(), prLayoutFlexStyleDefault()};
        box.first.display = PrLayoutDisplay_FlowRoot;
        box.first.height = prLayoutLengthPx(10);
        box.second.alignContent = PrLayoutAlignment(PrLayoutAlignment_Center | (unsafe ? PrLayoutAlignmentFlag_Unsafe : 0));
        PrLayoutNode container = s.add(s.root, box);
        Styled child{block(), prLayoutFlexStyleDefault()};
        child.first.height = prLayoutLengthPx(30);
        PrLayoutNode content = s.add(container, child);
        s.layout();
        CHECK(s.top(content) - s.top(container) == (unsafe ? -10 : 0));
    }
}

TEST_CASE("align-content places a multi-line container's lines, and stretches them to its cross size") {
    struct Expected {
        PrLayoutAlignment value;
        float firstTop;
        float thirdTop;
    };
    // Lines of 20 in 100 leave 60px free.
    const Expected cases[] = {
        {PrLayoutAlignment_FlexStart, 0, 20},    {PrLayoutAlignment_FlexEnd, 60, 80},
        {PrLayoutAlignment_Center, 30, 50},      {PrLayoutAlignment_SpaceBetween, 0, 80},
        {PrLayoutAlignment_SpaceAround, 15, 65}, {PrLayoutAlignment_SpaceEvenly, 20, 60},
        {PrLayoutAlignment_Start, 0, 20},        {PrLayoutAlignment_End, 60, 80},
    };
    for (const Expected& expected : cases) {
        CAPTURE(int(expected.value));
        Wrapped w(3, 100, 100, expected.value);
        w.s.layout();
        CHECK(w.top(0) == expected.firstTop);
        CHECK(w.top(2) == expected.thirdTop);
    }
    // Stretch, and normal, which behaves as stretch: lines of 50, with their auto-height items stretched to them.
    for (PrLayoutAlignment value : {PrLayoutAlignment_Stretch, PrLayoutAlignment_Normal}) {
        Wrapped w(3, 100, 100, value, PrLayoutFlexWrap_Wrap, -1);
        w.s.layout();
        CHECK(w.top(2) == 50.0f);
        CHECK(w.s.height(w.items[2]) == 50.0f);
    }
    // A cross size that its min-height gives is also stretched to.
    Stage s;
    Styled row = flexBox(PrLayoutFlexDirection_Row, 100);
    row.first.minHeight = prLayoutLengthPx(100);
    row.second.wrap = PrLayoutFlexWrap_Wrap;
    PrLayoutNode r = s.add(s.root, row);
    std::vector<PrLayoutNode> items;
    for (int i = 0; i < 3; ++i) {
        Styled item = flexItem(0, 1, prLayoutLengthAuto());
        item.first.width = prLayoutLengthPx(40);
        items.push_back(s.add(r, item));
    }
    s.layout();
    CHECK(s.height(items[0]) == 50.0f);
    CHECK(s.top(items[2]) - s.top(r) == 50.0f);
    // Lines that overflow: center overflows both ways, and safe center is flex-start.
    Wrapped centred(2, 100, 30, PrLayoutAlignment_Center, PrLayoutFlexWrap_Wrap, 20, 80);
    centred.s.layout();
    CHECK(centred.top(0) == -5.0f);
    Wrapped safely(2, 100, 30, safe(PrLayoutAlignment_Center), PrLayoutFlexWrap_Wrap, 20, 80);
    safely.s.layout();
    CHECK(safely.top(0) == 0.0f);
}

TEST_CASE("gaps separate lines, and each line flexes its own items") {
    Stage s;
    Styled row = flexBox(PrLayoutFlexDirection_Row, 100);
    row.second.wrap = PrLayoutFlexWrap_Wrap;
    row.second.rowGap = prLayoutLengthPx(10);
    row.second.columnGap = prLayoutLengthPx(20);
    PrLayoutNode r = s.add(s.root, row);
    std::vector<PrLayoutNode> items = fixedItems(s, r, {40, 40, 40});
    // Four items of flex 1 1 30px in 100px: three on a line, each a third, and the fourth alone, taking the whole line.
    Styled flexible = flexBox(PrLayoutFlexDirection_Row, 100);
    flexible.second.wrap = PrLayoutFlexWrap_Wrap;
    PrLayoutNode g = s.add(s.root, flexible);
    std::vector<PrLayoutNode> grown;
    for (int i = 0; i < 4; ++i) {
        Styled item = flexItem(1, 1, prLayoutLengthPx(30));
        item.first.height = prLayoutLengthPx(20);
        grown.push_back(s.add(g, item));
    }
    s.layout();
    CHECK(s.left(items[1]) - s.left(r) == 60.0f);
    CHECK(s.top(items[2]) - s.top(r) == 30.0f);
    CHECK(s.height(r) == 50.0f);
    CHECK(std::abs(s.width(grown[0]) - 100.0f / 3) <= 1.0f / 64);
    CHECK(s.width(grown[3]) == 100.0f);
    CHECK(s.top(grown[3]) - s.top(g) == 20.0f);
}

TEST_CASE("a wrapping column's lines are as wide as their widest items, and its width as its lines") {
    Stage s;
    // A column 70px tall of items 30 tall: two lines, the first of words 20 and 40, and the second of 60.
    Styled column = flexBox(PrLayoutFlexDirection_Column);
    column.first.width = prLayoutLengthPx(300);
    column.first.height = prLayoutLengthPx(70);
    column.second.wrap = PrLayoutFlexWrap_Wrap;
    column.second.alignContent = PrLayoutAlignment_FlexStart;
    PrLayoutNode c = s.add(s.root, column);
    std::vector<PrLayoutNode> items;
    for (float word : {20.0f, 40.0f, 60.0f}) {
        Styled item = flexItem(0, 1, prLayoutLengthAuto());
        item.first.height = prLayoutLengthPx(30);
        items.push_back(s.add(c, item));
        s.words(items.back(), {word});
    }
    // The same column, sized by its max-content width: its lines' widths.
    Styled fitted = column;
    fitted.first.width = prLayoutLengthMaxContent();
    PrLayoutNode d = s.add(s.root, fitted);
    for (float word : {20.0f, 40.0f, 60.0f}) {
        Styled item = flexItem(0, 1, prLayoutLengthAuto());
        item.first.height = prLayoutLengthPx(30);
        s.words(s.add(d, item), {word});
    }
    s.layout();
    // Stretched to its line: the first item is as wide as the second.
    CHECK(s.width(items[0]) == 40.0f);
    CHECK(s.width(items[1]) == 40.0f);
    CHECK(s.left(items[2]) - s.left(c) == 40.0f);
    CHECK(s.width(items[2]) == 60.0f);
    CHECK(s.width(d) == 100.0f);
}

TEST_CASE("a wrapping column's max-content width follows the limit its parent's height gives it") {
    // A max-content box 200px tall holding a wrapping column of max-height 100% and two items 100px tall and 50 wide:
    // one line, 50 wide. With the box then 100px tall: two lines, 100 wide.
    Stage s;
    PrLayoutBoxStyle target = block(200);
    target.width = prLayoutLengthMaxContent();
    PrLayoutNode t = s.add(s.root, target);
    Styled column = flexBox(PrLayoutFlexDirection_Column);
    column.second.wrap = PrLayoutFlexWrap_Wrap;
    column.first.maxHeight = prLayoutLengthPercent(100);
    PrLayoutNode c = s.add(t, column);
    for (int i = 0; i < 2; ++i) {
        Styled item = flexItem(0, 1, prLayoutLengthAuto());
        item.first.minWidth = prLayoutLengthPx(50);
        item.first.minHeight = prLayoutLengthPx(100);
        s.add(c, item);
    }
    s.layout();
    CHECK(s.width(t) == 50.0f);
    target.height = prLayoutLengthPx(100);
    prLayoutNodeSetStyle(s.f.tree, t, s.style(Styled{target, prLayoutFlexStyleDefault()}));
    s.layout();
    CHECK(s.width(t) == 100.0f);
}

TEST_CASE("a container's baselines are its startmost line's and item's, synthesized from its border box where it has none") {
    Styled row = flexBox(PrLayoutFlexDirection_Row);
    row.second.alignItems = PrLayoutAlignment_FlexStart;
    // The first item's border box ends 10 down, so the row's top is 10 below the box's.
    CHECK(baselineTop(row, {10, 30}) == 10.0f);
    // In a row-reverse, the second item is inline-startmost.
    Styled reverse = row;
    reverse.second.direction = PrLayoutFlexDirection_RowReverse;
    CHECK(baselineTop(reverse, {10, 30}) == 0.0f);
    // wrap-reverse puts the second line, of the 30px item, at the top.
    Styled wrapped = row;
    wrapped.first.width = prLayoutLengthPx(30);
    wrapped.second.wrap = PrLayoutFlexWrap_WrapReverse;
    CHECK(baselineTop(wrapped, {10, 30}) == 0.0f);
    // A column's block-startmost item, and a column-reverse's.
    CHECK(baselineTop(flexBox(PrLayoutFlexDirection_Column, 30), {10, 30}) == 10.0f);
    CHECK(baselineTop(flexBox(PrLayoutFlexDirection_ColumnReverse, 30), {10, 30}) == 0.0f);
}

TEST_CASE("an inline-flex box aligns by its first baseline on a line, and an inline-block by its last") {
    Stage s;
    PrLayoutFontStyle text = s.font;
    text.font = textFontOf(20);
    text.lineHeight = prLayoutLineHeightPx(40);
    PrLayoutNode paragraph = s.f.element(s.root, block(), nullptr, nullptr, &text);
    s.f.text(paragraph, "x");
    // Its first item, 30 tall, has no baseline, and its second, 20 tall, has one. An inline-flex box aligns by the
    // first item's baseline, synthesized at 30, and an inline-block holding the same blocks aligns by the second's.
    PrLayoutNode boxes[2];
    for (int i = 0; i < 2; ++i) {
        PrLayoutBoxStyle box = prLayoutBoxStyleDefault();
        box.display = i == 0 ? PrLayoutDisplay_InlineFlex : PrLayoutDisplay_InlineBlock;
        boxes[i] = s.f.element(paragraph, box, nullptr, nullptr, &text);
        PrLayoutBoxStyle empty = block(30);
        empty.width = prLayoutLengthPx(20);
        s.add(boxes[i], empty);
        PrLayoutFontStyle word = text;
        word.lineHeight = prLayoutLineHeightPx(20);
        PrLayoutNode second = s.f.element(boxes[i], block(), nullptr, nullptr, &word);
        s.f.text(second, "y");
    }
    s.layout();
    // On one baseline, the inline-flex box's baseline is 30 down, and the inline-block's is its last line's, past 30
    // and the line's ascent. The inline-flex box is therefore lower by that ascent.
    CHECK(s.top(boxes[0]) - s.top(boxes[1]) > 10.0f);
}

TEST_CASE("a child out of flow is placed statically as the container's sole item would be") {
    auto with = [](PrLayoutFlexDirection direction, PrLayoutAlignment justify, PrLayoutAlignment align,
            PrLayoutFlexWrap wrap = PrLayoutFlexWrap_NoWrap) {
        Styled styled = flexBox(direction);
        styled.second.justifyContent = justify;
        styled.second.alignItems = align;
        styled.second.wrap = wrap;
        return styled;
    };
    constexpr PrLayoutAlignment kNormal = PrLayoutAlignment_Normal;
    CHECK(staticOffset(with(PrLayoutFlexDirection_Row, PrLayoutAlignment_Center, kNormal)).x == 40.0f);
    CHECK(staticOffset(with(PrLayoutFlexDirection_Row, PrLayoutAlignment_FlexEnd, kNormal)).x == 80.0f);
    CHECK(staticOffset(with(PrLayoutFlexDirection_RowReverse, kNormal, kNormal)).x == 80.0f);
    CHECK(staticOffset(with(PrLayoutFlexDirection_Row, PrLayoutAlignment_SpaceAround, kNormal)).x == 40.0f);
    PrVec2 column =
        staticOffset(with(PrLayoutFlexDirection_Column, PrLayoutAlignment_FlexEnd, PrLayoutAlignment_Center));
    CHECK(column.x == 40.0f);
    CHECK(column.y == 80.0f);
    // wrap-reverse makes cross-start the bottom, and align-content is not read.
    Styled reversed =
        with(PrLayoutFlexDirection_Row, kNormal, PrLayoutAlignment_FlexStart, PrLayoutFlexWrap_WrapReverse);
    reversed.second.alignContent = PrLayoutAlignment_Center;
    CHECK(staticOffset(reversed).y == 80.0f);
    // align-self, and in rtl, flex-start is the right.
    Styled centred = flexItem(0, 1, prLayoutLengthAuto());
    centred.second.alignSelf = PrLayoutAlignment_Center;
    CHECK(staticOffset(with(PrLayoutFlexDirection_Row, kNormal, kNormal), centred).y == 40.0f);
    CHECK(staticOffset(with(PrLayoutFlexDirection_Row, kNormal, kNormal), flexItem(0, 1, prLayoutLengthAuto()),
        PrTextDirection_Rtl) .x == 80.0f);
    // A centered box wider than the container overflows it on both sides. A box with an inset is placed by the inset.
    Styled wide = flexItem(0, 1, prLayoutLengthAuto());
    wide.first.width = prLayoutLengthPx(200);
    CHECK(staticOffset(with(PrLayoutFlexDirection_Row, PrLayoutAlignment_Center, kNormal), wide).x == -50.0f);
    Styled inset = flexItem(0, 1, prLayoutLengthAuto());
    inset.first.inset[PrRectSide_Left] = prLayoutLengthPx(5);
    CHECK(staticOffset(with(PrLayoutFlexDirection_Row, PrLayoutAlignment_Center, kNormal), inset).x == 5.0f);
}

TEST_CASE("a collapsed item takes no room in its line, which is at least as tall as its strut") {
    // C1: a 200px row of a 20px item, a collapsed 60px item and a 20px item, each 20 wide except the second, which is
    // 50. The line is the collapsed item's 60, and the third item follows the first.
    {
        Stage s;
        PrLayoutNode r = s.add(s.root, flexBox(PrLayoutFlexDirection_Row, 200));
        Styled first = flexItem(0, 0, prLayoutLengthAuto());
        first.first.width = prLayoutLengthPx(50);
        PrLayoutNode a = s.add(r, first);
        PrLayoutNode b = s.add(r, collapsible(50, 60, true));
        PrLayoutNode c = s.add(r, collapsible(50, 20, false));
        s.layout();
        CHECK(s.height(r) == 60.0f);
        CHECK(s.height(a) == 60.0f);
        CHECK(s.left(c) - s.left(r) == 50.0f);
        CHECK(boxCount(s, b) == 0);
        CHECK(s.width(b) == 0.0f);
    }
    // C2: a wrapping 100px row of 40, a collapsed 60 and 60: one line, 20 tall, with the third at 40.
    {
        Stage s;
        Styled row = flexBox(PrLayoutFlexDirection_Row, 100);
        row.second.wrap = PrLayoutFlexWrap_Wrap;
        PrLayoutNode r = s.add(s.root, row);
        s.add(r, collapsible(40, 20, false));
        s.add(r, collapsible(60, 20, true));
        PrLayoutNode c = s.add(r, collapsible(60, 20, false));
        s.layout();
        CHECK(s.height(r) == 20.0f);
        CHECK(s.top(c) - s.top(r) == 0.0f);
        CHECK(s.left(c) - s.left(r) == 40.0f);
    }
    // C3: a wrapping row whose two items are both collapsed is as tall as their strut.
    {
        Stage s;
        Styled row = flexBox(PrLayoutFlexDirection_Row, 200);
        row.second.wrap = PrLayoutFlexWrap_Wrap;
        PrLayoutNode r = s.add(s.root, row);
        s.add(r, collapsible(50, 20, true));
        s.add(r, collapsible(50, 20, true));
        s.layout();
        CHECK(s.height(r) == 20.0f);
    }
    // C4: an inline-flex box's width leaves a collapsed item out, and its height keeps the item's strut.
    {
        Stage s;
        Styled inlineFlex = flexBox(PrLayoutFlexDirection_Row);
        inlineFlex.first.display = PrLayoutDisplay_InlineFlex;
        PrLayoutNode r = s.add(s.root, inlineFlex);
        s.add(r, collapsible(50, 20, false));
        s.add(r, collapsible(60, 40, true));
        PrLayoutNode c = s.add(r, collapsible(50, 20, false));
        s.layout();
        CHECK(s.width(r) == 100.0f);
        CHECK(s.height(r) == 40.0f);
        CHECK(s.left(c) - s.left(r) == 50.0f);
    }
    // C5 and C6: a collapsed item takes a gap when lines are collected, and none beside it once they are collected.
    {
        Stage s;
        Styled row = flexBox(PrLayoutFlexDirection_Row, 200);
        row.second.columnGap = prLayoutLengthPx(10);
        PrLayoutNode r = s.add(s.root, row);
        s.add(r, collapsible(50, 20, false));
        s.add(r, collapsible(50, 20, true));
        PrLayoutNode c = s.add(r, collapsible(50, 20, false));
        Styled wrapping = flexBox(PrLayoutFlexDirection_Row, 100);
        wrapping.second.wrap = PrLayoutFlexWrap_Wrap;
        wrapping.second.columnGap = prLayoutLengthPx(10);
        PrLayoutNode w = s.add(s.root, wrapping);
        s.add(w, collapsible(40, 20, false));
        s.add(w, collapsible(50, 20, false));
        s.add(w, collapsible(30, 40, true));
        PrLayoutNode last = s.add(w, collapsible(30, 20, false));
        s.layout();
        CHECK(s.left(c) - s.left(r) == 60.0f);
        CHECK(s.height(w) == 60.0f);
        CHECK(s.top(last) - s.top(w) == 20.0f);
    }
}

TEST_CASE("an item collapsed and made visible again leaves and rejoins the final tree with its descendants") {
    Stage s;
    Styled container = flexBox(PrLayoutFlexDirection_Row, 300);
    container.first.position = PrLayoutPosition_Relative;
    PrLayoutNode r = s.add(s.root, container);
    PrLayoutNode before = s.add(r, collapsible(50, 20, false));
    Styled itemStyle = collapsible(100, 40, false);
    // The last node, so that verification reaches its placements, which a collapse leaves stale.
    PrLayoutNode item = s.add(r, itemStyle);
    s.words(item, {20, 20});
    PrLayoutBoxStyle absolute = block(10);
    absolute.width = prLayoutLengthPx(10);
    absolute.position = PrLayoutPosition_Absolute;
    PrLayoutNode held = s.add(item, absolute);
    PrLayoutBoxStyle relative = block(10);
    relative.position = PrLayoutPosition_Relative;
    relative.zIndexed = 1;
    PrLayoutNode positioned = s.add(item, relative);
    s.layout();
    CHECK(boxCount(s, item) == 1);
    CHECK(boxCount(s, held) == 1);
    CHECK(s.f.hit(PrVec2{s.left(item) + 5, s.top(item) + 25}) == positioned);

    for (int round = 0; round < 2; ++round) {
        itemStyle.first.collapsed = 1;
        prLayoutNodeSetStyle(s.f.tree, item, s.style(itemStyle));
        s.layout();
        // Absent: no rects, no hits, nothing of it or its descendants, and the line keeps its strut.
        CHECK(boxCount(s, item) == 0);
        CHECK(boxCount(s, held) == 0);
        CHECK(boxCount(s, positioned) == 0);
        CHECK(s.height(r) == 40.0f);
        CHECK(s.f.hit(PrVec2{s.left(before) + 60, s.top(r) + 25}) == r);

        itemStyle.first.collapsed = 0;
        prLayoutNodeSetStyle(s.f.tree, item, s.style(itemStyle));
        s.layout();
        CHECK(boxCount(s, item) == 1);
        CHECK(boxCount(s, held) == 1);
        CHECK(s.left(item) - s.left(r) == 50.0f);
        CHECK(s.f.hit(PrVec2{s.left(item) + 5, s.top(item) + 25}) == positioned);
    }
}

TEST_CASE("an item's aspect ratio gives its flex base size from a definite cross size, preferred or stretched") {
    Stage s;
    Styled row = flexBox();
    row.first.height = prLayoutLengthPx(100);
    PrLayoutNode r = s.add(s.root, row);
    // Stretched in a single line of definite cross size: the cross size is the line's.
    Styled stretched = flexItem(0, 1, prLayoutLengthAuto());
    stretched.first.aspectRatio = 1;
    PrLayoutNode a = s.add(r, stretched);
    // A preferred cross size of 50px.
    Styled given = flexItem(0, 1, prLayoutLengthAuto());
    given.first.aspectRatio = 2;
    given.first.height = prLayoutLengthPx(50);
    given.second.alignSelf = PrLayoutAlignment_FlexStart;
    PrLayoutNode b = s.add(r, given);
    s.layout();
    CHECK(s.width(a) == 100.0f);
    CHECK(s.height(a) == 100.0f);
    CHECK(s.width(b) == 100.0f);
    CHECK(s.height(b) == 50.0f);
}

TEST_CASE("an item's aspect ratio gives its hypothetical cross size from its main size, in a row and a column") {
    Stage s;
    Styled start = flexBox();
    start.second.alignItems = PrLayoutAlignment_FlexStart;
    PrLayoutNode r = s.add(s.root, start);
    // A base of its content's width, 40, so it is 40 tall.
    Styled square = flexItem(0, 1, prLayoutLengthAuto());
    square.first.aspectRatio = 1;
    PrLayoutNode a = s.add(r, square);
    s.words(a, {20, 20});
    // A column item 50px tall, not stretched, so it is 100 wide.
    Styled column = flexBox(PrLayoutFlexDirection_Column, 200);
    PrLayoutNode c = s.add(s.root, column);
    Styled tall = flexItem(0, 1, prLayoutLengthAuto());
    tall.first.aspectRatio = 2;
    tall.first.height = prLayoutLengthPx(50);
    tall.second.alignSelf = PrLayoutAlignment_FlexStart;
    PrLayoutNode b = s.add(c, tall);
    // A stretched column item takes the container's width, which gives its base size through the ratio.
    Styled wide = flexItem(0, 1, prLayoutLengthAuto());
    wide.first.aspectRatio = 2;
    PrLayoutNode d = s.add(c, wide);
    s.layout();
    CHECK(s.width(a) == 40.0f);
    CHECK(s.height(a) == 40.0f);
    CHECK(s.width(b) == 100.0f);
    CHECK(s.width(d) == 200.0f);
    CHECK(s.height(d) == 100.0f);
}

TEST_CASE("a non-replaced item's automatic minimum is the larger of its content and transferred size suggestions") {
    Stage s;
    Styled row = flexBox(PrLayoutFlexDirection_Row, 50);
    row.second.alignItems = PrLayoutAlignment_FlexStart;
    // Its content suggestion is 20, and its transferred suggestion is 100, so its minimum is 100.
    PrLayoutNode r = s.add(s.root, row);
    Styled square = flexItem(0, 1, prLayoutLengthAuto());
    square.first.aspectRatio = 1;
    square.first.height = prLayoutLengthPx(100);
    PrLayoutNode a = s.add(r, square);
    s.words(a, {20});
    // A min-width of 0 lets it shrink to the line.
    PrLayoutNode q = s.add(s.root, row);
    Styled shrinks = square;
    shrinks.first.minWidth = prLayoutLengthPx(0);
    PrLayoutNode b = s.add(q, shrinks);
    s.words(b, {20});
    // In a column 100px tall, an item 200px wide is at least 200 tall, and one holding 300px of content is at least 300
    // tall.
    Styled column = flexBox(PrLayoutFlexDirection_Column);
    column.first.height = prLayoutLengthPx(100);
    column.second.alignItems = PrLayoutAlignment_FlexStart;
    PrLayoutNode c = s.add(s.root, column);
    Styled wide = square;
    wide.first.height = prLayoutLengthAuto();
    wide.first.width = prLayoutLengthPx(200);
    PrLayoutNode d = s.add(c, wide);
    PrLayoutNode c2 = s.add(s.root, column);
    Styled full = wide;
    full.first.width = prLayoutLengthPx(50);
    PrLayoutNode e = s.add(c2, full);
    s.add(e, block(300));
    s.layout();
    CHECK(s.width(a) == 100.0f);
    CHECK(s.width(b) == 50.0f);
    CHECK(s.height(d) == 200.0f);
    CHECK(s.height(e) == 300.0f);
}

TEST_CASE("a replaced item's automatic minimum is the lesser of its content and transferred size suggestions") {
    Stage s;
    Styled row = flexBox(PrLayoutFlexDirection_Row, 30);
    row.second.alignItems = PrLayoutAlignment_FlexStart;
    PrLayoutNode r = s.add(s.root, row);
    PrLayoutBoxStyle image = block(40);
    PrLayoutNode a = replaced(s.f, r, image, natural(100, 50));
    PrLayoutNode q = s.add(s.root, row);
    PrLayoutBoxStyle square = image;
    square.aspectRatio = 1;
    PrLayoutNode b = replaced(s.f, q, square, natural(100, 50));
    s.layout();
    CHECK(s.width(a) == 80.0f);
    CHECK(s.width(b) == 40.0f);
}

TEST_CASE("an item with an auto margin in the cross axis takes no part in its line's baseline alignment") {
    Stage s;
    Styled row = flexBox();
    row.second.alignItems = PrLayoutAlignment_Baseline;
    PrLayoutNode r = s.add(s.root, row);
    // Empty items, each with a baseline synthesized from its border box's bottom edge.
    Styled small = flexItem(0, 1, prLayoutLengthAuto());
    small.first.width = prLayoutLengthPx(20);
    small.first.height = prLayoutLengthPx(19);
    PrLayoutNode a = s.add(r, small);
    Styled medium = small;
    medium.first.height = prLayoutLengthPx(27);
    PrLayoutNode b = s.add(r, medium);
    // Taller, but its auto bottom margin places it at the line's start, outside the baseline group.
    Styled tall = small;
    tall.first.height = prLayoutLengthPx(31);
    tall.first.margin[PrRectSide_Bottom] = prLayoutLengthAuto();
    PrLayoutNode c = s.add(r, tall);
    s.layout();
    CHECK(s.top(a) == 8.0f);
    CHECK(s.top(b) == 0.0f);
    CHECK(s.top(c) == 0.0f);
    CHECK(s.height(r) == 31.0f);
}

TEST_CASE("a reversed line takes each item's end margin before it and its start margin after it") {
    Stage s;
    Styled column = flexBox(PrLayoutFlexDirection_ColumnReverse, 100);
    column.first.height = prLayoutLengthPx(80);
    PrLayoutNode c = s.add(s.root, column);
    Styled item = flexItem(0, 1, prLayoutLengthAuto());
    item.first.height = prLayoutLengthPx(20);
    item.first.margin[PrRectSide_Top] = prLayoutLengthPx(-10);
    PrLayoutNode a = s.add(c, item);
    PrLayoutNode b = s.add(c, item);
    // A's bottom margin, zero, is at the line's start: A is flush with the bottom, and B overlaps it by A's top margin.
    s.layout();
    CHECK(s.top(a) == 60.0f);
    CHECK(s.top(b) == 50.0f);
    // A row-reverse with a left margin: the margin is after the item along the line, between it and the next.
    Styled row = flexBox(PrLayoutFlexDirection_RowReverse, 100);
    PrLayoutNode r = s.add(s.root, row);
    Styled spaced = flexItem(0, 1, prLayoutLengthAuto());
    spaced.first.width = prLayoutLengthPx(20);
    spaced.first.height = prLayoutLengthPx(10);
    spaced.first.margin[PrRectSide_Left] = prLayoutLengthPx(5);
    PrLayoutNode d = s.add(r, spaced);
    PrLayoutNode e = s.add(r, spaced);
    s.layout();
    CHECK(s.left(d) - s.left(r) == 80.0f);
    CHECK(s.left(e) - s.left(r) == 55.0f);
}

TEST_CASE("wrap-reverse puts a baseline-sharing group flush with the line's cross-start edge, its bottom") {
    Stage s;
    Styled row = flexBox(PrLayoutFlexDirection_Row, 200);
    row.first.height = prLayoutLengthPx(60);
    row.second.wrap = PrLayoutFlexWrap_WrapReverse;
    row.second.alignItems = PrLayoutAlignment_Baseline;
    PrLayoutNode r = s.add(s.root, row);
    // Each item is a word whose baseline is 20px down. The second has 20px below its baseline, the furthest from
    // cross-start, so it is flush with the bottom, from 20 to 60, and the first is level with it, from 20 to 40.
    Styled item = flexItem(0, 1, prLayoutLengthAuto());
    item.first.width = prLayoutLengthPx(40);
    PrLayoutNode a = s.add(r, item);
    s.words(a, {20});
    Styled padded = item;
    padded.first.padding[PrRectSide_Bottom] = prLayoutLengthPx(20);
    PrLayoutNode b = s.add(r, padded);
    s.words(b, {20});
    s.layout();
    CHECK(s.top(a) - s.top(r) == 20.0f);
    CHECK(s.top(b) - s.top(r) == 20.0f);
    CHECK(s.height(b) == 40.0f);
}
