// Fragmentation and multi-column layout. Each stage is a multi-column container 420px wide, whose columns are 200px
// wide with a gap of 20px unless a case says otherwise, and each edge is measured from the container's border box.

#include "pr/layout_style.h"
#include "pr/layout_tree.h"

#include "layout/tests/layout_fixture.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

// A multi-column container in a root, with everything in a font of size 0, so that lines are only as tall as what is on
// them.
struct Stage {
    Fixture f;
    PrLayoutNode root = kPrLayoutNodeNone;
    PrLayoutNode container = kPrLayoutNodeNone;

    explicit Stage(PrLayoutFragmentationStyle columns = counted(2), float height = -1, float gap = 20) {
        root = add(kPrLayoutNodeNone, block());
        PrLayoutBoxStyle box = block();
        box.width = prLayoutLengthPx(420);
        if (height >= 0)
            box.height = prLayoutLengthPx(height);
        PrLayoutFlexStyle flex = prLayoutFlexStyleDefault();
        flex.columnGap = prLayoutLengthPx(gap);
        container = add(root, box, &columns, &flex);
    }

    // Returns the group of a multi-column container of `count` columns.
    static PrLayoutFragmentationStyle counted(uint16_t count) {
        PrLayoutFragmentationStyle columns = prLayoutFragmentationStyleDefault();
        columns.columnCount = count;
        return columns;
    }

    static PrLayoutFontStyle font0() {
        PrLayoutFontStyle font = prLayoutFontStyleDefault();
        font.font = textFontOf(0);
        return font;
    }

    // Creates an element under `parent` with the groups given.
    PrLayoutNode add(PrLayoutNode parent, const PrLayoutBoxStyle& box,
            const PrLayoutFragmentationStyle* PR_NULLABLE fragmentation = nullptr,
            const PrLayoutFlexStyle* PR_NULLABLE flex = nullptr, const PrLayoutDecorationStyle* PR_NULLABLE decoration = nullptr,
            const PrLayoutFontStyle* PR_NULLABLE font = nullptr) {
        PrLayoutStyle record = prLayoutStyleDefault();
        record.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(f.tree, &box);
        PrLayoutFontStyle zero = font0();
        record.groups[PrLayoutGroup_Font] = prLayoutFontStyleIntern(f.tree, font != nullptr ? font : &zero);
        if (fragmentation != nullptr)
            record.groups[PrLayoutGroup_Table] = fragmentationGroup(f.tree, *fragmentation);
        if (flex != nullptr)
            record.groups[PrLayoutGroup_Flex] = prLayoutFlexStyleIntern(f.tree, flex);
        if (decoration != nullptr)
            record.groups[PrLayoutGroup_Decoration] = prLayoutDecorationStyleIntern(f.tree, decoration);
        PrLayoutNode node = prLayoutNodeCreateElement(f.tree, parent, prLayoutStyleIntern(f.tree, &record));
        if (parent == kPrLayoutNodeNone)
            f.root = node;
        return node;
    }

    // Creates a replaced block `height` tall under `parent`, with the fragmentation values given. A replaced box is
    // monolithic, so it goes whole in one column, whereas a block of a fixed height breaks wherever a column ends.
    PrLayoutNode box(PrLayoutNode parent, float height, const PrLayoutFragmentationStyle* PR_NULLABLE fragmentation = nullptr) {
        PrLayoutStyle record = prLayoutStyleDefault();
        PrLayoutBoxStyle box = block(height);
        box.width = prLayoutLengthPx(100);
        record.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(f.tree, &box);
        if (fragmentation != nullptr)
            record.groups[PrLayoutGroup_Table] = fragmentationGroup(f.tree, *fragmentation);
        PrLayoutNode node = prLayoutNodeCreateReplaced(f.tree, parent, prLayoutStyleIntern(f.tree, &record));
        prLayoutNodeSetNaturalSize(f.tree, node, natural(100, height));
        return node;
    }

