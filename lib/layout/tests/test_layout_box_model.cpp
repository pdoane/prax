#include "pr/layout_query.h"
#include "pr/layout_scene.h"
#include "pr/layout_style.h"
#include "pr/layout_tree.h"

#include "layout/tests/layout_fixture.h"

#include "tools/harness/test.h"

#include <vector>

namespace pr_impl {

namespace {

// Returns the fixture's font on lines 20px tall.
PrLayoutFontStyle lineHeight20() {
    PrLayoutFontStyle font = prLayoutFontStyleDefault();
    font.lineHeight = prLayoutLineHeightPx(20);
    return font;
}

PrLayoutBoxStyle withDisplay(PrLayoutBoxStyle box, PrLayoutDisplay display) {
    box.display = display;
    return box;
}

PrLayoutBoxStyle inlineBlock() {
    return withDisplay(prLayoutBoxStyleDefault(), PrLayoutDisplay_InlineBlock);
}

PrLayoutBoxStyle sized(PrLayoutBoxStyle box, float width, float height) {
    box.width = prLayoutLengthPx(width);
    box.height = prLayoutLengthPx(height);
    return box;
}

double widthOf(PrRect rect) {
    return double(rect.right - rect.left);
}

double heightOf(PrRect rect) {
    return double(rect.bottom - rect.top);
}

// Lays the tree out in a viewport of `width` by `height`, and checks the incremental work against a layout from
// scratch.
void layout(Fixture& f, float width = 400, float height = 300) {
    f.layout(px(width, height));
    CHECK(verified(f.tree));
}

// Creates an element under `parent`, or the root, on lines 20px tall.
PrLayoutNode element20(Fixture& f, PrLayoutNode parent, const PrLayoutBoxStyle& box) {
    PrLayoutFontStyle font = lineHeight20();
    return f.element(parent, box, nullptr, nullptr, &font);
}
PrLayoutNode root20(Fixture& f) {
    return element20(f, kPrLayoutNodeNone, block());
}

// ---------------------------------------------------------------------------------------------------------------------
// justify-self in block flow, and semi-replaced boxes

// Returns an alignment group with justify-self `alignment` and justify-items `items`.
PrLayoutFlexStyle justified(PrLayoutAlignment alignment, PrLayoutAlignment items = PrLayoutAlignment_Normal) {
    PrLayoutFlexStyle flex = prLayoutFlexStyleDefault();
    flex.justifySelf = alignment;
    flex.justifyItems = items;
    return flex;
}

// Creates a block of `kind` under `parent`, aligned by `flex` and holding an inline-block 40 wide, so that its
// fit-content size is 40.
PrLayoutNode justifiedBlock(Fixture& f, PrLayoutNode parent, const PrLayoutFlexStyle& flex,
        PrLayoutBoxStyle box = block(), PrLayoutNodeKind kind = PrLayoutNodeKind_Element) {
    PrLayoutFontStyle font = lineHeight20();
    PrLayoutStyleIndex style = f.style(box, nullptr, nullptr, &font, &flex);
    PrLayoutNode node;
    if (kind == PrLayoutNodeKind_SemiReplaced)
        node = prLayoutNodeCreateSemiReplaced(f.tree, parent, style);
    else
        node = prLayoutNodeCreateElement(f.tree, parent, style);
    element20(f, node, sized(inlineBlock(), 40, 20));
    return node;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a flow-root keeps its children's margins, which collapse through a block") {
    for (PrLayoutDisplay display : {PrLayoutDisplay_Block, PrLayoutDisplay_FlowRoot}) {
        CAPTURE(int(display));
        Fixture f;
        PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
        PrLayoutNode box = f.element(root, withDisplay(block(), display));
        PrLayoutBoxStyle margined = block(20);
        setMargins(margined, 10, 10);
        PrLayoutNode child = f.element(box, margined);
        layout(f, 200, 100);
        checkRect(f.bounds(child), 0, 10, 200, 30);
        if (display == PrLayoutDisplay_FlowRoot)
            checkRect(f.bounds(box), 0, 0, 200, 40);
        else
            checkRect(f.bounds(box), 0, 10, 200, 30);
    }
}

TEST_CASE("justify-self sizes a block in flow to fit its content and places it in its containing block") {
    struct Case {
        PrLayoutAlignment justify;
        PrTextDirection direction;
        float left;
        float width;
    };
    const Case cases[] = {
        {PrLayoutAlignment_Normal, PrTextDirection_Ltr, 0, 200},
        {PrLayoutAlignment_Stretch, PrTextDirection_Ltr, 0, 200},
        {PrLayoutAlignment_Start, PrTextDirection_Ltr, 0, 40},
        {PrLayoutAlignment_Center, PrTextDirection_Ltr, 80, 40},
        {PrLayoutAlignment_End, PrTextDirection_Ltr, 160, 40},
        {PrLayoutAlignment_FlexEnd, PrTextDirection_Ltr, 160, 40},
        {PrLayoutAlignment_Left, PrTextDirection_Ltr, 0, 40},
        {PrLayoutAlignment_Right, PrTextDirection_Ltr, 160, 40},
        {PrLayoutAlignment_Start, PrTextDirection_Rtl, 160, 40},
        {PrLayoutAlignment_End, PrTextDirection_Rtl, 0, 40},
        {PrLayoutAlignment_Left, PrTextDirection_Rtl, 0, 40},
        {PrLayoutAlignment_Right, PrTextDirection_Rtl, 160, 40},
        {PrLayoutAlignment_Baseline, PrTextDirection_Ltr, 0, 40},
    };
    for (const Case& c : cases) {
        CAPTURE(int(c.justify));
        CAPTURE(int(c.direction));
        Fixture f;
        PrLayoutFontStyle font = lineHeight20();
        font.direction = c.direction;
        PrLayoutNode root = f.element(kPrLayoutNodeNone, block(), nullptr, nullptr, &font);
        PrLayoutNode box = justifiedBlock(f, root, justified(c.justify));
        layout(f, 200, 100);
        CHECK(f.bounds(box).left == c.left);
        CHECK(widthOf(f.bounds(box)) == double(c.width));
    }
}

TEST_CASE("justify-self places what auto margins leave, and safe alignment keeps an overflowing box at the start") {
    Fixture f;
    PrLayoutNode root = root20(f);
    PrLayoutBoxStyle autoLeft = block();
    autoLeft.margin[PrRectSide_Left] = prLayoutLengthAuto();
    PrLayoutNode margined = justifiedBlock(f, root, justified(PrLayoutAlignment_Center), autoLeft);
    PrLayoutBoxStyle wide = block();
    wide.width = prLayoutLengthPx(300);
    PrLayoutNode unsafe = justifiedBlock(f, root, justified(PrLayoutAlignment_Center), wide);
    PrLayoutAlignment safeCenter = PrLayoutAlignment_Center | PrLayoutAlignmentFlag_Safe;
    PrLayoutNode safe = justifiedBlock(f, root, justified(safeCenter), wide);
    layout(f, 200, 100);
    CHECK(f.bounds(margined).left == 160.0f);
    CHECK(f.bounds(unsafe).left == -50.0f);
    CHECK(f.bounds(safe).left == 0.0f);
}

TEST_CASE("justify-self auto takes the parent's justify-items, and a change to it lays the children out again") {
    Fixture f;
    PrLayoutFontStyle font = lineHeight20();
    PrLayoutFlexStyle items = justified(PrLayoutAlignment_Auto, PrLayoutAlignment_Center);
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block(), nullptr, nullptr, &font, &items);
    PrLayoutNode child = justifiedBlock(f, root, justified(PrLayoutAlignment_Auto));
    PrLayoutNode own = justifiedBlock(f, root, justified(PrLayoutAlignment_End));
    layout(f, 200, 100);
    CHECK(f.bounds(child).left == 80.0f);
    CHECK(f.bounds(own).left == 160.0f);
    PrLayoutFlexStyle normal = justified(PrLayoutAlignment_Auto, PrLayoutAlignment_Normal);
    prLayoutNodeSetStyle(f.tree, root, f.style(block(), nullptr, nullptr, &font, &normal));
    layout(f, 200, 100);
    CHECK(f.bounds(child).left == 0.0f);
    CHECK(widthOf(f.bounds(child)) == 200.0);
    // A resize keeps an aligned box of the same size, at its new position.
    layout(f, 300, 100);
    CHECK(f.bounds(own).left == 260.0f);
}

TEST_CASE("a block inside an inline box takes normal for justify-self auto, as an inline box has no justify-items") {
    Fixture f;
    PrLayoutFontStyle font = lineHeight20();
    PrLayoutFlexStyle right = justified(PrLayoutAlignment_Auto, PrLayoutAlignment_Right);
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block(), nullptr, nullptr, &font, &right);
    PrLayoutFlexStyle center = justified(PrLayoutAlignment_Auto, PrLayoutAlignment_Center);
    PrLayoutNode span = f.element(root, prLayoutBoxStyleDefault(), nullptr, nullptr, &font, &center);
    PrLayoutNode inside = justifiedBlock(f, span, justified(PrLayoutAlignment_Auto));
    layout(f, 200, 100);
    CHECK(f.bounds(inside).left == 0.0f);
    CHECK(widthOf(f.bounds(inside)) == 200.0);
}

