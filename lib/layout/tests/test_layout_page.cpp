// Paged media: a tree laid out as pages. Each page is 400 by 500 px with margins of 50 px, so its page area is 300 by
// 400 px, unless a case says otherwise.

#include "pr/array.h"
#include "pr/draw_list.h"
#include "pr/draw_transform.h"
#include "pr/layout_page.h"
#include "pr/layout_style.h"
#include "pr/layout_tree.h"
#include "pr/span.h"
#include "pr/string.h"

#include "layout/tests/layout_fixture.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

// A paginated tree whose pages the case's function sizes, and which records the pages that layout asked for.
struct Book {
    Fixture f;
    PrLayoutNode root = kPrLayoutNodeNone;
    Array<PrLayoutPageContext> asked;
    PrLayoutName wide = kPrLayoutNameNone;    // a name whose pages are 600 px wide
    float bleed = 0;                          // px
    PrLayoutPageMarkFlags marks = PrLayoutPageMarkFlag_None;

    Book() {
        root = add(kPrLayoutNodeNone, block());
        wide = prLayoutNameIntern(f.tree, PR_SV("wide"));
        PrLayoutPagination pagination{.box = boxOf, .context = this};
        prLayoutTreeSetPagination(f.tree, &pagination);
    }

    static void boxOf(void* PR_NULLABLE context, const PrLayoutPageContext* page, PrLayoutPageBox* box) {
        Book* book = static_cast<Book*>(context);
        book->asked.push(*page);
        box->extent = px(page->name == book->wide ? 600 : 400, 500);
        for (PrLayoutUnit& margin : box->margin)
            margin = prLayoutUnitFromPx(50);
        box->background = prColor16FromColor(PrColor{0, 0, 1, 1});
        box->bleed = prLayoutUnitFromPx(book->bleed);
        box->marks = book->marks;
    }

    // Creates an element under `parent` with the groups given.
    PrLayoutNode add(PrLayoutNode parent, const PrLayoutBoxStyle& box,
            const PrLayoutFragmentationStyle* PR_NULLABLE fragmentation = nullptr) {
        PrLayoutStyle record = prLayoutStyleDefault();
        record.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(f.tree, &box);
        if (fragmentation != nullptr)
            record.groups[PrLayoutGroup_Table] = fragmentationGroup(f.tree, *fragmentation);
        PrLayoutNode node = prLayoutNodeCreateElement(f.tree, parent, prLayoutStyleIntern(f.tree, &record));
        if (parent == kPrLayoutNodeNone)
            f.root = node;
        return node;
    }

    // Creates a replaced block 100 px wide and `height` tall under the root, which is monolithic, with the
    // fragmentation values given and a red content.
    PrLayoutNode box(float height, const PrLayoutFragmentationStyle* PR_NULLABLE fragmentation = nullptr) {
        PrLayoutStyle record = prLayoutStyleDefault();
        PrLayoutBoxStyle style = block(height);
        style.width = prLayoutLengthPx(100);
        record.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(f.tree, &style);
        if (fragmentation != nullptr)
            record.groups[PrLayoutGroup_Table] = fragmentationGroup(f.tree, *fragmentation);
        PrLayoutNode node = prLayoutNodeCreateReplaced(f.tree, root, prLayoutStyleIntern(f.tree, &record));
        prLayoutNodeSetNaturalSize(f.tree, node, natural(100, height));
        PrLayoutContent red = solid(PrColor{1, 0, 0, 1}, 100, height);
        prLayoutNodeSetContent(f.tree, node, &red);
        prDrawListRelease(red.list);
        return node;
    }

    void layout() {
        asked.clear();
        f.layout(px(400, 600));
        CHECK(verified(f.tree));
    }

    uint32_t pages() { return prLayoutTreePageCount(f.tree); }

    // Returns the number of boxes of `node`, one for each page that it reaches.
    uint32_t pieces(PrLayoutNode node) {
        return prLayoutNodeBoxes(f.tree, node, PrLayoutCoordinates_Viewport, nullptr, 0);
    }

    // Returns the box of `node` on the `index`th page that it reaches.
    PrRect piece(PrLayoutNode node, uint32_t index) {
        Array<PrLayoutRect> boxes(pieces(node));
        REQUIRE(index < boxes.count());
        prLayoutNodeBoxes(f.tree, node, PrLayoutCoordinates_Viewport, boxes.data(), boxes.count());
        return prLayoutRectToPx(boxes[index]);
    }
    PrLayoutPage page(uint32_t index) { return prLayoutTreePage(f.tree, index); }

    // Returns the RGBA pixels of the page at `index`, painted alone onto a sheet of `width` by `height` px.
    Array<uint8_t> paint(uint32_t index, uint32_t width = 400, uint32_t height = 500) {
        PrDrawList* list = prDrawListCreate();
        prLayoutTreePaintPage(f.tree, index, list);
        Array<uint8_t> bytes(size_t(width) * height * 4);
        PrDrawRenderer* renderer = prDrawCpuCreate(nullptr);
        PrPixels into{bytes.data(), size_t(width) * 4, {width, height}};
        REQUIRE(prDrawRendererRenderPixels(renderer, list, into, PrPixelFormat_Rgba8) == PrStatus_Ok);
        prDrawRendererDestroy(renderer);
        prDrawListRelease(list);
        return bytes;
    }
};

// Adds to `count` the commands that a consumer walks of `list` played under `transform`, within `clip`, a rectangle in
// target pixels. Like every consumer, it skips an embedded list that misses the clip (prDrawListMisses).
void countDrawn(const PrDrawList* list, PrDrawTransform transform, PrRect clip, uint32_t& count) {
    Array<PrDrawTransform> transforms;
    transforms.push(transform);
    Array<PrRect> clips;
    clips.push(clip);
    const PrDrawCommand* commands = prDrawListCommands(list);
    for (uint32_t i = 0; i < prDrawListCommandCount(list); ++i) {
        const PrDrawCommand& c = commands[i];
        ++count;
        PrAffine2 affine = transforms.last().affine;
        switch (c.kind) {
            case PrDrawCommandKind_PushTransformAffine2:
                transforms.push(prDrawTransformPush(&transforms.last(), &c));
                break;
            case PrDrawCommandKind_PopTransform:
                transforms.pop();
                break;
            case PrDrawCommandKind_PushClipRect:
                clips.push(prRectIntersect(clips.last(), prAffine2MapRect(affine, c.rect)));
                break;
            case PrDrawCommandKind_PushClipCornerRect:
                clips.push(prRectIntersect(clips.last(), prAffine2MapRect(affine, c.cornerRect->rect)));
                break;
            case PrDrawCommandKind_PushClipPath:
                clips.push(prRectIntersect(clips.last(), prAffine2MapRect(affine, prPathTightBounds(c.path))));
                break;
            case PrDrawCommandKind_PushClipTargetRect:
                clips.push(prRectIntersect(clips.last(), c.rect));
                break;
            case PrDrawCommandKind_PopClip:
                clips.pop();
                break;
            case PrDrawCommandKind_Embed: {
                PrDrawTransform moved = prDrawTransformMoved(&transforms.last(), c.embed.offset);
                if (!prDrawListMisses(c.embed.list, moved.affine, clips.last()))
                    countDrawn(c.embed.list, moved, clips.last(), count);
                break;
            }
            default:
                break;
        }
    }
}

// Returns the number of commands that a consumer walks of a page's `list`, through the lists that it embeds.
uint32_t commandsDrawn(const PrDrawList* list) {
    uint32_t count = 0;
    PrRect everywhere{-1e30f, -1e30f, 1e30f, 1e30f};
    countDrawn(list, prDrawTransformFromAffine2(prAffine2Identity()), everywhere, count);
    return count;
}

// Returns fragmentation values whose break-before is `value`.
PrLayoutFragmentationStyle before(PrLayoutBreak value) {
    PrLayoutFragmentationStyle fragmentation = prLayoutFragmentationStyleDefault();
    fragmentation.breakBefore = value;
    return fragmentation;
}

// Adds under the root of `b` a block padded by 10 px that holds a title 20 px tall, which avoids a break after it, and
// a list of `items` items, each a block in a block that holds a replaced box 30 px tall. Returns the title.
PrLayoutNode titledList(Book& b, uint32_t items) {
    auto replacedIn = [&](PrLayoutNode parent, float height, const PrLayoutFragmentationStyle* fragmentation) {
        PrLayoutStyle record = prLayoutStyleDefault();
        PrLayoutBoxStyle content = block(height);
        content.width = prLayoutLengthPx(100);
        record.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(b.f.tree, &content);
        if (fragmentation != nullptr)
            record.groups[PrLayoutGroup_Table] = fragmentationGroup(b.f.tree, *fragmentation);
        PrLayoutNode node = prLayoutNodeCreateReplaced(b.f.tree, parent, prLayoutStyleIntern(b.f.tree, &record));
        prLayoutNodeSetNaturalSize(b.f.tree, node, natural(100, height));
        return node;
    };
    PrLayoutBoxStyle padded = block();
    const PrRectSide sides[] = {PrRectSide_Top, PrRectSide_Bottom};
    for (PrRectSide side : sides)
        padded.padding[side] = prLayoutLengthPx(10);
    PrLayoutNode sidebar = b.add(b.add(b.root, padded), block());
    PrLayoutFragmentationStyle avoid = prLayoutFragmentationStyleDefault();
    avoid.breakAfter = PrLayoutBreak_AvoidPage;
    PrLayoutNode title = replacedIn(sidebar, 20, &avoid);
    PrLayoutNode list = b.add(b.add(sidebar, block()), block());
    for (uint32_t i = 0; i < items; ++i)
        replacedIn(b.add(b.add(list, block()), block()), 30, nullptr);
    return title;
}