    // Creates a word `width` by `height` under `parent`, and a space after it, which has no width.
    PrLayoutNode word(PrLayoutNode parent, float width, float height = 20) {
        PrLayoutBoxStyle box = prLayoutBoxStyleDefault();
        box.display = PrLayoutDisplay_InlineBlock;
        box.width = prLayoutLengthPx(width);
        box.height = prLayoutLengthPx(height);
        PrLayoutNode node = add(parent, box);
        f.text(parent, " ");
        return node;
    }

    void layout(float width = 800, float height = 600) {
        f.layout(px(width, height));
        CHECK(verified(f.tree));
    }

    PrRect at(PrLayoutNode node) {
        PrRect stage = f.bounds(container);
        PrRect rect = f.bounds(node);
        return PrRect{rect.left - stage.left, rect.top - stage.top, rect.right - stage.left, rect.bottom - stage.top};
    }
    double top(PrLayoutNode node) { return double(at(node).top); }
    double left(PrLayoutNode node) { return double(at(node).left); }
    double height(PrLayoutNode node) { return double(at(node).bottom - at(node).top); }
};

// Returns fragmentation values whose break-before is `value`.
PrLayoutFragmentationStyle before(PrLayoutBreak value) {
    PrLayoutFragmentationStyle fragmentation = prLayoutFragmentationStyleDefault();
    fragmentation.breakBefore = value;
    return fragmentation;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a multi-column container balances its blocks over its columns") {
    Stage s;
    PrLayoutNode blocks[4];
    for (PrLayoutNode& b : blocks)
        b = s.box(s.container, 50);
    s.layout();
    CHECK(s.height(s.container) == 100);
    CHECK(s.left(blocks[0]) == 0);
    CHECK(s.top(blocks[1]) == 50);
    CHECK(s.left(blocks[2]) == 220);
    CHECK(s.top(blocks[2]) == 0);
    CHECK(s.left(blocks[3]) == 220);
    CHECK(s.top(blocks[3]) == 50);
}

TEST_CASE("a multi-column container establishes a block formatting context, which avoids the floats before it") {
    Stage s;
    PrLayoutBoxStyle floated = block(50);
    floated.width = prLayoutLengthPx(100);
    floated.floating = PrLayoutFloat_Left;
    PrLayoutNode before = prLayoutNodeCreateElement(s.f.tree, kPrLayoutNodeNone, s.f.style(floated));
    prLayoutTreeInsert(s.f.tree, s.root, before, s.container);
    PrLayoutNode blocks[2] = {s.box(s.container, 50), s.box(s.container, 50)};
    s.layout();
    CHECK(double(s.f.bounds(s.container).left) == 100);
    CHECK(double(s.f.bounds(s.container).top) == 0);
    CHECK(s.left(blocks[1]) == 220);
    CHECK(s.height(s.container) == 50);
}

TEST_CASE("column-width finds the number of columns that fit, which share the room") {
    PrLayoutFragmentationStyle columns = prLayoutFragmentationStyleDefault();
    columns.columnWidth = prLayoutLengthPx(120);
    Stage s(columns, -1, 30);
    PrLayoutNode blocks[3];
    for (PrLayoutNode& b : blocks)
        b = s.box(s.container, 40);
    s.layout();
    // Three columns of 120px and two gaps of 30px fit in 420px, and each column is (420 - 60) / 3 wide.
    CHECK(s.height(s.container) == 40);
    CHECK(s.left(blocks[1]) == 150);
    CHECK(s.left(blocks[2]) == 300);
    CHECK(double(s.at(blocks[2]).right) == 400);
}

TEST_CASE("a box that a column break crosses has a piece in each column, and its placement is the first") {
    Stage s;
    PrLayoutNode parent = s.add(s.container, block());
    PrLayoutNode children[4];
    for (PrLayoutNode& c : children)
        c = s.box(parent, 50);
    s.layout();
    CHECK(s.height(s.container) == 100);
    CHECK(s.top(children[1]) == 50);
    CHECK(s.left(children[2]) == 220);
    CHECK(s.top(children[2]) == 0);
    CHECK(s.top(children[3]) == 50);
    // The first piece fills its column.
    CHECK(s.height(parent) == 100);
    CHECK(s.left(parent) == 0);
}