TEST_CASE("a semi-replaced element fits its content in block flow, and is stretched as an item and between insets") {
    Fixture f;
    PrLayoutNode root = root20(f);
    auto semi = [&](PrLayoutNode parent, PrLayoutBoxStyle box) {
        return justifiedBlock(f, parent, prLayoutFlexStyleDefault(), box, PrLayoutNodeKind_SemiReplaced);
    };
    PrLayoutNode inFlow = semi(root, block());
    PrLayoutBoxStyle centered = block();
    centered.margin[PrRectSide_Left] = prLayoutLengthAuto();
    centered.margin[PrRectSide_Right] = prLayoutLengthAuto();
    PrLayoutNode inMargins = semi(root, centered);
    PrLayoutNode ended =
        justifiedBlock(f, root, justified(PrLayoutAlignment_End), block(), PrLayoutNodeKind_SemiReplaced);
    PrLayoutBoxStyle columnBox = withDisplay(block(), PrLayoutDisplay_Flex);
    PrLayoutFlexStyle column = prLayoutFlexStyleDefault();
    column.direction = PrLayoutFlexDirection_Column;
    PrLayoutFontStyle font = lineHeight20();
    PrLayoutNode container = f.element(root, columnBox, nullptr, nullptr, &font, &column);
    PrLayoutNode item = semi(container, block());
    PrLayoutBoxStyle holder = block(100);
    holder.position = PrLayoutPosition_Relative;
    PrLayoutNode cb = element20(f, root, holder);
    PrLayoutBoxStyle inset = block();
    inset.position = PrLayoutPosition_Absolute;
    inset.inset[PrRectSide_Left] = prLayoutLengthPx(10);
    inset.inset[PrRectSide_Right] = prLayoutLengthPx(10);
    PrLayoutNode placed = semi(cb, inset);
    layout(f, 200, 300);
    CHECK(widthOf(f.bounds(inFlow)) == 40.0);
    CHECK(widthOf(f.bounds(inMargins)) == 40.0);
    CHECK(f.bounds(inMargins).left == 80.0f);
    CHECK(f.bounds(ended).left == 160.0f);
    CHECK(widthOf(f.bounds(item)) == 200.0);
    CHECK(widthOf(f.bounds(placed)) == 180.0);
    // It is an element, whose children are laid out in it, and a box.
    CHECK(prLayoutNodeKind(f.tree, inFlow) == PrLayoutNodeKind_SemiReplaced);
    CHECK((PrLayoutNodeKind_SemiReplaced & PrLayoutNodeKindFlag_Element) != 0);
    CHECK((PrLayoutNodeKind_SemiReplaced & PrLayoutNodeKindFlag_Box) != 0);
}