// Returns the style of a margin box: a page-margin box with auto sizes, no edges and the given background.
PrLayoutStyleIndex marginStyle(Book& b, const PrLayoutBoxStyle* PR_NULLABLE given = nullptr,
        PrColor color = PrColor{0, 0, 0, 0}) {
    PrLayoutBoxStyle box = given != nullptr ? *given : prLayoutBoxStyleDefault();
    box.display = PrLayoutDisplay_PageMargin;
    PrLayoutDecorationStyle decoration = background(color);
    return b.f.style(box, &decoration);
}

// Creates a margin box under the root of `b` that holds a replaced block of `width` by `height` px, which its
// min-content and max-content sizes are.
PrLayoutNode marginBox(Book& b, float width, float height, const PrLayoutBoxStyle* PR_NULLABLE given = nullptr,
        PrColor color = PrColor{0, 0, 0, 0}) {
    PrLayoutNode box = prLayoutNodeCreateElement(b.f.tree, b.root, marginStyle(b, given, color));
    PrLayoutBoxStyle content = block();
    replaced(b.f, box, content, natural(width, height));
    return box;
}

// The margin boxes of a page, by PrLayoutMarginBox.
struct MarginBoxes {
    PrLayoutNode nodes[PrLayoutMarginBox_Count];
};

// A margin box of a page: the node at `which`.
struct MarginBoxAt {
    PrLayoutMarginBox which;
    PrLayoutNode node;
};

// Sets the margin boxes of the first pages of `b`, one set of boxes for each page.
void setMargins(Book& b, Span<const MarginBoxes> pages) {
    Array<PrLayoutNode> all;
    for (const MarginBoxes& page : pages)
        all.append(Span(page.nodes));
    prLayoutTreeSetPageMargins(b.f.tree, all.data(), pages.count());
}
// Sets the margin boxes of the first page of `b`.
void setMargins(Book& b, const MarginBoxes& page) {
    setMargins(b, Span<const MarginBoxes>(&page, 1));
}

// Returns a set of margin boxes with each of `boxes` at its place and none elsewhere.
MarginBoxes only(Span<const MarginBoxAt> boxes) {
    MarginBoxes set;
    for (PrLayoutNode& node : set.nodes)
        node = kPrLayoutNodeNone;
    for (const MarginBoxAt& box : boxes)
        set.nodes[box.which] = box.node;
    return set;
}
// Returns a set of margin boxes with `box` at its place and none elsewhere.
MarginBoxes only(const MarginBoxAt& box) {
    return only(Span<const MarginBoxAt>(&box, 1));
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a paginated tree puts what fits in each page area on the page, and stacks the pages") {
    Book b;
    PrLayoutNode boxes[4];
    for (PrLayoutNode& box : boxes)
        box = b.box(150);
    b.layout();
    REQUIRE(b.pages() == 2);
    CHECK(b.page(0).context.first);
    CHECK(b.page(0).context.side == PrLayoutPageSide_Right);
    CHECK(b.page(1).context.side == PrLayoutPageSide_Left);
    CHECK(!b.page(1).context.first);
    CHECK(prLayoutUnitToPx(b.page(1).offset.y) == 500);
    checkRect(b.f.bounds(boxes[0]), 50, 50, 150, 200);
    checkRect(b.f.bounds(boxes[1]), 50, 200, 150, 350);
    checkRect(b.f.bounds(boxes[2]), 50, 550, 150, 700);
    checkRect(b.f.bounds(boxes[3]), 50, 700, 150, 850);
}

TEST_CASE("a forced page break starts a new page") {
    Book b;
    PrLayoutFragmentationStyle page = before(PrLayoutBreak_Page);
    b.box(100);
    PrLayoutNode broken = b.box(100, &page);
    b.layout();
    REQUIRE(b.pages() == 2);
    CHECK(b.f.bounds(broken).top == 550);
}

TEST_CASE("a break to the right of the spread inserts a blank left page") {
    Book b;
    PrLayoutFragmentationStyle right = before(PrLayoutBreak_Right);
    b.box(100);
    PrLayoutNode broken = b.box(100, &right);
    b.layout();
    REQUIRE(b.pages() == 3);
    CHECK(b.page(1).context.blank);
    CHECK(b.page(1).context.side == PrLayoutPageSide_Left);
    CHECK(b.page(2).context.side == PrLayoutPageSide_Right);
    CHECK(!b.page(2).context.blank);
    CHECK(b.f.bounds(broken).top == 1050);
}

TEST_CASE("a box of another page name goes on a page of that name, which sizes its box") {
    Book b;
    PrLayoutFragmentationStyle named = prLayoutFragmentationStyleDefault();
    named.page = b.wide;
    b.box(100);
    PrLayoutNode wide = b.box(100, &named);
    b.layout();
    REQUIRE(b.pages() == 2);
    CHECK(b.page(0).context.name == kPrLayoutNameNone);
    CHECK(b.page(1).context.name == b.wide);
    CHECK(prLayoutUnitToPx(b.page(1).box.extent.x) == 600);
    CHECK(b.f.bounds(wide).top == 550);
}

TEST_CASE("painting a page draws its background and its own content, from the top left of its box") {
    Book b;
    PrLayoutNode boxes[2] = {b.box(300), b.box(300)};
    (void)boxes;
    b.layout();
    REQUIRE(b.pages() == 2);
    Array<uint8_t> bytes = b.paint(1);
    CHECK(pixel(bytes, 400, 10, 10) == "0 0 255 255");
    CHECK(pixel(bytes, 400, 60, 60) == "255 0 0 255");
    CHECK(pixel(bytes, 400, 60, 360) == "0 0 255 255");
    CHECK(pixel(bytes, 400, 200, 60) == "0 0 255 255");
}

TEST_CASE("a page's sheet holds its bleed area, and the band of its marks outside it") {
    PrLayoutPageBox box = prLayoutPageBoxDefault();
    box.extent = px(400, 500);
    auto rect = [](PrLayoutRect r) {
        PrRect p = prLayoutRectToPx(r);
        return format("{} {} {} {}", int(p.left), int(p.top), int(p.right), int(p.bottom));
    };
    // A page with no bleed and no marks is its own sheet.
    PrLayoutPageSheet sheet = prLayoutPageBoxSheet(&box);
    CHECK(prLayoutVecToPx(sheet.extent) == PrVec2{400, 500});
    CHECK(rect(sheet.bleed) == "0 0 400 500");
    CHECK(rect(sheet.trim) == "0 0 400 500");
    // A bleed with no marks makes the sheet the bleed area.
    box.bleed = prLayoutUnitFromPx(10);
    sheet = prLayoutPageBoxSheet(&box);
    CHECK(prLayoutVecToPx(sheet.extent) == PrVec2{420, 520});
    CHECK(rect(sheet.bleed) == "0 0 420 520");
    CHECK(rect(sheet.trim) == "10 10 410 510");
    // Marks add a band outside the bleed area: a crop mark starts 6 pt from the page box, or at the bleed area's edge
    // if that is farther, runs 24 pt, and the band ends 6 pt past it.
    box.marks = PrLayoutPageMarkFlag_Crop;
    sheet = prLayoutPageBoxSheet(&box);
    CHECK(prLayoutVecToPx(sheet.extent) == PrVec2{500, 600});
    CHECK(rect(sheet.bleed) == "40 40 460 560");
    CHECK(rect(sheet.trim) == "50 50 450 550");
    box.bleed = prLayoutUnitFromPx(0);
    sheet = prLayoutPageBoxSheet(&box);
    CHECK(prLayoutVecToPx(sheet.extent) == PrVec2{496, 596});
    CHECK(rect(sheet.bleed) == "48 48 448 548");
    // A negative bleed puts the bleed area inside the page box, which is the sheet, and leaves it at least empty.
    box.marks = PrLayoutPageMarkFlag_None;
    box.bleed = prLayoutUnitFromPx(-10);
    sheet = prLayoutPageBoxSheet(&box);
    CHECK(prLayoutVecToPx(sheet.extent) == PrVec2{400, 500});
    CHECK(rect(sheet.bleed) == "10 10 390 490");
    box.bleed = prLayoutUnitFromPx(-1000);
    CHECK(rect(prLayoutPageBoxSheet(&box).bleed) == "200 200 200 300");
}