TEST_CASE("a box that a column break crosses has a box in each column, inside a box that the break crosses too") {
    Stage s;
    PrLayoutNode outer = s.add(s.container, block());
    PrLayoutNode inner = s.add(outer, block());
    PrLayoutNode children[4];
    for (PrLayoutNode& c : children)
        c = s.box(inner, 50);
    s.layout();
    PrRect stage = s.f.bounds(s.container);
    for (PrLayoutNode node : {outer, inner}) {
        PrLayoutRect boxes[3];
        REQUIRE(prLayoutNodeBoxes(s.f.tree, node, PrLayoutCoordinates_Viewport, boxes, 3) == 2);
        PrRect first = prLayoutRectToPx(boxes[0]);
        PrRect second = prLayoutRectToPx(boxes[1]);
        CHECK(double(first.left - stage.left) == 0);
        CHECK(double(first.bottom - stage.top) == 100);
        CHECK(double(second.left - stage.left) == 220);
        CHECK(double(second.top - stage.top) == 0);
        CHECK(double(second.bottom - stage.top) == 100);
        // The bounds cover every box.
        CHECK(double(s.at(node).right) == 420);
    }
    CHECK(prLayoutNodeBoxes(s.f.tree, children[2], PrLayoutCoordinates_Viewport, nullptr, 0) == 1);
}

TEST_CASE("a forced column break starts a new column, and balancing keeps it") {
    Stage s;
    PrLayoutFragmentationStyle column = before(PrLayoutBreak_Column);
    PrLayoutNode first = s.box(s.container, 50);
    PrLayoutNode second = s.box(s.container, 50, &column);
    PrLayoutNode third = s.box(s.container, 50);
    s.layout();
    CHECK(s.top(first) == 0);
    CHECK(s.left(second) == 220);
    CHECK(s.top(second) == 0);
    CHECK(s.left(third) == 220);
    CHECK(s.top(third) == 50);
    CHECK(s.height(s.container) == 100);
}

TEST_CASE("a page break does not break a column") {
    Stage s;
    PrLayoutFragmentationStyle page = before(PrLayoutBreak_Page);
    PrLayoutNode blocks[2] = {s.box(s.container, 50), s.box(s.container, 50, &page)};
    s.layout();
    CHECK(s.left(blocks[1]) == 220);
    CHECK(s.height(s.container) == 50);
}

TEST_CASE("break-inside avoid moves a box to the next column whole") {
    Stage s;
    PrLayoutFragmentationStyle avoid = prLayoutFragmentationStyleDefault();
    avoid.breakInside = PrLayoutBreakInside_Avoid;
    PrLayoutNode first = s.box(s.container, 60);
    PrLayoutNode kept = s.add(s.container, block(), &avoid);
    PrLayoutNode inner[2] = {s.box(kept, 40), s.box(kept, 40)};
    s.layout();
    CHECK(s.top(first) == 0);
    CHECK(s.left(kept) == 220);
    CHECK(s.top(kept) == 0);
    CHECK(s.top(inner[1]) == 40);
    CHECK(s.height(s.container) == 80);
}

TEST_CASE("a replaced box is monolithic and goes to the next column whole") {
    Stage s;
    PrLayoutNode first = s.box(s.container, 60);
    PrLayoutBoxStyle box = block(60);
    box.width = prLayoutLengthPx(100);
    PrLayoutNode image = replaced(s.f, s.container, box, natural(100, 60));
    s.layout();
    CHECK(s.top(first) == 0);
    CHECK(s.left(image) == 220);
    CHECK(s.top(image) == 0);
    CHECK(s.height(s.container) == 60);
}

TEST_CASE("lines break between columns") {
    Stage s;
    PrLayoutNode words[6];
    for (PrLayoutNode& w : words)
        w = s.word(s.container, 200);
    s.layout();
    CHECK(s.height(s.container) == 60);
    CHECK(s.top(words[2]) == 40);
    CHECK(s.left(words[3]) == 220);
    CHECK(s.top(words[3]) == 0);
}

