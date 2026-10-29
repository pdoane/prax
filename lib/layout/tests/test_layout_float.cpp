// Block formatting contexts, floats, clearance and margin collapsing. Each stage is 200px wide, words are inline-blocks
// 20px tall in a font of size 0, and each edge is measured from the stage's edge.

#include "pr/layout_style.h"
#include "pr/layout_tree.h"

#include "layout/tests/layout_fixture.h"

#include "doctest/doctest.h"

#include <vector>

namespace pr_impl {

namespace {

// A stage of a measured case: a flow-root 200px wide in a root, with everything in a font of size 0, so that lines are
// only as tall as what is on them.
struct Stage {
    Fixture f;
    PrLayoutNode root = kPrLayoutNodeNone;
    PrLayoutNode stage = kPrLayoutNodeNone;

    explicit Stage(float width = 200, PrLayoutWritingMode writingMode = PrLayoutWritingMode_HorizontalTb,
                   PrTextDirection direction = PrTextDirection_Ltr) {
        // The root takes the stage's writing mode, so that the stage is in its root's flow.
        PrLayoutFontStyle rootFont = font0();
        rootFont.writingMode = writingMode;
        root = add(kPrLayoutNodeNone, block(), &rootFont);
        PrLayoutBoxStyle box = block();
        box.display = PrLayoutDisplay_FlowRoot;
        box.width = prLayoutLengthPx(width);
        PrLayoutFontStyle font = font0();
        font.writingMode = writingMode;
        font.direction = direction;
        if (writingMode != PrLayoutWritingMode_HorizontalTb) {
            box.width = prLayoutLengthPx(100);
            box.height = prLayoutLengthPx(width);
        }
        stage = f.element(root, box, nullptr, nullptr, &font);
    }

    static PrLayoutFontStyle font0() {
        PrLayoutFontStyle font = prLayoutFontStyleDefault();
        font.font = textFontOf(0);
        return font;
    }

    // Creates an element under `parent`, in the stage's font if `font` is null.
    PrLayoutNode add(PrLayoutNode parent, const PrLayoutBoxStyle& box,
                     const PrLayoutFontStyle* PR_NULLABLE font = nullptr) {
        PrLayoutFontStyle zero = font0();
        return f.element(parent, box, nullptr, nullptr, font != nullptr ? font : &zero);
    }

    // Creates a word `width` by `height`, and a space, which has no width.
    PrLayoutNode word(PrLayoutNode parent, float width, float height = 20) {
        PrLayoutBoxStyle box = prLayoutBoxStyleDefault();
        box.display = PrLayoutDisplay_InlineBlock;
        box.width = prLayoutLengthPx(width);
        box.height = prLayoutLengthPx(height);
        return add(parent, box);
    }
    void space(PrLayoutNode parent) { f.text(parent, " "); }

    void layout(float width = 400, float height = 800) {
        f.layout(px(width, height));
        CHECK(verified(f.tree));
    }