TEST_CASE("the viewport stacks the sheets of pages, and a page paints its background and content into its bleed area") {
    Book b;
    b.bleed = 10;
    b.marks = PrLayoutPageMarkFlag_All;
    PrLayoutPagination pagination{.box = Book::boxOf, .context = &b, .gap = prLayoutUnitFromPx(10)};
    prLayoutTreeSetPagination(b.f.tree, &pagination);
    // A block that overflows the page box on the left, 70 px past the page area's edge and 20 px past the page box's.
    PrLayoutStyle record = prLayoutStyleDefault();
    PrLayoutBoxStyle style = block(100);
    style.width = prLayoutLengthPx(100);
    style.margin[PrRectSide_Left] = prLayoutLengthPx(-70);
    record.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(b.f.tree, &style);
    PrLayoutNode wide = prLayoutNodeCreateReplaced(b.f.tree, b.root, prLayoutStyleIntern(b.f.tree, &record));
    prLayoutNodeSetNaturalSize(b.f.tree, wide, natural(100, 100));
    PrLayoutContent red = solid(PrColor{1, 0, 0, 1}, 100, 100);
    prLayoutNodeSetContent(b.f.tree, wide, &red);
    prDrawListRelease(red.list);
    b.box(350);
    b.f.layout(px(500, 600));
    REQUIRE(b.pages() == 2);
    // Each sheet is 500 by 600 px, with its page box 50 px in, and the sheets are the gap apart.
    PrLayoutPageBox first = b.page(0).box;
    PrLayoutPageSheet sheet = prLayoutPageBoxSheet(&first);
    CHECK(prLayoutVecToPx(sheet.extent) == PrVec2{500, 600});
    CHECK(prLayoutVecToPx(b.page(0).offset) == PrVec2{50, 50});
    CHECK(prLayoutVecToPx(b.page(1).offset) == PrVec2{50, 660});
    CHECK(b.f.bounds(wide).left == 30);
    Array<uint8_t> bytes = b.paint(0, 500, 600);
    // The block shows in the bleed area, 10 px past the page box, and is clipped there.
    CHECK(pixel(bytes, 500, 45, 120) == "255 0 0 255");
    CHECK(pixel(bytes, 500, 35, 120) == "0 0 0 0");
    // The background fills the bleed area.
    CHECK(pixel(bytes, 500, 45, 300) == "0 0 255 255");
    CHECK(pixel(bytes, 500, 455, 300) == "0 0 255 255");
    CHECK(pixel(bytes, 500, 300, 555) == "0 0 255 255");
    // A crop mark extends the page box's left edge from 10 to 42 px above it, and nothing else is drawn outside the
    // bleed area.
    auto alpha = [&](uint32_t x, uint32_t y) { return bytes[(y * 500 + x) * 4 + 3]; };
    CHECK(alpha(49, 25) + alpha(50, 25) > 0);
    CHECK(alpha(49, 5) + alpha(50, 5) == 0);
    CHECK(alpha(30, 25) == 0);
    // A registration mark is centered in the band of the marks, outside the middle of each edge.
    CHECK(alpha(249, 24) + alpha(250, 24) > 0);
}

TEST_CASE("a tree that is no longer paginated lays out continuously") {
    Book b;
    PrLayoutNode boxes[2] = {b.box(300), b.box(300)};
    b.layout();
    CHECK(b.pages() == 2);
    prLayoutTreeSetPagination(b.f.tree, nullptr);
    b.layout();
    CHECK(b.pages() == 0);
    CHECK(b.f.bounds(boxes[1]).top == 300);
}

TEST_CASE("a change of page name between siblings inside a box that fits on its page forces a break") {
    Book b;
    PrLayoutFragmentationStyle named = prLayoutFragmentationStyleDefault();
    named.page = b.wide;
    PrLayoutNode body = b.add(b.root, block());
    PrLayoutNode first = b.add(body, block(50), &named);
    PrLayoutNode second = b.add(body, block(50));
    b.layout();
    REQUIRE(b.pages() == 2);
    CHECK(b.page(0).context.name == b.wide);
    CHECK(b.page(1).context.name == kPrLayoutNameNone);
    CHECK(b.f.bounds(first).top == 50);
    CHECK(b.f.bounds(second).top == 550);
}

TEST_CASE("a box's start and end page values are those of its first and last children") {
    Book b;
    PrLayoutFragmentationStyle named = prLayoutFragmentationStyleDefault();
    named.page = b.wide;
    PrLayoutNode body = b.add(b.root, block());
    PrLayoutNode first = b.add(body, block(50), &named);
    // The wrapper's start page value is its first child's, which matches the box before it, so no break falls between
    // them. Its last child's used value is the wrapper's own, which differs from its first child's.
    PrLayoutNode wrapper = b.add(body, block());
    PrLayoutNode inner = b.add(wrapper, block(50), &named);
    PrLayoutNode last = b.add(wrapper, block(50));
    b.layout();
    REQUIRE(b.pages() == 2);
    CHECK(b.page(0).context.name == b.wide);
    CHECK(b.page(1).context.name == kPrLayoutNameNone);
    CHECK(b.f.bounds(first).top == 50);
    CHECK(b.f.bounds(inner).top == 100);
    CHECK(b.f.bounds(last).top == 550);
}

TEST_CASE("the first page of a root is known before a layout, and its area leaves out margins that fit") {
    Book b;
    PrLayoutFragmentationStyle named = prLayoutFragmentationStyleDefault();
    named.page = b.wide;
    PrLayoutNode body = b.add(b.root, block());
    b.add(body, block(50), &named);
    PrLayoutPageContext first = prLayoutNodeStartPage(b.f.tree, b.root);
    CHECK(first.name == b.wide);
    CHECK(first.first);
    CHECK(first.side == PrLayoutPageSide_Right);
    PrLayoutPageBox box = prLayoutPageBoxDefault();
    box.extent = px(400, 500);
    box.margin[PrRectSide_Left] = prLayoutUnitFromPx(50);
    box.margin[PrRectSide_Right] = prLayoutUnitFromPx(500);
    box.margin[PrRectSide_Top] = prLayoutUnitFromPx(100);
    PrLayoutVec area = prLayoutPageBoxArea(&box);
    CHECK(prLayoutUnitToPx(area.x) == 0);
    CHECK(prLayoutUnitToPx(area.y) == 400);
}

TEST_CASE("each page lays its content out at its own page area's width") {
    Book b;
    PrLayoutFragmentationStyle named = prLayoutFragmentationStyleDefault();
    named.page = b.wide;
    PrLayoutNode narrow = b.add(b.root, block(100));
    PrLayoutNode wide = b.add(b.root, block(100), &named);
    b.layout();
    REQUIRE(b.pages() == 2);
    // The pages' areas are 300 and 500 px wide.
    CHECK(b.f.bounds(narrow).right - b.f.bounds(narrow).left == 300);
    CHECK(b.f.bounds(wide).right - b.f.bounds(wide).left == 500);
}

TEST_CASE("a long document lays out in work proportional to its content, and each page paints only itself") {
    Book b;
    constexpr uint32_t kBoxes = 2000;
    for (uint32_t i = 0; i < kBoxes; ++i)
        b.box(20);
    b.layout();
    // Each page area holds 20 boxes.
    REQUIRE(b.pages() == kBoxes / 20);
    PrLayoutTreeStats stats = prLayoutTreeStats(b.f.tree);
    CHECK(stats.blocksLaidOut < 2 * kBoxes);
    uint32_t first = 0;
    const uint32_t pages[] = {0u, b.pages() / 2, b.pages() - 1};
    for (uint32_t page : pages) {
        PrDrawList* list = prDrawListCreate();
        prLayoutTreePaintPage(b.f.tree, page, list);
        uint32_t drawn = commandsDrawn(list);
        prDrawListRelease(list);
        if (page == 0)
            first = drawn;
        // A page's list draws its own boxes, and nothing of the rest of the document.
        CHECK(drawn < 400);
        CHECK(drawn <= first + 20);
    }
}

TEST_CASE("a paginated tree laid out again with no change does no work, and a later change inside a broken box shows") {
    Book b;
    PrLayoutNode section = b.add(b.root, block());
    PrLayoutNode last = kPrLayoutNodeNone;
    for (uint32_t i = 0; i < 30; ++i)
        last = b.add(section, block(50));
    b.layout();
    REQUIRE(b.pages() == 4);
    b.layout();
    PrLayoutTreeStats stats = prLayoutTreeStats(b.f.tree);
    CHECK(stats.blocksLaidOut == 0);
    CHECK(stats.linesLaidOut == 0);
    // The section broke across every page, and a change to its last child still reaches it.
    PrLayoutStyle record = prLayoutStyleDefault();
    PrLayoutBoxStyle taller = block(450);
    record.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(b.f.tree, &taller);
    prLayoutNodeSetStyle(b.f.tree, last, prLayoutStyleIntern(b.f.tree, &record));
    b.layout();
    CHECK(b.pages() == 5);
}