TEST_CASE("orphans move a paragraph whose first lines would end a column to the next column") {
    PrLayoutFragmentationStyle columns = Stage::counted(2);
    columns.columnFill = PrLayoutColumnFill_Auto;
    Stage s(columns, 60);
    s.box(s.container, 30);
    PrLayoutNode paragraph = s.add(s.container, block());
    PrLayoutNode words[3];
    for (PrLayoutNode& w : words)
        w = s.word(paragraph, 200);
    s.layout();
    // One line fits after the block, which orphans of 2 forbid, so the paragraph starts the second column.
    CHECK(s.left(words[0]) == 220);
    CHECK(s.top(words[0]) == 0);
    CHECK(s.top(words[2]) == 40);
}

TEST_CASE("widows move the last lines of a column to the next column with the lines after the break") {
    PrLayoutFragmentationStyle columns = Stage::counted(2);
    columns.columnFill = PrLayoutColumnFill_Auto;
    Stage s(columns, 60);
    PrLayoutNode paragraph = s.add(s.container, block());
    PrLayoutNode words[4];
    for (PrLayoutNode& w : words)
        w = s.word(paragraph, 200);
    s.layout();
    // Three lines fit in the first column, which would leave one after the break, so widows of 2 move the third.
    CHECK(s.left(words[1]) == 0);
    CHECK(s.left(words[2]) == 220);
    CHECK(s.top(words[2]) == 0);
    CHECK(s.top(words[3]) == 20);
}

TEST_CASE("a flex item taller than a column breaks inside, and its content continues in the next column") {
    PrLayoutFragmentationStyle columns = Stage::counted(2);
    columns.columnFill = PrLayoutColumnFill_Auto;
    Stage s(columns, 100);
    PrLayoutBoxStyle flexBox = block();
    flexBox.display = PrLayoutDisplay_Flex;
    PrLayoutFlexStyle flex = prLayoutFlexStyleDefault();
    PrLayoutNode container = s.add(s.container, flexBox, nullptr, &flex);
    PrLayoutBoxStyle itemBox = block();
    itemBox.width = prLayoutLengthPx(100);
    PrLayoutNode item = s.add(container, itemBox);
    PrLayoutNode boxes[3] = {s.box(item, 40), s.box(item, 40), s.box(item, 40)};
    s.layout();
    // Two boxes fit in the first column, and the third continues the item at the top of the second.
    CHECK(s.left(boxes[1]) == 0);
    CHECK(s.top(boxes[1]) == 40);
    CHECK(s.left(boxes[2]) == 220);
    CHECK(s.top(boxes[2]) == 0);
}

TEST_CASE("a spanner goes across the container between column sets") {
    Stage s;
    PrLayoutFragmentationStyle span = prLayoutFragmentationStyleDefault();
    span.columnSpan = PrLayoutColumnSpan_All;
    PrLayoutNode first = s.box(s.container, 50);
    PrLayoutNode spanner = s.add(s.container, block(30), &span);
    PrLayoutNode after[2] = {s.box(s.container, 50), s.box(s.container, 50)};
    s.layout();
    CHECK(s.top(first) == 0);
    CHECK(s.top(spanner) == 50);
    CHECK(double(s.at(spanner).right) == 420);
    CHECK(s.top(after[0]) == 80);
    CHECK(s.left(after[1]) == 220);
    CHECK(s.top(after[1]) == 80);
    CHECK(s.height(s.container) == 130);
}

TEST_CASE("column-fill auto fills each column in turn in a definite height") {
    PrLayoutFragmentationStyle columns = Stage::counted(2);
    columns.columnFill = PrLayoutColumnFill_Auto;
    Stage s(columns, 120);
    PrLayoutNode blocks[4];
    for (PrLayoutNode& b : blocks)
        b = s.box(s.container, 50);
    s.layout();
    CHECK(s.top(blocks[1]) == 50);
    CHECK(s.left(blocks[2]) == 220);
    CHECK(s.top(blocks[3]) == 50);
    CHECK(s.height(s.container) == 120);
}