TEST_CASE("an inline-block shrinks to fit, takes a given width, and has zero auto margins") {
    Fixture f;
    PrLayoutNode root = root20(f);
    PrLayoutBoxStyle measured = block();
    measured.width = prLayoutLengthMaxContent();
    PrLayoutNode reference = element20(f, root, measured);
    f.text(reference, "some text");
    PrLayoutNode line = element20(f, root, block());
    PrLayoutNode fit = element20(f, line, inlineBlock());
    f.text(fit, "some text");
    PrLayoutBoxStyle given = inlineBlock();
    given.width = prLayoutLengthPx(50);
    given.margin[PrRectSide_Left] = prLayoutLengthAuto();
    given.margin[PrRectSide_Right] = prLayoutLengthAuto();
    PrLayoutNode fixed = element20(f, line, given);
    layout(f);
    double content = widthOf(f.bounds(reference));
    CHECK(widthOf(f.bounds(fit)) == doctest::Approx(content));
    CHECK(double(f.bounds(fit).left) == 0);
    CHECK(double(f.bounds(fixed).left) == doctest::Approx(content));
    CHECK(widthOf(f.bounds(fixed)) == 50.0);

    // In less room than its content asks, it fills the room, and its own lines break inside it.
    float room = float(int(content * 0.75));
    layout(f, room);
    CHECK(widthOf(f.bounds(fit)) == doctest::Approx(double(room)));
    CHECK(heightOf(f.bounds(fit)) == 40);
    CHECK(double(f.bounds(fixed).left) == 0);
}