TEST_CASE("a table breaks between its rows, and its header repeats on every page") {
    Book b;
    PrLayoutBoxStyle tableBox = block();
    tableBox.display = PrLayoutDisplay_Table;
    tableBox.width = prLayoutLengthPx(300);
    PrLayoutNode table = b.add(b.root, tableBox);
    auto group = [&](PrLayoutDisplay display) {
        PrLayoutBoxStyle box = prLayoutBoxStyleDefault();
        box.display = display;
        return b.add(table, box);
    };
    // Each row holds one cell with a box 20 px tall.
    auto row = [&](PrLayoutNode parent) {
        PrLayoutBoxStyle rowBox = prLayoutBoxStyleDefault();
        rowBox.display = PrLayoutDisplay_TableRow;
        PrLayoutNode r = b.add(parent, rowBox);
        PrLayoutBoxStyle cellBox = prLayoutBoxStyleDefault();
        cellBox.display = PrLayoutDisplay_TableCell;
        PrLayoutNode cell = b.add(r, cellBox);
        PrLayoutStyle record = prLayoutStyleDefault();
        PrLayoutBoxStyle content = block(20);
        content.width = prLayoutLengthPx(100);
        record.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(b.f.tree, &content);
        PrLayoutNode box = prLayoutNodeCreateReplaced(b.f.tree, cell, prLayoutStyleIntern(b.f.tree, &record));
        prLayoutNodeSetNaturalSize(b.f.tree, box, natural(100, 20));
        return box;
    };
    PrLayoutNode head = group(PrLayoutDisplay_TableHeaderGroup);
    row(head);
    PrLayoutNode body = group(PrLayoutDisplay_TableRowGroup);
    Array<PrLayoutNode> rows;
    for (int i = 0; i < 40; ++i)
        rows.push(row(body));
    b.layout();
    // Each page area is 400 px tall: the header and 19 body rows.
    REQUIRE(b.pages() == 3);
    CHECK(b.f.bounds(rows[18]).top == 50 + 20 + 18 * 20);
    CHECK(b.f.bounds(rows[19]).top == 500 + 50 + 20);
    CHECK(b.f.bounds(rows[39]).top == 1000 + 50 + 20 + 20);
}

TEST_CASE("a fixed box repeats on every page, at the same place in each page area") {
    Book b;
    PrLayoutStyle record = prLayoutStyleDefault();
    PrLayoutBoxStyle style = block(20);
    style.width = prLayoutLengthPx(20);
    style.position = PrLayoutPosition_Fixed;
    style.inset[PrRectSide_Left] = prLayoutLengthPx(200);
    style.inset[PrRectSide_Top] = prLayoutLengthPx(10);
    record.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(b.f.tree, &style);
    PrLayoutNode fixed = prLayoutNodeCreateReplaced(b.f.tree, b.root, prLayoutStyleIntern(b.f.tree, &record));
    prLayoutNodeSetNaturalSize(b.f.tree, fixed, natural(20, 20));
    PrLayoutContent green = solid(PrColor{0, 1, 0, 1}, 20, 20);
    prLayoutNodeSetContent(b.f.tree, fixed, &green);
    prDrawListRelease(green.list);
    for (int i = 0; i < 3; ++i)
        b.box(300);
    b.layout();
    REQUIRE(b.pages() == 3);
    // Its placement is on the first page.
    checkRect(b.f.bounds(fixed), 250, 60, 270, 80);
    for (uint32_t index = 0; index < 3; ++index) {
        Array<uint8_t> bytes = b.paint(index);
        CHECK(pixel(bytes, 400, 260, 70) == "0 255 0 255");
        CHECK(pixel(bytes, 400, 260, 90) == "0 0 255 255");
    }
}

TEST_CASE("a fixed box that a later page holds repeats on the pages before it too") {
    Book b;
    for (int i = 0; i < 3; ++i)
        b.box(300);
    PrLayoutStyle record = prLayoutStyleDefault();
    PrLayoutBoxStyle style = block(20);
    style.width = prLayoutLengthPx(20);
    style.position = PrLayoutPosition_Fixed;
    style.inset[PrRectSide_Left] = prLayoutLengthPx(200);
    style.inset[PrRectSide_Top] = prLayoutLengthPx(10);
    record.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(b.f.tree, &style);
    PrLayoutNode fixed = prLayoutNodeCreateReplaced(b.f.tree, b.root, prLayoutStyleIntern(b.f.tree, &record));
    prLayoutNodeSetNaturalSize(b.f.tree, fixed, natural(20, 20));
    PrLayoutContent green = solid(PrColor{0, 1, 0, 1}, 20, 20);
    prLayoutNodeSetContent(b.f.tree, fixed, &green);
    prDrawListRelease(green.list);
    b.layout();
    REQUIRE(b.pages() == 3);
    for (uint32_t index = 0; index < 3; ++index) {
        Array<uint8_t> bytes = b.paint(index);
        CHECK(pixel(bytes, 400, 260, 70) == "0 255 0 255");
        CHECK(pixel(bytes, 400, 260, 90) == "0 0 255 255");
    }
}

TEST_CASE("a multi-column container breaks between rows of columns across pages, and balances its last row") {
    Book b;
    PrLayoutFragmentationStyle columns = prLayoutFragmentationStyleDefault();
    columns.columnCount = 2;
    PrLayoutNode multicol = b.add(b.root, block(), &columns);
    Array<PrLayoutNode> boxes;
    for (int i = 0; i < 10; ++i) {
        PrLayoutStyle record = prLayoutStyleDefault();
        PrLayoutBoxStyle style = block(100);
        style.width = prLayoutLengthPx(100);
        record.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(b.f.tree, &style);
        PrLayoutNode node = prLayoutNodeCreateReplaced(b.f.tree, multicol, prLayoutStyleIntern(b.f.tree, &record));
        prLayoutNodeSetNaturalSize(b.f.tree, node, natural(100, 100));
        boxes.push(node);
    }
    b.layout();
    // Each page area is 400 px tall, so the first page's columns hold four boxes each, and the second page's row
    // balances the two boxes left.
    REQUIRE(b.pages() == 2);
    float second = b.f.bounds(boxes[4]).left;
    CHECK(second > 150);
    CHECK(b.f.bounds(boxes[3]).top == 350);
    CHECK(b.f.bounds(boxes[4]).top == 50);
    CHECK(b.f.bounds(boxes[7]).top == 350);
    CHECK(b.f.bounds(boxes[8]).top == 550);
    CHECK(b.f.bounds(boxes[8]).left == 50);
    CHECK(b.f.bounds(boxes[9]).top == 550);
    CHECK(b.f.bounds(boxes[9]).left == second);
}

TEST_CASE("a multi-column container below other content on a page fills the room left on it") {
    Book b;
    PrLayoutNode above = b.box(100);
    PrLayoutFragmentationStyle columns = prLayoutFragmentationStyleDefault();
    columns.columnCount = 2;
    PrLayoutNode multicol = b.add(b.root, block(), &columns);
    Array<PrLayoutNode> boxes;
    for (int i = 0; i < 10; ++i) {
        PrLayoutStyle record = prLayoutStyleDefault();
        PrLayoutBoxStyle style = block(100);
        style.width = prLayoutLengthPx(100);
        record.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(b.f.tree, &style);
        PrLayoutNode node = prLayoutNodeCreateReplaced(b.f.tree, multicol, prLayoutStyleIntern(b.f.tree, &record));
        prLayoutNodeSetNaturalSize(b.f.tree, node, natural(100, 100));
        boxes.push(node);
    }
    b.layout();
    // The first page's columns hold the three boxes that fit below the box above, and the second page's row balances
    // the four boxes left.
    REQUIRE(b.pages() == 2);
    CHECK(b.f.bounds(above).top == 50);
    CHECK(b.f.bounds(multicol).top == 150);
    float second = b.f.bounds(boxes[3]).left;
    CHECK(second > 150);
    CHECK(b.f.bounds(boxes[0]).top == 150);
    CHECK(b.f.bounds(boxes[2]).top == 350);
    CHECK(b.f.bounds(boxes[3]).top == 150);
    CHECK(b.f.bounds(boxes[5]).top == 350);
    CHECK(b.f.bounds(boxes[6]).top == 550);
    CHECK(b.f.bounds(boxes[6]).left == 50);
    CHECK(b.f.bounds(boxes[7]).top == 650);
    CHECK(b.f.bounds(boxes[8]).top == 550);
    CHECK(b.f.bounds(boxes[8]).left == second);
    CHECK(b.f.bounds(boxes[9]).top == 650);
}