TEST_CASE("a nested multi-column container that starts past a column's end goes to the next column whole") {
    PrLayoutFragmentationStyle columns = Stage::counted(2);
    columns.columnFill = PrLayoutColumnFill_Auto;
    Stage s(columns, 100);
    s.box(s.container, 150);
    PrLayoutNode inner = s.add(s.container, block(), &columns);
    PrLayoutNode content = s.add(inner, block(100));
    s.layout();
    CHECK(prLayoutNodeBoxes(s.f.tree, inner, PrLayoutCoordinates_Viewport, nullptr, 0) == 1);
    CHECK(s.left(inner) == 220);
    CHECK(s.top(inner) == 0);
    CHECK(s.top(content) == 0);
}

TEST_CASE("a nested multi-column container below other content moves what does not fit its columns to the next column") {
    PrLayoutFragmentationStyle columns = Stage::counted(2);
    columns.columnFill = PrLayoutColumnFill_Auto;
    Stage s(columns, 100);
    s.box(s.container, 70);
    PrLayoutNode inner = s.add(s.container, block(), &columns);
    PrLayoutNode first = s.box(inner, 20);
    PrLayoutNode second = s.box(inner, 50);
    s.layout();
    CHECK(s.top(first) == 70);
    CHECK(s.left(second) == 220);
    CHECK(s.top(second) == 0);
}

TEST_CASE("a nested multi-column container whose orphans fit in none of its columns goes to the next column whole") {
    PrLayoutFragmentationStyle columns = Stage::counted(2);
    columns.columnFill = PrLayoutColumnFill_Auto;
    Stage s(columns, 160);
    s.box(s.container, 100);
    PrLayoutFontStyle font = Stage::font0();
    font.orphans = 2;
    font.widows = 2;
    PrLayoutNode inner = s.add(s.container, block(100), &columns, nullptr, nullptr, &font);
    PrLayoutNode words[2] = {s.word(inner, 90, 50), s.word(inner, 90, 50)};
    s.layout();
    CHECK(prLayoutNodeBoxes(s.f.tree, inner, PrLayoutCoordinates_Viewport, nullptr, 0) == 1);
    CHECK(s.left(inner) == 220);
    CHECK(s.top(inner) == 0);
    CHECK(s.left(words[0]) == 220);
    CHECK(s.top(words[0]) == 0);
    CHECK(s.top(words[1]) == 50);
}

TEST_CASE("a nested multi-column container taller than its content lays the content out in its first piece alone") {
    PrLayoutFragmentationStyle columns = Stage::counted(2);
    columns.columnFill = PrLayoutColumnFill_Auto;
    Stage s(columns, 100);
    PrLayoutNode inner = s.add(s.container, block(200), &columns);
    PrLayoutNode content = s.box(inner, 50);
    s.layout();
    CHECK(prLayoutNodeBoxes(s.f.tree, inner, PrLayoutCoordinates_Viewport, nullptr, 0) == 2);
    CHECK(prLayoutNodeBoxes(s.f.tree, content, PrLayoutCoordinates_Viewport, nullptr, 0) == 1);
    CHECK(s.left(content) == 0);
    CHECK(s.top(content) == 0);
    // The second piece holds no content, so the point where a copy of the content would be hits the container.
    PrRect stage = s.f.bounds(s.container);
    CHECK(s.f.hit(PrVec2{stage.left + 225, stage.top + 5}) == inner);
}

TEST_CASE("an unforced column break truncates the block-start margin of a box that establishes a formatting context") {
    PrLayoutFragmentationStyle columns = Stage::counted(2);
    columns.columnFill = PrLayoutColumnFill_Auto;
    Stage s(columns, 100);
    s.box(s.container, 100);
    PrLayoutBoxStyle root = block(50);
    root.display = PrLayoutDisplay_FlowRoot;
    setMargins(root, 20, 0);
    PrLayoutNode after = s.add(s.container, root);
    s.layout();
    CHECK(s.left(after) == 220);
    CHECK(s.top(after) == 0);
}

TEST_CASE("columns past the count overflow in the inline direction") {
    Stage s(Stage::counted(2), 100);
    PrLayoutNode blocks[6];
    for (PrLayoutNode& b : blocks)
        b = s.box(s.container, 50);
    s.layout();
    CHECK(s.left(blocks[2]) == 220);
    CHECK(s.left(blocks[4]) == 440);
    CHECK(s.top(blocks[5]) == 50);
    CHECK(s.height(s.container) == 100);
}

