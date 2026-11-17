// lib/ui's elements: their style, its inheritance and memo, their states, transitions and grids, and deferred calls.

#include "pr/layout_query.h"
#include "pr/ui_element.h"

#include "ui/tests/ui_fixture.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

// A fixture whose builder sets text in no font, since the tree has none.
struct ElementFixture : UiFixture {
    UiBuilder builder{{.ui = ui, .textColor = PrColor{0, 0, 1, 1}}};

    PrLayoutStyle record(PrLayoutNode node) { return prLayoutStyleRecord(tree, prLayoutNodeStyle(tree, node)); }
    PrLayoutBoxStyle box(PrLayoutNode node) {
        return prLayoutBoxStyleRecord(tree, record(node).groups[PrLayoutGroup_Box]);
    }
    PrLayoutDecorationStyle decoration(PrLayoutNode node) {
        return prLayoutDecorationStyleRecord(tree, record(node).groups[PrLayoutGroup_Decoration]);
    }
    PrLayoutColorStyle color(PrLayoutNode node) {
        return prLayoutColorStyleRecord(tree, record(node).groups[PrLayoutGroup_Color]);
    }
    PrRect bounds(PrLayoutNode node) {
        return prLayoutRectToPx(prLayoutNodeBounds(tree, node, PrLayoutCoordinates_Viewport));
    }
};

float width(PrRect rect) {
    return rect.right - rect.left;
}

bool sameColor(PrColor16 a, PrColor b) {
    PrColor wide = prColorFromColor16(a);
    return wide.r == b.r && wide.g == b.g && wide.b == b.b && wide.a == b.a;
}

// How often each memo of the tests below was built.
struct Builds {
    uint32_t rows = 0;
    uint32_t panels = 0;
    uint32_t meters = 0;
};

// A row that shows a label and is wider while it is hovered, and reports a click.
struct TestRow {
    Builds* builds;
    uint32_t value;
    bool operator==(const TestRow&) const = default;

    struct Clicked {
        bool clicked;
    };

    Clicked build(UiBuilder& ui, UiElement root) const {
        ++builds->rows;
        root.h(20).w(root.hovered() ? 200 : 100);
        ui.div(root).w(float(value)).h(10);
        return Clicked{.clicked = root.clicked()};
    }
};

// A row that reports nothing, and is wider while it is hovered.
struct TestCell {
    Builds* builds;
    uint32_t value;
    bool operator==(const TestCell&) const = default;

    void build(UiBuilder& ui, UiElement root) const {
        ++builds->rows;
        root.h(20).w(root.hovered() ? 200 : 100);
        ui.div(root).w(float(value)).h(10);
    }
};

// A panel of rows, whose last row shows `last`. Its rows report nothing, so it depends on none of them.
struct TestPanel {
    Builds* builds;
    uint32_t last;
    bool operator==(const TestPanel&) const = default;

    void build(UiBuilder& ui, UiElement root) const {
        ++builds->panels;
        root.flexCol().w(400);
        for (uint32_t i = 0; i < 3; ++i)
            ui.memo(root, TestCell{.builds = builds, .value = i == 2 ? last : i});
    }
};

// A box that fills its parent's width, and holds as many cells as fit across it at 100 px each.
struct TestMeter {
    Builds* builds;
    bool operator==(const TestMeter&) const = default;

    void build(UiBuilder& ui, UiElement root) const {
        ++builds->meters;
        root.wFull().h(10).flexRow();
        PrVec2 extent{};
        prUiExtent(ui.ui(), root, PrUiAxisFlag_X, &extent);
        for (uint32_t i = 0; i < uint32_t(extent.x / 100); ++i)
            ui.div(root).w(100).h(10);
    }
};