TEST_CASE("an inline-block wraps as one object, and lines break around it") {
    Fixture f;
    PrLayoutNode root = root20(f);
    PrLayoutNode line = element20(f, root, block());
    PrLayoutNode words = f.text(line, "aa aa ");
    PrLayoutNode box = element20(f, line, inlineBlock());
    f.text(box, "b b b");
    layout(f);
    double before = double(f.bounds(words).right);
    double width = widthOf(f.bounds(box));
    CHECK(double(f.bounds(box).left) == doctest::Approx(before));

    // Too narrow for the inline-block beside the words, the line moves the inline-block to the next line whole: its
    // spaces are not break opportunities of the line around it.
    layout(f, float(before + width / 2));
    checkRect(f.bounds(box), 0, 20, width, 40);
    CHECK(heightOf(f.bounds(line)) == 40);
}

TEST_CASE("text flows on either side of an inline-block that holds text, and inline-blocks nest") {
    Fixture f;
    PrLayoutNode root = root20(f);
    PrLayoutNode paragraph = element20(f, root, block());
    PrLayoutNode before = f.text(paragraph, "before ");
    PrLayoutNode one = element20(f, paragraph, inlineBlock());
    PrLayoutNode oneText = f.text(one, "one ");
    PrLayoutNode two = element20(f, one, inlineBlock());
    PrLayoutNode twoText = f.text(two, "two ");
    PrLayoutNode three = element20(f, two, inlineBlock());
    PrLayoutNode threeText = f.text(three, "three");
    PrLayoutNode after = f.text(paragraph, " after");
    layout(f);
    auto check = [&]() {
        CHECK(f.bounds(one).left == f.bounds(before).right);
        CHECK(f.bounds(two).left == f.bounds(oneText).right);
        CHECK(f.bounds(three).left == f.bounds(twoText).right);
        CHECK(f.bounds(three).right == f.bounds(two).right);
        CHECK(f.bounds(two).right == f.bounds(one).right);
        CHECK(f.bounds(after).left == f.bounds(one).right);
        // Every line sits on one baseline, through the nesting.
        for (PrLayoutNode text : {oneText, twoText, threeText, after})
            CHECK(f.bounds(text).top == f.bounds(before).top);
        CHECK(heightOf(f.bounds(paragraph)) == 20);
    };
    check();

    // A change inside the innermost lays out each inline-block around it, and shapes its text alone.
    prLayoutNodeSetText(f.tree, threeText, "three and more");
    layout(f);
    PrLayoutTreeStats stats = prLayoutTreeStats(f.tree);
    CHECK(stats.inlinesShaped == 1);
    CHECK(stats.blocksLaidOut == 5);
    check();

    // A new width reuses every shaping.
    layout(f, 500);
    CHECK(prLayoutTreeStats(f.tree).inlinesShaped == 0);
    check();
}

TEST_CASE("intrinsic sizes measure inline-blocks nested in contexts of different numbers of items") {
    Fixture f;
    PrLayoutNode root = root20(f);
    PrLayoutBoxStyle measured = block();
    measured.width = prLayoutLengthMaxContent();
    PrLayoutNode container = element20(f, root, measured);
    f.text(container, "aa ");
    PrLayoutNode outer = element20(f, container, inlineBlock());
    PrLayoutNode inner = element20(f, outer, inlineBlock());
    f.text(inner, "b");
    f.text(outer, " cc ");
    PrLayoutBoxStyle spanBox = inlineBox();
    spanBox.padding[PrRectSide_Left] = prLayoutLengthPx(3);
    PrLayoutNode span = element20(f, outer, spanBox);
    f.text(span, "dd");
    f.text(container, " ee ");
    PrLayoutNode last = element20(f, container, inlineBlock());
    f.text(last, "ffff gggg");
    layout(f);
    // One line, as wide as its content.
    CHECK(heightOf(f.bounds(container)) == 20);
    CHECK(f.bounds(last).right == f.bounds(container).right);
    CHECK(heightOf(f.bounds(outer)) == 20);
    CHECK(f.bounds(span).right == f.bounds(outer).right);

    // Measured again, from scratch and from what each context keeps.
    prLayoutNodeSetText(f.tree, prLayoutNodeFirstChild(f.tree, inner), "bbbbbb");
    layout(f);
    CHECK(prLayoutTreeStats(f.tree).inlinesShaped == 1);
    CHECK(heightOf(f.bounds(container)) == 20);
    CHECK(f.bounds(last).right == f.bounds(container).right);

    // min-content: the widest unbreakable piece, which is an inline-block's own min-content.
    measured.width = prLayoutLengthMinContent();
    prLayoutNodeSetStyle(f.tree, container, f.style(measured));
    layout(f);
    CHECK(widthOf(f.bounds(container)) == doctest::Approx(widthOf(f.bounds(outer))));
}

