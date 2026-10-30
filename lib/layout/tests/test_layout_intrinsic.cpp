#include "pr/layout_query.h"
#include "pr/layout_style.h"
#include "pr/layout_tree.h"

#include "layout/tests/layout_fixture.h"

#include "doctest/doctest.h"

namespace pr_impl {

namespace {

// The images the cases measure: R is 400 by 200, Q has a 2:1 ratio and no natural size, N has neither, and H has a
// natural height of 40 and a 2:1 ratio.
PrLayoutNaturalSize imageR() {
    return natural(400, 200);
}
PrLayoutNaturalSize imageQ() {
    return natural(-1, -1, 2);
}
PrLayoutNaturalSize imageN() {
    return natural(-1, -1);
}
PrLayoutNaturalSize imageH() {
    return natural(-1, 40, 2);
}

PrLayoutBoxStyle withWidth(PrLayoutBoxStyle box, PrLayoutLength width) {
    box.width = width;
    return box;
}

// Returns `pixels` plus `percent`, as calc() gives it.
PrLayoutLength calc(Fixture& f, double pixels, double percent) {
    PrLayoutCalcNode nodes[] = {
        prLayoutCalcOperation(PrLayoutCalcOp_Sum, 2),
        prLayoutCalcLeaf(PrLayoutCalcLeaf_Px, pixels),
        prLayoutCalcLeaf(PrLayoutCalcLeaf_Percent, percent),
    };
    return prLayoutLengthIntern(f.tree, nodes, 3);
}

double widthOf(Fixture& f, PrLayoutNode node) {
    PrRect rect = f.bounds(node);
    return double(rect.right - rect.left);
}

double heightOf(Fixture& f, PrLayoutNode node) {
    PrRect rect = f.bounds(node);
    return double(rect.bottom - rect.top);
}

// Lays the tree out in a viewport of `width` by 600, and checks the incremental work.
void layout(Fixture& f, float width = 800) {
    f.layout(px(width, 600));
    CHECK(verified(f.tree));
}

// The widths of a box sized by `parentWidth` in a root block, which holds an image, and of the image.
struct Measured {
    double box;
    double image;
};

// `style` makes the image's style in the fixture, whose context holds any calculation that the style names.
template <typename Style>
Measured measureWith(PrLayoutLength parentWidth, Style style, PrLayoutNaturalSize size) {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutNode box = f.element(root, withWidth(block(), parentWidth));
    PrLayoutNode node = replaced(f, box, style(f), size);
    layout(f);
    return Measured{widthOf(f, box), widthOf(f, node)};
}
Measured measure(PrLayoutLength parentWidth, PrLayoutBoxStyle image, PrLayoutNaturalSize size) {
    return measureWith(parentWidth, [&](Fixture&) { return image; }, size);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("intrinsic sizes: an image's natural size against its own width") {
    // A fixed width is the contribution, and the intrinsic size ignores the box's own width.
    Measured fixed = measure(prLayoutLengthMaxContent(), withWidth(inlineBox(), prLayoutLengthPx(100)), imageR());
    CHECK(fixed.box == 100);
    CHECK(fixed.image == 100);
    CHECK(measure(prLayoutLengthAuto(), withWidth(inlineBox(), prLayoutLengthMinContent()), imageR()).image == 400);
}

TEST_CASE("intrinsic sizes: an image with a ratio and no natural size") {
    // An auto width contributes zero, and a minimum floors it.
    Measured unsized = measure(prLayoutLengthMaxContent(), inlineBox(), imageQ());
    CHECK(unsized.box == 0);
    CHECK(unsized.image == 0);
    PrLayoutBoxStyle minimum = inlineBox();
    minimum.minWidth = prLayoutLengthPx(50);
    Measured floored = measure(prLayoutLengthMaxContent(), minimum, imageQ());
    CHECK(floored.box == 50);
    CHECK(floored.image == 50);
    // The image's own keywords: max-content is the initial containing block's width, and min-content is zero.
    CHECK(measure(prLayoutLengthAuto(), withWidth(inlineBox(), prLayoutLengthMaxContent()), imageQ()).image == 800);
    CHECK(measure(prLayoutLengthAuto(), withWidth(inlineBox(), prLayoutLengthMinContent()), imageQ()).image == 0);
}

TEST_CASE("intrinsic sizes: an image with no ratio and no natural size") {
    // The default object size, 300px (CSS2/#inline-replaced-width). With a length minimum, the box contributes the
    // minimum and the image keeps its size.
    Measured fallback = measure(prLayoutLengthMaxContent(), inlineBox(), imageN());
    CHECK(fallback.box == 300);
    CHECK(fallback.image == 300);
    PrLayoutBoxStyle minimum = inlineBox();
    minimum.minWidth = prLayoutLengthPx(50);
    Measured floored = measure(prLayoutLengthMaxContent(), minimum, imageN());
    CHECK(floored.box == 50);
    CHECK(floored.image == 300);
}

TEST_CASE("intrinsic sizes: a block size decides an image's width through its ratio") {
    // A definite height comes before the natural width.
    PrLayoutBoxStyle high = withWidth(inlineBox(), prLayoutLengthMinContent());
    high.height = prLayoutLengthPx(50);
    CHECK(measure(prLayoutLengthAuto(), high, imageR()).image == 100);
    // A percentage height of a definite containing block.
    {
        Fixture f;
        PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
        PrLayoutNode box = f.element(root, block(100));
        PrLayoutBoxStyle image = withWidth(inlineBox(), prLayoutLengthMinContent());
        image.height = prLayoutLengthPercent(50);
        PrLayoutNode node = replaced(f, box, image, imageR());
        layout(f);
        CHECK(widthOf(f, node) == 100);
        CHECK(heightOf(f, node) == 50);
    }
    // A natural height through the ratio.
    Measured natural = measure(prLayoutLengthMaxContent(), inlineBox(), imageH());
    CHECK(natural.box == 80);
    CHECK(natural.image == 80);
    // A maximum height transfers through the ratio.
    PrLayoutBoxStyle limited = inlineBox();
    limited.maxHeight = prLayoutLengthPx(50);
    Measured transferred = measure(prLayoutLengthMaxContent(), limited, imageR());
    CHECK(transferred.box == 100);
    CHECK(transferred.image == 100);
    CHECK(measure(prLayoutLengthAuto(), withWidth(limited, prLayoutLengthMinContent()), imageR()).image == 100);
}

TEST_CASE("intrinsic sizes: a cyclic percentage compresses an image at min-content and is auto at max-content") {
    // A width of calc(50% + 50px), and of 50%; a maximum of calc(10% + 30px), alone and with padding and a minimum.
    auto mixed = [](Fixture& f) { return withWidth(inlineBox(), calc(f, 50, 50)); };
    CHECK(measureWith(prLayoutLengthMinContent(), mixed, imageR()).box == 0);
    CHECK(measureWith(prLayoutLengthMaxContent(), mixed, imageR()).box == 400);
    CHECK(measure(prLayoutLengthMinContent(), withWidth(inlineBox(), prLayoutLengthPercent(50)), imageR()).box == 0);
    auto capped = [](Fixture& f) {
        PrLayoutBoxStyle box = inlineBox();
        box.maxWidth = calc(f, 30, 10);
        return box;
    };
    CHECK(measureWith(prLayoutLengthMinContent(), capped, imageR()).box == 0);
    auto padded = [&](Fixture& f) {
        PrLayoutBoxStyle box = capped(f);
        box.minWidth = prLayoutLengthPx(20);
        for (uint32_t side = 0; side < PrRectSide_Count; ++side)
            box.padding[side] = prLayoutLengthPx(5);
        return box;
    };
    CHECK(measureWith(prLayoutLengthMinContent(), padded, imageR()).box == 30);
    // A block minimum does not transfer through the ratio, and an inline minimum floors the contribution.
    PrLayoutBoxStyle blockMinimum = withWidth(inlineBox(), prLayoutLengthPercent(50));
    blockMinimum.minHeight = prLayoutLengthPx(30);
    CHECK(measure(prLayoutLengthMinContent(), blockMinimum, imageR()).box == 0);
    PrLayoutBoxStyle inlineMinimum = blockMinimum;
    inlineMinimum.minWidth = prLayoutLengthPx(50);
    CHECK(measure(prLayoutLengthMinContent(), inlineMinimum, imageR()).box == 50);
}

TEST_CASE("intrinsic sizes: a block's contribution from its sizing properties") {
    // Each child holds a 120px image, in a box sized by its contribution.
    auto contribution = [](PrLayoutLength parentWidth, auto style) {
        Fixture f;
        PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
        PrLayoutNode box = f.element(root, withWidth(block(), parentWidth));
        PrLayoutBoxStyle child = block();
        style(f, child);
        PrLayoutNode node = f.element(box, child);
        replaced(f, node, inlineBox(), natural(120, 10));
        layout(f);
        return widthOf(f, box);
    };
    // A percentage width behaves as auto.
    CHECK(contribution(prLayoutLengthMaxContent(),
        [](Fixture&, PrLayoutBoxStyle& box) { box.width = prLayoutLengthPercent(50); }) == 120);
    // A minimum resolves its percentage against zero (css-sizing-3/#cyclic-percentage-contribution).
    CHECK(contribution(prLayoutLengthMinContent(),
        [](Fixture& f, PrLayoutBoxStyle& box) { box.minWidth = calc(f, 200, 50); }) == 200);
    // A maximum with a percentage is none.
    CHECK(contribution(prLayoutLengthMaxContent(),
        [](Fixture& f, PrLayoutBoxStyle& box) { box.maxWidth = calc(f, 10, 1); }) == 120);
    // Percentage margins and padding are zero.
    CHECK(contribution(prLayoutLengthMaxContent(), [](Fixture&, PrLayoutBoxStyle& box) {
        box.padding[PrRectSide_Left] = prLayoutLengthPercent(10);
        box.margin[PrRectSide_Left] = prLayoutLengthPercent(20);
    }) == 120);
    // Fixed margins add to it, and a fixed width replaces the content's.
    CHECK(contribution(prLayoutLengthMaxContent(), [](Fixture&, PrLayoutBoxStyle& box) {
        box.margin[PrRectSide_Left] = prLayoutLengthPx(7);
        box.margin[PrRectSide_Right] = prLayoutLengthPx(3);
    }) == 130);
    CHECK(contribution(prLayoutLengthMaxContent(),
        [](Fixture&, PrLayoutBoxStyle& box) { box.width = prLayoutLengthPx(60); }) == 60);
    // A minimum wins over a maximum, fixed or a keyword.
    CHECK(contribution(prLayoutLengthMaxContent(), [](Fixture&, PrLayoutBoxStyle& box) {
        box.width = prLayoutLengthPx(150);
        box.minWidth = prLayoutLengthPx(200);
        box.maxWidth = prLayoutLengthPx(100);
    }) == 200);
    CHECK(contribution(prLayoutLengthMaxContent(), [](Fixture&, PrLayoutBoxStyle& box) {
        box.width = prLayoutLengthPx(50);
        box.minWidth = prLayoutLengthMaxContent();
        box.maxWidth = prLayoutLengthPx(100);
    }) == 120);
}

TEST_CASE("intrinsic sizes: text gives its longest word and its whole line") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutNode word = f.element(root, withWidth(block(), prLayoutLengthMaxContent()));
    f.text(word, "bbbbbb");
    PrLayoutNode narrow = f.element(root, withWidth(block(), prLayoutLengthMinContent()));
    PrLayoutNode narrowText = f.text(narrow, "aaa bbbbbb");
    PrLayoutNode wide = f.element(root, withWidth(block(), prLayoutLengthMaxContent()));
    PrLayoutNode wideText = f.text(wide, "aaa bbbbbb");
    PrLayoutNode hanging = f.element(root, withWidth(block(), prLayoutLengthMaxContent()));
    f.text(hanging, "aaa bbbbbb ");
    PrLayoutNode padded = f.element(root, withWidth(block(), prLayoutLengthMinContent()));
    PrLayoutBoxStyle span = inlineBox();
    span.padding[PrRectSide_Left] = prLayoutLengthPx(10);
    f.text(f.element(padded, span), "bbbbbb");
    layout(f);
    CHECK(widthOf(f, narrow) == widthOf(f, word));
    CHECK(widthOf(f, wide) > widthOf(f, word));
    CHECK(widthOf(f, hanging) == widthOf(f, wide));
    CHECK(widthOf(f, padded) == widthOf(f, word) + 10);
    // Laid out at max-content, the text takes one line. At min-content, it takes a line for each word.
    CHECK(prLayoutNodeBoxes(f.tree, wideText, PrLayoutCoordinates_Layout, nullptr, 0) == 1);
    CHECK(prLayoutNodeBoxes(f.tree, narrowText, PrLayoutCoordinates_Layout, nullptr, 0) == 2);
}

TEST_CASE("intrinsic sizes: fit-content shrinks to fit, and auto margins share what it leaves") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutBoxStyle centered = withWidth(block(), prLayoutLengthFitContent());
    centered.margin[PrRectSide_Left] = prLayoutLengthAuto();
    centered.margin[PrRectSide_Right] = prLayoutLengthAuto();
    PrLayoutNode small = f.element(root, centered);
    replaced(f, small, inlineBox(), natural(100, 10));
    PrLayoutNode container = f.element(root, withWidth(block(), prLayoutLengthPx(150)));
    PrLayoutNode squeezed = f.element(container, withWidth(block(), prLayoutLengthFitContent()));
    replaced(f, squeezed, inlineBox(), natural(100, 10));
    f.text(squeezed, " ");
    replaced(f, squeezed, inlineBox(), natural(100, 10));
    PrLayoutBoxStyle grown = withWidth(block(), prLayoutLengthPx(20));
    grown.minWidth = prLayoutLengthMaxContent();
    PrLayoutNode minimum = f.element(root, grown);
    replaced(f, minimum, inlineBox(), natural(90, 10));
    layout(f);
    CHECK(widthOf(f, small) == 100);
    CHECK(double(f.bounds(small).left) == 350);
    CHECK(widthOf(f, squeezed) == 150);
    CHECK(widthOf(f, minimum) == 90);
}

TEST_CASE("intrinsic sizes: a keyword limit on the block size clamps after layout") {
    // The children are laid out against the fixed height, and then the limit clamps the box.
    auto article = [](PrLayoutBoxStyle box, float percent, float content) {
        Fixture f;
        PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
        PrLayoutNode stage = f.element(root, block(400));
        PrLayoutNode node = f.element(stage, box);
        PrLayoutBoxStyle aside = block();
        aside.height = prLayoutLengthPercent(percent);
        PrLayoutNode child = f.element(node, aside);
        f.element(child, block(content));
        f.element(node, block(30));
        layout(f);
        return std::vector<double>{heightOf(f, node), heightOf(f, child)};
    };
    PrLayoutBoxStyle minimum = block(100);
    minimum.minHeight = prLayoutLengthMinContent();
    CHECK(article(minimum, 50, 150) == std::vector<double>{100, 50});
    CHECK(article(minimum, 200, 150) == std::vector<double>{230, 200});
    minimum.minHeight = prLayoutLengthMaxContent();
    CHECK(article(minimum, 50, 150) == std::vector<double>{100, 50});
    PrLayoutBoxStyle maximum = block(100);
    maximum.maxHeight = prLayoutLengthMinContent();
    CHECK(article(maximum, 50, 20) == std::vector<double>{80, 50});
    // A keyword block size is auto, and a fixed limit clamps it without making the children's base definite.
    PrLayoutBoxStyle keyword = block();
    keyword.height = prLayoutLengthMinContent();
    keyword.minHeight = prLayoutLengthPx(300);
    CHECK(article(keyword, 50, 20) == std::vector<double>{300, 20});
}

TEST_CASE("intrinsic sizes: the children's block base takes the fixed limits") {
    // A block-level image of height 100% in a max-content box whose limits decide its height.
    auto box = [](float height, float min, float max) {
        Fixture f;
        PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
        PrLayoutBoxStyle style = withWidth(block(height), prLayoutLengthMaxContent());
        if (min >= 0)
            style.minHeight = prLayoutLengthPx(min);
        if (max >= 0)
            style.maxHeight = prLayoutLengthPx(max);
        PrLayoutNode node = f.element(root, style);
        PrLayoutBoxStyle image = block();
        image.height = prLayoutLengthPercent(100);
        PrLayoutNode child = replaced(f, node, image, imageQ());
        layout(f);
        return std::vector<double>{widthOf(f, node), widthOf(f, child)};
    };
    CHECK(box(50, 100, -1) == std::vector<double>{200, 200});
    CHECK(box(200, -1, 100) == std::vector<double>{200, 200});
}

TEST_CASE("an inline image's percentage height resolves against its block container") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutBoxStyle image = inlineBox();
    image.height = prLayoutLengthPercent(50);
    PrLayoutNode definite = f.element(root, block(100));
    PrLayoutNode bare = replaced(f, definite, image, imageR());
    PrLayoutNode wrapped = replaced(f, f.element(definite, inlineBox()), image, imageR());
    PrLayoutNode content = f.element(root, block());
    PrLayoutNode unresolved = replaced(f, content, image, imageR());
    layout(f);
    CHECK(heightOf(f, bare) == 50);
    CHECK(widthOf(f, bare) == 100);
    CHECK(heightOf(f, wrapped) == 50);
    CHECK(heightOf(f, unresolved) == 200);
}