uint32_t childCount(PrLayoutTree* tree, PrLayoutNode node) {
    uint32_t count = 0;
    for (PrLayoutNode at = prLayoutNodeFirstChild(tree, node); at != kPrLayoutNodeNone;
            at = prLayoutNodeNextSibling(tree, at))
        ++count;
    return count;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("an element's chained calls style its node once the build creates the next element or returns") {
    ElementFixture f;
    PrLayoutNode row = kPrLayoutNodeNone;
    PrLayoutNode first = kPrLayoutNodeNone;
    PrLayoutNode last = kPrLayoutNodeNone;
    f.build = [&] {
        row = f.builder.div(f.root).flexRow().gap(10).p(5).bg(prColorFromHex(0xff0000));
        first = f.builder.div(row).w(100).h(20);
        // The last element of the build takes its style as the build returns.
        last = f.builder.div(row).size(30).rounded(4).border(2);
    };
    f.frame();
    CHECK(f.box(row).display == PrLayoutDisplay_Flex);
    CHECK(sameColor(f.decoration(row).background, PrColor{1, 0, 0, 1}));
    CHECK(f.bounds(first).left == 5);
    CHECK(width(f.bounds(first)) == 100);
    CHECK(f.bounds(last).left == 115);
    CHECK(width(f.bounds(last)) == 30);
    // A border with no color of its own takes the text's color.
    CHECK(sameColor(f.decoration(last).borderColor[PrRectSide_Top], PrColor{0, 0, 1, 1}));
}

TEST_CASE("an element inherits its parent's text, and one whose style is unchanged keeps its record") {
    ElementFixture f;
    PrLayoutNode parent = kPrLayoutNodeNone;
    PrLayoutNode child = kPrLayoutNodeNone;
    PrLayoutNode plain = kPrLayoutNodeNone;
    f.build = [&] {
        parent = f.builder.div(f.root).textColor(prColorFromHex(0x00ff00));
        child = f.builder.div(parent).w(10);
        plain = f.builder.div(f.root);
    };
    f.frame();
    CHECK(sameColor(f.color(child).color, PrColor{0, 1, 0, 1}));
    CHECK(sameColor(f.color(plain).color, PrColor{0, 0, 1, 1}));
    PrLayoutStyleIndex before = prLayoutNodeStyle(f.tree, child);
    f.frame();
    CHECK(prLayoutNodeStyle(f.tree, child) == before);
}

TEST_CASE("an element's hover block applies while the pointer is over it, and clicked reports a click") {
    ElementFixture f;
    PrLayoutNode button = kPrLayoutNodeNone;
    int clicks = 0;
    f.build = [&] {
        UiElement e = f.builder.div(f.root).size(50).bg(prColorFromHex(0x000000));
        e.hover([](UiElement& h) { h.bg(prColorFromHex(0xffffff)); });
        button = e;
        if (e.clicked())
            ++clicks;
    };
    f.frame();
    CHECK(sameColor(f.decoration(button).background, PrColor{0, 0, 0, 1}));
    f.move(10, 10);
    f.frame();
    CHECK(sameColor(f.decoration(button).background, PrColor{1, 1, 1, 1}));
    f.down(10, 10);
    f.up(10, 10);
    f.frame();
    CHECK(clicks == 1);
}

TEST_CASE("a hover shortcut applies its property while the pointer is over the element") {
    ElementFixture f;
    PrLayoutNode button = kPrLayoutNodeNone;
    f.build = [&] { button =
        f.builder.div(f.root).size(50).bg(prColorFromHex(0x000000)).hoverBg(prColorFromHex(0xff0000)); };
    f.frame();
    CHECK(sameColor(f.decoration(button).background, PrColor{0, 0, 0, 1}));
    f.move(10, 10);
    f.frame();
    CHECK(sameColor(f.decoration(button).background, PrColor{1, 0, 0, 1}));
}

TEST_CASE("an element styled after the next element is created opens again, and keeps the style it had") {
    ElementFixture f;
    PrLayoutNode first = kPrLayoutNodeNone;
    PrLayoutNode second = kPrLayoutNodeNone;
    f.build = [&] {
        UiElement a = f.builder.div(f.root).w(30).h(10);
        first = a;
        UiElement b = f.builder.div(f.root).w(40).h(10);
        second = b;
        a.bg(prColorFromHex(0x00ff00)).text("x");
        b.h(20);
    };
    f.frame();
    CHECK(sameColor(f.decoration(first).background, PrColor{0, 1, 0, 1}));
    CHECK(width(f.bounds(first)) == 30);
    CHECK(f.bounds(second).bottom - f.bounds(second).top == 20);
    CHECK(width(f.bounds(second)) == 40);
}

TEST_CASE("an animation's progress rises over its duration after its delay, and holds at its end") {
    ElementFixture f;
    float before = -1;
    float during = -1;
    float after = -1;
    double start = 0;
    UiAnimation animation{.duration = 1, .ease = UiEase_Linear};
    uint32_t round = 0;
    f.build = [&] {
        if (round == 0)
            start = f.builder.time();
        animation.start = start;
        animation.delay = 0.5;
        float p = f.builder.progress(animation);
        if (round == 0)
            before = p;
        else if (round == 1)
            during = p;
        else
            after = p;
        ++round;
    };
    f.frame(1.0 / 60, 1);
    CHECK(before == 0);
    // An animation that waits out its delay asks for a frame at the time that it starts.
    CHECK(prUiOutput(f.ui).frameTime == start + 0.5);
    f.frame(1, 1);
    CHECK(during > 0.4f);
    CHECK(during < 0.6f);
    f.frame(1, 1);
    CHECK(after == 1);
    CHECK(!prUiOutput(f.ui).wantsFrame);
}

TEST_CASE("an element with a transition moves its values toward a new target over its duration") {
    ElementFixture f;
    PrLayoutNode node = kPrLayoutNodeNone;
    float target = 0;
    f.build = [&] { node = f.builder.div(f.root).size(10).opacity(target).transition(1); };
    f.frame();
    auto opacity = [&] {
        return prLayoutEffectStyleRecord(f.tree, f.record(node).groups[PrLayoutGroup_Effect]).opacity;
    };
    CHECK(opacity() == 0);
    target = 1;
    f.frame();
    f.frame(0.5);
    CHECK(opacity() > 0);
    CHECK(opacity() < 1);
    f.frame(1);
    CHECK(opacity() == 1);
}

TEST_CASE("a grid of columns places its items in equal shares of its width, and a span covers several") {
    ElementFixture f;
    PrLayoutNode cells[3] = {};
    f.build = [&] {
        PrLayoutNode grid = f.builder.div(f.root).w(300).gridCols(3).gapX(0);
        cells[0] = f.builder.div(grid).h(10);
        cells[1] = f.builder.div(grid).h(10).colSpan(2);
        cells[2] = f.builder.div(grid).h(10);
    };
    f.frame();
    CHECK(width(f.bounds(cells[0])) == 100);
    CHECK(f.bounds(cells[1]).left == 100);
    CHECK(width(f.bounds(cells[1])) == 200);
    CHECK(f.bounds(cells[2]).top == 10);
}

TEST_CASE("a deferred call runs once as the build returns, however often the build defers it") {
    UiFixture f;
    int runs = 0;
    auto count = [](void* context, PrUi*) { ++*static_cast<int*>(context); };
    f.build = [&] {
        prUiDefer(f.ui, count, &runs);
        prUiDefer(f.ui, count, &runs);
        CHECK(runs == 0);
    };
    f.frame(1.0 / 60, 1);
    CHECK(runs == 1);
}

TEST_CASE("a memo whose inputs are unchanged is kept unbuilt, and one whose inputs change is built again") {
    ElementFixture f;
    Builds builds;
    uint32_t last = 2;
    PrLayoutNode panel = kPrLayoutNodeNone;
    f.build = [&] { panel = f.builder.memo(f.root, TestPanel{.builds = &builds, .last = last}); };
    f.frame();
    CHECK(builds.panels == 1);
    CHECK(builds.rows == 3);
    CHECK(f.bounds(panel).right == 400);
    f.frame();
    f.frame();
    CHECK(builds.panels == 1);
    CHECK(builds.rows == 3);
    CHECK(prLayoutTreeStats(f.tree).frameNodesKept != 0);
    CHECK(f.bounds(panel).right == 400);

    // The panel's inputs change, so it builds again, and of its rows only the one whose inputs changed.
    last = 50;
    f.frame();
    CHECK(builds.panels == 2);
    CHECK(builds.rows == 4);
}

TEST_CASE("input that reaches a memo inside a kept one builds it alone, and its result builds the build around it") {
    ElementFixture f;
    Builds builds;
    uint32_t clicks = 0;
    f.build = [&] {
        UiElement panel = f.builder.div(f.root).flexCol();
        for (uint32_t i = 0; i < 2; ++i) {
            if (f.builder.memo(panel, TestRow{.builds = &builds, .value = i}).clicked)
                ++clicks;
        }
    };
    f.frame();
    CHECK(builds.rows == 2);
    f.frame();
    CHECK(builds.rows == 2);

    // Hovering the first row builds it again, wider, and keeps the second.
    f.move(10, 5);
    f.frame();
    CHECK(builds.rows == 3);
    f.frame();
    CHECK(builds.rows == 3);

    // A click on it reports the click to the build around it.
    f.press(10, 5);
    f.frame();
    CHECK(clicks == 1);
}

TEST_CASE("a row inside a kept panel is built alone from the inputs that it kept when the pointer reaches it") {
    ElementFixture f;
    Builds builds;
    PrLayoutNode panel = kPrLayoutNodeNone;
    f.build = [&] { panel = f.builder.memo(f.root, TestPanel{.builds = &builds, .last = 2}); };
    f.frame();
    f.frame();
    CHECK(builds.panels == 1);
    CHECK(builds.rows == 3);
    f.move(10, 25);
    f.frame();
    // The panel reads no row's result, so it stays kept while the hovered row is built again in its place.
    CHECK(builds.panels == 1);
    CHECK(builds.rows == 4);
    PrLayoutNode second = prLayoutNodeNextSibling(f.tree, prLayoutNodeFirstChild(f.tree, panel));
    CHECK(width(f.bounds(second)) == 200);
}

TEST_CASE("a memo that read an extent is built again when the layout gives the extent another value") {
    ElementFixture f;
    Builds builds;
    float width = 400;
    PrLayoutNode meter = kPrLayoutNodeNone;
    f.build = [&] {
        UiElement box = f.builder.div(f.root).w(width);
        meter = f.builder.memo(box, TestMeter{.builds = &builds});
    };
    f.frame();
    CHECK(childCount(f.tree, meter) == 4);
    uint32_t settled = builds.meters;
    f.frame();
    CHECK(builds.meters == settled);
    width = 250;
    f.frame();
    CHECK(builds.meters > settled);
    CHECK(childCount(f.tree, meter) == 2);
}

TEST_CASE("a memo kept under a parent whose text changed is built again, and inherits the new text") {
    ElementFixture f;
    Builds builds;
    PrColor color{1, 0, 0, 1};
    PrLayoutNode row = kPrLayoutNodeNone;
    f.build = [&] {
        UiElement box = f.builder.div(f.root).textColor(color);
        row = f.builder.memo(box, TestRow{.builds = &builds, .value = 1});
    };
    f.frame();
    f.frame();
    CHECK(builds.rows == 1);
    color = PrColor{0, 1, 0, 1};
    f.frame();
    CHECK(builds.rows == 2);
    CHECK(sameColor(f.color(row).color, color));
}

TEST_CASE("a kept memo's root takes the caller's style after memo returns, as a built one does") {
    ElementFixture f;
    Builds builds;
    PrLayoutNode row = kPrLayoutNodeNone;
    f.build = [&] { row = f.builder.memo(f.root, TestRow{.builds = &builds, .value = 1}).root.ml(30); };
    f.frame();
    CHECK(f.bounds(row).left == 30);
    f.frame();
    f.frame();
    CHECK(builds.rows == 1);
    CHECK(f.bounds(row).left == 30);
    CHECK(width(f.bounds(row)) == 100);
}

TEST_CASE("a memo may be a struct local to the function that memoizes part of its build with it") {
    ElementFixture f;
    uint32_t builds = 0;
    uint32_t wide = 40;
    PrLayoutNode node = kPrLayoutNodeNone;
    f.build = [&] {
        // The memo's build sees nothing of the function but its inputs.
        struct Memo {
            uint32_t* builds;
            uint32_t wide;
            bool operator==(const Memo&) const = default;

            void build(UiBuilder& ui, UiElement root) const {
                ++*builds;
                root.flexRow().h(10);
                ui.div(root).w(float(wide)).h(10);
            }
        };
        node = f.builder.memo(f.root, Memo{.builds = &builds, .wide = wide});
    };
    f.frame();
    f.frame();
    CHECK(builds == 1);
    wide = 60;
    f.frame();
    CHECK(builds == 2);
    CHECK(width(f.bounds(prLayoutNodeFirstChild(f.tree, node))) == 60);
}

TEST_CASE("a path element draws a path that its resources parse once, sized by its style over its view box") {
    UiFixture f;
    UiResources resources(nullptr);
    UiBuilder builder{{.ui = f.ui, .resources = &resources}};
    PrLayoutNode icon = kPrLayoutNodeNone;
    const PrPath* path = resources.path("M2 2L22 22");
    CHECK(resources.path("M2 2L22 22") == path);
    f.build = [&] {
        UiPath drawn{.path = path, .viewBox = PrVec2{24, 24}, .color = PrColor{1, 0, 0, 1}, .strokeWidth = 2};
        icon = builder.path(f.root, drawn).size(14);
    };
    f.frame();
    PrLayoutContent content = prLayoutNodeContent(f.tree, icon);
    REQUIRE(content.kind == PrLayoutContentKind_Path);
    CHECK(prPathId(content.path.path) == prPathId(path));
    CHECK(content.path.strokeWidth == 2);
    PrRect bounds = prLayoutRectToPx(prLayoutNodeBounds(f.tree, icon, PrLayoutCoordinates_Viewport));
    CHECK(width(bounds) == 14);
}