TEST_CASE("an inline-block sits on its last line's baseline, or on its margin box's bottom") {
    Fixture f;
    PrLayoutNode root = root20(f);
    PrLayoutNode paragraph = element20(f, root, block());
    PrLayoutNode x = f.text(paragraph, "x");
    // Its last line is the second, in the second of its block children.
    PrLayoutNode lines = element20(f, paragraph, inlineBlock());
    f.text(element20(f, lines, block()), "a");
    PrLayoutNode b = f.text(element20(f, lines, block()), "b");
    // With no line, or with its lines clipped, it sits by its margin box's bottom, as a replaced box does.
    PrLayoutBoxStyle margined = sized(inlineBlock(), 30, 30);
    margined.margin[PrRectSide_Bottom] = prLayoutLengthPx(5);
    PrLayoutNode empty = element20(f, paragraph, margined);
    PrLayoutBoxStyle clipping = margined;
    clipping.overflowX = PrLayoutOverflow_Hidden;
    clipping.overflowY = PrLayoutOverflow_Hidden;
    PrLayoutNode clipped = element20(f, paragraph, clipping);
    f.text(clipped, "c");
    PrLayoutBoxStyle image = sized(inlineBox(), 30, 30);
    image.margin[PrRectSide_Bottom] = prLayoutLengthPx(5);
    PrLayoutNode replacedBox = replaced(f, paragraph, image, natural(30, 30));
    layout(f);
    CHECK(f.bounds(b).top == f.bounds(x).top);
    // The line grows above the baseline to hold the first of its two lines.
    CHECK(f.bounds(lines).top == f.bounds(paragraph).top);
    CHECK(f.bounds(empty).top == f.bounds(replacedBox).top);
    CHECK(f.bounds(clipped).top == f.bounds(replacedBox).top);
}

TEST_CASE("an inline-block's bottom padding and margin take the line's height below its baseline") {
    Fixture f;
    PrLayoutNode root = root20(f);
    PrLayoutNode paragraph = element20(f, root, block());
    PrLayoutNode x = f.text(paragraph, "x");
    PrLayoutBoxStyle padded = inlineBlock();
    padded.padding[PrRectSide_Bottom] = prLayoutLengthPx(40);
    padded.margin[PrRectSide_Bottom] = prLayoutLengthPx(20);
    PrLayoutNode box = element20(f, paragraph, padded);
    PrLayoutNode a = f.text(box, "a");
    PrLayoutNode next = f.text(paragraph, " next");
    layout(f);
    CHECK(f.bounds(a).top == f.bounds(x).top);
    // Its line is 20 for the text, plus the padding and margin below it.
    CHECK(heightOf(f.bounds(paragraph)) == 80);
    CHECK(heightOf(f.bounds(box)) == 60);

    // A line that follows starts below them.
    layout(f, f.bounds(box).right + 1);
    CHECK(heightOf(f.bounds(paragraph)) == 100);
    CHECK(f.bounds(next).top == f.bounds(x).top + 80);
}

TEST_CASE("a block's baseline is its last in-flow line's, through anonymous blocks and block children") {
    Fixture f;
    PrLayoutNode root = root20(f);
    PrLayoutNode paragraph = element20(f, root, block());
    PrLayoutNode x = f.text(paragraph, "x");
    // A later child with no line keeps the earlier child's baseline.
    PrLayoutNode kept = element20(f, paragraph, inlineBlock());
    PrLayoutNode a = f.text(element20(f, kept, block()), "a");
    element20(f, kept, block(30));
    // Text among block children is in anonymous blocks, which give their lines' baselines.
    PrLayoutNode anonymous = element20(f, paragraph, inlineBlock());
    element20(f, anonymous, block(10));
    PrLayoutNode c = f.text(anonymous, "c");
    element20(f, anonymous, block(10));
    layout(f);
    CHECK(f.bounds(a).top == f.bounds(x).top);
    CHECK(f.bounds(c).top == f.bounds(x).top);
    CHECK(f.bounds(kept).top == f.bounds(paragraph).top + 10);
    CHECK(heightOf(f.bounds(kept)) == 50);
    CHECK(heightOf(f.bounds(paragraph)) == 60);
}