TEST_CASE("intrinsic sizes: a change measures its path again and reuses the rest") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutNode parent = f.element(root, withWidth(block(), prLayoutLengthFitContent()));
    std::vector<PrLayoutNode> texts;
    for (int i = 0; i < 3; ++i)
        texts.push_back(f.text(f.element(parent, block()), "some card text"));
    layout(f);
    prLayoutNodeSetText(f.tree, texts[1], "other card text");
    layout(f);
    PrLayoutTreeStats stats = prLayoutTreeStats(f.tree);
    // The parent and the changed card, which measures its text again; the other two cards are reused.
    CHECK(stats.intrinsicsComputed == 2);
    CHECK(stats.intrinsicsReused == 2);
    // Several changes before one layout.
    prLayoutNodeSetText(f.tree, texts[0], "a");
    prLayoutNodeSetText(f.tree, texts[2], "a much longer line of card text");
    layout(f);
}

TEST_CASE("intrinsic sizes: a normal-flow layout that clears a change leaves no stale sizes") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutNode node = f.element(root, withWidth(block(), prLayoutLengthMinContent()));
    PrLayoutNode text = f.text(node, "aaa");
    layout(f);
    double before = widthOf(f, node);
    // Measured, then laid out in normal flow while its text changes, then asked for its sizes again.
    prLayoutNodeSetStyle(f.tree, node, f.style(block()));
    layout(f);
    prLayoutNodeSetText(f.tree, text, "aaaaaaaa");
    layout(f);
    prLayoutNodeSetStyle(f.tree, node, f.style(withWidth(block(), prLayoutLengthMinContent())));
    layout(f);
    CHECK(widthOf(f, node) > before);
}