TEST_CASE("a table does not break between rows that a cell spans") {
    Book b;
    PrLayoutBoxStyle tableBox = block();
    tableBox.display = PrLayoutDisplay_Table;
    tableBox.width = prLayoutLengthPx(300);
    PrLayoutNode table = b.add(b.root, tableBox);
    PrLayoutBoxStyle groupBox = prLayoutBoxStyleDefault();
    groupBox.display = PrLayoutDisplay_TableRowGroup;
    PrLayoutNode body = b.add(table, groupBox);
    // Each row holds a cell with a box 110 px tall, and the third row's cell spans the fourth row too.
    Array<PrLayoutNode> rows;
    for (int i = 0; i < 6; ++i) {
        PrLayoutBoxStyle rowBox = prLayoutBoxStyleDefault();
        rowBox.display = PrLayoutDisplay_TableRow;
        PrLayoutNode r = b.add(body, rowBox);
        PrLayoutStyle cellRecord = prLayoutStyleDefault();
        PrLayoutBoxStyle cellBox = prLayoutBoxStyleDefault();
        cellBox.display = PrLayoutDisplay_TableCell;
        cellRecord.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(b.f.tree, &cellBox);
        PrLayoutTableStyle spans = prLayoutTableStyleDefault();
        spans.rowSpan = i == 2 ? 2 : 1;
        cellRecord.groups[PrLayoutGroup_Table] = prLayoutTableStyleIntern(b.f.tree, &spans);
        PrLayoutNode cell = prLayoutNodeCreateElement(b.f.tree, r, prLayoutStyleIntern(b.f.tree, &cellRecord));
        PrLayoutStyle record = prLayoutStyleDefault();
        PrLayoutBoxStyle content = block(110);
        content.width = prLayoutLengthPx(100);
        record.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(b.f.tree, &content);
        PrLayoutNode box = prLayoutNodeCreateReplaced(b.f.tree, cell, prLayoutStyleIntern(b.f.tree, &record));
        prLayoutNodeSetNaturalSize(b.f.tree, box, natural(100, 110));
        rows.push(box);
    }
    b.layout();
    // Three rows fit in a page area 400 px tall, but the break between the third and fourth rows falls inside the
    // spanning cell, so the third row starts the second page.
    REQUIRE(b.pages() == 3);
    CHECK(b.f.bounds(rows[1]).top < 500);
    CHECK(b.f.bounds(rows[2]).top > 500);
}

TEST_CASE("a long table with collapsed borders is sized once, and lays out and paints on each page its own rows") {
    Book b;
    auto element = [&](PrLayoutNode parent, const PrLayoutBoxStyle& box, const PrLayoutTableStyle* table,
            const PrLayoutDecorationStyle* decoration) {
        PrLayoutStyle record = prLayoutStyleDefault();
        record.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(b.f.tree, &box);
        if (table != nullptr)
            record.groups[PrLayoutGroup_Table] = prLayoutTableStyleIntern(b.f.tree, table);
        if (decoration != nullptr)
            record.groups[PrLayoutGroup_Decoration] = prLayoutDecorationStyleIntern(b.f.tree, decoration);
        return prLayoutNodeCreateElement(b.f.tree, parent, prLayoutStyleIntern(b.f.tree, &record));
    };
    PrLayoutBoxStyle tableBox = block();
    tableBox.display = PrLayoutDisplay_Table;
    tableBox.width = prLayoutLengthPx(300);
    PrLayoutTableStyle collapse = prLayoutTableStyleDefault();
    collapse.borderCollapse = PrLayoutBorderCollapse_Collapse;
    PrLayoutNode table = element(b.root, tableBox, &collapse, nullptr);
    PrLayoutBoxStyle groupBox = prLayoutBoxStyleDefault();
    groupBox.display = PrLayoutDisplay_TableRowGroup;
    PrLayoutBoxStyle rowBox = prLayoutBoxStyleDefault();
    rowBox.display = PrLayoutDisplay_TableRow;
    PrLayoutBoxStyle cellBox = prLayoutBoxStyleDefault();
    cellBox.display = PrLayoutDisplay_TableCell;
    cellBox.height = prLayoutLengthPx(18);
    PrLayoutDecorationStyle bordered = prLayoutDecorationStyleDefault();
    for (uint32_t side = 0; side < PrRectSide_Count; ++side) {
        cellBox.border[side] = prLayoutUnitFromPx(2);
        bordered.borderStyle[side] = PrDrawBorderStyle_Solid;
        bordered.borderColor[side] = prColor16FromColor(PrColor{0, 0, 0, 1});
    }
    PrLayoutBoxStyle headBox = prLayoutBoxStyleDefault();
    headBox.display = PrLayoutDisplay_TableHeaderGroup;
    PrLayoutNode head = element(table, headBox, nullptr, nullptr);
    PrLayoutNode headRow = element(head, rowBox, nullptr, nullptr);
    for (int c = 0; c < 3; ++c)
        element(headRow, cellBox, nullptr, &bordered);
    PrLayoutNode body = element(table, groupBox, nullptr, nullptr);
    constexpr uint32_t kRows = 1000;
    for (uint32_t i = 0; i < kRows; ++i) {
        PrLayoutNode row = element(body, rowBox, nullptr, nullptr);
        for (int c = 0; c < 3; ++c)
            element(row, cellBox, nullptr, &bordered);
    }
    b.layout();
    REQUIRE(b.pages() > 40);
    // The table is sized a bounded number of times, and each page places its header row and the body rows on it.
    PrLayoutTreeStats stats = prLayoutTreeStats(b.f.tree);
    CHECK(stats.tableRowsSized >= kRows + 1);
    CHECK(stats.tableRowsSized <= 2 * (kRows + 1));
    CHECK(stats.tableRowsPlaced <= 2 * (kRows + 1) + 2 * b.pages());
    uint32_t first = 0;
    const uint32_t pages[] = {0u, 1u, b.pages() / 2, b.pages() - 2};
    for (uint32_t page : pages) {
        PrDrawList* list = prDrawListCreate();
        prLayoutTreePaintPage(b.f.tree, page, list);
        uint32_t drawn = commandsDrawn(list);
        prDrawListRelease(list);
        if (page == 0)
            first = drawn;
        // A page draws the border segments of its own rows, about 7 for each of its 20 rows, and none of the rest.
        CHECK(drawn < 300);
        CHECK(drawn <= first + 20);
    }
}

TEST_CASE("a table row taller than a page breaks inside, and its cells continue on the next page") {
    Book b;
    auto element = [&](PrLayoutNode parent, PrLayoutDisplay display) {
        PrLayoutBoxStyle box = prLayoutBoxStyleDefault();
        box.display = display;
        if (display == PrLayoutDisplay_Table)
            box.width = prLayoutLengthPx(300);
        return b.add(parent, box);
    };
    auto replaced = [&](PrLayoutNode parent, float height) {
        PrLayoutStyle record = prLayoutStyleDefault();
        PrLayoutBoxStyle content = block(height);
        content.width = prLayoutLengthPx(100);
        record.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(b.f.tree, &content);
        PrLayoutNode box = prLayoutNodeCreateReplaced(b.f.tree, parent, prLayoutStyleIntern(b.f.tree, &record));
        prLayoutNodeSetNaturalSize(b.f.tree, box, natural(100, height));
        return box;
    };
    PrLayoutNode table = element(b.root, PrLayoutDisplay_Table);
    PrLayoutNode body = element(table, PrLayoutDisplay_TableRowGroup);
    PrLayoutNode tall = element(body, PrLayoutDisplay_TableRow);
    PrLayoutNode long_ = element(tall, PrLayoutDisplay_TableCell);
    PrLayoutNode boxes[3] = {replaced(long_, 150), replaced(long_, 150), replaced(long_, 150)};
    PrLayoutNode shortCell = element(tall, PrLayoutDisplay_TableCell);
    PrLayoutNode small = replaced(shortCell, 20);
    PrLayoutNode after = replaced(element(element(body, PrLayoutDisplay_TableRow), PrLayoutDisplay_TableCell), 20);
    b.layout();
    // The page area is 400 px tall: the long cell holds two boxes on the first page and the third on the second, where
    // the row ends and the next row follows it.
    REQUIRE(b.pages() == 2);
    CHECK(b.f.bounds(boxes[0]).top == 50);
    CHECK(b.f.bounds(boxes[1]).top == 200);
    CHECK(b.f.bounds(boxes[2]).top == 550);
    CHECK(b.f.bounds(small).top == 50);
    CHECK(b.f.bounds(after).top == 700);
}