TEST_CASE("an inline-block contributes to its container's intrinsic sizes, and changes inside it reshape nothing around it") {
    for (bool inSpan : {false, true}) {
        CAPTURE(inSpan);
        Fixture f;
        PrLayoutNode root = root20(f);
        PrLayoutBoxStyle measured = block();
        measured.width = prLayoutLengthMaxContent();
        PrLayoutNode container = element20(f, root, measured);
        PrLayoutNode words = f.text(container, "words ");
        PrLayoutNode parent = inSpan ? element20(f, container, inlineBox()) : container;
        PrLayoutBoxStyle margined = inlineBlock();
        margined.margin[PrRectSide_Left] = prLayoutLengthPx(7);
        PrLayoutNode box = element20(f, parent, margined);
        PrLayoutNode image = replaced(f, box, inlineBox(), natural(40, 10));
        PrLayoutNode text = f.text(box, "t");
        layout(f);
        CHECK(f.bounds(box).left == f.bounds(words).right + 7);
        CHECK(f.bounds(box).right == f.bounds(container).right);
        double before = widthOf(f.bounds(container));

        // A new natural size lays out the image's container, and shapes nothing.
        prLayoutNodeSetNaturalSize(f.tree, image, natural(80, 10));
        layout(f);
        CHECK(prLayoutTreeStats(f.tree).inlinesShaped == 0);
        CHECK(widthOf(f.bounds(container)) == doctest::Approx(before + 40));
        CHECK(f.bounds(box).right == f.bounds(container).right);

        // New text in it shapes its own context alone.
        prLayoutNodeSetText(f.tree, text, "longer text");
        layout(f);
        CHECK(prLayoutTreeStats(f.tree).inlinesShaped == 1);
        CHECK(f.bounds(box).right == f.bounds(container).right);
        CHECK(heightOf(f.bounds(container)) == 20);
    }
}

TEST_CASE("a change inside an inline-block lays it out again and shapes no text of its line") {
    Fixture f;
    PrLayoutNode root = root20(f);
    PrLayoutNode paragraph = element20(f, root, block());
    f.text(paragraph, "before ");
    PrLayoutNode box = element20(f, paragraph, inlineBlock());
    PrLayoutNode child = element20(f, box, sized(block(), 30, 10));
    PrLayoutNode after = f.text(paragraph, " after");
    layout(f);
    double start = double(f.bounds(after).left);
    prLayoutNodeSetStyle(f.tree, child, f.style(sized(block(), 50, 10)));
    layout(f);
    PrLayoutTreeStats stats = prLayoutTreeStats(f.tree);
    CHECK(stats.inlinesShaped == 0);
    CHECK(stats.blocksLaidOut == 4);
    CHECK(double(f.bounds(after).left) == doctest::Approx(start + 20));

    // Shaping ahead finds nothing to shape either.
    prLayoutNodeSetStyle(f.tree, child, f.style(sized(block(), 40, 10)));
    layout(f);
    CHECK(prLayoutTreeStats(f.tree).inlinesShaped == 0);
}

TEST_CASE("a relatively positioned inline-block moves alone, and nothing of its line") {
    Fixture f;
    PrLayoutNode root = root20(f);
    PrLayoutNode paragraph = element20(f, root, block());
    PrLayoutBoxStyle box = sized(inlineBlock(), 20, 20);
    box.position = PrLayoutPosition_Relative;
    PrLayoutNode moved = element20(f, paragraph, box);
    PrLayoutNode sibling = element20(f, paragraph, sized(inlineBlock(), 20, 20));
    PrLayoutNode words = f.text(paragraph, " words that wrap");
    layout(f, 100);
    PrRect at = f.bounds(moved);
    PrRect siblingAt = f.bounds(sibling);
    PrLayoutRect lines[4];
    CHECK(prLayoutNodeBoxes(f.tree, words, PrLayoutCoordinates_Viewport, lines, 4) == 2);
    PrLayoutRect second = lines[1];
    double height = heightOf(f.bounds(paragraph));

    auto check = [&](float dx, float dy) {
        PrRect now = f.bounds(moved);
        CHECK(double(now.left) == doctest::Approx(double(at.left + dx)));
        CHECK(double(now.top) == doctest::Approx(double(at.top + dy)));
        checkRect(f.bounds(sibling), double(siblingAt.left), double(siblingAt.top), double(siblingAt.right),
            double(siblingAt.bottom));
        PrLayoutRect boxes[4];
        CHECK(prLayoutNodeBoxes(f.tree, words, PrLayoutCoordinates_Viewport, boxes, 4) == 2);
        CHECK(boxes[1].top == second.top);
        CHECK(heightOf(f.bounds(paragraph)) == height);
        // The line's text is reused.
        CHECK(prLayoutTreeStats(f.tree).inlinesShaped == 0);
    };
    box.inset[PrRectSide_Left] = prLayoutLengthPx(7);
    box.inset[PrRectSide_Top] = prLayoutLengthPx(3);
    prLayoutNodeSetStyle(f.tree, moved, f.style(box));
    layout(f, 100);
    check(7, 3);
    box.inset[PrRectSide_Left] = prLayoutLengthAuto();
    box.inset[PrRectSide_Top] = prLayoutLengthAuto();
    box.inset[PrRectSide_Right] = prLayoutLengthPx(4);
    box.inset[PrRectSide_Bottom] = prLayoutLengthPx(6);
    prLayoutNodeSetStyle(f.tree, moved, f.style(box));
    layout(f, 100);
    check(-4, -6);
}