    PrRect at(PrLayoutNode node) {
        PrRect stageRect = f.bounds(stage);
        PrRect rect = f.bounds(node);
        return PrRect{rect.left - stageRect.left, rect.top - stageRect.top, rect.right - stageRect.left,
                      rect.bottom - stageRect.top};
    }
    double top(PrLayoutNode node) { return double(at(node).top); }
    double left(PrLayoutNode node) { return double(at(node).left); }
    double width(PrLayoutNode node) { return double(at(node).right - at(node).left); }
    double height(PrLayoutNode node) { return double(at(node).bottom - at(node).top); }
};

PrLayoutBoxStyle sized(PrLayoutBoxStyle box, float width, float height) {
    if (width >= 0)
        box.width = prLayoutLengthPx(width);
    if (height >= 0)
        box.height = prLayoutLengthPx(height);
    return box;
}

// Returns a float on `side`, `width` by `height` if given.
PrLayoutBoxStyle floated(PrLayoutFloat side, float width = -1, float height = -1) {
    PrLayoutBoxStyle box = sized(block(), width, height);
    box.floating = side;
    return box;
}
PrLayoutBoxStyle left(float width = -1, float height = -1) {
    return floated(PrLayoutFloat_Left, width, height);
}
PrLayoutBoxStyle right(float width = -1, float height = -1) {
    return floated(PrLayoutFloat_Right, width, height);
}

PrLayoutBoxStyle margined(PrLayoutBoxStyle box, float top, float bottom) {
    setMargins(box, top, bottom);
    return box;
}

PrLayoutBoxStyle cleared(PrLayoutBoxStyle box, PrLayoutClear clear) {
    box.clear = clear;
    return box;
}

PrLayoutBoxStyle hidden(PrLayoutBoxStyle box) {
    box.overflowX = PrLayoutOverflow_Hidden;
    box.overflowY = PrLayoutOverflow_Hidden;
    return box;
}

PrLayoutBoxStyle withSide(PrLayoutBoxStyle box, PrRectSide side, float margin) {
    box.margin[side] = prLayoutLengthPx(margin);
    return box;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a float between collapsing margins is placed after the margins before it") {
    Stage s;
    s.add(s.stage, margined(block(20), 0, 10));
    PrLayoutNode f = s.add(s.stage, left(40, 20));
    PrLayoutNode b = s.add(s.stage, margined(block(20), 30, 0));
    s.layout();
    CHECK(s.top(f) == 30);
    CHECK(s.top(b) == 50);
}

TEST_CASE("a float waits for the margins of a parent that collapses with its child, and goes where they resolve") {
    Stage s;
    s.add(s.stage, block(20));
    PrLayoutNode parent = s.add(s.stage, block());
    PrLayoutNode f = s.add(parent, left(30, 60));
    PrLayoutNode child = s.add(parent, margined(block(20), 40, 0));
    s.layout();
    CHECK(s.top(parent) == 60);
    CHECK(s.top(f) == 60);
    CHECK(s.top(child) == 60);
    CHECK(prLayoutTreeStats(s.f.tree).blocksRestarted == 1);
}

TEST_CASE("a box that collapses through holding a float is placed after its block-start margins") {
    struct Case {
        const char* name;
        float bottom;               // E's block-end margin
        int empty;                  // an empty box with margins 20 in E: 0 none, 1 before the float, 2 after
        double e, f, g;
    };
    const Case cases[] = {{"no block-end margin", 0, 0, 30, 30, 50},
                          {"a block-end margin", 25, 0, 30, 30, 50},
                          {"an empty box before the float", 0, 1, 40, 40, 50},
                          {"an empty box after the float", 0, 2, 40, 40, 50}};
    for (const Case& c : cases) {
        CAPTURE(c.name);
        Stage s;
        s.add(s.stage, margined(block(20), 0, 10));
        PrLayoutNode e = s.add(s.stage, margined(block(), 5, c.bottom));
        if (c.empty == 1)
            s.add(e, margined(block(), 20, 0));
        PrLayoutNode f = s.add(e, left(30, 20));
        if (c.empty == 2)
            s.add(e, margined(block(), 20, 0));
        PrLayoutNode g = s.add(s.stage, margined(block(20), 30, 0));
        s.layout();
        CHECK(s.top(e) == c.e);
        CHECK(s.top(f) == c.f);
        CHECK(s.top(g) == c.g);
    }
}

TEST_CASE("clearance of floats adjoining a child puts it at their bottom, its margin taking no room") {
    for (float margin : {40.0f, 100.0f}) {
        CAPTURE(margin);
        Stage s;
        s.add(s.stage, block(20));
        PrLayoutNode parent = s.add(s.stage, block());
        PrLayoutNode f = s.add(parent, left(30, 60));
        PrLayoutNode child = s.add(parent, cleared(margined(block(20), margin, 0), PrLayoutClear_Left));
        s.layout();
        CHECK(s.top(parent) == 20);
        CHECK(s.top(f) == 20);
        CHECK(s.top(child) == 80);
    }
}

TEST_CASE("the margins after a cleared box that collapses through start its block-start margins above it") {
    struct Case {
        const char* name;
        float top, bottom, next;
        bool child;                 // true if its margins are an empty child's, with margins 10
        double e, b;
    };
    const Case cases[] = {{"margins on both sides", 10, 10, 20, false, 70, 80},
                          {"no block-start margin", 0, 10, 20, false, 70, 90},
                          {"a block-start margin past the floats", 30, 10, 20, false, 70, 70},
                          {"an empty child's margins", 0, 0, 20, true, 70, 80},
                          {"a negative margin after it", 30, 0, -10, false, 70, 60}};
    for (const Case& c : cases) {
        CAPTURE(c.name);
        Stage s;
        s.add(s.stage, block(20));
        PrLayoutNode f = s.add(s.stage, left(30, 50));
        PrLayoutNode e = s.add(s.stage, cleared(margined(block(), c.top, c.bottom), PrLayoutClear_Both));
        if (c.child)
            s.add(e, margined(block(), 10, 10));
        PrLayoutNode b = s.add(s.stage, margined(block(20), c.next, 0));
        s.layout();
        CHECK(s.top(f) == 20);
        CHECK(s.top(e) == c.e);
        CHECK(s.top(b) == c.b);
    }
}

TEST_CASE("a cleared empty box past margins larger than its own") {
    Stage s;
    s.add(s.stage, margined(block(20), 0, 40));
    PrLayoutNode f = s.add(s.stage, left(30, 20));
    PrLayoutNode e = s.add(s.stage, cleared(margined(block(), 30, 0), PrLayoutClear_Both));
    PrLayoutNode b = s.add(s.stage, margined(block(20), 5, 0));
    s.layout();
    CHECK(s.top(f) == 60);
    CHECK(s.top(e) == 80);
    CHECK(s.top(b) == 80);
}

TEST_CASE("the margins after a cleared box that collapses through stay inside its parent") {
    Stage s;
    PrLayoutNode parent = s.add(s.stage, margined(block(), 0, 20));
    s.add(parent, block(20));
    s.add(parent, left(30, 50));
    PrLayoutNode e = s.add(parent, cleared(block(), PrLayoutClear_Both));
    PrLayoutNode n = s.add(s.stage, block(20));
    s.layout();
    CHECK(s.height(parent) == 70);
    CHECK(s.top(e) == 70);
    CHECK(s.top(n) == 90);
}

TEST_CASE("clearance takes the hypothetical position with a first child's margin") {
    Stage s;
    s.add(s.stage, block(20));
    s.add(s.stage, left(30, 50));
    PrLayoutNode c = s.add(s.stage, cleared(margined(block(), 10, 0), PrLayoutClear_Both));
    PrLayoutNode g = s.add(c, margined(block(20), 30, 0));
    s.layout();
    CHECK(s.top(c) == 70);
    CHECK(s.top(g) == 70);
}

TEST_CASE("a float in a parent's first child that collapses through goes where the parent's margins resolve") {
    Stage s;
    s.add(s.stage, block(20));
    PrLayoutNode p = s.add(s.stage, margined(block(), 10, 0));
    PrLayoutNode c = s.add(p, block());
    PrLayoutNode f = s.add(c, left(30, 20));
    PrLayoutNode d = s.add(p, margined(block(20), 30, 0));
    s.layout();
    CHECK(s.top(p) == 50);
    CHECK(s.top(c) == 50);
    CHECK(s.top(f) == 50);
    CHECK(s.top(d) == 50);
}

TEST_CASE("a float before a block with a margin is placed before the margin") {
    for (bool avoids : {false, true}) {
        CAPTURE(avoids);
        Stage s;
        s.add(s.stage, block(20));
        PrLayoutNode f = s.add(s.stage, left(30, 20));
        PrLayoutBoxStyle box = margined(block(), 30, 0);
        if (avoids)
            box = hidden(sized(box, -1, 20));
        PrLayoutNode b = s.add(s.stage, box);
        PrLayoutNode w = avoids ? b : s.word(b, 40);
        s.layout();
        CHECK(s.top(f) == 20);
        CHECK(s.top(b) == 50);
        CHECK(s.left(w) == 0);
    }
}

TEST_CASE("a box that avoids floats resolves adjoining floats with its margin where it fits beside them") {
    for (bool fits : {true, false}) {
        CAPTURE(fits);
        Stage s;
        s.add(s.stage, block(20));
        PrLayoutNode p = s.add(s.stage, block());
        PrLayoutNode f = s.add(p, left(fits ? 30 : 150, 20));
        PrLayoutBoxStyle box = hidden(margined(block(20), 30, 0));
        if (!fits)
            box.width = prLayoutLengthPx(100);
        PrLayoutNode r = s.add(p, box);
        s.layout();
        if (fits) {
            CHECK(s.top(p) == 50);
            CHECK(s.top(f) == 50);
            CHECK(s.top(r) == 50);
            CHECK(s.left(r) == 30);
        } else {
            CHECK(s.top(p) == 20);
            CHECK(s.top(f) == 20);
            CHECK(s.top(r) == 40);
        }
    }
}

TEST_CASE("a float beside no content waits for the margins before a box that avoids it, its line or none") {
    // A float, alone among white space, in an empty span, or beside the placeholder of a box out of flow. Then a block
    // holding a box that avoids floats, too wide to fit beside the float, with a margin of 100. The box separates its
    // margin from the float, which stays at the top.
    for (int kind = 0; kind < 3; ++kind) {
        CAPTURE(kind);
        Stage s(100);
        PrLayoutNode p = s.add(s.stage, block());
        s.space(p);
        PrLayoutNode f = s.add(kind == 1 ? s.add(p, inlineBox()) : p, right(50, 30));
        if (kind == 2) {
            PrLayoutBoxStyle out = block(10);
            out.position = PrLayoutPosition_Absolute;
            out.staticInline = 1;
            s.add(p, out);
        }
        s.space(p);
        PrLayoutNode wrap = s.add(p, block());
        PrLayoutBoxStyle avoider = hidden(margined(block(70), 100, 0));
        avoider.width = prLayoutLengthPx(100);
        PrLayoutNode r = s.add(wrap, avoider);
        s.space(p);
        s.layout();
        CHECK(s.top(f) == 0);
        CHECK(s.top(r) == 30);
    }
}

TEST_CASE("a box that avoids floats keeps its border box beside them, its margins overlapping them") {
    struct Case {
        const char* name;
        PrLayoutFloat side;
        PrRectSide margin;
        float value;
        double left, width;
    };
    const Case cases[] = {{"a left margin under a left float", PrLayoutFloat_Left, PrRectSide_Left, 20, 60, 140},
                          {"a left margin past a left float", PrLayoutFloat_Left, PrRectSide_Left, 80, 80, 120},
                          {"a right margin under a right float", PrLayoutFloat_Right, PrRectSide_Right, 30, 0, 140},
                          {"negative left, no float", PrLayoutFloat_None, PrRectSide_Left, -20, -20, 220},
                          {"negative left, a right float", PrLayoutFloat_Right, PrRectSide_Left, -20, 0, 140},
                          {"negative left, a left float", PrLayoutFloat_Left, PrRectSide_Left, -20, 60, 140},
                          {"negative right, a left float", PrLayoutFloat_Left, PrRectSide_Right, -20, 60, 140}};
    for (const Case& c : cases) {
        CAPTURE(c.name);
        Stage s;
        if (c.side != PrLayoutFloat_None)
            s.add(s.stage, floated(c.side, 60, 60));
        PrLayoutNode box = s.add(s.stage, withSide(hidden(block(20)), c.margin, c.value));
        s.layout();
        CHECK(s.left(box) == c.left);
        CHECK(s.width(box) == c.width);
    }
}

TEST_CASE("a box that avoids floats goes to the first offset where it fits beside them") {
    {
        INFO("too wide beside the float");
        Stage s;
        s.add(s.stage, left(60, 40));
        PrLayoutNode box = s.add(s.stage, hidden(sized(block(), 160, 20)));
        s.layout();
        CHECK(s.top(box) == 40);
        CHECK(s.left(box) == 0);
    }
    {
        INFO("an auto width fills the space whatever its content asks");
        Stage s;
        s.add(s.stage, left(60, 40));
        PrLayoutNode box = s.add(s.stage, hidden(block()));
        s.word(box, 160);
        s.layout();
        CHECK(s.top(box) == 0);
        CHECK(s.left(box) == 60);
        CHECK(s.width(box) == 140);
    }
    {
        INFO("beside the float at its margin");
        Stage s;
        s.add(s.stage, block(20));
        PrLayoutNode f = s.add(s.stage, left(60, 60));
        PrLayoutNode box = s.add(s.stage, hidden(margined(block(20), 20, 0)));
        s.layout();
        CHECK(s.top(f) == 20);
        CHECK(s.top(box) == 40);
        CHECK(s.left(box) == 60);
    }
    {
        INFO("below the float, its margin overlapping it");
        Stage s;
        s.add(s.stage, margined(block(20), 0, 30));
        PrLayoutNode f = s.add(s.stage, left(150, 40));
        PrLayoutNode box = s.add(s.stage, hidden(margined(sized(block(), 100, 20), 10, 0)));
        s.layout();
        CHECK(s.top(f) == 50);
        CHECK(s.top(box) == 90);
    }
    {
        INFO("flow-root and a block-level replaced box avoid floats");
        Stage s;
        s.add(s.stage, left(60, 40));
        PrLayoutBoxStyle flowRoot = block(20);
        flowRoot.display = PrLayoutDisplay_FlowRoot;
        PrLayoutNode root = s.add(s.stage, flowRoot);
        PrLayoutNode image = replaced(s.f, s.stage, sized(block(), 100, 20), natural(10, 2));
        s.layout();
        CHECK(s.left(root) == 60);
        CHECK(s.width(root) == 140);
        CHECK(s.top(image) == 20);
        CHECK(s.left(image) == 60);
    }
    {
        INFO("it must fit over its whole block size");
        Stage s;
        s.add(s.stage, left(60, 20));
        s.add(s.stage, cleared(left(150, 20), PrLayoutClear_Left));
        PrLayoutNode box = s.add(s.stage, hidden(sized(block(), 100, 30)));
        s.layout();
        CHECK(s.top(box) == 40);
        CHECK(s.left(box) == 0);
    }
    {
        INFO("auto margins share the space beside floats, percentages are of the containing block");
        Stage s;
        s.add(s.stage, left(60, 40));
        PrLayoutBoxStyle centered = hidden(sized(block(), 100, 20));
        centered.margin[PrRectSide_Left] = prLayoutLengthAuto();
        centered.margin[PrRectSide_Right] = prLayoutLengthAuto();
        PrLayoutNode a = s.add(s.stage, centered);
        s.layout();
        CHECK(s.left(a) == 80);
    }
    {
        Stage s;
        s.add(s.stage, left(60, 40));
        PrLayoutBoxStyle half = hidden(block(20));
        half.width = prLayoutLengthPercent(50);
        PrLayoutNode a = s.add(s.stage, half);
        s.layout();
        CHECK(s.left(a) == 60);
        CHECK(s.width(a) == 100);
    }
    {
        INFO("wider than its containing block, below the float, and with no float, where it is");
        Stage s;
        s.add(s.stage, left(60, 40));
        PrLayoutNode box = s.add(s.stage, hidden(sized(block(), 300, 20)));
        s.layout();
        CHECK(s.top(box) == 40);
        CHECK(s.left(box) == 0);
    }
    {
        Stage s;
        s.add(s.stage, block(20));
        PrLayoutNode box = s.add(s.stage, hidden(sized(block(), 300, 20)));
        s.layout();
        CHECK(s.top(box) == 20);
        CHECK(s.left(box) == 0);
    }
    {
        INFO("an auto width is refined at one offset before it is given up");
        Stage s;
        s.add(s.stage, left(30, 20));
        s.add(s.stage, cleared(left(100, 20), PrLayoutClear_Left));
        PrLayoutBoxStyle flowRoot = block(40);
        flowRoot.display = PrLayoutDisplay_FlowRoot;
        PrLayoutNode box = s.add(s.stage, flowRoot);
        s.layout();
        CHECK(s.top(box) == 0);
        CHECK(s.left(box) == 100);
        CHECK(s.width(box) == 100);
    }
}

TEST_CASE("a box that avoids floats clear of a float below it is laid out as any block box") {
    struct Case {
        const char* name;
        float marginLeft, marginRight, width, minWidth, height;
        double top, left, boxWidth;
    };
    const Case cases[] = {{"a negative left margin", -20, 0, -1, -1, 20, 0, -20, 220},
                          {"a negative right margin", 0, -20, -1, -1, 20, 0, 0, 220},
                          {"wider than its containing block", 0, 0, 300, -1, 20, 0, 0, 300},
                          {"a minimum wider than its containing block", 0, 0, -1, 300, 20, 0, 0, 300},
                          {"wider, and tall enough to meet the float", 0, 0, 300, -1, 110, 140, 0, 300},
                          {"a width that fits", 0, 0, 100, -1, 20, 0, 0, 100}};
    for (const Case& c : cases) {
        CAPTURE(c.name);
        Stage s;
        s.add(s.stage, block(100));
        s.add(s.stage, left(60, 40));
        PrLayoutBoxStyle box = hidden(margined(sized(block(), c.width, c.height), -100, 0));
        box.margin[PrRectSide_Left] = prLayoutLengthPx(c.marginLeft);
        box.margin[PrRectSide_Right] = prLayoutLengthPx(c.marginRight);
        if (c.minWidth >= 0)
            box.minWidth = prLayoutLengthPx(c.minWidth);
        PrLayoutNode node = s.add(s.stage, box);
        s.layout();
        CHECK(s.top(node) == c.top);
        CHECK(s.left(node) == c.left);
        CHECK(s.width(node) == c.boxWidth);
    }
}

TEST_CASE("a float shrinks to fit its containing block whatever floats are beside it") {
    Stage s;
    s.add(s.stage, left(60, 20));
    PrLayoutNode f = s.add(s.stage, left());
    s.word(f, 80);
    s.space(f);
    s.word(f, 80);
    s.layout();
    CHECK(s.top(f) == 20);
    CHECK(s.left(f) == 0);
    CHECK(s.width(f) == 160);
}

TEST_CASE("a float in a line goes on it where it fits, and after it where it does not") {
    struct Case {
        const char* name;
        float first, floatWidth, next;
        double floatTop, floatLeft, firstLeft, nextLeft;
    };
    const Case cases[] = {{"a float too wide for what the line leaves", 100, 150, 40, 20, 0, 0, 100},
                          {"a float that fits beside the line", 100, 60, 40, 0, 0, 60, 160},
                          {"a float past a full line", 150, 60, 30, 20, 0, 0, 150}};
    for (const Case& c : cases) {
        CAPTURE(c.name);
        Stage s;
        PrLayoutNode first = s.word(s.stage, c.first);
        s.space(s.stage);
        PrLayoutNode f = s.add(s.stage, left(c.floatWidth, 20));
        PrLayoutNode next = s.word(s.stage, c.next);
        s.layout();
        CHECK(s.top(f) == c.floatTop);
        CHECK(s.left(f) == c.floatLeft);
        CHECK(s.left(first) == c.firstLeft);
        CHECK(s.top(next) == 0);
        CHECK(s.left(next) == c.nextLeft);
    }
}

TEST_CASE("lines beside a float are shortened over its height, and moved down where nothing fits") {
    {
        INFO("words beside a float wrap over its height");
        Stage s;
        s.add(s.stage, left(60, 30));
        PrLayoutNode a = s.word(s.stage, 100);
        s.space(s.stage);
        PrLayoutNode b = s.word(s.stage, 100);
        s.space(s.stage);
        PrLayoutNode c = s.word(s.stage, 100);
        s.layout();
        CHECK(s.left(a) == 60);
        CHECK(s.top(a) == 0);
        CHECK(s.left(b) == 60);
        CHECK(s.top(b) == 20);
        CHECK(s.top(c) == 40);
        CHECK(s.left(c) == 0);
    }
    {
        INFO("a line taller than its first guess is broken again in the space over its height");
        Stage s;
        s.add(s.stage, left(30, 20));
        s.add(s.stage, cleared(left(100, 20), PrLayoutClear_Left));
        PrLayoutNode tall = s.word(s.stage, 40, 40);
        s.space(s.stage);
        PrLayoutNode word = s.word(s.stage, 100);
        s.layout();
        CHECK(s.top(tall) == 0);
        CHECK(s.left(tall) == 100);
        CHECK(s.top(word) == 40);
        CHECK(s.left(word) == 0);
    }
    {
        INFO("floats-room: a word too wide beside a float moves below it");
        Stage s;
        s.add(s.stage, left(120, 40));
        PrLayoutNode word = s.word(s.stage, 160);
        s.layout();
        CHECK(s.top(word) == 40);
        CHECK(s.left(word) == 0);
    }
}

TEST_CASE("a line below many stacked floats moves down to where it fits with one search") {
    for (bool inLine : {true, false}) {
        CAPTURE(inLine);
        Stage s;
        // 70 full-width floats 1px tall, each clearing the one before, so that they stack.
        for (int i = 0; i < 70; ++i)
            s.add(s.stage, cleared(left(200, 1), PrLayoutClear_Left));
        PrLayoutNode container = inLine ? s.stage : s.add(s.stage, block());
        PrLayoutNode word = s.word(container, 40);
        s.layout();
        CHECK(s.top(word) == 70);
        CHECK(s.left(word) == 0);
        // Broken where it starts, and once more where it fits, not at each float's edge on the way.
        CHECK(prLayoutTreeStats(s.f.tree).linesBroken == 2);
    }
}

TEST_CASE("a float with no outer block size pushes nothing aside") {
    Stage s;
    s.add(s.stage, withSide(left(60, 20), PrRectSide_Bottom, -20));
    PrLayoutNode next = s.add(s.stage, left(50, 20));
    PrLayoutNode word = s.word(s.stage, 40);
    s.layout();
    CHECK(s.top(next) == 0);
    CHECK(s.left(next) == 0);
    CHECK(s.left(word) == 50);
}

TEST_CASE("floats take their sides by the line, and relative positioning leaves their place") {
    {
        Stage s;
        PrLayoutBoxStyle moved = left(60, 20);
        moved.position = PrLayoutPosition_Relative;
        moved.inset[PrRectSide_Left] = prLayoutLengthPx(20);
        PrLayoutNode f = s.add(s.stage, moved);
        PrLayoutNode word = s.word(s.stage, 40);
        s.layout();
        CHECK(s.left(f) == 20);
        CHECK(s.left(word) == 60);
    }
    {
        Stage s(200, PrLayoutWritingMode_HorizontalTb, PrTextDirection_Rtl);
        PrLayoutNode f = s.add(s.stage, left(60, 20), nullptr);
        PrLayoutNode word = s.word(s.stage, 40);
        s.layout();
        CHECK(s.left(f) == 0);
        CHECK(s.left(word) == 160);
    }
    for (PrLayoutFloat side : {PrLayoutFloat_Left, PrLayoutFloat_Right}) {
        CAPTURE(int(side));
        Stage s(200, PrLayoutWritingMode_VerticalRl);
        PrLayoutFontStyle font = Stage::font0();
        font.writingMode = PrLayoutWritingMode_VerticalRl;
        PrLayoutNode f = s.add(s.stage, floated(side, 30, 30), &font);
        PrLayoutBoxStyle word = prLayoutBoxStyleDefault();
        word.display = PrLayoutDisplay_InlineBlock;
        word.width = prLayoutLengthPx(20);
        word.height = prLayoutLengthPx(40);
        PrLayoutNode w = s.add(s.stage, word, &font);
        s.layout();
        CHECK(s.left(f) == 70);
        if (side == PrLayoutFloat_Left) {
            CHECK(s.top(f) == 0);
            CHECK(s.top(w) == 30);
            CHECK(s.left(w) == 80);
        } else {
            CHECK(s.top(f) == 170);
            CHECK(s.top(w) == 0);
        }
    }
}

TEST_CASE("floats of the logical sides follow the containing block's direction") {
    for (PrTextDirection direction : {PrTextDirection_Ltr, PrTextDirection_Rtl}) {
        CAPTURE(int(direction));
        Stage s(200, PrLayoutWritingMode_HorizontalTb, direction);
        PrLayoutNode start = s.add(s.stage, floated(PrLayoutFloat_InlineStart, 30, 20));
        PrLayoutNode end = s.add(s.stage, floated(PrLayoutFloat_InlineEnd, 40, 20));
        PrLayoutNode next = s.add(s.stage, cleared(block(10), PrLayoutClear_InlineStart));
        s.layout();
        bool ltr = direction == PrTextDirection_Ltr;
        CHECK(s.left(start) == (ltr ? 0 : 170));
        CHECK(s.left(end) == (ltr ? 160 : 0));
        CHECK(s.top(next) == 20);
    }
}

TEST_CASE("clearance of a float that leaked from a block, and a negative margin beside a float") {
    {
        Stage s;
        PrLayoutNode holder = s.add(s.stage, block());
        s.add(holder, left(30, 40));
        PrLayoutNode c = s.add(s.stage, cleared(block(20), PrLayoutClear_Left));
        s.layout();
        CHECK(s.height(holder) == 0);
        CHECK(s.top(c) == 40);
    }
    {
        Stage s;
        s.add(s.stage, left(60, 40));
        s.add(s.stage, block(50));
        PrLayoutNode n = s.add(s.stage, margined(block(), -40, 0));
        PrLayoutNode word = s.word(n, 40);
        s.layout();
        CHECK(s.top(n) == 10);
        CHECK(s.left(word) == 60);
    }
}

TEST_CASE("clearance level with a float's bottom is clearance, and one unit past is none") {
    struct Case {
        const char* name;
        float floatHeight;
        double parent, next;
    };
    const Case cases[] = {{"level with the float's bottom", 10, 60, 60},
                          {"one unit past it", 11, 61, 61},
                          {"short of it", 9, 20, 60}};
    for (const Case& c : cases) {
        CAPTURE(c.name);
        Stage s;
        PrLayoutNode parent = s.add(s.stage, block());
        s.add(parent, margined(block(20), 0, 30));
        s.add(parent, left(30, c.floatHeight));
        s.add(parent, cleared(margined(block(), 40, 0), PrLayoutClear_Both));
        PrLayoutNode next = s.add(s.stage, block(20));
        s.layout();
        CHECK(s.height(parent) == c.parent);
        CHECK(s.top(next) == c.next);
    }
}

TEST_CASE("intrinsic sizes sum floats beside one another, which clearance and blocks in flow end") {
    enum Kind { Left, Right, LeftClear, Block, Avoider, ClearBlock, Word };
    struct Item {
        Kind kind;
        float width;
    };
    struct Case {
        const char* name;
        bool min;
        std::vector<Item> items;
        double width;
    };
    const Case cases[] = {
        {"two left floats", false, {{Left, 50}, {Left, 70}}, 120},
        {"a left float, and one that clears it", false, {{Left, 50}, {LeftClear, 70}}, 70},
        {"a float, then a block", false, {{Left, 50}, {Block, 100}}, 100},
        {"a float, then a box that avoids floats", false, {{Left, 50}, {Avoider, 100}}, 150},
        {"a word, then a float", false, {{Word, 100}, {Left, 50}}, 150},
        {"min-content of two floats", true, {{Left, 50}, {Left, 70}}, 70},
        {"a left and a right float", false, {{Left, 50}, {Right, 70}}, 120},
        {"a float, a block that clears, a float", false, {{Left, 50}, {ClearBlock, 100}, {Left, 70}}, 100},
        {"a float, a block, a float", false, {{Left, 50}, {Block, 100}, {Left, 70}}, 100},
    };
    for (const Case& c : cases) {
        CAPTURE(c.name);
        Stage s;
        PrLayoutBoxStyle measured = block();
        measured.width = c.min ? prLayoutLengthMinContent() : prLayoutLengthMaxContent();
        PrLayoutNode box = s.add(s.stage, measured);
        for (const Item& item : c.items) {
            switch (item.kind) {
                case Left: s.add(box, left(item.width, 10)); break;
                case Right: s.add(box, right(item.width, 10)); break;
                case LeftClear: s.add(box, cleared(left(item.width, 10), PrLayoutClear_Left)); break;
                case Block: s.word(s.add(box, block()), item.width); break;
                case Avoider: s.add(box, hidden(sized(block(), item.width, 10))); break;
                case ClearBlock: s.add(box, cleared(sized(block(), item.width, 10), PrLayoutClear_Left)); break;
                case Word: s.word(box, item.width); break;
            }
        }
        s.layout();
        CHECK(s.width(box) == c.width);
    }
}

TEST_CASE("a formatting context root grows to contain its floats, and only its own") {
    struct Case {
        const char* name;
        float floatHeight, floatMargin, child, childMargin;
        double height;
    };
    const Case cases[] = {{"a float with a negative block-end margin", 40, -10, 0, 0, 30},
                          {"a float shorter than a block after it", 10, 0, 20, 0, 20},
                          {"a float taller than a block with a margin", 40, 0, 20, 30, 50}};
    for (const Case& c : cases) {
        CAPTURE(c.name);
        Stage s;
        PrLayoutBoxStyle flowRoot = block();
        flowRoot.display = PrLayoutDisplay_FlowRoot;
        PrLayoutNode root = s.add(s.stage, flowRoot);
        s.add(root, withSide(left(30, c.floatHeight), PrRectSide_Bottom, c.floatMargin));
        if (c.child > 0)
            s.add(root, margined(block(c.child), 0, c.childMargin));
        s.layout();
        CHECK(s.height(root) == c.height);
    }
    {
        INFO("bfc-root-floats: a float inside a float counts for the outer one only");
        Stage s;
        PrLayoutBoxStyle flowRoot = block();
        flowRoot.display = PrLayoutDisplay_FlowRoot;
        PrLayoutNode root = s.add(s.stage, flowRoot);
        PrLayoutNode outer = s.add(root, right(60, 20));
        s.add(outer, left(30, 60));
        s.layout();
        CHECK(s.height(root) == 20);
    }
}

TEST_CASE("a float paints over a block's background and under its text, and is hit there") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutDecorationStyle red = background(PrColor{1, 0, 0, 1});
    PrLayoutDecorationStyle green = background(PrColor{0, 1, 0, 1});
    PrLayoutDecorationStyle blue = background(PrColor{0, 0, 1, 1});
    PrLayoutBoxStyle fl = left(20, 20);
    fl.margin[PrRectSide_Right] = prLayoutLengthPx(-20);
    PrLayoutFontStyle font = Stage::font0();
    PrLayoutNode floated = f.element(root, fl, &green);
    PrLayoutNode under = f.element(root, block(40), &red, nullptr, &font);
    PrLayoutBoxStyle word = prLayoutBoxStyleDefault();
    word.display = PrLayoutDisplay_InlineBlock;
    word.width = prLayoutLengthPx(10);
    word.height = prLayoutLengthPx(10);
    PrLayoutNode over = f.element(under, word, &blue, nullptr, &font);
    f.layout(px(40, 40));
    CHECK(verified(f.tree));
    std::vector<uint8_t> pixels = render(f.tree, 40, 40);
    CHECK(pixel(pixels, 40, 15, 15) == "0 255 0 255");
    CHECK(pixel(pixels, 40, 5, 5) == "0 0 255 255");
    CHECK(pixel(pixels, 40, 30, 30) == "255 0 0 255");
    CHECK(f.hit(PrVec2{15, 15}) == floated);
    CHECK(f.hit(PrVec2{5, 5}) == over);
    CHECK(f.hit(PrVec2{30, 30}) == under);
}

TEST_CASE("a change inside a float lays out again the boxes beside it, and reuses those below it") {
    Stage s;
    PrLayoutNode f = s.add(s.stage, left(60, 30));
    PrLayoutNode beside = s.add(s.stage, block());
    PrLayoutNode word = s.word(beside, 40);
    PrLayoutNode below = s.add(s.stage, margined(block(), 40, 0));
    s.word(below, 40);
    s.layout();
    CHECK(s.left(word) == 60);
    CHECK(s.top(below) == 60);
    PrLayoutFontStyle font = Stage::font0();
    prLayoutNodeSetStyle(s.f.tree, f, s.f.style(left(80, 30), nullptr, nullptr, &font));
    s.layout();
    CHECK(s.left(word) == 80);
    CHECK(s.top(below) == 60);
    CHECK(prLayoutTreeStats(s.f.tree).blocksReused >= 1);
}

TEST_CASE("text added above a float moves the boxes below it with reuse") {
    Stage s;
    PrLayoutNode top = s.add(s.stage, block());
    PrLayoutNode first = s.word(top, 40);
    s.add(s.stage, left(60, 20));
    PrLayoutNode beside = s.add(s.stage, block());
    s.word(beside, 40);
    PrLayoutNode below = s.add(s.stage, margined(block(), 30, 0));
    s.word(below, 40);
    s.layout();
    double before = s.top(below);
    PrLayoutBoxStyle taller = prLayoutBoxStyleDefault();
    taller.display = PrLayoutDisplay_InlineBlock;
    taller.width = prLayoutLengthPx(40);
    taller.height = prLayoutLengthPx(30);
    PrLayoutFontStyle font = Stage::font0();
    prLayoutNodeSetStyle(s.f.tree, first, s.f.style(taller, nullptr, nullptr, &font));
    s.layout();
    CHECK(s.top(below) == before + 10);
    CHECK(prLayoutTreeStats(s.f.tree).blocksReused >= 1);
}

TEST_CASE("a float inserted before a reused paragraph shortens its lines") {
    Stage s;
    PrLayoutNode wrapper = s.add(s.stage, block());
    PrLayoutNode paragraph = s.add(wrapper, block());
    PrLayoutNode word = s.word(paragraph, 40);
    s.layout();
    CHECK(s.left(word) == 0);
    PrLayoutNode f = prLayoutNodeCreateElement(s.f.tree, kPrLayoutNodeNone, s.f.style(left(60, 30)));
    prLayoutTreeInsert(s.f.tree, s.stage, f, wrapper);
    s.layout();
    CHECK(s.left(word) == 60);
    prLayoutTreeErase(s.f.tree, f);
    s.layout();
    CHECK(s.left(word) == 0);
}

TEST_CASE("a float placed later beside the lower part of a tall line laid out with no floats moves the line") {
    // A block of no height whose content overflows it puts the float 50px down, past the top of the line after it.
    Stage s;
    PrLayoutNode overflowing = s.add(s.stage, sized(block(), -1, 0));
    s.add(overflowing, block(50));
    PrLayoutNode paragraph = s.add(s.stage, block());
    PrLayoutNode word = s.word(paragraph, 100, 100);
    s.layout();
    CHECK(s.top(word) == 0);
    PrLayoutNode f =
        prLayoutNodeCreateElement(s.f.tree, overflowing, s.f.style(left(150, 40), nullptr, nullptr, nullptr));
    s.layout();
    CHECK(s.top(f) == 50);
    CHECK(s.top(word) == 90);
    prLayoutTreeErase(s.f.tree, f);
    s.layout();
    CHECK(s.top(word) == 0);
}

TEST_CASE("a reused wrapper gives back its floats, which a cleared sibling clears") {
    Stage s;
    PrLayoutNode before = s.add(s.stage, block(5));
    PrLayoutNode wrapper = s.add(s.stage, block());
    s.add(wrapper, left(30, 20));
    s.add(wrapper, withSide(left(40, 0), PrRectSide_Top, 50));
    s.add(wrapper, left(30, 40));
    PrLayoutNode clear = s.add(s.stage, cleared(block(10), PrLayoutClear_Left));
    s.layout();
    double cleared0 = s.top(clear);
    // A change before the wrapper moves it, and its floats, without laying it out.
    PrLayoutFontStyle font = Stage::font0();
    prLayoutNodeSetStyle(s.f.tree, before, s.f.style(block(7), nullptr, nullptr, &font));
    s.layout();
    CHECK(s.top(clear) == cleared0 + 2);
    CHECK(prLayoutTreeStats(s.f.tree).blocksReused >= 1);
}

TEST_CASE("a float's own clear puts it below the floats it clears") {
    Stage s;
    s.add(s.stage, left(50, 30));
    PrLayoutNode next = s.add(s.stage, cleared(right(50, 10), PrLayoutClear_Left));
    s.layout();
    CHECK(s.top(next) == 30);
    CHECK(s.left(next) == 150);
}

TEST_CASE("a float or an inline-block of width stretch fills its containing block, as a block does") {
    Stage s;
    PrLayoutBoxStyle stretched = left(-1, 20);
    stretched.width = prLayoutLengthStretch();
    PrLayoutNode first = s.add(s.stage, stretched);
    s.add(s.stage, block(100));
    PrLayoutNode second = s.add(s.stage, stretched);
    PrLayoutBoxStyle inlineStretch = prLayoutBoxStyleDefault();
    inlineStretch.display = PrLayoutDisplay_InlineBlock;
    inlineStretch.width = prLayoutLengthStretch();
    inlineStretch.height = prLayoutLengthPx(10);
    inlineStretch.margin[PrRectSide_Left] = prLayoutLengthPx(10);
    PrLayoutNode line = s.add(s.stage, cleared(block(), PrLayoutClear_Both));
    PrLayoutNode inner = s.add(line, inlineStretch);
    s.layout();
    CHECK(s.width(first) == 200);
    CHECK(s.width(second) == 200);
    CHECK(s.top(second) == 100);
    CHECK(s.width(inner) == 190);
}