TEST_CASE("a long flex or grid container lays out its items once, and places on each page the items there") {
    const PrLayoutDisplay displays[] = {PrLayoutDisplay_Flex, PrLayoutDisplay_Grid};
    for (PrLayoutDisplay display : displays) {
        CAPTURE(int(display));
        Book b;
        PrLayoutBoxStyle containerBox = block();
        containerBox.display = display;
        PrLayoutFlexStyle flex = prLayoutFlexStyleDefault();
        flex.direction = PrLayoutFlexDirection_Column;
        PrLayoutStyle containerRecord = prLayoutStyleDefault();
        containerRecord.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(b.f.tree, &containerBox);
        containerRecord.groups[PrLayoutGroup_Flex] = prLayoutFlexStyleIntern(b.f.tree, &flex);
        PrLayoutNode container =
            prLayoutNodeCreateElement(b.f.tree, b.root, prLayoutStyleIntern(b.f.tree, &containerRecord));
        constexpr uint32_t kItems = 1000;
        PrLayoutStyle record = prLayoutStyleDefault();
        PrLayoutBoxStyle content = block(20);
        content.width = prLayoutLengthPx(100);
        record.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(b.f.tree, &content);
        PrLayoutStyleIndex style = prLayoutStyleIntern(b.f.tree, &record);
        Array<PrLayoutNode> items;
        for (uint32_t i = 0; i < kItems; ++i) {
            PrLayoutNode item = prLayoutNodeCreateReplaced(b.f.tree, container, style);
            prLayoutNodeSetNaturalSize(b.f.tree, item, natural(100, 20));
            items.push(item);
        }
        b.layout();
        // Each page area holds 20 items.
        REQUIRE(b.pages() == kItems / 20);
        CHECK(b.f.bounds(items[20]).top == 500 + 50);
        CHECK(b.f.bounds(items[kItems - 1]).top == 500 * (kItems / 20 - 1) + 50 + 19 * 20);
        PrLayoutTreeStats stats = prLayoutTreeStats(b.f.tree);
        CHECK(stats.flexLaidOut + stats.gridsLaidOut <= 2);
        CHECK(stats.boxFragmentsMade < 3 * kItems);
    }
}

TEST_CASE("a page paints the floats and the boxes out of flow on it") {
    Book b;
    auto colored = [&](PrLayoutNode parent, const PrLayoutBoxStyle& style, PrColor color) {
        PrLayoutStyle record = prLayoutStyleDefault();
        record.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(b.f.tree, &style);
        PrLayoutNode node = prLayoutNodeCreateReplaced(b.f.tree, parent, prLayoutStyleIntern(b.f.tree, &record));
        prLayoutNodeSetNaturalSize(b.f.tree, node, natural(20, 20));
        PrLayoutContent content = solid(color, 20, 20);
        prLayoutNodeSetContent(b.f.tree, node, &content);
        prDrawListRelease(content.list);
        return node;
    };
    PrLayoutBoxStyle floating = block(20);
    floating.width = prLayoutLengthPx(20);
    floating.floating = PrLayoutFloat_Right;
    PrLayoutBoxStyle positioned = floating;
    positioned.floating = PrLayoutFloat_None;
    positioned.position = PrLayoutPosition_Absolute;
    positioned.inset[PrRectSide_Left] = prLayoutLengthPx(100);
    positioned.inset[PrRectSide_Top] = prLayoutLengthPx(0);
    PrLayoutBoxStyle relative = block();
    relative.position = PrLayoutPosition_Relative;
    // A float of the root and one in a block on the first page, and the same on the second, with a box out of flow. The
    // floats stack leftward from the right edge of the page area.
    colored(b.root, floating, PrColor{0, 1, 0, 1});
    colored(b.add(b.root, block()), floating, PrColor{0, 1, 0, 1});
    PrLayoutFragmentationStyle page = before(PrLayoutBreak_Page);
    PrLayoutNode holder = b.add(b.root, relative, &page);
    colored(holder, floating, PrColor{0, 1, 0, 1});
    colored(holder, positioned, PrColor{0, 1, 0, 1});
    colored(b.add(b.root, block()), floating, PrColor{0, 1, 0, 1});
    b.layout();
    REQUIRE(b.pages() == 2);
    for (uint32_t index = 0; index < 2; ++index) {
        CAPTURE(index);
        Array<uint8_t> bytes = b.paint(index);
        CHECK(pixel(bytes, 400, 340, 60) == "0 255 0 255");
        CHECK(pixel(bytes, 400, 320, 60) == "0 255 0 255");
        CHECK(pixel(bytes, 400, 300, 60) == "0 0 255 255");
    }
    CHECK(pixel(b.paint(1), 400, 160, 60) == "0 255 0 255");
}

TEST_CASE("a box that would place none of its content on a page goes to the next one whole") {
    // Each case leaves 20 px of the first page's area under a box 380 px tall, less than a line of 30 px, and lays out
    // three lines in a box 100 px wide.
    Book b;
    b.box(380);
    PrLayoutFontStyle font = prLayoutFontStyleDefault();
    font.font = textFontOf(16);
    font.lineHeight = prLayoutLineHeightPx(30);
    const char* text = "word word word word word word";
    PrLayoutBoxStyle narrow = block();
    narrow.width = prLayoutLengthPx(100);
    SUBCASE("a list item goes with its marker, which never ends a page alone") {
        PrLayoutNode item = b.f.element(b.root, block(), nullptr, nullptr, &font);
        PrLayoutBoxStyle markerBox = prLayoutBoxStyleDefault();
        markerBox.display = PrLayoutDisplay_Marker;
        PrLayoutNode marker = b.f.element(item, markerBox, nullptr, nullptr, &font);
        b.f.text(marker, "*");
        b.f.text(b.f.element(item, narrow, nullptr, nullptr, &font), text);
        b.layout();
        REQUIRE(b.pages() == 2);
        CHECK(b.f.bounds(item).top == 550);
        CHECK(b.f.bounds(marker).top >= 550);
    }
    SUBCASE("a list item whose first child is empty goes with its marker and its first line") {
        PrLayoutNode item = b.f.element(b.root, block(), nullptr, nullptr, &font);
        PrLayoutBoxStyle markerBox = prLayoutBoxStyleDefault();
        markerBox.display = PrLayoutDisplay_Marker;
        PrLayoutNode marker = b.f.element(item, markerBox, nullptr, nullptr, &font);
        b.f.text(marker, "*");
        b.f.element(item, block(), nullptr, nullptr, &font);
        b.f.text(b.f.element(item, narrow, nullptr, nullptr, &font), text);
        b.layout();
        REQUIRE(b.pages() == 2);
        CHECK(b.f.bounds(item).top == 550);
        CHECK(b.f.bounds(marker).top >= 550);
    }
    SUBCASE("a table row that fits on the next page is not split, and its first line does not straddle the break") {
        PrLayoutBoxStyle tableBox = prLayoutBoxStyleDefault();
        tableBox.display = PrLayoutDisplay_Table;
        PrLayoutBoxStyle rowBox = prLayoutBoxStyleDefault();
        rowBox.display = PrLayoutDisplay_TableRow;
        PrLayoutBoxStyle cellBox = narrow;
        cellBox.display = PrLayoutDisplay_TableCell;
        cellBox.padding[PrRectSide_Top] = prLayoutLengthPx(5);
        PrLayoutNode table = b.f.element(b.root, tableBox, nullptr, nullptr, &font);
        PrLayoutNode row = b.f.element(table, rowBox, nullptr, nullptr, &font);
        PrLayoutNode cell = b.f.element(row, cellBox, nullptr, nullptr, &font);
        b.f.text(cell, text);
        b.layout();
        REQUIRE(b.pages() == 2);
        CHECK(b.f.bounds(table).top == 550);
        CHECK(b.f.bounds(cell).bottom - b.f.bounds(cell).top > 90);
    }
    SUBCASE("a flex container whose first line fits on the next page is not split") {
        PrLayoutBoxStyle flexBox = block();
        flexBox.display = PrLayoutDisplay_Flex;
        PrLayoutNode flex = b.f.element(b.root, flexBox, nullptr, nullptr, &font);
        b.f.text(b.f.element(flex, narrow, nullptr, nullptr, &font), text);
        b.layout();
        REQUIRE(b.pages() == 2);
        CHECK(b.f.bounds(flex).top == 550);
    }
}

TEST_CASE("a table whose header and first body row do not fit goes to the next page whole") {
    Book b;
    b.box(300);
    auto element = [&](PrLayoutNode parent, PrLayoutDisplay display) {
        PrLayoutBoxStyle box = prLayoutBoxStyleDefault();
        box.display = display;
        return b.add(parent, box);
    };
    auto row = [&](PrLayoutNode group, float height) {
        PrLayoutNode cell = element(element(group, PrLayoutDisplay_TableRow), PrLayoutDisplay_TableCell);
        PrLayoutStyle record = prLayoutStyleDefault();
        PrLayoutBoxStyle content = block(height);
        content.width = prLayoutLengthPx(100);
        record.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(b.f.tree, &content);
        PrLayoutNode box = prLayoutNodeCreateReplaced(b.f.tree, cell, prLayoutStyleIntern(b.f.tree, &record));
        prLayoutNodeSetNaturalSize(b.f.tree, box, natural(100, height));
    };
    PrLayoutNode table = element(b.root, PrLayoutDisplay_Table);
    row(element(table, PrLayoutDisplay_TableHeaderGroup), 40);
    PrLayoutNode body = element(table, PrLayoutDisplay_TableRowGroup);
    row(body, 80);
    row(body, 80);
    b.layout();
    REQUIRE(b.pages() == 2);
    CHECK(b.f.bounds(table).top == 550);
}

