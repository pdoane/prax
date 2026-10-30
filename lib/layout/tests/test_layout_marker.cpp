#include "pr/layout_query.h"
#include "pr/layout_style.h"
#include "pr/layout_tree.h"

#include "layout/tests/layout_fixture.h"

#include "doctest/doctest.h"

namespace pr_impl {

namespace {

// Outside markers (PrLayoutDisplay_Marker). Each case is a list 400px wide whose content starts 100px in, of 20px Arimo
// on 40px lines, whose first line's baseline is 27px below its top. Each item opens with its marker, which holds its
// marker string, with its spaces preserved and no wrapping, as for white-space: pre.

PrLayoutFontStyle font(float size, float lineHeight) {
    PrLayoutFontStyle font = prLayoutFontStyleDefault();
    font.font = textFontOf(size);
    font.lineHeight = prLayoutLineHeightPx(lineHeight);
    font.whiteSpaceCollapse = PrLayoutWhiteSpaceCollapse_Preserve;
    font.textWrapMode = PrLayoutTextWrapMode_Nowrap;
    return font;
}

PrLayoutBoxStyle markerBox() {
    PrLayoutBoxStyle box = prLayoutBoxStyleDefault();
    box.display = PrLayoutDisplay_Marker;
    return box;
}

struct List {
    Fixture f;
    PrLayoutFontStyle style = font(20, 40);
    PrLayoutNode list = kPrLayoutNodeNone;

    explicit List(PrTextDirection direction = PrTextDirection_Ltr) {
        style.direction = direction;
        PrLayoutNode root = f.element(kPrLayoutNodeNone, block(), nullptr, nullptr, &style);
        PrLayoutBoxStyle box = block();
        box.width = prLayoutLengthPx(400);
        box.padding[direction == PrTextDirection_Ltr ? PrRectSide_Left : PrRectSide_Right] = prLayoutLengthPx(100);
        list = f.element(root, box, nullptr, nullptr, &style);
    }

    PrLayoutNode add(PrLayoutNode parent, const PrLayoutBoxStyle& box, const PrLayoutFontStyle* PR_NULLABLE withFont = nullptr) {
        return f.element(parent, box, nullptr, nullptr, withFont != nullptr ? withFont : &style);
    }

    // Adds a list item under `parent` that opens with a marker of `text`, set in `markerFont` if one is given.
    PrLayoutNode item(PrLayoutNode parent, const char* text, PrLayoutNode* PR_NULLABLE marker = nullptr,
            const PrLayoutFontStyle* PR_NULLABLE markerFont = nullptr) {
        PrLayoutNode node = add(parent, block());
        PrLayoutNode made = add(node, markerBox(), markerFont);
        f.text(made, text);
        if (marker != nullptr)
            *marker = made;
        return node;
    }

    void layout() {
        f.layout(px(600, 400));
        CHECK(verified(f.tree));
    }