TEST_CASE("a column rule paints in the middle of the gap between columns with content") {
    Stage s;
    PrLayoutDecorationStyle decoration = prLayoutDecorationStyleDefault();
    decoration.columnRuleStyle = PrDrawBorderStyle_Solid;
    decoration.columnRuleWidth = prLayoutUnitFromPx(4);
    decoration.columnRuleColor = prColor16FromColor(PrColor{1, 0, 0, 1});
    PrLayoutFragmentationStyle columns = Stage::counted(2);
    PrLayoutFlexStyle flex = prLayoutFlexStyleDefault();
    flex.columnGap = prLayoutLengthPx(20);
    PrLayoutBoxStyle box = block();
    box.width = prLayoutLengthPx(420);
    PrLayoutNode ruled = s.add(s.root, box, &columns, &flex, &decoration);
    for (int i = 0; i < 2; ++i)
        s.box(ruled, 50);
    s.layout();
    PrRect at = s.f.bounds(ruled);
    std::vector<uint8_t> pixels = render(s.f.tree, 800, 200);
    uint32_t y = uint32_t(at.top) + 25;
    CHECK(pixel(pixels, 800, 210, y) == "255 0 0 255");
    CHECK(pixel(pixels, 800, 205, y) == "0 0 0 0");
    CHECK(pixel(pixels, 800, 210, uint32_t(at.top) + 55) == "0 0 0 0");
}

TEST_CASE("a change in one column lays out the container again from its cached boxes") {
    Stage s;
    PrLayoutNode blocks[4];
    for (PrLayoutNode& b : blocks)
        b = s.box(s.container, 50);
    s.layout();
    PrLayoutStyle record = prLayoutStyleRecord(s.f.tree, prLayoutNodeStyle(s.f.tree, blocks[3]));
    PrLayoutBoxStyle box = block(70);
    record.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(s.f.tree, &box);
    prLayoutNodeSetStyle(s.f.tree, blocks[3], prLayoutStyleIntern(s.f.tree, &record));
    s.layout();
    CHECK(s.height(s.container) == 120);
    CHECK(s.left(blocks[3]) == 220);
    CHECK(s.top(blocks[3]) == 50);
}

TEST_CASE("a block of a fixed height breaks where a column ends") {
    Stage s;
    PrLayoutNode tall = s.add(s.container, block(100));
    s.layout();
    CHECK(s.height(s.container) == 50);
    CHECK(s.height(tall) == 50);
}

TEST_CASE("box-decoration-break clone gives each piece its border, which takes room in each column") {
    for (PrLayoutBoxDecorationBreak decoration : {PrLayoutBoxDecorationBreak_Slice, PrLayoutBoxDecorationBreak_Clone}) {
        Stage s;
        PrLayoutFragmentationStyle fragmentation = prLayoutFragmentationStyleDefault();
        fragmentation.boxDecorationBreak = decoration;
        PrLayoutBoxStyle box = block();
        box.border[PrRectSide_Top] = prLayoutUnitFromPx(10);
        box.border[PrRectSide_Bottom] = prLayoutUnitFromPx(10);
        PrLayoutNode parent = s.add(s.container, box, &fragmentation);
        PrLayoutNode children[4];
        for (PrLayoutNode& c : children)
            c = s.box(parent, 50);
        s.layout();
        bool clones = decoration == PrLayoutBoxDecorationBreak_Clone;
        CHECK(s.height(s.container) == (clones ? 120 : 110));
        CHECK(s.top(children[0]) == 10);
        CHECK(s.left(children[2]) == 220);
        CHECK(s.top(children[2]) == (clones ? 10 : 0));
    }
}