TEST_CASE("a min-height that the content does not reach applies to the fragments of a box together") {
    Book b;
    b.box(300);
    PrLayoutBoxStyle tall = block();
    tall.minHeight = prLayoutLengthPx(300);
    PrLayoutNode box = b.add(b.root, tall);
    PrLayoutNode after = b.box(20);
    b.layout();
    REQUIRE(b.pages() == 2);
    // The box fills the 100 px left on the first page, and takes the other 200 px on the second.
    REQUIRE(b.pieces(box) == 2);
    checkRect(b.piece(box, 0), 50, 350, 350, 450);
    checkRect(b.piece(box, 1), 50, 550, 350, 750);
    CHECK(b.f.bounds(after).top == 750);
}

TEST_CASE("an unforced break that break-after avoids moves back to an earlier break") {
    Book b;
    PrLayoutFragmentationStyle avoid = prLayoutFragmentationStyleDefault();
    avoid.breakAfter = PrLayoutBreak_AvoidPage;
    SUBCASE("a heading goes to the next page with the content after it") {
        b.box(250);
        b.box(50);
        PrLayoutNode heading = b.box(50, &avoid);
        PrLayoutNode after = b.box(100);
        b.layout();
        REQUIRE(b.pages() == 2);
        CHECK(b.f.bounds(heading).top == 550);
        CHECK(b.f.bounds(after).top == 600);
    }
    SUBCASE("a run of boxes that each avoid the break after them moves together") {
        b.box(250);
        PrLayoutNode first = b.box(50, &avoid);
        b.box(50, &avoid);
        b.box(100);
        b.layout();
        REQUIRE(b.pages() == 2);
        CHECK(b.f.bounds(first).top == 550);
    }
    SUBCASE("the break stays if no earlier break is possible") {
        PrLayoutNode first = b.box(350, &avoid);
        PrLayoutNode after = b.box(100);
        b.layout();
        REQUIRE(b.pages() == 2);
        CHECK(b.f.bounds(first).top == 50);
        CHECK(b.f.bounds(after).top == 550);
    }
}

TEST_CASE("a box whose content fits on a page but whose block-end padding does not breaks after its content") {
    Book b;
    b.box(300);
    PrLayoutBoxStyle padded = block();
    padded.padding[PrRectSide_Bottom] = prLayoutLengthPx(30);
    PrLayoutNode box = b.add(b.root, padded);
    PrLayoutStyle record = prLayoutStyleDefault();
    PrLayoutBoxStyle content = block(80);
    content.width = prLayoutLengthPx(100);
    record.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(b.f.tree, &content);
    PrLayoutNode inner = prLayoutNodeCreateReplaced(b.f.tree, box, prLayoutStyleIntern(b.f.tree, &record));
    prLayoutNodeSetNaturalSize(b.f.tree, inner, natural(100, 80));
    b.layout();
    REQUIRE(b.pages() == 2);
    CHECK(b.f.bounds(inner).top == 350);
    // The padding continues at the top of the second page.
    REQUIRE(b.pieces(box) == 2);
    checkRect(b.piece(box, 0), 50, 350, 350, 450);
    checkRect(b.piece(box, 1), 50, 550, 350, 580);
}

TEST_CASE("a titled list that starts mid-page breaks between its items, and leaves no page short") {
    SUBCASE("the list starts on the page that its title is on") {
        Book b;
        b.box(200);
        PrLayoutNode title = titledList(b, 20);
        b.layout();
        // 200 px, then 10 px of padding, the 20 px title and 600 px of items: 6 items fit on the first page, 13 on the
        // second and the last on the third.
        REQUIRE(b.pages() == 3);
        CHECK(b.f.bounds(title).top == 260);
    }
    SUBCASE("a long document of titled lists takes no more pages than it fills") {
        Book b;
        constexpr uint32_t kLists = 40;
        for (uint32_t i = 0; i < kLists; ++i)
            titledList(b, 10);
        b.layout();
        // Each list is 340 px tall, and a page area holds 400 px. A page loses at most an item and a title at its end,
        // so each page holds at least 350 px.
        uint32_t ideal = (kLists * 340 + 399) / 400;
        CHECK(b.pages() >= ideal);
        CHECK(b.pages() <= (kLists * 340 + 349) / 350);
    }
}

TEST_CASE("the left and right boxes of a side share it in proportion to their content") {
    Book b;
    b.box(100);
    PrLayoutNode left = marginBox(b, 100, 10);
    PrLayoutNode right = marginBox(b, 50, 10);
    const MarginBoxAt placed[] = {{PrLayoutMarginBox_TopLeft, left}, {PrLayoutMarginBox_TopRight, right}};
    setMargins(b, only(placed));
    b.layout();
    // The side is 300 px between the corners. The boxes' content asks for 150 px, and the 150 px left go to the boxes
    // in proportion to their max-content sizes. Each fills the top margin.
    checkRect(b.f.bounds(left), 50, 0, 250, 50);
    checkRect(b.f.bounds(right), 250, 0, 350, 50);
}

TEST_CASE("the middle box of a side is centered, and the boxes beside it take half of what it leaves") {
    Book b;
    b.box(100);
    PrLayoutNode left = marginBox(b, 100, 10);
    PrLayoutNode center = marginBox(b, 60, 10);
    PrLayoutNode right = marginBox(b, 20, 10);
    const MarginBoxAt placed[] = {
        {PrLayoutMarginBox_BottomLeft, left},
        {PrLayoutMarginBox_BottomCenter, center},
        {PrLayoutMarginBox_BottomRight, right},
    };
    setMargins(b, only(placed));
    b.layout();
    // The middle box shares 300 px with a box twice the wider of the others, 200 px: 40 px are left, of which it takes
    // 60/260.
    double middle = 60 + 40.0 * 60 / 260;
    double side = (300 - middle) / 2;
    PrRect bounds = b.f.bounds(center);
    CHECK(double(bounds.right - bounds.left) == doctest::Approx(middle).epsilon(0.001));
    CHECK(double(bounds.left + bounds.right) / 2 == doctest::Approx(200).epsilon(0.001));
    CHECK(bounds.top == 450);
    CHECK(bounds.bottom == 500);
    PrRect l = b.f.bounds(left);
    CHECK(l.left == 50);
    CHECK(double(l.right) == doctest::Approx(50 + side).epsilon(0.001));
    PrRect r = b.f.bounds(right);
    CHECK(r.right == 350);
    CHECK(double(r.left) == doctest::Approx(350 - side).epsilon(0.001));
}

TEST_CASE("a box with a width of its own keeps it, and the auto box beside it takes the rest of the side") {
    Book b;
    b.box(100);
    PrLayoutBoxStyle fixed = prLayoutBoxStyleDefault();
    fixed.width = prLayoutLengthPx(40);
    fixed.margin[PrRectSide_Left] = prLayoutLengthPx(10);
    PrLayoutNode left = marginBox(b, 10, 10);
    PrLayoutNode right = marginBox(b, 10, 10, &fixed);
    const MarginBoxAt placed[] = {{PrLayoutMarginBox_TopLeft, left}, {PrLayoutMarginBox_TopRight, right}};
    setMargins(b, only(placed));
    b.layout();
    checkRect(b.f.bounds(right), 310, 0, 350, 50);
    checkRect(b.f.bounds(left), 50, 0, 300, 50);
}

TEST_CASE("a max width caps a box of a side, and the others share the rest") {
    Book b;
    b.box(100);
    PrLayoutBoxStyle capped = prLayoutBoxStyleDefault();
    capped.maxWidth = prLayoutLengthPx(60);
    PrLayoutNode left = marginBox(b, 100, 10, &capped);
    PrLayoutNode right = marginBox(b, 100, 10);
    const MarginBoxAt placed[] = {{PrLayoutMarginBox_TopLeft, left}, {PrLayoutMarginBox_TopRight, right}};
    setMargins(b, only(placed));
    b.layout();
    checkRect(b.f.bounds(left), 50, 0, 110, 50);
    checkRect(b.f.bounds(right), 110, 0, 350, 50);
}

TEST_CASE("a corner box fills its corner, and auto margins center a box of a fixed size in it") {
    Book b;
    b.box(100);
    PrLayoutNode corner = marginBox(b, 5, 5);
    PrLayoutBoxStyle small = prLayoutBoxStyleDefault();
    small.width = prLayoutLengthPx(20);
    small.height = prLayoutLengthPx(10);
    const PrRectSide sides[] = {PrRectSide_Left, PrRectSide_Right, PrRectSide_Top, PrRectSide_Bottom};
    for (PrRectSide side : sides)
        small.margin[side] = prLayoutLengthAuto();
    PrLayoutNode centered = marginBox(b, 5, 5, &small);
    PrLayoutBoxStyle sized = prLayoutBoxStyleDefault();
    sized.width = prLayoutLengthPx(20);
    sized.height = prLayoutLengthPx(10);
    PrLayoutNode pinned = marginBox(b, 5, 5, &sized);
    const MarginBoxAt placed[] = {
        {PrLayoutMarginBox_TopLeftCorner, corner},
        {PrLayoutMarginBox_BottomRightCorner, centered},
        {PrLayoutMarginBox_TopRightCorner, pinned},
    };
    setMargins(b, only(placed));
    b.layout();
    checkRect(b.f.bounds(corner), 0, 0, 50, 50);
    checkRect(b.f.bounds(centered), 365, 470, 385, 480);
    // With every value given, the margin at the page's edge gives way, so the box keeps to the page area's corner.
    checkRect(b.f.bounds(pinned), 350, 40, 370, 50);
}