TEST_CASE("an inline-block's descendants that paint late paint above its following siblings") {
    enum { Late_None, Late_Relative, Late_Layer, Late_Opacity };
    typedef uint8_t Late;
    for (Late late : {Late_None, Late_Relative, Late_Layer, Late_Opacity}) {
        CAPTURE(int(late));
        Fixture f;
        PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
        PrLayoutNode paragraph = f.element(root, block());
        PrLayoutNode first = f.element(paragraph, sized(inlineBlock(), 20, 20));
        // It overflows its inline-block into the next one.
        PrLayoutBoxStyle wide = sized(block(), 30, 20);
        PrLayoutEffectStyle effects = prLayoutEffectStyleDefault();
        if (late == Late_Relative)
            wide.position = PrLayoutPosition_Relative;
        else if (late == Late_Layer)
            effects.layer = true;
        else if (late == Late_Opacity)
            effects.opacity = 0.5f;
        PrLayoutDecorationStyle red = background(PrColor{1, 0, 0, 1});
        PrLayoutNode child = f.element(first, wide, &red, &effects);
        PrLayoutDecorationStyle green = background(PrColor{0, 1, 0, 1});
        PrLayoutNode second = f.element(paragraph, sized(inlineBlock(), 20, 20), &green);
        layout(f, 60, 40);
        checkRect(f.bounds(child), 0, 0, 30, 20);
        checkRect(f.bounds(second), 20, 0, 40, 20);
        std::vector<uint8_t> pixels = render(f.tree, 60, 40);
        if (late != Late_Opacity)
            CHECK(pixel(pixels, 60, 10, 10) == "255 0 0 255");
        CHECK(pixel(pixels, 60, 35, 10) == "0 255 0 255");
        if (late == Late_None) {
            CHECK(pixel(pixels, 60, 25, 10) == "0 255 0 255");
            CHECK(f.hit(PrVec2{25, 10}) == second);
        } else {
            CHECK(pixel(pixels, 60, 25, 10) != "0 255 0 255");
            CHECK(f.hit(PrVec2{25, 10}) == child);
        }
        CHECK(f.hit(PrVec2{10, 10}) == child);
        CHECK(verified(f.tree));
    }
}

TEST_CASE("an inline-block with a transform paints whole, and holds its children") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutNode paragraph = f.element(root, block());
    PrLayoutEffectStyle moved = prLayoutEffectStyleDefault();
    moved.transform = prLayoutTransformFromAffine2(prAffine2Translate(PrVec2{5, 0}));
    PrLayoutNode first = f.element(paragraph, sized(inlineBlock(), 20, 20), nullptr, &moved);
    PrLayoutBoxStyle relative = sized(block(), 30, 20);
    relative.position = PrLayoutPosition_Relative;
    PrLayoutDecorationStyle red = background(PrColor{1, 0, 0, 1});
    PrLayoutNode child = f.element(first, relative, &red);
    PrLayoutDecorationStyle green = background(PrColor{0, 1, 0, 1});
    f.element(paragraph, sized(inlineBlock(), 20, 20), &green);
    layout(f, 60, 40);
    const PrLayoutScene* scene = prLayoutTreeScene(f.tree);
    CHECK(prLayoutSceneHasLayer(scene, first));
    CHECK(!prLayoutSceneHasLayer(scene, child));
    prLayoutSceneRelease(scene);
    checkRect(f.bounds(child), 5, 0, 35, 20);
    std::vector<uint8_t> pixels = render(f.tree, 60, 40);
    CHECK(pixel(pixels, 60, 2, 10) == "0 0 0 0");
    CHECK(pixel(pixels, 60, 32, 10) == "255 0 0 255");
    CHECK(f.hit(PrVec2{32, 10}) == child);
}