    PrRect bounds(PrLayoutNode node) {
        return f.bounds(node);
    }
};

double heightOf(PrRect r) {
    return double(r.bottom - r.top);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("an outside marker ends at its item's border box and sits on the item's first line") {
    List l;
    PrLayoutNode marker = kPrLayoutNodeNone;
    PrLayoutNode item = l.item(l.list, "1. ", &marker);
    l.f.text(item, "X");
    l.layout();
    PrRect m = l.bounds(marker);
    PrRect i = l.bounds(item);
    CHECK(m.right == i.left);
    CHECK(i.left == 100.f);
    CHECK(m.top == i.top);
    CHECK(heightOf(i) == 40);
    // The marker takes no room on the line.
    PrLayoutNode after = l.item(l.list, "2. ");
    PrLayoutNode text = l.f.text(after, "Y");
    l.layout();
    CHECK(l.bounds(text).left == 100.f);
    CHECK(l.bounds(after).top == i.bottom);
}

TEST_CASE("an item with no in-flow content lays out a line for its marker") {
    List l;
    PrLayoutNode marker = kPrLayoutNodeNone;
    PrLayoutNode empty = l.item(l.list, "1. ", &marker);
    PrLayoutNode next = l.item(l.list, "2. ");
    l.f.text(next, "X");
    l.layout();
    CHECK(heightOf(l.bounds(empty)) == 40);
    CHECK(l.bounds(marker).top == l.bounds(empty).top);
    CHECK(l.bounds(next).top == l.bounds(empty).bottom);
}

TEST_CASE("a marker taller than its line makes the line taller, aligned on its baseline") {
    List l;
    PrLayoutFontStyle tall = font(20, 80);
    PrLayoutNode marker = kPrLayoutNodeNone;
    PrLayoutNode item = l.item(l.list, "1. ", &marker, &tall);
    PrLayoutNode text = l.f.text(item, "X");
    l.layout();
    // The marker reaches 47px above its baseline and 33px below, where the strut reaches 27px and 13px.
    CHECK(heightOf(l.bounds(item)) == 80);
    CHECK(l.bounds(marker).top == l.bounds(item).top);
    // The line's baseline moves 20px down, to 47px, and the text's content area with it, which starts 18px above.
    CHECK(l.bounds(text).top - l.bounds(item).top == 29.f);
}

TEST_CASE("an outside marker sits on the first line at any depth of the item's blocks in flow") {
    SUBCASE("in a block with padding") {
        List l;
        PrLayoutNode marker = kPrLayoutNodeNone;
        PrLayoutNode item = l.item(l.list, "1. ", &marker);
        PrLayoutBoxStyle padded = block();
        padded.padding[PrRectSide_Top] = prLayoutLengthPx(30);
        l.f.text(l.add(item, padded), "X");
        l.layout();
        CHECK(l.bounds(marker).top - l.bounds(item).top == 30.f);
        CHECK(heightOf(l.bounds(item)) == 70);
    }
    SUBCASE("past a block that has no line") {
        List l;
        PrLayoutNode marker = kPrLayoutNodeNone;
        PrLayoutNode item = l.item(l.list, "1. ", &marker);
        l.add(item, block(50));
        l.f.text(item, "X");
        l.layout();
        CHECK(l.bounds(marker).top - l.bounds(item).top == 50.f);
        CHECK(heightOf(l.bounds(item)) == 90);
    }
    SUBCASE("in a formatting context root") {
        List l;
        PrLayoutNode marker = kPrLayoutNodeNone;
        PrLayoutNode item = l.item(l.list, "1. ", &marker);
        PrLayoutBoxStyle root = block();
        root.display = PrLayoutDisplay_FlowRoot;
        root.padding[PrRectSide_Top] = prLayoutLengthPx(30);
        l.f.text(l.add(item, root), "X");
        l.layout();
        CHECK(l.bounds(marker).top - l.bounds(item).top == 30.f);
    }
    SUBCASE("on the line of a nested item, with that item's marker") {
        List l;
        PrLayoutNode outer = kPrLayoutNodeNone;
        PrLayoutNode inner = kPrLayoutNodeNone;
        PrLayoutNode item = l.item(l.list, "1. ", &outer);
        PrLayoutBoxStyle list = block();
        list.padding[PrRectSide_Left] = prLayoutLengthPx(100);
        PrLayoutNode nested = l.item(l.add(item, list), "a. ", &inner);
        l.f.text(nested, "X");
        l.f.text(item, "Y");
        l.layout();
        CHECK(l.bounds(outer).top == l.bounds(item).top);
        CHECK(l.bounds(inner).top == l.bounds(item).top);
        CHECK(heightOf(l.bounds(item)) == 80);
    }
}

TEST_CASE("an item whose content has no line puts its marker at its content box's top, and keeps its height") {
    List l;
    PrLayoutNode marker = kPrLayoutNodeNone;
    PrLayoutNode item = l.item(l.list, "1. ", &marker);
    l.add(item, block(10));
    l.layout();
    CHECK(heightOf(l.bounds(item)) == 10);
    CHECK(l.bounds(marker).top == l.bounds(item).top);
}

TEST_CASE("a float placed before the item moves its marker with the start of the line, and a float in it does not") {
    SUBCASE("before") {
        List l;
        PrLayoutBoxStyle floated = block(30);
        floated.width = prLayoutLengthPx(50);
        floated.floating = PrLayoutFloat_Left;
        l.add(l.list, floated);
        PrLayoutNode marker = kPrLayoutNodeNone;
        PrLayoutBoxStyle edges = block();
        edges.padding[PrRectSide_Left] = prLayoutLengthPx(20);
        PrLayoutNode item = l.add(l.list, edges);
        marker = l.add(item, markerBox());
        l.f.text(marker, "1. ");
        l.f.text(item, "X");
        l.layout();
        // The float reaches 150px, 30px past the start of the item's content box, which moves the marker 30px.
        CHECK(l.bounds(marker).right == 130.f);
    }
    SUBCASE("in the item") {
        List l;
        PrLayoutNode marker = kPrLayoutNodeNone;
        PrLayoutNode item = l.item(l.list, "1. ", &marker);
        PrLayoutBoxStyle floated = block(30);
        floated.width = prLayoutLengthPx(50);
        floated.floating = PrLayoutFloat_Left;
        l.add(item, floated);
        l.f.text(item, "X");
        l.layout();
        CHECK(l.bounds(marker).right == 100.f);
    }
}

TEST_CASE("an rtl item's marker starts at its border box's right edge") {
    List l(PrTextDirection_Rtl);
    PrLayoutNode marker = kPrLayoutNodeNone;
    PrLayoutNode item = l.item(l.list, "1. ", &marker);
    l.f.text(item, "X");
    l.layout();
    CHECK(l.bounds(marker).left == l.bounds(item).right);
    CHECK(l.bounds(item).right == 500.f);
}

TEST_CASE("a change to the marker lays out the item again, and its children only if the marker's extent changed") {
    List l;
    PrLayoutNode marker = kPrLayoutNodeNone;
    PrLayoutNode item = l.item(l.list, "1. ", &marker);
    PrLayoutNode child = l.add(item, block());
    l.f.text(child, "X");
    l.layout();
    float left = l.bounds(marker).left;
    PrLayoutNode text = prLayoutNodeFirstChild(l.f.tree, marker);
    prLayoutNodeSetText(l.f.tree, text, PR_SV("10. "));
    l.layout();
    // The item and its marker are laid out, and the child keeps its lines.
    CHECK(prLayoutTreeStats(l.f.tree).linesLaidOut == 1);
    CHECK(l.bounds(marker).right == 100.f);
    CHECK(l.bounds(marker).left < left - 10);
    // A taller marker lays the child's line out again.
    PrLayoutFontStyle tall = font(20, 80);
    prLayoutNodeSetStyle(l.f.tree, marker, l.f.style(markerBox(), nullptr, nullptr, &tall));
    l.layout();
    CHECK(heightOf(l.bounds(item)) == 80);
    // A marker that becomes inline is on the line, and one that becomes a marker again leaves it.
    PrLayoutBoxStyle inside = prLayoutBoxStyleDefault();
    prLayoutNodeSetStyle(l.f.tree, marker, l.f.style(inside, nullptr, nullptr, &l.style));
    l.layout();
    CHECK(l.bounds(marker).left == 100.f);
    prLayoutNodeSetStyle(l.f.tree, marker, l.f.style(markerBox(), nullptr, nullptr, &l.style));
    l.layout();
    CHECK(l.bounds(marker).right == 100.f);
}

TEST_CASE("an item that clips its overflow clips its marker, and a point on the marker hits it") {
    List l;
    PrLayoutNode marker = kPrLayoutNodeNone;
    PrLayoutNode item = l.item(l.list, "1. ", &marker);
    l.f.text(item, "X");
    l.layout();
    PrRect m = l.bounds(marker);
    PrLayoutNode hit = l.f.hit(PrVec2{(m.left + m.right) / 2, (m.top + m.bottom) / 2});
    CHECK((hit == marker || prLayoutNodeParent(l.f.tree, hit) == marker));
    PrLayoutBoxStyle clipped = block();
    clipped.overflowX = PrLayoutOverflow_Hidden;
    clipped.overflowY = PrLayoutOverflow_Hidden;
    prLayoutNodeSetStyle(l.f.tree, item, l.f.style(clipped, nullptr, nullptr, &l.style));
    l.layout();
    CHECK(l.f.hit(PrVec2{(m.left + m.right) / 2, (m.top + m.bottom) / 2}) != marker);
}