TEST_CASE("the boxes of the left and right sides share the side's height by their content's height") {
    Book b;
    b.box(100);
    PrLayoutNode top = marginBox(b, 10, 100);
    PrLayoutNode middle = marginBox(b, 10, 30);
    const MarginBoxAt placed[] = {{PrLayoutMarginBox_LeftTop, top}, {PrLayoutMarginBox_LeftMiddle, middle}};
    setMargins(b, only(placed));
    b.layout();
    // The side is 400 px. The middle box shares it with a box of 200 px.
    double height = 30 + 170.0 * 30 / 230;
    PrRect m = b.f.bounds(middle);
    CHECK(m.left == 0);
    CHECK(m.right == 50);
    CHECK(double(m.bottom - m.top) == doctest::Approx(height).epsilon(0.001));
    CHECK(double(m.top + m.bottom) / 2 == doctest::Approx(250).epsilon(0.001));
    PrRect t = b.f.bounds(top);
    CHECK(t.top == 50);
    CHECK(double(t.bottom) == doctest::Approx(50 + (400 - height) / 2).epsilon(0.001));
}

TEST_CASE("each page lays out its own margin boxes, paints them over its content, and a change to one lays out it alone") {
    Book b;
    for (uint32_t i = 0; i < 4; ++i)
        b.box(150);
    PrColor green{0, 1, 0, 1};
    PrLayoutNode first = marginBox(b, 100, 10, nullptr, green);
    PrLayoutNode second = marginBox(b, 100, 10, nullptr, green);
    const MarginBoxes perPage[] = {
        only({PrLayoutMarginBox_TopCenter, first}),
        only({PrLayoutMarginBox_BottomCenter, second}),
    };
    setMargins(b, perPage);
    b.layout();
    REQUIRE(b.pages() == 2);
    CHECK(prLayoutNodePage(b.f.tree, first) == 0);
    CHECK(prLayoutNodePage(b.f.tree, second) == 1);
    checkRect(b.f.bounds(first), 50, 0, 350, 50);
    checkRect(b.f.bounds(second), 50, 950, 350, 1000);
    Array<uint8_t> page = b.paint(0);
    CHECK(pixel(page, 400, 200, 25) == "0 255 0 255");
    CHECK(pixel(page, 400, 200, 475) == "0 0 255 255");
    page = b.paint(1);
    CHECK(pixel(page, 400, 200, 25) == "0 0 255 255");
    CHECK(pixel(page, 400, 200, 475) == "0 255 0 255");
    // A change to a margin box keeps the pages' content.
    PrLayoutNode content = prLayoutNodeFirstChild(b.f.tree, second);
    prLayoutNodeSetNaturalSize(b.f.tree, content, natural(200, 10));
    b.layout();
    PrLayoutTreeStats stats = prLayoutTreeStats(b.f.tree);
    CHECK(stats.blocksLaidOut <= 2);
    // Margin boxes given to the pages after the content is laid out lay out without laying the content out again.
    PrLayoutNode third = marginBox(b, 50, 10);
    const MarginBoxAt secondPage[] = {{PrLayoutMarginBox_BottomCenter, second}, {PrLayoutMarginBox_TopLeft, third}};
    const MarginBoxes withThird[] = {only({PrLayoutMarginBox_TopCenter, first}), only(secondPage)};
    setMargins(b, withThird);
    b.layout();
    stats = prLayoutTreeStats(b.f.tree);
    CHECK(stats.blocksLaidOut <= 2);
    checkRect(b.f.bounds(third), 50, 500, 350, 550);
    b.layout();
    CHECK(prLayoutTreeStats(b.f.tree).blocksLaidOut == 0);
}

TEST_CASE("a page records its first content, and a running box is on the page of its placeholder") {
    Book b;
    PrLayoutNode boxes[4];
    for (PrLayoutNode& box : boxes)
        box = b.box(150);
    PrLayoutBoxStyle running = block(50);
    running.position = PrLayoutPosition_Running;
    PrLayoutNode header = b.add(b.root, running);
    PrLayoutNode after = b.box(150);
    b.layout();
    REQUIRE(b.pages() == 3);
    CHECK(b.page(0).first == boxes[0]);
    CHECK(b.page(1).first == boxes[2]);
    CHECK(b.page(2).first == after);
    // The running box takes no room and has no box of its own. Its placeholder ends the second page, which the box
    // after it does not fit.
    CHECK(prLayoutNodePage(b.f.tree, header) == 1);
    CHECK(b.f.bounds(after).top == 1050);
    CHECK(prLayoutNodePage(b.f.tree, boxes[3]) == 1);
}

TEST_CASE("a margin box places its content by its vertical-align, as a table cell does") {
    Book b;
    b.box(100);
    PrLayoutBoxStyle middle = prLayoutBoxStyleDefault();
    middle.alignmentBaseline = PrLayoutAlignmentBaseline_Middle;
    PrLayoutBoxStyle bottom = prLayoutBoxStyleDefault();
    bottom.baselineShift = prLayoutBaselineShiftKeyword(PrLayoutKeyword_Bottom);
    PrLayoutNode centered = marginBox(b, 20, 10, &middle);
    PrLayoutNode low = marginBox(b, 20, 10, &bottom);
    PrLayoutNode top = marginBox(b, 20, 10);
    const MarginBoxAt placed[] = {
        {PrLayoutMarginBox_TopLeft, centered},
        {PrLayoutMarginBox_TopCenter, low},
        {PrLayoutMarginBox_TopRight, top},
    };
    setMargins(b, only(placed));
    b.layout();
    CHECK(b.f.bounds(prLayoutNodeFirstChild(b.f.tree, centered)).top == 20);
    CHECK(b.f.bounds(prLayoutNodeFirstChild(b.f.tree, low)).top == 40);
    CHECK(b.f.bounds(prLayoutNodeFirstChild(b.f.tree, top)).top == 0);
}

TEST_CASE("margin boxes given after the content leave the placements of text broken across pages as a fresh layout's") {
    Book b;
    PrLayoutNode section = b.add(b.root, block());
    Array<char> words;
    for (uint32_t i = 0; i < 400; ++i)
        words.append("word ", 5);
    words.push('\0');
    b.f.text(b.add(section, block()), words.data());
    b.layout();
    REQUIRE(b.pages() > 1);
    PrLayoutNode first = marginBox(b, 50, 10);
    PrLayoutNode second = marginBox(b, 50, 10);
    const MarginBoxes perPage[] = {
        only({PrLayoutMarginBox_TopCenter, first}),
        only({PrLayoutMarginBox_TopCenter, second}),
    };
    setMargins(b, perPage);
    b.layout();
    // The margin boxes and the boxes in them are all that is laid out.
    CHECK(prLayoutTreeStats(b.f.tree).blocksLaidOut <= 4);
}

TEST_CASE("the viewport stacks the pages the pagination's gap apart, centered across it, and scrolls through them") {
    Book b;
    PrLayoutPagination pagination{.box = Book::boxOf, .context = &b, .gap = prLayoutUnitFromPx(10)};
    prLayoutTreeSetPagination(b.f.tree, &pagination);
    PrLayoutNode boxes[2] = {b.box(300), b.box(300)};
    b.f.layout(px(800, 600));
    REQUIRE(b.pages() == 2);
    CHECK(prLayoutUnitToPx(b.page(0).offset.x) == 200);
    CHECK(prLayoutUnitToPx(b.page(1).offset.y) == 510);
    CHECK(b.f.bounds(boxes[0]).left == 250);
    CHECK(b.f.bounds(boxes[1]).top == 560);
    PrLayoutScrollInfo info{};
    REQUIRE(prLayoutNodeScrollInfo(b.f.tree, kPrLayoutNodeNone, &info));
    CHECK(prLayoutUnitToPx(info.portExtent.y) == 600);
    CHECK(prLayoutUnitToPx(info.range.bottom) == 410);
    CHECK(prLayoutUnitToPx(info.range.right) == 0);
    // A viewport narrower than the pages scrolls across them too.
    b.f.layout(px(300, 600));
    REQUIRE(prLayoutNodeScrollInfo(b.f.tree, kPrLayoutNodeNone, &info));
    CHECK(prLayoutUnitToPx(b.page(0).offset.x) == 0);
    CHECK(prLayoutUnitToPx(info.range.right) == 100);
}