TEST_CASE("intrinsic sizes: a percentage block base that comes and goes") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutNode box = f.element(root, block(100));
    PrLayoutBoxStyle image = withWidth(block(), prLayoutLengthMaxContent());
    image.height = prLayoutLengthPercent(50);
    PrLayoutNode node = replaced(f, box, image, imageQ());
    layout(f);
    CHECK(widthOf(f, node) == 100);
    prLayoutNodeSetStyle(f.tree, box, f.style(block()));
    layout(f);
    CHECK(widthOf(f, node) == 800);
    prLayoutNodeSetStyle(f.tree, box, f.style(block(100)));
    layout(f);
    CHECK(widthOf(f, node) == 100);
}

TEST_CASE("intrinsic sizes: a resize of the viewport lays out what read it, through a fixed-size block") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutNode fixed = f.element(root, withWidth(block(300), prLayoutLengthPx(300)));
    PrLayoutNode image = replaced(f, fixed, withWidth(block(), prLayoutLengthMaxContent()), imageQ());
    // A sibling's child keeps its space, as the image does, but reads nothing of the viewport.
    PrLayoutNode sibling = f.element(root, withWidth(block(20), prLayoutLengthPx(300)));
    f.text(f.element(sibling, block()), "text");
    layout(f);
    CHECK(widthOf(f, image) == 800);
    layout(f, 1000);
    CHECK(widthOf(f, image) == 1000);
    CHECK(prLayoutTreeStats(f.tree).blocksReused == 1);
}

