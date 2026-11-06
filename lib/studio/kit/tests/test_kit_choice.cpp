#include "pr/draw_image.h"
#include "pr/kit_choice.h"
#include "pr/kit_image.h"

#include "tools/harness/kit_fixture.h"

#include "tools/harness/test.h"

using namespace harness;

namespace pr_impl {

namespace {

// What a test's tab shows: its label, a badge if `badge` is not null, and a close control if `closable` is true.
struct TabContent {
    const char* label;
    const char* PR_NULLABLE badge;
    bool closable;
};

constexpr TabContent kTabs[] = {{"Console", nullptr, false}, {"Output", nullptr, false}, {"Profiler", nullptr, false}};
constexpr PrKitChoice kItems[] = {{.label = "Local"}, {.label = "World"}, {.label = "Parent"}};

// Creates the tabs of `items` under `strip`, the tab at `*selected` selected, and fills each with its content. Writes
// each tab's node to `nodes`, and chooses a tab that is activated. Returns the index of the tab whose close control was
// activated, or `count` if none was.
uint32_t tabs(KitFixture& f, PrLayoutNode strip, const TabContent* items, uint32_t count, uint32_t* selected,
        PrLayoutNode* nodes) {
    uint32_t closed = count;
    for (uint32_t i = 0; i < count; ++i) {
        nodes[i] = prKitTab(f.kit, strip, {.selected = *selected == i});
        prKitTabLabel(f.kit, nodes[i], {.text = items[i].label});
        if (items[i].badge != nullptr)
            prKitBadge(f.kit, nodes[i], {.text = items[i].badge}, {});
        if (items[i].closable && prKitActivated(f.kit, prKitTabClose(f.kit, nodes[i])))
            closed = i;
        if (prKitActivated(f.kit, nodes[i]))
            *selected = i;
    }
    return closed;
}

// Creates the segments or radios of kItems under `strip`, the item at `*selected` selected, and chooses an item that is
// activated.
template <typename Item>
void choices(KitFixture& f, PrLayoutNode strip, uint32_t* selected, Item item) {
    for (uint32_t i = 0; i < 3; ++i) {
        PrKitChoice choice = kItems[i];
        choice.selected = *selected == i;
        if (prKitActivated(f.kit, item(f.kit, strip, choice)))
            *selected = i;
    }
}

// Returns the child of `node` at `index`, in tree order.
PrLayoutNode childOf(KitFixture& f, PrLayoutNode node, uint32_t index) {
    PrLayoutNode child = prLayoutNodeFirstChild(f.tree, node);
    for (uint32_t i = 0; i < index; ++i)
        child = prLayoutNodeNextSibling(f.tree, child);
    return child;
}

float widthOf(KitFixture& f, PrLayoutNode node) {
    PrRect r = f.bounds(node);
    return r.right - r.left;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("clicking a tab selects it") {
    KitFixture f;
    uint32_t selected = 0;
    PrLayoutNode nodes[3] = {};
    f.build = [&](PrLayoutNode root) { tabs(f, prKitTabStrip(f.kit, root, {}, {}), kTabs, 3, &selected, nodes); };
    f.frame();
    CHECK(widthOf(f, nodes[1]) > 20.0f);
    f.click(f.center(nodes[2]));
    f.frame();
    CHECK(selected == 2);
    CHECK(prUiFocus(f.ui) == nodes[2]);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a tab holds what its caller creates under it, in order") {
    KitFixture f;
    uint8_t pixels[16 * 16 * 4] = {};
    PrDrawImageDesc desc{
        .pixels = PrPixelView{pixels, 16 * 4, {16, 16}},
        .format = PrPixelFormat_Rgba8,
        .alpha = PrDrawAlpha_Straight,
        .space = PrColorSpace_Srgb,
    };
    const PrDrawImage* image = nullptr;
    REQUIRE(prDrawImageCreate(&desc, &image) == PrStatus_Ok);
    PrLayoutNode tab = kPrLayoutNodeNone;
    PrLayoutNode close = kPrLayoutNodeNone;
    f.build = [&](PrLayoutNode root) {
        tab = prKitTab(f.kit, prKitTabStrip(f.kit, root, {}, {}), {.selected = true});
        prKitImage(f.kit, tab, {.image = image}, {});
        prKitTabLabel(f.kit, tab, {.text = "Page"});
        close = prKitTabClose(f.kit, tab);
    };
    f.frame();
    PrLayoutNode first = childOf(f, tab, 0);
    CHECK(prLayoutNodeContent(f.tree, first).image == image);
    CHECK(f.bounds(childOf(f, tab, 1)).left > f.bounds(first).right);
    CHECK(prLayoutNodeLastChild(f.tree, tab) == close);
    CHECK(kitVerified(f.tree));
    prDrawImageRelease(image);
}

TEST_CASE("the arrow keys move a focused tab strip's focus and activate the tab they reach, stopping at the ends") {
    KitFixture f;
    uint32_t selected = 0;
    PrLayoutNode nodes[3] = {};
    f.build = [&](PrLayoutNode root) { tabs(f, prKitTabStrip(f.kit, root, {}, {}), kTabs, 3, &selected, nodes); };
    f.frame();
    // Keys reach nothing until a tab has focus.
    f.key(PrAppKey_Right);
    f.frame();
    CHECK(selected == 0);
    f.click(f.center(nodes[0]));
    f.frame();
    f.key(PrAppKey_Right);
    f.frame();
    CHECK(selected == 1);
    CHECK(prUiFocus(f.ui) == nodes[1]);
    f.key(PrAppKey_Right);
    f.frame();
    f.key(PrAppKey_Right);
    f.frame();
    CHECK(selected == 2);
    f.key(PrAppKey_Left);
    f.frame();
    CHECK(selected == 1);
    CHECK(prUiFocus(f.ui) == nodes[1]);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a tab's close control reports its click, and leaves the selection to its caller") {
    KitFixture f;
    uint32_t selected = 0;
    PrLayoutNode nodes[2] = {};
    uint32_t closes = 0;
    f.build = [&](PrLayoutNode root) {
        const TabContent items[] = {{"a.cpp", nullptr, true}, {"b.cpp", nullptr, true}};
        uint32_t closed = tabs(f, prKitTabStrip(f.kit, root, {}, {}), items, 2, &selected, nodes);
        CHECK(closed != 0);
        closes += closed == 1 ? 1 : 0;
    };
    f.frame();
    PrLayoutNode close = prLayoutNodeLastChild(f.tree, nodes[1]);
    f.click(f.center(close));
    f.frame();
    CHECK(closes == 1);
    CHECK(selected == 0);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a reorderable tab dragged past its neighbor reports the move on its strip") {
    KitFixture f;
    uint32_t selected = 0;
    PrLayoutNode nodes[3] = {};
    uint32_t moves = 0;
    uint32_t from = 0;
    uint32_t to = 0;
    f.build = [&](PrLayoutNode root) {
        PrLayoutNode strip = prKitTabStrip(f.kit, root, {.reorderable = true}, {});
        tabs(f, strip, kTabs, 3, &selected, nodes);
        moves += prKitMoved(f.kit, strip, &from, &to) ? 1 : 0;
    };
    f.frame();
    PrVec2 start = f.center(nodes[0]);
    PrRect last = f.bounds(nodes[2]);
    PrVec2 end{last.right - 2, start.y};
    f.push(PrAppEvent{.kind = PrAppEventKind_PointerDown, .position = start});
    f.frame();
    f.move(PrVec2{start.x + 10, start.y});
    f.frame();
    f.move(end);
    f.frame();
    f.push(PrAppEvent{.kind = PrAppEventKind_PointerUp, .position = end});
    f.frame();
    CHECK(moves == 1);
    CHECK(from == 0);
    CHECK(to == 2);
    // The release that drops a tab chooses nothing.
    CHECK(selected == 0);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a segment reports its click, and the arrow keys move the focus and activate the neighbor") {
    KitFixture f;
    uint32_t selected = 0;
    PrLayoutNode track = kPrLayoutNodeNone;
    f.build = [&](PrLayoutNode root) {
        PrLayoutNode row = prKitBox(f.kit, root, {}, {.direction = PrKitDirection_Row, .gap = 0});
        track = prKitSegmented(f.kit, row, {}, {.size = PrKitControlSize_Sm});
        choices(f, track, &selected, prKitSegment);
        PrKitPlace place{.width = prKitPx(240)};
        prKitPlace(f.kit, track, &place);
    };
    f.frame();
    CHECK(f.bounds(track).bottom - f.bounds(track).top == doctest::Approx(double(kPrKitHeightSm)));
    // The segments share the track's width.
    CHECK(widthOf(f, childOf(f, track, 0)) == doctest::Approx(double(widthOf(f, childOf(f, track, 2)))));
    f.click(f.center(childOf(f, track, 1)));
    f.frame();
    CHECK(selected == 1);
    f.key(PrAppKey_Right);
    f.frame();
    CHECK(selected == 2);
    f.key(PrAppKey_Left);
    f.frame();
    f.key(PrAppKey_Left);
    f.frame();
    CHECK(selected == 0);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a hovered segment fades to its hover fill, and keeps one record once it is there") {
    KitFixture f;
    uint32_t selected = 0;
    PrLayoutNode track = kPrLayoutNodeNone;
    f.build = [&](PrLayoutNode root) {
        track = prKitSegmented(f.kit, root, {}, {});
        choices(f, track, &selected, prKitSegment);
    };
    f.frame();
    PrLayoutNode segment = childOf(f, track, 1);
    PrLayoutStyleIndex rest = prLayoutNodeStyle(f.tree, segment);
    f.move(f.center(segment));
    f.frame(0.01);
    f.frame(0.02);
    CHECK(prLayoutNodeStyle(f.tree, segment) != rest);
    f.frame(0.5);
    f.frame(0.5);
    PrLayoutStyleIndex hovered = prLayoutNodeStyle(f.tree, segment);
    f.frame(0.5);
    CHECK(prLayoutNodeStyle(f.tree, segment) == hovered);
    CHECK_FALSE(prUiOutput(f.ui).wantsFrame);
    CHECK(prLayoutTreeStats(f.tree).blocksLaidOut == 0);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a checkbox flips its flag on a click of its label, and on Space while it holds focus") {
    KitFixture f;
    bool value = false;
    PrLayoutNode row = kPrLayoutNodeNone;
    f.build = [&](PrLayoutNode root) {
        row = prKitCheckbox(f.kit, root, {.value = &value, .label = "Cast shadows"}, {});
    };
    f.frame();
    PrLayoutNode box = prLayoutNodeFirstChild(f.tree, row);
    CHECK(widthOf(f, box) == 14.0f);
    PrRect label = f.bounds(prLayoutNodeLastChild(f.tree, row));
    f.click(PrVec2{label.right - 4, (label.top + label.bottom) / 2});
    f.frame();
    CHECK(value);
    // A checked box shows its mark.
    CHECK(prLayoutNodeFirstChild(f.tree, box) != kPrLayoutNodeNone);
    f.key(' ');
    f.frame();
    CHECK_FALSE(value);
    CHECK(prLayoutNodeFirstChild(f.tree, box) == kPrLayoutNodeNone);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a toggle flips its flag on a click and on Enter") {
    KitFixture f;
    bool value = false;
    PrLayoutNode row = kPrLayoutNodeNone;
    f.build = [&](PrLayoutNode root) {
        row = prKitToggle(f.kit, root, {.value = &value}, {});
    };
    f.frame();
    f.click(f.center(prLayoutNodeFirstChild(f.tree, row)));
    f.frame();
    CHECK(value);
    f.key(PrAppKey_Enter);
    f.frame();
    CHECK_FALSE(value);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a disabled checkbox and toggle take no click, no focus and no key") {
    KitFixture f;
    bool checked = false;
    bool on = true;
    PrLayoutNode box = kPrLayoutNodeNone;
    PrLayoutNode toggle = kPrLayoutNodeNone;
    f.build = [&](PrLayoutNode root) {
        box = prKitCheckbox(f.kit, root, {.value = &checked, .label = "Off", .disabled = true}, {});
        PrLayoutNode disabled = prKitBox(f.kit, root, {.disabled = true}, {});
        toggle = prKitToggle(f.kit, disabled, {.value = &on, .label = "On"}, {});
    };
    f.frame();
    f.click(f.center(box));
    f.frame();
    f.click(f.center(toggle));
    f.frame();
    f.key(' ');
    f.frame();
    CHECK_FALSE(checked);
    CHECK(on);
    CHECK(prUiFocus(f.ui) == kPrLayoutNodeNone);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a toggle's knob eases across its track, then keeps the record of its end, laying nothing out") {
    KitFixture f;
    bool value = false;
    PrLayoutNode row = kPrLayoutNodeNone;
    f.build = [&](PrLayoutNode root) {
        row = prKitToggle(f.kit, root, {.value = &value}, {});
    };
    f.frame();
    PrLayoutNode rail = prLayoutNodeFirstChild(f.tree, row);
    PrLayoutNode knob = prLayoutNodeFirstChild(f.tree, rail);
    float start = f.bounds(knob).left;
    CHECK(start == f.bounds(rail).left + 2);
    f.click(f.center(rail));
    f.frame(0.01);
    CHECK(value);
    f.frame(0.04);
    float middle = f.bounds(knob).left;
    CHECK(middle > start);
    CHECK(middle < start + 12);
    CHECK(prUiOutput(f.ui).wantsFrame);
    f.frame(0.5);
    f.frame(0.5);
    CHECK(f.bounds(knob).left == start + 12);
    PrLayoutStyleIndex settled = prLayoutNodeStyle(f.tree, knob);
    f.frame(0.5);
    CHECK(prLayoutNodeStyle(f.tree, knob) == settled);
    CHECK_FALSE(prUiOutput(f.ui).wantsFrame);
    PrLayoutTreeStats stats = prLayoutTreeStats(f.tree);
    CHECK(stats.blocksLaidOut == 0);
    CHECK(stats.frameNodesMade == 0);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a radio reports its click, and Up and Down move to its neighbors in a column") {
    KitFixture f;
    uint32_t selected = 0;
    PrLayoutNode group = kPrLayoutNodeNone;
    f.build = [&](PrLayoutNode root) {
        group = prKitRadioGroup(f.kit, root, {}, {});
        choices(f, group, &selected, prKitRadio);
    };
    f.frame();
    PrLayoutNode first = childOf(f, group, 0);
    PrLayoutNode third = childOf(f, group, 2);
    CHECK(f.bounds(third).top > f.bounds(first).bottom - 1);
    f.click(f.center(third));
    f.frame();
    CHECK(selected == 2);
    // Only the chosen ring holds a dot.
    CHECK(prLayoutNodeFirstChild(f.tree, prLayoutNodeFirstChild(f.tree, third)) != kPrLayoutNodeNone);
    CHECK(prLayoutNodeFirstChild(f.tree, prLayoutNodeFirstChild(f.tree, first)) == kPrLayoutNodeNone);
    f.key(PrAppKey_Up);
    f.frame();
    CHECK(selected == 1);
    f.key(PrAppKey_Down);
    f.frame();
    CHECK(selected == 2);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a progress bar's run covers its fraction of the track, clamped to the ends") {
    KitFixture f;
    float value = 0.25f;
    PrLayoutNode bar = kPrLayoutNodeNone;
    f.build = [&](PrLayoutNode root) {
        bar = prKitProgress(f.kit, root, {.value = value});
        PrKitPlace place{.width = prKitPx(200)};
        prKitPlace(f.kit, bar, &place);
    };
    f.frame();
    PrLayoutNode run = prLayoutNodeFirstChild(f.tree, bar);
    CHECK(f.bounds(bar).bottom - f.bounds(bar).top == 6.0f);
    CHECK(widthOf(f, run) == doctest::Approx(50.0));
    value = 0.75f;
    f.frame();
    CHECK(widthOf(f, run) == doctest::Approx(150.0));
    // A fraction change affects only compositing.
    CHECK(prLayoutTreeStats(f.tree).blocksLaidOut == 0);
    value = 2;
    f.frame();
    CHECK(widthOf(f, run) == doctest::Approx(200.0));
    value = -1;
    f.frame();
    CHECK(widthOf(f, run) == doctest::Approx(0.0));
    CHECK(kitVerified(f.tree));
}

TEST_CASE("an indeterminate bar sweeps its band and requests the frames that move it") {
    KitFixture f;
    PrLayoutNode bar = kPrLayoutNodeNone;
    f.build = [&](PrLayoutNode root) {
        bar = prKitProgress(f.kit, root, {.indeterminate = true});
    };
    f.frame();
    f.frame(0.3);
    PrLayoutNode band = prLayoutNodeFirstChild(f.tree, bar);
    float before = f.bounds(band).left;
    CHECK(prUiOutput(f.ui).wantsFrame);
    f.frame(0.2);
    CHECK(f.bounds(band).left > before);
    CHECK(widthOf(f, band) == doctest::Approx(0.3 * double(widthOf(f, bar))).epsilon(0.001));
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a meter lights the segments up to its reading, within its range") {
    KitFixture f;
    PrKitMeter meter{.value = 15, .low = 10, .high = 20};
    PrLayoutNode bar = kPrLayoutNodeNone;
    f.build = [&](PrLayoutNode root) { bar = prKitMeter(f.kit, root, meter, {.tone = PrKitTone_Success}); };
    f.frame();
    uint32_t segments = 0;
    for (PrLayoutNode n = prLayoutNodeFirstChild(f.tree, bar); n != kPrLayoutNodeNone;
            n = prLayoutNodeNextSibling(f.tree, n))
        ++segments;
    CHECK(segments == kPrKitMeterSegments);
    PrLayoutStyleIndex lit = prLayoutNodeStyle(f.tree, childOf(f, bar, 5));
    PrLayoutStyleIndex unlit = prLayoutNodeStyle(f.tree, childOf(f, bar, 6));
    CHECK(lit != unlit);
    CHECK(prLayoutNodeStyle(f.tree, childOf(f, bar, 0)) == lit);
    CHECK(prLayoutNodeStyle(f.tree, childOf(f, bar, 11)) == unlit);
    meter.value = 0;
    f.frame();
    CHECK(prLayoutNodeStyle(f.tree, childOf(f, bar, 0)) == unlit);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a spinner turns by kept steps, requesting frames, and paints nothing again") {
    KitFixture f;
    PrLayoutNode spinner = kPrLayoutNodeNone;
    f.build = [&](PrLayoutNode root) {
        PrLayoutNode row = prKitBox(f.kit, root, {}, {.direction = PrKitDirection_Row, .gap = 0});
        spinner = prKitSpinner(f.kit, row, {}, {.size = 16});
    };
    f.frame();
    CHECK(widthOf(f, spinner) == 16.0f);
    PrLayoutStyleIndex first = prLayoutNodeStyle(f.tree, spinner);
    CHECK(prUiOutput(f.ui).wantsFrame);
    f.frame(0.25);
    PrLayoutStyleIndex turned = prLayoutNodeStyle(f.tree, spinner);
    CHECK(turned != first);
    CHECK(prLayoutTreeStats(f.tree).blocksLaidOut == 0);
    // A whole turn later, the spinner is back on the same step's record.
    f.frame(1.0);
    CHECK(prLayoutNodeStyle(f.tree, spinner) == turned);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a spinner scrolled out of view requests no frames, and requests them again once it shows") {
    KitFixture f;
    PrLayoutNode view = kPrLayoutNodeNone;
    f.build = [&](PrLayoutNode root) {
        PrKitBoxStyle box{.overflow = PrKitOverflow_ScrollY};
        box.place.height = prKitPx(100);
        box.place.flex = PrKitFlex_Fixed;
        view = prKitBox(f.kit, root, {}, box);
        PrKitBoxStyle filler{};
        filler.place.height = prKitPx(400);
        filler.place.flex = PrKitFlex_Fixed;
        prKitBox(f.kit, view, {}, filler);
        prKitSpinner(f.kit, view, {}, {});
    };
    // The first round asks for the frame that lays the spinner out, which finds it below the scrollport.
    f.frame();
    f.frame();
    CHECK(!prUiOutput(f.ui).wantsFrame);
    prLayoutNodeSetScroll(f.tree, view, PrLayoutVec{prLayoutUnitFromPx(0), prLayoutUnitFromPx(400)});
    f.frame();
    CHECK(prUiOutput(f.ui).wantsFrame);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a frame of the same choices makes no node and lays nothing out") {
    KitFixture f;
    uint32_t tab = 1;
    uint32_t segment = 0;
    uint32_t radio = 2;
    bool checked = true;
    bool on = true;
    f.build = [&](PrLayoutNode root) {
        PrLayoutNode column = prKitBox(f.kit, root, {}, {.gap = kPrKitSpace4});
        const TabContent items[] = {{"Console", "3", false}, {"Output", nullptr, true}};
        PrLayoutNode nodes[2] = {};
        tabs(f, prKitTabStrip(f.kit, column, {.reorderable = true}, {}), items, 2, &tab, nodes);
        choices(f, prKitSegmented(f.kit, column, {}, {}), &segment, prKitSegment);
        choices(f, prKitRadioGroup(f.kit, column, {}, {.direction = PrKitDirection_Row}), &radio, prKitRadio);
        prKitCheckbox(f.kit, column, {.value = &checked, .label = "Receive"}, {});
        prKitToggle(f.kit, column, {.value = &on, .label = "Gravity"}, {});
        prKitProgress(f.kit, column, {.value = 0.4f});
        prKitMeter(f.kit, column, {.value = 0.6f}, {});
    };
    // The pointer rests away from every widget once the hover from the first round has faded.
    f.move(PrVec2{790, 590});
    f.frame();
    f.frame(1.0);
    f.frame();
    PrLayoutTreeStats stats = prLayoutTreeStats(f.tree);
    CHECK(stats.frameNodesMade == 0);
    CHECK(stats.blocksLaidOut == 0);
    CHECK_FALSE(prUiOutput(f.ui).wantsFrame);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a disabled strip takes no click and no key") {
    KitFixture f;
    uint32_t selected = 0;
    PrLayoutNode nodes[3] = {};
    PrLayoutNode track = kPrLayoutNodeNone;
    f.build = [&](PrLayoutNode root) {
        tabs(f, prKitTabStrip(f.kit, root, {.disabled = true}, {}), kTabs, 3, &selected, nodes);
        track = prKitSegmented(f.kit, root, {.disabled = true}, {});
        choices(f, track, &selected, prKitSegment);
    };
    f.frame();
    f.click(f.center(nodes[2]));
    f.frame();
    f.click(f.center(childOf(f, track, 1)));
    f.frame();
    f.key(PrAppKey_Right);
    f.frame();
    CHECK(selected == 0);
    CHECK(prUiFocus(f.ui) == kPrLayoutNodeNone);
    CHECK(kitVerified(f.tree));
}