TEST_CASE("a positioned inline box that starts in a later column, with a box out of flow in it, lays out and paints") {
    Stage s;
    for (int i = 0; i < 3; ++i)
        s.word(s.container, 200);
    PrLayoutBoxStyle relative = inlineBox();
    relative.position = PrLayoutPosition_Relative;
    PrLayoutNode span = s.add(s.container, relative);
    PrLayoutNode word = s.word(span, 200);
    PrLayoutBoxStyle absolute = block(10);
    absolute.width = prLayoutLengthPx(10);
    absolute.position = PrLayoutPosition_Absolute;
    PrLayoutDecorationStyle red = background(PrColor{1, 0, 0, 1});
    s.add(word, absolute, nullptr, nullptr, &red);
    s.layout();
    CHECK(s.height(s.container) == 40);
    // The lines of the container's own inline content are placed through its columns. The span's part has the font's
    // content area, which is empty at size 0, on the baseline of the second line of the second column.
    CHECK(s.left(span) == 220);
    CHECK(double(s.at(span).bottom) == 40);
    std::vector<uint8_t> pixels = render(s.f.tree, 800, 100);
    CHECK(pixels.size() == size_t(800) * 100 * 4);
}

TEST_CASE("a multi-column container's first baseline is its first column's, which a flex line aligns by") {
    Stage s;
    PrLayoutBoxStyle flexBox = block();
    flexBox.display = PrLayoutDisplay_Flex;
    PrLayoutFlexStyle flex = prLayoutFlexStyleDefault();
    flex.alignItems = PrLayoutAlignment_Baseline;
    PrLayoutNode line = s.add(s.root, flexBox, nullptr, &flex);
    PrLayoutFragmentationStyle columns = Stage::counted(2);
    PrLayoutBoxStyle box = block();
    box.width = prLayoutLengthPx(420);
    PrLayoutNode multicol = s.add(line, box, &columns);
    s.word(multicol, 200);
    s.word(multicol, 200);
    PrLayoutNode tall = s.add(line, block());
    s.word(tall, 100, 40);
    s.layout();
    // The words are lines of 20px, one in each column, whose baselines are their bottom edges.
    CHECK(double(s.f.bounds(multicol).top - s.f.bounds(tall).top) == 20);
}

TEST_CASE("a float that does not fit and avoids breaks goes to the next column, and the lines beside it move with it") {
    PrLayoutFragmentationStyle columns = Stage::counted(2);
    columns.columnFill = PrLayoutColumnFill_Auto;
    Stage s(columns, 100);
    s.box(s.container, 60);
    PrLayoutBoxStyle box = block(60);
    box.floating = PrLayoutFloat_Left;
    box.width = prLayoutLengthPx(100);
    PrLayoutFragmentationStyle avoid = prLayoutFragmentationStyleDefault();
    avoid.breakInside = PrLayoutBreakInside_Avoid;
    PrLayoutNode floated = s.add(s.container, box, &avoid);
    PrLayoutNode word = s.word(s.container, 100);
    s.layout();
    CHECK(s.left(floated) == 220);
    CHECK(s.top(floated) == 0);
    CHECK(s.left(word) == 320);
    CHECK(s.top(word) == 0);
}

TEST_CASE("a flex container breaks between its lines, which continue at the top of the next column") {
    PrLayoutFragmentationStyle columns = Stage::counted(2);
    columns.columnFill = PrLayoutColumnFill_Auto;
    Stage s(columns, 100);
    PrLayoutBoxStyle flexBox = block();
    flexBox.display = PrLayoutDisplay_Flex;
    PrLayoutFlexStyle flex = prLayoutFlexStyleDefault();
    flex.wrap = PrLayoutFlexWrap_Wrap;
    PrLayoutNode container = s.add(s.container, flexBox, nullptr, &flex);
    PrLayoutNode items[3];
    for (PrLayoutNode& item : items) {
        PrLayoutBoxStyle box = block(40);
        box.width = prLayoutLengthPx(200);
        item = s.add(container, box);
    }
    s.layout();
    CHECK(s.top(items[1]) == 40);
    CHECK(s.left(items[2]) == 220);
    CHECK(s.top(items[2]) == 0);
}