TEST_CASE("intrinsic sizes: a keyword size has the edges the box resolves in its containing block") {
    // The content is intrinsic, but percentage padding resolves against the 800px containing block: 10% is 80px, and
    // calc(10px + 5%) is 50px.
    auto padded = [](auto padding, bool image) {
        Fixture f;
        PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
        PrLayoutBoxStyle box = withWidth(block(), prLayoutLengthMaxContent());
        box.padding[PrRectSide_Left] = padding(f);
        PrLayoutNode node = image ? replaced(f, root, box, natural(100, 10)) : f.element(root, box);
        if (!image)
            replaced(f, node, inlineBox(), natural(100, 10));
        layout(f);
        return widthOf(f, node);
    };
    auto percent = [](Fixture&) { return prLayoutLengthPercent(10); };
    auto mixed = [](Fixture& f) { return calc(f, 10, 5); };
    CHECK(padded(percent, false) == 180);
    CHECK(padded(mixed, false) == 150);
    CHECK(padded(percent, true) == 180);
    CHECK(padded(mixed, true) == 150);
}

TEST_CASE("intrinsic sizes: an empty inline box at either end of the content counts its edges") {
    // A 100px image, with empty inline boxes before and after it, nested or not, whose edges face it or face away.
    auto measured = [](auto build) {
        Fixture f;
        PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
        PrLayoutNode box = f.element(root, withWidth(block(), prLayoutLengthMaxContent()));
        build(f, box);
        layout(f);
        return widthOf(f, box);
    };
    auto edged = [](float start, float end) {
        PrLayoutBoxStyle span = inlineBox();
        span.margin[PrRectSide_Left] = prLayoutLengthPx(start);
        span.padding[PrRectSide_Right] = prLayoutLengthPx(end);
        span.border[PrRectSide_Right] = prLayoutUnitFromPx(1);
        return span;
    };
    auto image = [](Fixture& f, PrLayoutNode parent) { replaced(f, parent, inlineBox(), natural(100, 10)); };
    CHECK(measured([&](Fixture& f, PrLayoutNode box) {
        image(f, box);
        f.element(box, edged(20, 0));
    }) == 121);
    CHECK(measured([&](Fixture& f, PrLayoutNode box) {
        f.element(box, edged(0, 20));
        image(f, box);
    }) == 121);
    CHECK(measured([&](Fixture& f, PrLayoutNode box) {
        f.element(f.element(box, edged(5, 7)), edged(5, 7));
        image(f, box);
        f.element(f.element(box, edged(5, 7)), edged(5, 7));
    }) == 152);
}

TEST_CASE("intrinsic sizes: a change of writing mode measures again in the other axis") {
    // The same nodes in each mode: a box sized to its content, around an image 100 wide and 50 high in a line. The
    // box's inline size is its width in horizontal-tb and its height in vertical-rl.
    Fixture f;
    PrLayoutFontStyle vertical = prLayoutFontStyleDefault();
    vertical.writingMode = PrLayoutWritingMode_VerticalRl;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutBoxStyle sized = withWidth(block(), prLayoutLengthMaxContent());
    sized.height = prLayoutLengthMaxContent();
    PrLayoutNode box = f.element(root, sized);
    replaced(f, box, inlineBox(), natural(100, 50));
    layout(f);
    CHECK(widthOf(f, box) == 100);
    prLayoutNodeSetStyle(f.tree, root, f.style(block(), nullptr, nullptr, &vertical));
    prLayoutNodeSetStyle(f.tree, box, f.style(sized, nullptr, nullptr, &vertical));
    layout(f);
    CHECK(heightOf(f, box) == 50);
    prLayoutNodeSetStyle(f.tree, root, f.style(block()));
    prLayoutNodeSetStyle(f.tree, box, f.style(sized));
    layout(f);
    CHECK(widthOf(f, box) == 100);
}