TEST_CASE("the root is laid out as a block box whatever its display") {
    for (PrLayoutDisplay display : {PrLayoutDisplay_Inline, PrLayoutDisplay_InlineBlock}) {
        CAPTURE(int(display));
        Fixture f;
        PrLayoutBoxStyle box = withDisplay(block(), display);
        box.width = prLayoutLengthPx(100);
        box.margin[PrRectSide_Left] = prLayoutLengthAuto();
        box.margin[PrRectSide_Right] = prLayoutLengthAuto();
        PrLayoutNode root = f.element(kPrLayoutNodeNone, withDisplay(block(), display));
        PrLayoutNode child = f.element(root, box);
        f.text(child, "x");
        f.text(root, "y");
        layout(f, 300, 100);
        // It fills the viewport. An inline-block in it shrinks, with auto margins of zero.
        CHECK(widthOf(f.bounds(root)) == 300.0);
        if (display == PrLayoutDisplay_InlineBlock)
            CHECK(double(f.bounds(child).left) == 0);
    }
}

TEST_CASE("a box's content extent is its border box less the borders and the padding layout resolved") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutBoxStyle containing = block();
    containing.width = prLayoutLengthPx(200);
    containing.padding[PrRectSide_Left] = prLayoutLengthPx(30);
    PrLayoutNode container = f.element(root, containing);
    PrLayoutNode wrapper = f.element(container, inlineBox());
    PrLayoutBoxStyle padded = sized(inlineBlock(), 20, 10);
    padded.padding[PrRectSide_Left] = prLayoutLengthPercent(10);
    padded.padding[PrRectSide_Top] = prLayoutLengthPercent(5);
    padded.border[PrRectSide_Right] = px(3, 0).x;
    PrLayoutNode box = f.element(wrapper, padded);
    PrLayoutBoxStyle image = inlineBox();
    image.padding[PrRectSide_Bottom] = prLayoutLengthPercent(10);
    PrLayoutNode img = replaced(f, wrapper, image, natural(30, 15));
    layout(f);
    CHECK(prLayoutNodeContentExtent(f.tree, box) == px(20, 10));
    CHECK(widthOf(f.bounds(box)) == 43.0);
    CHECK(prLayoutNodeContentExtent(f.tree, img) == px(30, 15));
    CHECK(heightOf(f.bounds(img)) == 35);
    // An inline box, text and the root's own box.
    CHECK(prLayoutNodeContentExtent(f.tree, wrapper) == PrLayoutVec{});
    PrLayoutRect rootBox = prLayoutNodeBounds(f.tree, root, PrLayoutCoordinates_Layout);
    CHECK(prLayoutNodeContentExtent(f.tree, root) ==
        PrLayoutVec{rootBox.right - rootBox.left, rootBox.bottom - rootBox.top});

    // A box no longer generated has none, though its node's cache outlives it.
    padded.display = PrLayoutDisplay_Inline;
    prLayoutNodeSetStyle(f.tree, box, f.style(padded));
    layout(f);
    CHECK(prLayoutNodeContentExtent(f.tree, box) == PrLayoutVec{});
    containing.display = PrLayoutDisplay_None;
    prLayoutNodeSetStyle(f.tree, container, f.style(containing));
    layout(f);
    CHECK(prLayoutNodeContentExtent(f.tree, img) == PrLayoutVec{});
    containing.display = PrLayoutDisplay_Block;
    prLayoutNodeSetStyle(f.tree, container, f.style(containing));
    layout(f);
    CHECK(prLayoutNodeContentExtent(f.tree, img) == px(30, 15));
    prLayoutTreeRemove(f.tree, wrapper);
    layout(f);
    CHECK(prLayoutNodeContentExtent(f.tree, img) == PrLayoutVec{});
}

TEST_CASE("an inline replaced box is laid out by its own cache, again only where it changed") {
    Fixture f;
    PrLayoutNode root = root20(f);
    PrLayoutNode paragraph = element20(f, root, block());
    f.text(paragraph, "before ");
    PrLayoutNode image = replaced(f, paragraph, inlineBox(), natural(30, 10));
    PrLayoutNode after = f.text(paragraph, " after");
    layout(f);
    double start = double(f.bounds(after).left);

    // New text lays out the paragraph, and reuses the image's box.
    prLayoutNodeSetText(f.tree, after, " later");
    layout(f);
    CHECK(prLayoutTreeStats(f.tree).blocksReused == 1);

    // A new natural size lays out the image again, and shapes nothing.
    prLayoutNodeSetNaturalSize(f.tree, image, natural(50, 10));
    layout(f);
    PrLayoutTreeStats stats = prLayoutTreeStats(f.tree);
    CHECK(stats.inlinesShaped == 0);
    CHECK(stats.blocksReused == 0);
    CHECK(widthOf(f.bounds(image)) == 50.0);
    CHECK(double(f.bounds(after).left) == doctest::Approx(start + 20));
}