TEST_CASE("a positioned box that breaks across columns paints each of its pieces") {
    PrLayoutFragmentationStyle columns = Stage::counted(2);
    columns.columnFill = PrLayoutColumnFill_Auto;
    Stage s(columns, 100);
    PrLayoutBoxStyle relative = block();
    relative.position = PrLayoutPosition_Relative;
    PrLayoutDecorationStyle green = background(PrColor{0, 1, 0, 1});
    PrLayoutNode positioned = s.add(s.container, relative, nullptr, nullptr, &green);
    s.box(positioned, 60);
    s.box(positioned, 60);
    s.layout();
    PrRect at = s.f.bounds(s.container);
    std::vector<uint8_t> pixels = render(s.f.tree, 800, 200);
    // The first piece fills the first column, and the second holds the second box at the top of the second column, to
    // the right of its replaced content.
    CHECK(pixel(pixels, 800, 150, uint32_t(at.top) + 50) == "0 255 0 255");
    CHECK(pixel(pixels, 800, 370, uint32_t(at.top) + 30) == "0 255 0 255");
    CHECK(pixel(pixels, 800, 370, uint32_t(at.top) + 80) == "0 0 0 0");
}

TEST_CASE("a change in a later piece of a positioned box paints again") {
    PrLayoutFragmentationStyle columns = Stage::counted(2);
    columns.columnFill = PrLayoutColumnFill_Auto;
    Stage s(columns, 100);
    PrLayoutBoxStyle relative = block();
    relative.position = PrLayoutPosition_Relative;
    PrLayoutNode positioned = s.add(s.container, relative);
    s.box(positioned, 100);
    PrLayoutDecorationStyle blue = background(PrColor{0, 0, 1, 1});
    PrLayoutNode later = s.add(positioned, block(50), nullptr, nullptr, &blue);
    s.layout();
    PrRect at = s.f.bounds(s.container);
    CHECK(pixel(render(s.f.tree, 800, 200), 800, 300, uint32_t(at.top) + 25) == "0 0 255 255");
    // Only the piece in the second column changes.
    PrLayoutStyle record = prLayoutStyleDefault();
    PrLayoutBoxStyle box = block(50);
    record.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(s.f.tree, &box);
    PrLayoutFontStyle font = Stage::font0();
    record.groups[PrLayoutGroup_Font] = prLayoutFontStyleIntern(s.f.tree, &font);
    PrLayoutDecorationStyle red = background(PrColor{1, 0, 0, 1});
    record.groups[PrLayoutGroup_Decoration] = prLayoutDecorationStyleIntern(s.f.tree, &red);
    prLayoutNodeSetStyle(s.f.tree, later, prLayoutStyleIntern(s.f.tree, &record));
    s.layout();
    CHECK(pixel(render(s.f.tree, 800, 200), 800, 300, uint32_t(at.top) + 25) == "255 0 0 255");
}

TEST_CASE("a float that a column ends inside breaks, and continues at the top of the next column before what clears it") {
    // Among inline content, and among blocks in flow.
    for (bool blocks : {false, true}) {
        CAPTURE(blocks);
        PrLayoutFragmentationStyle columns = Stage::counted(2);
        columns.columnFill = PrLayoutColumnFill_Auto;
        Stage s(columns, 100);
        PrLayoutBoxStyle floatBox = block(160);
        floatBox.floating = PrLayoutFloat_Left;
        floatBox.width = prLayoutLengthPx(100);
        PrLayoutNode floated = s.add(s.container, floatBox);
        PrLayoutNode first = s.box(floated, 80);
        PrLayoutNode second = s.box(floated, 80);
        if (!blocks)
            s.f.text(s.container, " ");
        PrLayoutBoxStyle clearing = block(20);
        clearing.floating = PrLayoutFloat_Left;
        clearing.clear = PrLayoutClear_Left;
        clearing.width = prLayoutLengthPx(100);
        PrLayoutNode cleared = s.add(s.container, clearing);
        if (blocks)
            s.box(s.container, 0);
        s.layout();
        CHECK(s.top(first) == 0);
        CHECK(s.left(second) == 220);
        CHECK(s.top(second) == 0);
        CHECK(s.left(cleared) == 220);
        CHECK(s.top(cleared) == 60);
    }
}
