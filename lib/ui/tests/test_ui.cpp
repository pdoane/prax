#include "pr/layout_query.h"
#include "pr/mem.h"
#include "pr/ui.h"

#include "tools/harness/app_drag_window.h"
#include "ui/tests/ui_fixture.h"

#include "tools/harness/test.h"

extern "C" int prTestUiCApi(void);

namespace pr_impl {

namespace {

// The phases that a sink saw, by the control they were reported on.
struct Reports {
    struct Report {
        PrLayoutNode control;
        PrUiEditPhase phase;
    };
    Array<Report> seen;

    static void record(void* context, PrLayoutNode control, PrUiEditPhase phase) {
        static_cast<Reports*>(context)->seen.push(Report{.control = control, .phase = phase});
    }
};

// The owner of a recipe for a box of some width. It counts the groups that the recipe made and the lookups that the
// memo missed.
struct Maker {
    mutable uint32_t made = 0;
    uint32_t missed = 0;
};

// Writes a box whose width in px is the recipe's bytes, and counts the group on its owner.
void makeBox(const void* owner, const void* bytes, void* value) {
    ++static_cast<const Maker*>(owner)->made;
    static_cast<PrLayoutBoxStyle*>(value)->width = prLayoutLengthPx(*static_cast<const float*>(bytes));
}

// The identities of the recipe's type and of the part's.
const char kWidthType = 0;
const char kPartType = 0;

// Returns the record of a box `width` px wide that `owner` makes, from the memo if the last frame made it at this place
// in the round.
PrLayoutStyleIndex widthStyle(PrUi* ui, Maker& owner, float width) {
    PrLayoutStyleIndex style = 0;
    const void* kept = prUiMemoFind(ui, &kPartType, &owner, &style);
    if (kept != nullptr && prMemEquals(kept, &width, sizeof width))
        return style;
    PrUiPart part{.type = &kPartType, .owner = &owner, .bytes = &width, .size = uint32_t(sizeof width)};
    ++owner.missed;
    PrUiGroupRecipe recipe{
        .type = &kWidthType,
        .bytes = &width,
        .size = uint32_t(sizeof width),
        .kind = PrLayoutGroup_Box,
        .make = makeBox,
        .owner = &owner,
    };
    PrLayoutStyle record = prLayoutStyleDefault();
    record.groups[PrLayoutGroup_Box] = prUiGroup(ui, &recipe);
    return prUiMemoKeep(ui, &part, prLayoutStyleIntern(prUiTree(ui), &record), 0);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the public headers compile as C") {
    CHECK(prTestUiCApi() == 0);
}

TEST_CASE("a context's first round routes nothing, and the next routes what was queued") {
    UiFixture f;
    PrLayoutNode button = kPrLayoutNodeNone;
    bool clicked = false;
    f.build = [&] {
        button = f.box(f.root, 10, 10, 100, 40, PrUiFlag_Pressable);
        clicked = clicked || prUiClicked(f.ui, button, 0);
    };
    f.press(20, 20);
    // The first round lays out the tree. The second routes the press, the third routes the release, and the fourth
    // settles.
    CHECK(f.frame() == 4);
    CHECK(clicked);
    CHECK_FALSE(prUiOutput(f.ui).wantsFrame);
}

TEST_CASE("a press and its release are rounds of their own, and a round that settles follows the release") {
    UiFixture f;
    Array<String> seen;
    f.build = [&] {
        PrLayoutNode button = f.box(f.root, 10, 10, 100, 40, PrUiFlag_Pressable);
        if (prUiIsPressed(f.ui, button, 0))
            seen.push("pressed");
        if (prUiClicked(f.ui, button, 0))
            seen.push("clicked");
        if (prUiActivated(f.ui, button))
            seen.push("activated");
    };
    f.frame();
    f.press(20, 20);
    CHECK(f.frame() == 3);
    CHECK(seen == Array<String>({"pressed", "clicked", "activated"}));
}

TEST_CASE("a round that reads a node's extent runs again if its layout changes the extent, as far as the limit") {
    UiFixture f;
    float width = 100;
    Array<float> read;
    f.build = [&] {
        PrLayoutNode node = f.block(f.root, width, 40);
        PrVec2 extent{};
        bool measured = prUiExtent(f.ui, node, &extent);
        read.push(measured ? extent.x : -1);
    };
    // The first round reads no extent, and the round after it reads the one that the first laid out.
    CHECK(f.frame() == 2);
    CHECK(read == Array<float>({-1, 100}));
    // A frame whose layout keeps the extent runs one round.
    read.clear();
    CHECK(f.frame() == 1);
    width = 140;
    read.clear();
    CHECK(f.frame() == 2);
    CHECK(read == Array<float>({100, 140}));
    // A frame stops at its limit, whatever the extent.
    width = 180;
    CHECK(f.frame(1.0 / 60, 1) == 1);
}

TEST_CASE("a node with no interest passes the press to the pressable node around it") {
    UiFixture f;
    PrLayoutNode button = kPrLayoutNodeNone;
    PrLayoutNode label = kPrLayoutNodeNone;
    bool clicked = false;
    f.build = [&] {
        button = f.box(f.root, 10, 10, 200, 40, PrUiFlag_Pressable);
        label = f.box(button, 5, 5, 50, 20);
        clicked = clicked || prUiClicked(f.ui, button, 0);
    };
    f.frame();
    f.press(20, 20);
    f.frame();
    CHECK(clicked);
    CHECK(prUiIsHovered(f.ui, label));
    CHECK(prUiIsHovered(f.ui, button));
    CHECK(prUiIsHovered(f.ui, f.root));
}

TEST_CASE("flags added to a node join those that the round set, and a later round starts from none") {
    UiFixture f;
    PrLayoutNode node = kPrLayoutNodeNone;
    bool adding = true;
    PrUiFlags seen = 0;
    f.build = [&] {
        node = f.box(f.root, 10, 10, 200, 40, PrUiFlag_Pressable);
        if (adding)
            prUiAddFlags(f.ui, node, PrUiFlag_Focusable);
        seen = prUiFlags(f.ui, node);
    };
    f.frame();
    CHECK(seen == (PrUiFlag_Pressable | PrUiFlag_Focusable));
    adding = false;
    f.frame();
    CHECK(seen == PrUiFlag_Pressable);
}

TEST_CASE("the pointer hits what paints on top: a later sibling, and a box of a higher z-index") {
    UiFixture f;
    PrLayoutNode under = kPrLayoutNodeNone;
    PrLayoutNode over = kPrLayoutNodeNone;
    bool raised = false;
    PrLayoutNode clicked = kPrLayoutNodeNone;
    f.build = [&] {
        under = f.box(f.root, 0, 0, 100, 100, PrUiFlag_Pressable, 0, raised ? 5 : 0, raised);
        over = f.box(f.root, 50, 50, 100, 100, PrUiFlag_Pressable);
        const PrLayoutNode kNodes[] = {under, over};
        for (PrLayoutNode node : kNodes) {
            if (prUiClicked(f.ui, node, 0))
                clicked = node;
        }
    };
    f.frame();
    f.press(75, 75);
    f.frame();
    CHECK(clicked == over);
    raised = true;
    f.frame();
    f.press(75, 75);
    f.frame();
    CHECK(clicked == under);
}

TEST_CASE("a press goes to a node turned in perspective where it is drawn, and maps into its plane") {
    UiFixture f;
    PrLayoutNode stage = kPrLayoutNodeNone;
    PrLayoutNode card = kPrLayoutNodeNone;
    PrLayoutNode clicked = kPrLayoutNodeNone;
    // rotateY(60deg) of a 200 px card about its center, under a perspective of 200 px about the same center: the left
    // edge comes toward the eye and lands at x = 11.8, and the right edge goes away and lands at x = 134.9.
    PrLayoutTransformList list{};
    list.count = 1;
    list.functions[0].kind = PrLayoutTransformKind_Rotate;
    list.functions[0].values[1] = 1;
    list.functions[0].values[3] = 1.0471976f;
    auto styled = [&](float perspective, const PrLayoutTransform* transform) {
        PrLayoutBoxStyle box = prLayoutBoxStyleDefault();
        box.display = PrLayoutDisplay_Block;
        box.position = PrLayoutPosition_Absolute;
        box.width = prLayoutLengthPx(200);
        box.height = prLayoutLengthPx(200);
        box.transformed = 1;
        PrLayoutEffectStyle effects = prLayoutEffectStyleDefault();
        effects.perspective = perspective;
        if (transform != nullptr)
            effects.transform = *transform;
        PrLayoutStyle record = prLayoutStyleDefault();
        record.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(f.tree, &box);
        record.groups[PrLayoutGroup_Effect] = prLayoutEffectStyleIntern(f.tree, &effects);
        return prLayoutStyleIntern(f.tree, &record);
    };
    PrLayoutTransform turn = prLayoutTransformListTransform(&list);
    f.build = [&] {
        stage = prUiElement(f.ui, f.root, 0, styled(200, nullptr), PrUiFlag_Pressable);
        card = prUiElement(f.ui, stage, 0, styled(0, &turn), PrUiFlag_Pressable);
        const PrLayoutNode kNodes[] = {stage, card};
        for (PrLayoutNode node : kNodes) {
            if (prUiClicked(f.ui, node, 0))
                clicked = node;
        }
    };
    f.frame();
    f.press(20, 100);
    f.frame();
    CHECK(clicked == card);
    // Without the perspective the far edge would reach x = 150.
    f.press(145, 100);
    f.frame();
    CHECK(clicked == stage);
    // The point at x = 20 lands on the card 5.5 px from its left edge, on its middle line.
    PrVec2 local{};
    REQUIRE(prUiLocalPoint(f.ui, card, PrVec2{20, 100}, &local));
    CHECK(double(local.x) == doctest::Approx(5.483).epsilon(0.01));
    CHECK(double(local.y) == doctest::Approx(100).epsilon(0.01));
}

TEST_CASE("a transparent node lets the pointer through, and a subtree transparent one its whole subtree") {
    UiFixture f;
    PrLayoutNode under = kPrLayoutNodeNone;
    PrUiFlags overlay = PrUiFlag_Transparent;
    bool clicked = false;
    f.build = [&] {
        under = f.box(f.root, 0, 0, 200, 200, PrUiFlag_Pressable);
        PrLayoutNode cover = f.box(f.root, 0, 0, 200, 200, overlay);
        f.box(cover, 0, 0, 100, 100);
        clicked = prUiClicked(f.ui, under, 0) || clicked;
    };
    f.frame();
    f.press(150, 150);
    f.frame();
    CHECK(clicked);
    // The cover's child still hides the node under it.
    clicked = false;
    f.press(50, 50);
    f.frame();
    CHECK_FALSE(clicked);
    overlay = PrUiFlag_SubtreeTransparent;
    f.frame();
    f.press(50, 50);
    f.frame();
    CHECK(clicked);
}

TEST_CASE("a press keeps the pointer captured outside its node, and a release outside clicks nothing") {
    UiFixture f;
    PrLayoutNode button = kPrLayoutNodeNone;
    bool clicked = false;
    PrUiDrag drag{};
    bool dragging = false;
    f.build = [&] {
        button = f.box(f.root, 10, 10, 100, 40, PrUiFlag_Pressable);
        clicked = clicked || prUiClicked(f.ui, button, 0);
        dragging = prUiDrag(f.ui, button, 0, 5, &drag);
    };
    f.frame();
    f.down(20, 20);
    f.frame();
    CHECK(prUiIsPressed(f.ui, button, 0));
    f.move(22, 21);
    f.frame();
    CHECK(dragging);
    CHECK_FALSE(drag.moved);
    CHECK(drag.delta.x == 2.0f);
    f.move(300, 300);
    f.frame();
    CHECK(prUiIsPressed(f.ui, button, 0));
    CHECK(drag.moved);
    CHECK(drag.position.x == 300.0f);
    f.up(300, 300);
    f.frame(1.0 / 60, 1);
    CHECK(dragging);
    CHECK(drag.released);
    CHECK_FALSE(clicked);
    CHECK_FALSE(prUiIsPressed(f.ui, button, 0));
}

TEST_CASE("a captured node that leaves the tree, or is disabled, releases the capture") {
    UiFixture f;
    bool shown = true;
    PrUiFlags flags = PrUiFlag_Pressable;
    PrLayoutNode button = kPrLayoutNodeNone;
    f.build = [&] {
        if (shown)
            button = f.box(f.root, 10, 10, 100, 40, flags);
    };
    f.frame();
    f.down(20, 20);
    f.frame();
    CHECK(prUiIsPressed(f.ui, button, 0));
    flags = PrUiFlag_Pressable | PrUiFlag_Disabled;
    f.frame();
    CHECK_FALSE(prUiIsPressed(f.ui, button, 0));
    f.up(20, 20);
    flags = PrUiFlag_Pressable;
    f.frame();
    f.down(20, 20);
    f.frame();
    CHECK(prUiIsPressed(f.ui, button, 0));
    shown = false;
    f.frame();
    CHECK_FALSE(prUiIsPressed(f.ui, button, 0));
}

TEST_CASE("a disabled container disables what is created under it, through nodes that take no input") {
    UiFixture f;
    bool disabled = true;
    bool activated = false;
    bool seenDisabled = false;
    PrLayoutNode button = kPrLayoutNodeNone;
    f.build = [&] {
        PrLayoutNode panel = f.box(f.root, 0, 0, 200, 100, disabled ? PrUiFlag_Disabled : PrUiFlags(0));
        PrLayoutNode row = f.box(panel, 0, 0, 200, 50);
        button = f.box(row, 10, 10, 100, 30, PrUiFlag_Pressable | PrUiFlag_Focusable);
        activated = activated || prUiActivated(f.ui, button);
        seenDisabled = prUiIsDisabled(f.ui, button);
    };
    f.frame();
    CHECK(seenDisabled);
    CHECK((prUiFlags(f.ui, button) & PrUiFlag_Disabled) != 0);
    f.press(20, 20);
    f.frame();
    CHECK_FALSE(activated);
    CHECK(prUiFocus(f.ui) == kPrLayoutNodeNone);
    disabled = false;
    f.frame();
    CHECK_FALSE(seenDisabled);
    f.press(20, 20);
    f.frame();
    CHECK(activated);
}

TEST_CASE("a node finds the context of the nearest node it was created under, by owner and key") {
    UiFixture f;
    static const char owner = 0;
    static const char other = 0;
    int nearest = 0;
    int outer = 0;
    PrLayoutNode found = kPrLayoutNodeNone;
    bool missing = false;
    f.build = [&] {
        PrLayoutNode strip = f.box(f.root, 0, 0, 200, 100);
        *static_cast<int*>(prUiProvide(f.ui, strip, &owner, 1, sizeof(int))) = 7;
        *static_cast<int*>(prUiProvide(f.ui, strip, &owner, 2, sizeof(int))) = 9;
        PrLayoutNode group = f.box(strip, 0, 0, 200, 50);
        *static_cast<int*>(prUiProvide(f.ui, group, &owner, 1, sizeof(int))) = 3;
        PrLayoutNode item = f.box(group, 0, 0, 50, 50);
        nearest = *static_cast<int*>(prUiContext(f.ui, item, &owner, 1, &found));
        outer = *static_cast<int*>(prUiContext(f.ui, item, &owner, 2, nullptr));
        missing = prUiContext(f.ui, item, &other, 1, nullptr) == nullptr;
    };
    f.frame();
    CHECK(nearest == 3);
    CHECK(outer == 9);
    CHECK(missing);
    CHECK(found != kPrLayoutNodeNone);
}

TEST_CASE("a signal is asked of the node it was given to, under its owner, in the round that gave it") {
    UiFixture f;
    static const char owner = 0;
    static const char other = 0;
    bool signal = true;
    PrLayoutNode node = kPrLayoutNodeNone;
    bool seen = false;
    bool seenOther = false;
    f.build = [&] {
        node = f.box(f.root, 0, 0, 50, 50);
        if (signal)
            prUiSignal(f.ui, node, &owner, 1u << 2);
        seen = prUiSignaled(f.ui, node, &owner, 1u << 2);
        seenOther = prUiSignaled(f.ui, node, &other, 1u << 2) || prUiSignaled(f.ui, node, &owner, 1u << 1);
    };
    f.frame();
    CHECK(seen);
    CHECK_FALSE(seenOther);
    signal = false;
    f.frame();
    CHECK_FALSE(seen);
}

TEST_CASE("a click reports the modifiers its press was made with") {
    UiFixture f;
    PrAppModifierFlags seen = 0;
    f.build = [&] {
        PrLayoutNode node = f.box(f.root, 0, 0, 50, 50, PrUiFlag_Pressable);
        if (prUiClicked(f.ui, node, 0))
            seen = prUiPressModifiers(f.ui, 0);
    };
    f.frame();
    f.push(PrAppEvent{
        .kind = PrAppEventKind_PointerDown,
        .modifiers = PrAppModifierFlag_Shift,
        .pointer = {.position = PrVec2{10, 10}},
    });
    f.push(PrAppEvent{
        .kind = PrAppEventKind_PointerUp,
        .modifiers = PrAppModifierFlag_Shift,
        .pointer = {.position = PrVec2{10, 10}},
    });
    f.frame();
    CHECK(seen == PrAppModifierFlag_Shift);
}

TEST_CASE("a multi-click that the application reports counts on the node that it began on") {
    UiFixture f;
    f.build = [&] {
        f.box(f.root, 0, 0, 50, 50, PrUiFlag_Pressable);
        f.box(f.root, 100, 0, 50, 50, PrUiFlag_Pressable);
    };
    f.frame();
    f.press(10, 10);
    f.frame();
    CHECK(prUiPressCount(f.ui, 0) == 1);
    f.press(11, 10, 0, 2);
    f.frame();
    CHECK(prUiPressCount(f.ui, 0) == 2);
    f.press(11, 11, 0, 3);
    f.frame();
    CHECK(prUiPressCount(f.ui, 0) == 3);
    // Another node starts a new sequence, whatever the application counted.
    f.press(110, 10, 0, 4);
    f.frame();
    CHECK(prUiPressCount(f.ui, 0) == 1);
}

TEST_CASE("a press outside a node is reported to it, over another node or none") {
    UiFixture f;
    PrLayoutNode menu = kPrLayoutNodeNone;
    bool outside = false;
    f.build = [&] {
        menu = f.box(f.root, 10, 10, 100, 100, PrUiFlag_Pressable);
        f.box(menu, 10, 10, 20, 20, PrUiFlag_Pressable);
        f.box(f.root, 300, 300, 50, 50, PrUiFlag_Pressable);
        outside = outside || prUiPressedOutside(f.ui, menu, 0);
    };
    f.frame();
    f.press(25, 25);
    f.frame();
    CHECK_FALSE(outside);
    f.press(310, 310);
    f.frame();
    CHECK(outside);
}

TEST_CASE("hover covers a node's descendants and counts the seconds it lasts") {
    UiFixture f;
    PrLayoutNode panel = kPrLayoutNodeNone;
    PrLayoutNode child = kPrLayoutNodeNone;
    f.build = [&] {
        panel = f.box(f.root, 0, 0, 400, 400);
        child = f.box(panel, 10, 10, 50, 50);
    };
    f.frame();
    f.move(100, 100);
    f.frame(0.5);
    CHECK(prUiIsHovered(f.ui, panel));
    CHECK_FALSE(prUiIsHovered(f.ui, child));
    f.frame(0.5);
    CHECK(prUiHoverSeconds(f.ui, panel) == doctest::Approx(0.5));
    // Moving onto the child keeps the panel hovered.
    f.move(20, 20);
    f.frame(0.5);
    CHECK(prUiIsHovered(f.ui, child));
    CHECK(prUiHoverSeconds(f.ui, panel) == doctest::Approx(1.0));
    CHECK(prUiHoverSeconds(f.ui, child) == doctest::Approx(0.0));
}

TEST_CASE("the cursor is the request nearest the pointer's node") {
    UiFixture f;
    f.build = [&] {
        PrLayoutNode panel = f.box(f.root, 0, 0, 400, 400);
        prUiSetCursor(f.ui, panel, PrAppCursor_Crosshair);
        PrLayoutNode field = f.box(panel, 10, 10, 50, 50);
        prUiSetCursor(f.ui, field, PrAppCursor_Text);
        f.box(panel, 100, 100, 50, 50);
    };
    f.frame();
    f.move(20, 20);
    f.frame();
    CHECK(prUiOutput(f.ui).cursor == PrAppCursor_Text);
    f.move(120, 120);
    f.frame();
    CHECK(prUiOutput(f.ui).cursor == PrAppCursor_Crosshair);
    f.move(600, 500);
    f.frame();
    CHECK(prUiOutput(f.ui).cursor == PrAppCursor_Arrow);
}

TEST_CASE("a primary press focuses the innermost focusable node, and Tab steps through the tree's order") {
    UiFixture f;
    Array<PrLayoutNode> fields;
    bool disableSecond = false;
    f.build = [&] {
        fields.clear();
        for (uint32_t i = 0; i < 3; ++i) {
            PrUiFlags flags = PrUiFlag_Focusable | PrUiFlag_Pressable;
            if (i == 1 && disableSecond)
                flags |= PrUiFlag_Disabled;
            fields.push(f.box(f.root, 10, 10 + 50 * float(i), 100, 40, flags));
        }
    };
    f.frame();
    f.press(20, 20);
    f.frame();
    CHECK(prUiFocus(f.ui) == fields[0]);
    CHECK_FALSE(prUiIsFocusVisible(f.ui));
    // A modifier key alone does not show the focus.
    f.key(PrAppKey_Shift, PrAppModifierFlag_Shift);
    f.frame();
    CHECK_FALSE(prUiIsFocusVisible(f.ui));
    f.key(PrAppKey_Tab);
    f.frame();
    CHECK(prUiFocus(f.ui) == fields[1]);
    CHECK(prUiIsFocusVisible(f.ui));
    f.key(PrAppKey_Tab);
    f.key(PrAppKey_Tab);
    f.frame();
    CHECK(prUiFocus(f.ui) == fields[0]);
    f.key(PrAppKey_Tab, PrAppModifierFlag_Shift);
    f.frame();
    CHECK(prUiFocus(f.ui) == fields[2]);
    disableSecond = true;
    f.frame();
    f.key(PrAppKey_Tab);
    f.key(PrAppKey_Tab);
    f.frame();
    CHECK(prUiFocus(f.ui) == fields[2]);
    // A press over nothing focusable clears focus.
    f.press(500, 500);
    f.frame();
    CHECK(prUiFocus(f.ui) == kPrLayoutNodeNone);
}

TEST_CASE("traversal wraps within the innermost focus scope") {
    UiFixture f;
    PrLayoutNode outer = kPrLayoutNodeNone;
    Array<PrLayoutNode> inside;
    f.build = [&] {
        outer = f.box(f.root, 0, 0, 50, 50, PrUiFlag_Focusable | PrUiFlag_Pressable);
        PrLayoutNode dialog = f.box(f.root, 100, 100, 300, 300, PrUiFlag_FocusScope);
        inside.clear();
        inside.push(f.box(dialog, 10, 10, 50, 50, PrUiFlag_Focusable | PrUiFlag_Pressable));
        inside.push(f.box(dialog, 10, 100, 50, 50, PrUiFlag_Focusable | PrUiFlag_Pressable));
    };
    f.frame();
    f.press(120, 120);
    f.frame();
    CHECK(prUiFocus(f.ui) == inside[0]);
    f.key(PrAppKey_Tab);
    f.key(PrAppKey_Tab);
    f.frame();
    CHECK(prUiFocus(f.ui) == inside[0]);
}

TEST_CASE("a key press goes to the nearest claim on the focused node's ancestors, and to the root's with no focus") {
    UiFixture f;
    PrLayoutNode panel = kPrLayoutNodeNone;
    PrLayoutNode field = kPrLayoutNodeNone;
    Array<String> seen;
    f.build = [&] {
        panel = f.box(f.root, 0, 0, 400, 400);
        field = f.box(panel, 10, 10, 100, 40, PrUiFlag_Focusable | PrUiFlag_Pressable);
        if (prUiKeyPressed(f.ui, field, PrAppKey_Escape, 0))
            seen.push("field escape");
        if (prUiKeyPressed(f.ui, panel, PrAppKey_Escape, 0))
            seen.push("panel escape");
        if (prUiKeyPressed(f.ui, panel, 'F', PrAppModifierFlag_Super))
            seen.push("panel find");
        if (prUiKeyPressed(f.ui, f.root, 'S', PrAppModifierFlag_Super))
            seen.push("root save");
    };
    f.frame();
    f.key('S', PrAppModifierFlag_Super);
    f.frame();
    CHECK(seen == Array<String>({"root save"}));
    f.press(20, 20);
    f.key(PrAppKey_Escape);
    f.key('F', PrAppModifierFlag_Super);
    f.key('S', PrAppModifierFlag_Super);
    f.key('F', PrAppModifierFlag_Super | PrAppModifierFlag_Shift);
    f.frame(1.0 / 60, 16);
    CHECK(seen == Array<String>({"root save", "field escape", "panel find", "root save"}));
}

TEST_CASE("caps lock takes no part in routing a key press to claims, focus traversal and activation") {
    UiFixture f;
    PrLayoutNode first = kPrLayoutNodeNone;
    PrLayoutNode second = kPrLayoutNodeNone;
    Array<String> seen;
    f.build = [&] {
        first = f.box(f.root, 10, 10, 100, 40, PrUiFlag_Focusable | PrUiFlag_Pressable);
        second = f.box(f.root, 10, 60, 100, 40, PrUiFlag_Focusable | PrUiFlag_Pressable);
        if (prUiKeyPressed(f.ui, f.root, PrAppKey_Enter, PrAppModifierFlag_Shift))
            seen.push("shift enter");
        if (prUiActivated(f.ui, second))
            seen.push("second activated");
    };
    f.frame();
    f.press(20, 20);
    f.frame();
    f.key(PrAppKey_Enter, PrAppModifierFlag_Shift | PrAppModifierFlag_CapsLock);
    f.key(PrAppKey_Tab, PrAppModifierFlag_CapsLock);
    f.frame();
    CHECK(prUiFocus(f.ui) == second);
    f.key(PrAppKey_Enter, PrAppModifierFlag_CapsLock);
    f.frame();
    CHECK(seen == Array<String>({"shift enter", "second activated"}));
}

TEST_CASE("a captured key goes to the outermost capture on the focus path, ahead of every claim") {
    UiFixture f;
    PrLayoutNode panel = kPrLayoutNodeNone;
    PrLayoutNode field = kPrLayoutNodeNone;
    Array<String> seen;
    f.build = [&] {
        panel = f.box(f.root, 0, 0, 400, 400);
        field = f.box(panel, 10, 10, 100, 40, PrUiFlag_Focusable | PrUiFlag_Pressable);
        if (prUiKeyPressed(f.ui, field, PrAppKey_Backspace, PrAppModifierFlag_Super))
            seen.push("field claim");
        if (prUiKeyCaptured(f.ui, panel, PrAppKey_Backspace, PrAppModifierFlag_Super))
            seen.push("panel capture");
        if (prUiKeyCaptured(f.ui, f.root, PrAppKey_Backspace, PrAppModifierFlag_Super))
            seen.push("root capture");
        if (prUiKeyCaptured(f.ui, panel, PrAppKey_Enter, PrAppModifierFlag_Super))
            seen.push("panel enter");
        if (prUiKeyPressed(f.ui, panel, PrAppKey_Enter, PrAppModifierFlag_Super))
            seen.push("panel enter claim");
    };
    f.frame();
    f.press(20, 20);
    f.key(PrAppKey_Backspace, PrAppModifierFlag_Super);
    f.key(PrAppKey_Enter, PrAppModifierFlag_Super);
    f.frame(1.0 / 60, 8);
    CHECK(seen == Array<String>({"root capture", "panel enter"}));
    // With no focus, only the root's captures and claims are on the path.
    seen.clear();
    f.press(390, 390);
    f.key(PrAppKey_Enter, PrAppModifierFlag_Super);
    f.key(PrAppKey_Backspace, PrAppModifierFlag_Super);
    f.frame(1.0 / 60, 8);
    CHECK(seen == Array<String>({"root capture"}));
}

TEST_CASE("Enter and Space activate the focused pressable node, and text goes to the focused node") {
    UiFixture f;
    PrLayoutNode button = kPrLayoutNodeNone;
    uint32_t activations = 0;
    String typed;
    f.build = [&] {
        button = f.box(f.root, 10, 10, 100, 40, PrUiFlag_Focusable | PrUiFlag_Pressable);
        activations += prUiActivated(f.ui, button) ? 1 : 0;
        PrUiEvents events = prUiEvents(f.ui, button);
        for (uint32_t i = 0; i < events.count; ++i) {
            if (events.events[i].kind == PrAppEventKind_Text)
                typed.append(events.events[i].input.text);
        }
    };
    f.frame();
    f.press(20, 20);
    f.frame();
    CHECK(activations == 1);
    f.key(PrAppKey_Enter);
    f.key(' ');
    f.text("hé");
    f.text("llo");
    f.frame(1.0 / 60, 8);
    CHECK(activations == 3);
    CHECK(typed == "héllo");
}

TEST_CASE("an activation marked in one round is seen in the next") {
    UiFixture f;
    PrLayoutNode node = kPrLayoutNodeNone;
    bool mark = false;
    uint32_t seen = 0;
    f.build = [&] {
        node = f.box(f.root, 0, 0, 10, 10, PrUiFlag_Pressable);
        seen += prUiActivated(f.ui, node) ? 1 : 0;
        if (mark) {
            prUiActivate(f.ui, node);
            mark = false;
        }
    };
    f.frame();
    mark = true;
    f.frame(1.0 / 60, 1);
    CHECK(seen == 0);
    CHECK(prUiOutput(f.ui).wantsFrame);
    f.frame(1.0 / 60, 1);
    CHECK(seen == 1);
    CHECK_FALSE(prUiOutput(f.ui).wantsFrame);
}

TEST_CASE("the wheel scrolls the innermost scroll container, and what it cannot take passes outward") {
    UiFixture f;
    PrLayoutNode outer = kPrLayoutNodeNone;
    PrLayoutNode inner = kPrLayoutNodeNone;
    f.build = [&] {
        PrLayoutNode outerContent = kPrLayoutNodeNone;
        outer = f.scroller(f.root, 0, 0, 400, 400, 400, 1000, &outerContent);
        inner = f.scroller(outerContent, 10, 10, 200, 200, 200, 300);
    };
    f.frame();
    f.wheel(50, 50, 0, 60);
    f.frame();
    CHECK(prLayoutNodeScroll(f.tree, inner).y.raw == prLayoutUnitFromPx(60).raw);
    CHECK(prLayoutNodeScroll(f.tree, outer).y.raw == 0);
    // The inner container has 40 px left to scroll, and the outer container takes the other 60 px.
    f.wheel(50, 50, 0, 100);
    f.frame();
    CHECK(prLayoutNodeScroll(f.tree, inner).y.raw == prLayoutUnitFromPx(100).raw);
    CHECK(prLayoutNodeScroll(f.tree, outer).y.raw == prLayoutUnitFromPx(60).raw);
    // A scroll records nothing and lays out nothing.
    CHECK(prLayoutTreeStats(f.tree).blocksLaidOut == 0);
}

TEST_CASE("a wheel that turns by lines scrolls by lines of the default line height") {
    UiFixture f;
    PrLayoutNode scroller = kPrLayoutNodeNone;
    f.build = [&] { scroller = f.scroller(f.root, 0, 0, 400, 400, 400, 1000); };
    f.frame();
    f.push(PrAppEvent{
        .kind = PrAppEventKind_Scroll,
        .scroll = {.position = PrVec2{50, 50}, .delta = PrVec2{0, 2}, .unit = PrAppScrollUnit_Line},
    });
    f.frame();
    CHECK(prLayoutNodeScroll(f.tree, scroller).y.raw == prLayoutUnitFromPx(2 * kPrAppScrollLineHeight).raw);
}

TEST_CASE("a wheel node takes the deltas itself") {
    UiFixture f;
    PrVec2 panned{};
    f.build = [&] {
        PrLayoutNode canvas = f.box(f.root, 0, 0, 400, 400, PrUiFlag_Wheel);
        PrVec2 delta{};
        if (prUiWheel(f.ui, canvas, &delta)) {
            panned.x += delta.x;
            panned.y += delta.y;
        }
    };
    f.frame();
    f.wheel(50, 50, 3, 4);
    f.wheel(50, 50, 1, 1);
    f.frame();
    CHECK(panned.x == 4.0f);
    CHECK(panned.y == 5.0f);
}

TEST_CASE("state is kept while every round asks for it, by owner and key, and moves with its node") {
    UiFixture f;
    static const char kitOwner = 0;
    static const char appOwner = 0;
    Array<uint64_t> keys({1, 2});
    bool ask = true;
    Array<int*> counters;
    f.build = [&] {
        PrLayoutNode list = f.box(f.root, 0, 0, 400, 400);
        counters.clear();
        for (uint64_t key : keys) {
            PrLayoutNode row = f.box(list, 0, 0, 10, 10, 0, prUiKeyIndex(f.ui, list, key));
            if (!ask)
                continue;
            int* kit = static_cast<int*>(prUiState(f.ui, row, &kitOwner, 7, sizeof(int)));
            int* app = static_cast<int*>(prUiState(f.ui, row, &appOwner, 7, sizeof(int)));
            *kit += 1;
            *app += 10;
            counters.push(kit);
        }
    };
    f.frame();
    f.frame();
    CHECK(*counters[0] == 2);
    CHECK(*counters[1] == 2);
    keys = Array<uint64_t>({2, 1});
    f.frame();
    CHECK(*counters[0] == 3);
    CHECK(*counters[1] == 3);
    ask = false;
    f.frame();
    ask = true;
    f.frame();
    CHECK(*counters[0] == 1);
}

TEST_CASE("retained state with a destroy function is destroyed when no round asks for it, and with the context") {
    static int destroyed;
    destroyed = 0;
    PrUiStateDestroyFn destroy = [](void* state) { destroyed += *static_cast<int*>(state); };
    static const char owner = 0;
    {
        UiFixture f;
        bool ask = true;
        PrLayoutNode box = kPrLayoutNodeNone;
        f.build = [&] {
            box = f.box(f.root, 0, 0, 10, 10);
            if (ask)
                *static_cast<int*>(prUiStateWithDestroy(f.ui, box, &owner, 0, sizeof(int), destroy)) = 1;
        };
        f.frame();
        CHECK(destroyed == 0);
        ask = false;
        f.frame();
        CHECK(destroyed == 1);
        ask = true;
        f.frame();
        CHECK(destroyed == 1);
    }
    CHECK(destroyed == 2);
}

TEST_CASE("a tween moves to its target over its duration and asks for frames while it moves") {
    UiFixture f;
    static const char owner = 0;
    float target = 0;
    float value = -1;
    f.build = [&] {
        PrLayoutNode node = f.box(f.root, 0, 0, 10, 10);
        value = prUiTween(f.ui, node, &owner, 1, target, 1.0, nullptr);
    };
    f.frame();
    CHECK(value == 0.0f);
    target = 10;
    f.frame(0.25);
    CHECK(value == doctest::Approx(0.0));
    CHECK(prUiOutput(f.ui).wantsFrame);
    f.frame(0.5);
    CHECK(value == doctest::Approx(5.0));
    f.frame(0.6);
    CHECK(value == 10.0f);
    CHECK_FALSE(prUiOutput(f.ui).wantsFrame);
}

TEST_CASE("a held press repeats after its delay, at its interval") {
    UiFixture f;
    uint32_t repeats = 0;
    f.build = [&] {
        PrLayoutNode node = f.box(f.root, 0, 0, 50, 50, PrUiFlag_Pressable);
        repeats += prUiPressRepeats(f.ui, node, 0, 0.5, 0.1);
    };
    f.frame();
    f.down(10, 10);
    f.frame();
    CHECK(repeats == 0);
    CHECK(prUiOutput(f.ui).wantsFrame);
    f.frame(0.45);
    CHECK(repeats == 0);
    f.frame(0.3);
    CHECK(repeats == 3);
}

TEST_CASE("a payload dragged from its source drops on the innermost target under the release") {
    UiFixture f;
    PrLayoutNode source = kPrLayoutNodeNone;
    PrLayoutNode target = kPrLayoutNodeNone;
    bool over = false;
    const PrUiPayload* dropped = nullptr;
    uint64_t droppedValue = 0;
    bool sourceClicked = false;
    bool draggedAtDrop = false;
    f.build = [&] {
        source = f.box(f.root, 0, 0, 50, 50, PrUiFlag_Pressable);
        if (prUiIsPressed(f.ui, source, 0))
            prUiDragStart(f.ui, source, 4, 1, 42, 0);
        sourceClicked = sourceClicked || prUiClicked(f.ui, source, 0);
        PrLayoutNode outer = f.box(f.root, 200, 200, 200, 200);
        prUiIsDropTarget(f.ui, outer, 1);
        target = f.box(outer, 10, 10, 50, 50);
        over = prUiIsDropTarget(f.ui, target, 1);
        dropped = prUiDropped(f.ui, target);
        if (dropped != nullptr) {
            droppedValue = dropped->value;
            // The round that drops the payload still reports it, so the target that takes it is still built.
            draggedAtDrop = prUiDragging(f.ui) != nullptr && prUiDragging(f.ui)->value == 42;
        }
    };
    f.frame();
    f.down(10, 10);
    f.frame();
    CHECK(prUiDragging(f.ui) == nullptr);
    f.move(220, 220);
    f.frame();
    REQUIRE(prUiDragging(f.ui) != nullptr);
    CHECK(prUiDragging(f.ui)->kind == 1);
    f.frame();
    CHECK(over);
    f.up(220, 220);
    f.frame();
    CHECK(droppedValue == 42);
    CHECK(draggedAtDrop);
    CHECK_FALSE(sourceClicked);
    CHECK(prUiDragging(f.ui) == nullptr);
}

TEST_CASE("Escape cancels a payload in flight") {
    UiFixture f;
    f.build = [&] {
        PrLayoutNode source = f.box(f.root, 0, 0, 50, 50, PrUiFlag_Pressable);
        if (prUiIsPressed(f.ui, source, 0))
            prUiDragStart(f.ui, source, 0, 1, 1, 0);
    };
    f.frame();
    f.down(10, 10);
    f.move(100, 100);
    f.frame();
    REQUIRE(prUiDragging(f.ui) != nullptr);
    f.key(PrAppKey_Escape);
    f.frame();
    CHECK(prUiDragging(f.ui) == nullptr);
}

TEST_CASE("a target takes only payloads of its kind, and a payload of another kind reaches the target around it") {
    UiFixture f;
    bool innerOver = false;
    bool outerOver = false;
    const PrUiPayload* dropped = nullptr;
    f.build = [&] {
        PrLayoutNode source = f.box(f.root, 0, 0, 50, 50, PrUiFlag_Pressable);
        if (prUiIsPressed(f.ui, source, 0))
            prUiDragStart(f.ui, source, 0, 2, 7, 0);
        PrLayoutNode outer = f.box(f.root, 200, 200, 200, 200);
        outerOver = prUiIsDropTarget(f.ui, outer, 2);
        PrLayoutNode inner = f.box(outer, 10, 10, 50, 50);
        innerOver = prUiIsDropTarget(f.ui, inner, 1);
        if (prUiDropped(f.ui, outer) != nullptr)
            dropped = prUiDropped(f.ui, outer);
    };
    f.frame();
    f.down(10, 10);
    f.move(220, 220);
    f.frame();
    f.frame();
    CHECK(outerOver);
    CHECK_FALSE(innerOver);
    f.up(220, 220);
    f.frame();
    CHECK(dropped != nullptr);
}

TEST_CASE("a system drag of files reaches a system target, which the output answers with a copy") {
    UiFixture f;
    bool over = false;
    PrAppDataFormFlags offered = 0;
    PrAppDataSource dropped = kPrAppDataSourceClipboard;
    f.build = [&] {
        PrLayoutNode target = f.box(f.root, 100, 100, 100, 100);
        const PrUiPayload* dragging = prUiDragging(f.ui);
        if (dragging != nullptr && dragging->kind == kPrUiPayloadKindSystem)
            offered = dragging->forms;
        over = prUiIsDropTarget(f.ui, target, kPrUiPayloadKindSystem);
        if (const PrUiPayload* payload = prUiDropped(f.ui, target))
            dropped = payload->drop;
    };
    f.frame();
    PrAppDragEvent drag{
        .position = {150, 150},
        .forms = PrAppDataFormFlag_Files,
        .operations = PrAppDragOperationFlag_Copy | PrAppDragOperationFlag_Move,
    };
    f.push(PrAppEvent{.kind = PrAppEventKind_DragEnter, .drag = drag});
    f.frame();
    f.frame();
    CHECK(over);
    CHECK(offered == PrAppDataFormFlag_Files);
    CHECK(prUiOutput(f.ui).dropOperation == PrAppDragOperation_Copy);

    // The drop names its content, which the target reads.
    drag.operation = PrAppDragOperation_Copy;
    drag.drop = 7;
    f.push(PrAppEvent{.kind = PrAppEventKind_Drop, .drag = drag});
    f.frame();
    CHECK(dropped == 7);
    CHECK(prUiDragging(f.ui) == nullptr);
    CHECK(prUiOutput(f.ui).dropOperation == PrAppDragOperation_None);
}

TEST_CASE("a drag that leaves the window ends the system's payload, and a refused operation falls back") {
    UiFixture f;
    f.build = [&] {
        PrLayoutNode target = f.box(f.root, 100, 100, 100, 100);
        prUiIsDropTarget(f.ui, target, 5);
    };
    f.frame();
    PrAppDragEvent drag{
        .position = {150, 150},
        .forms = PrAppDataFormFlag_Payload,
        .operations = PrAppDragOperationFlag_Copy,
        .payload = {.kind = 5, .value = 9},
    };
    f.push(PrAppEvent{.kind = PrAppEventKind_DragEnter, .drag = drag});
    f.frame();
    f.frame();
    REQUIRE(prUiDragging(f.ui) != nullptr);
    CHECK(prUiDragging(f.ui)->value == 9);
    CHECK(prUiDragging(f.ui)->source == kPrLayoutNodeNone);
    // A payload moves, but this source allows only a copy.
    CHECK(prUiOutput(f.ui).dropOperation == PrAppDragOperation_Copy);
    f.push(PrAppEvent{.kind = PrAppEventKind_DragLeave, .drag = drag});
    f.frame();
    CHECK(prUiDragging(f.ui) == nullptr);
}

TEST_CASE("a drag of content passes to the system once its press moves past the threshold, once for the press") {
    UiFixture f;
    harness::DragWindow window;
    prUiSetWindow(f.ui, &window);
    PrLayoutNode source = kPrLayoutNodeNone;
    PrAppDragDesc desc{
        .content = {.forms = PrAppDataFormFlag_Text, .text = "dragged"},
        .operations = PrAppDragOperationFlag_Copy,
    };
    f.build = [&] {
        source = f.box(f.root, 0, 0, 50, 50, PrUiFlag_Pressable);
        prUiDragStartContent(f.ui, source, 4, &desc);
    };
    f.frame();
    f.down(10, 10);
    f.move(12, 10);
    f.frame();
    CHECK(window.starts == 0);
    f.move(30, 30);
    f.frame();
    f.move(40, 30);
    f.frame();
    REQUIRE(window.starts == 1);
    CHECK(window.started.content.forms == PrAppDataFormFlag_Text);
    CHECK(window.started.operations == PrAppDragOperationFlag_Copy);

    // The system takes the pointer, and the drag's end reports the content that left.
    f.push(PrAppEvent{.kind = PrAppEventKind_PointerCancel, .pointer = {.position = {40, 30}}});
    f.push(PrAppEvent{.kind = PrAppEventKind_DragEnd, .drag = {.operation = PrAppDragOperation_Copy}});
    PrAppDragOperation operation = PrAppDragOperation_None;
    uint32_t ends = 0;
    f.build = [&] {
        source = f.box(f.root, 0, 0, 50, 50, PrUiFlag_Pressable);
        prUiDragStartContent(f.ui, source, 4, &desc);
        PrAppDragOperation ended = PrAppDragOperation_None;
        const PrUiPayload* payload = prUiExportEnded(f.ui, &ended);
        if (payload != nullptr && payload->kind == kPrUiPayloadKindSystem) {
            ++ends;
            operation = ended;
        }
    };
    f.frame();
    CHECK(ends == 1);
    CHECK(operation == PrAppDragOperation_Copy);
    CHECK(window.starts == 1);
}

TEST_CASE("a payload that may leave the window passes to the system as the pointer leaves") {
    UiFixture f;
    harness::DragWindow window;
    prUiSetWindow(f.ui, &window);
    PrLayoutNode source = kPrLayoutNodeNone;
    bool sourceOver = false;
    bool clicked = false;
    f.build = [&] {
        source = f.box(f.root, 0, 0, 50, 50, PrUiFlag_Pressable);
        if (prUiIsPressed(f.ui, source, 0))
            prUiDragStart(f.ui, source, 4, 3, 11, PrUiPayloadFlag_AnyWindow);
        clicked = clicked || prUiClicked(f.ui, source, 0);
        sourceOver = prUiIsDropTarget(f.ui, source, 3);
    };
    f.frame();
    f.down(10, 10);
    f.move(30, 30);
    f.frame();
    REQUIRE(prUiDragging(f.ui) != nullptr);
    CHECK(window.starts == 0);
    f.move(900, 30);
    f.frame();
    REQUIRE(window.starts == 1);
    CHECK(window.started.content.forms == PrAppDataFormFlag_Payload);
    CHECK(window.started.content.payload.kind == 3);
    CHECK(window.started.content.payload.value == 11);
    CHECK(window.started.operations == PrAppDragOperationFlag_Move);
    CHECK(prUiDragging(f.ui) == nullptr);

    // The system takes the pointer, and the press ends without a click.
    f.push(PrAppEvent{.kind = PrAppEventKind_PointerCancel, .pointer = {.position = {900, 30}}});
    f.frame();
    CHECK_FALSE(prUiIsPressed(f.ui, source, 0));
    // The payload returns over its own source, which is never its target.
    PrAppDragEvent drag{
        .position = {10, 10},
        .forms = PrAppDataFormFlag_Payload,
        .operations = PrAppDragOperationFlag_Move,
        .fromThisWindow = true,
        .payload = {.kind = 3, .value = 11},
    };
    f.push(PrAppEvent{.kind = PrAppEventKind_DragOver, .drag = drag});
    f.frame();
    f.frame();
    REQUIRE(prUiDragging(f.ui) != nullptr);
    CHECK(prUiDragging(f.ui)->source == source);
    CHECK_FALSE(sourceOver);
    CHECK(prUiOutput(f.ui).dropOperation == PrAppDragOperation_None);
    CHECK_FALSE(clicked);
    CHECK(window.starts == 1);
}

TEST_CASE("a payload that left the window reports how its drag ended, once, and a press forgets a lost end") {
    UiFixture f;
    harness::DragWindow window;
    prUiSetWindow(f.ui, &window);
    PrLayoutNode source = kPrLayoutNodeNone;
    const PrUiPayload* ended = nullptr;
    PrAppDragOperation operation = PrAppDragOperation_Copy;
    f.build = [&] {
        source = f.box(f.root, 0, 0, 50, 50, PrUiFlag_Pressable);
        if (prUiIsPressed(f.ui, source, 0))
            prUiDragStart(f.ui, source, 4, 3, 11, PrUiPayloadFlag_AnyWindow);
        ended = prUiExportEnded(f.ui, &operation);
    };
    auto handOff = [&] {
        f.frame();
        f.down(10, 10);
        f.move(30, 30);
        f.frame();
        f.move(900, 30);
        f.frame();
        f.push(PrAppEvent{.kind = PrAppEventKind_PointerCancel, .pointer = {.position = {900, 30}}});
        f.frame();
    };
    handOff();
    REQUIRE(window.starts == 1);
    CHECK(ended == nullptr);
    // No window took the payload.
    f.push(PrAppEvent{.kind = PrAppEventKind_DragEnd, .drag = {.operation = PrAppDragOperation_None}});
    f.frame();
    REQUIRE(ended != nullptr);
    CHECK(ended->kind == 3);
    CHECK(ended->value == 11);
    CHECK(operation == PrAppDragOperation_None);
    f.frame();
    CHECK(ended == nullptr);

    // A DragEnd that never comes does not keep the next drag from starting.
    handOff();
    REQUIRE(window.starts == 2);
    f.down(10, 10);
    f.move(30, 30);
    f.frame();
    f.move(900, 30);
    f.frame();
    CHECK(window.starts == 3);
}

TEST_CASE("a pointer that leaves the window hovers nothing until it comes back") {
    UiFixture f;
    PrLayoutNode node = kPrLayoutNodeNone;
    bool hovered = false;
    f.build = [&] {
        node = f.box(f.root, 0, 0, 50, 50, PrUiFlag_Pressable);
        hovered = prUiIsHovered(f.ui, node);
    };
    f.frame();
    f.move(10, 10);
    f.frame();
    f.frame();
    CHECK(hovered);
    f.push(PrAppEvent{.kind = PrAppEventKind_PointerLeave, .pointer = {.position = PrVec2{10, 10}}});
    f.frame();
    f.frame();
    CHECK_FALSE(hovered);
    PrVec2 at{};
    CHECK_FALSE(prUiPointer(f.ui, nullptr, &at));
    f.push(PrAppEvent{.kind = PrAppEventKind_PointerEnter, .pointer = {.position = PrVec2{10, 10}}});
    f.frame();
    f.frame();
    CHECK(hovered);
}

TEST_CASE("a pointer cancel ends a press without a click") {
    UiFixture f;
    PrLayoutNode node = kPrLayoutNodeNone;
    bool clicked = false;
    f.build = [&] {
        node = f.box(f.root, 0, 0, 50, 50, PrUiFlag_Pressable);
        clicked = clicked || prUiClicked(f.ui, node, 0);
    };
    f.frame();
    f.down(10, 10);
    f.frame();
    CHECK(prUiIsPressed(f.ui, node, 0));
    f.push(PrAppEvent{.kind = PrAppEventKind_PointerCancel, .pointer = {.position = PrVec2{10, 10}}});
    f.up(10, 10);
    f.frame();
    CHECK_FALSE(prUiIsPressed(f.ui, node, 0));
    CHECK_FALSE(clicked);
}

TEST_CASE("reveal scrolls a node into its scroll container's view") {
    UiFixture f;
    PrLayoutNode scroller = kPrLayoutNodeNone;
    PrLayoutNode item = kPrLayoutNodeNone;
    bool reveal = false;
    f.build = [&] {
        PrLayoutNode content = kPrLayoutNodeNone;
        scroller = f.scroller(f.root, 0, 0, 200, 200, 200, 1000, &content);
        item = f.box(content, 0, 500, 50, 50);
        if (reveal) {
            prUiReveal(f.ui, item);
            reveal = false;
        }
    };
    f.frame();
    reveal = true;
    f.frame();
    CHECK(prLayoutNodeScroll(f.tree, scroller).y.raw == prLayoutUnitFromPx(350).raw);
}

TEST_CASE("a container that follows its end keeps its bottom in view as it grows, until a scroll leaves the bottom") {
    UiFixture f;
    PrLayoutNode scroller = kPrLayoutNodeNone;
    float height = 400;
    f.build = [&] {
        scroller = f.scroller(f.root, 0, 0, 200, 200, 200, height);
        prUiFollowEnd(f.ui, scroller);
    };
    // Returns how far the container shows from its bottom, in px.
    auto above = [&] {
        PrLayoutScrollInfo info{};
        REQUIRE(prLayoutNodeScrollInfo(f.tree, scroller, &info));
        return prLayoutUnitToPx(PrLayoutUnit{info.range.bottom.raw - info.offset.y.raw});
    };
    f.frame();
    CHECK(above() == 0);
    height = 500;
    f.frame();
    CHECK(above() == 0);

    f.wheel(100, 100, 0, -60);
    f.frame();
    CHECK(above() == 60);
    height = 600;
    f.frame();
    f.frame();
    CHECK(above() == 160);

    f.wheel(100, 100, 0, 1000);
    f.frame();
    CHECK(above() == 0);
    height = 700;
    f.frame();
    CHECK(above() == 0);
}

TEST_CASE("a frame that declares the same tree lays nothing out, with input or without") {
    UiFixture f;
    f.build = [&] {
        for (uint32_t i = 0; i < 20; ++i)
            f.box(f.root, 0, 20 * float(i), 100, 18, PrUiFlag_Pressable);
    };
    f.frame();
    f.frame();
    CHECK(prLayoutTreeStats(f.tree).blocksLaidOut == 0);
    f.move(10, 10);
    f.frame();
    CHECK(prLayoutTreeStats(f.tree).blocksLaidOut == 0);
    CHECK(prLayoutTreeStats(f.tree).frameNodesMade == 0);
}

TEST_CASE("the node that holds a press keeps its cursor while the pointer leaves it") {
    UiFixture f;
    f.build = [&] {
        PrLayoutNode handle = f.box(f.root, 0, 0, 50, 50, PrUiFlag_Pressable);
        prUiSetCursor(f.ui, handle, PrAppCursor_ResizeX);
        PrLayoutNode other = f.box(f.root, 100, 0, 50, 50, PrUiFlag_Pressable);
        prUiSetCursor(f.ui, other, PrAppCursor_Hand);
    };
    f.frame();
    f.down(10, 10);
    f.frame();
    CHECK(prUiOutput(f.ui).cursor == PrAppCursor_ResizeX);
    f.push(PrAppEvent{.kind = PrAppEventKind_PointerMove, .pointer = {.position = PrVec2{120, 10}}});
    f.frame();
    CHECK(prUiOutput(f.ui).cursor == PrAppCursor_ResizeX);
    f.up(120, 10);
    f.frame();
    CHECK(prUiOutput(f.ui).cursor == PrAppCursor_Hand);
}

TEST_CASE("keys made under one base differ by name and index, and keys of one name differ by base") {
    UiFixture f;
    PrUiKey keys[8] = {};
    f.build = [&] {
        PrLayoutNode keyed = f.box(f.root, 0, 0, 10, 10, 0, prUiKey(f.ui, kPrLayoutNodeNone, "panel"));
        PrLayoutNode unkeyed = f.box(f.root, 0, 0, 10, 10);
        PrLayoutNode other = f.box(f.root, 0, 0, 10, 10);
        keys[0] = prUiKey(f.ui, keyed, "row");
        keys[1] = prUiKey(f.ui, unkeyed, "row");
        keys[2] = prUiKey(f.ui, other, "row");
        keys[3] = prUiKey(f.ui, keyed, "column");
        keys[4] = prUiKeyIndex(f.ui, keyed, 1);
        keys[5] = prUiKeyIndex(f.ui, keyed, 2);
        keys[6] = prUiKeyIndex2(f.ui, keyed, 1, 2);
        keys[7] = prUiKeyIndex2(f.ui, keyed, 2, 1);
    };
    f.frame();
    for (uint32_t i = 0; i < 8; ++i) {
        CHECK(keys[i] != 0);
        for (uint32_t j = i + 1; j < 8; ++j)
            CHECK(keys[i] != keys[j]);
    }
    // A base's identity holds from round to round, so the keys made under it do too.
    PrUiKey before[8];
    for (uint32_t i = 0; i < 8; ++i)
        before[i] = keys[i];
    f.frame();
    for (uint32_t i = 0; i < 8; ++i)
        CHECK(keys[i] == before[i]);
}

TEST_CASE("the overlay covers the root above its content, lets the pointer through, and exists only while asked for") {
    UiFixture f;
    bool floating = true;
    PrLayoutNode content = kPrLayoutNodeNone;
    PrLayoutNode menu = kPrLayoutNodeNone;
    PrLayoutNode overlay = kPrLayoutNodeNone;
    bool contentClicked = false;
    f.build = [&] {
        // The overlay is asked for before the content is created, and still stacks above it.
        overlay = floating ? prUiOverlay(f.ui) : kPrLayoutNodeNone;
        content = f.box(f.root, 0, 0, 800, 600, PrUiFlag_Pressable, 0, 5, true);
        contentClicked = contentClicked || prUiClicked(f.ui, content, 0);
        if (floating) {
            CHECK(prUiOverlay(f.ui) == overlay);
            menu = f.box(overlay, 100, 100, 50, 50, PrUiFlag_Pressable);
        }
    };
    f.frame();
    PrRect bounds = prLayoutRectToPx(prLayoutNodeBounds(f.tree, overlay, PrLayoutCoordinates_Viewport));
    CHECK(bounds.right == 800.0f);
    CHECK(bounds.bottom == 600.0f);
    PrLayoutNode first = overlay;
    f.frame();
    CHECK(overlay == first);
    // A press on the menu reaches the menu, and a press elsewhere passes through the overlay to the content.
    f.down(110, 110);
    f.frame();
    CHECK(prUiIsPressed(f.ui, menu, 0));
    f.up(110, 110);
    f.press(400, 400);
    f.frame();
    CHECK(contentClicked);
    floating = false;
    f.frame();
    CHECK_FALSE(prLayoutNodeIsValid(f.tree, first));
}

TEST_CASE("without a host, a popup floats in the overlay against its anchor, and a modal dialog covers its parent") {
    UiFixture f;
    bool dialog = false;
    PrLayoutNode under = kPrLayoutNodeNone;
    PrLayoutNode popup = kPrLayoutNodeNone;
    PrLayoutNode shown = kPrLayoutNodeNone;
    bool underPressed = false;
    f.build = [&] {
        under = f.box(f.root, 0, 0, 800, 600, PrUiFlag_Pressable);
        underPressed = underPressed || prUiIsPressed(f.ui, under, 0);
        PrUiSurfaceDesc desc{
            .kind = PrUiSurfaceKind_Popup,
            .placement = {.anchor = PrRect{10, 560, 110, 590}, .adjust = PrAppPopupAdjustFlag_Flip},
        };
        if (dialog)
            desc = PrUiSurfaceDesc{.kind = PrUiSurfaceKind_Dialog, .modal = true};
        popup = prUiSurface(f.ui, prUiKey(f.ui, kPrLayoutNodeNone, "popup"), &desc, 0);
        shown = f.block(popup, 200, 100);
    };
    // The first layout gives the popup its extent, by which the next round places it above its anchor.
    f.frame();
    f.frame();
    PrRect r = prLayoutRectToPx(prLayoutNodeBounds(f.tree, shown, PrLayoutCoordinates_Viewport));
    CHECK(r.left == 10.0f);
    CHECK(r.top == 460.0f);
    CHECK(r.bottom == 560.0f);
    CHECK(prUiNodeViewport(f.ui, shown) == kPrUiViewportMain);
    CHECK(prUiViewports(f.ui, nullptr, 0) == 0);
    // A press beside the popup reaches what is under it.
    f.down(400, 100);
    f.frame();
    CHECK(underPressed);
    f.up(400, 100);
    f.frame();
    // A modal dialog is centered, and its cover takes the press.
    dialog = true;
    underPressed = false;
    f.frame();
    f.frame();
    r = prLayoutRectToPx(prLayoutNodeBounds(f.tree, shown, PrLayoutCoordinates_Viewport));
    CHECK(r.left == 300.0f);
    CHECK(r.top == 250.0f);
    f.down(20, 20);
    f.frame();
    CHECK_FALSE(underPressed);
}

// A window whose content is at `origin` on one display, which converts its points to and from that display.
struct PlacedWindow : PrAppWindow {
    PrVec2 origin{};

    explicit PlacedWindow(PrVec2 at) : PrAppWindow{}, origin(at) {
        pointToDisplayFn = [](const PrAppWindow* window, PrVec2 point, PrAppDisplayPosition* out) {
            PrVec2 o = static_cast<const PlacedWindow*>(window)->origin;
            *out = PrAppDisplayPosition{.display = 1, .position = PrVec2{o.x + point.x, o.y + point.y}};
            return true;
        };
        pointFromDisplayFn = [](const PrAppWindow* window, const PrAppDisplayPosition* position, PrVec2* out) {
            PrVec2 o = static_cast<const PlacedWindow*>(window)->origin;
            *out = PrVec2{position->position.x - o.x, position->position.y - o.y};
            return true;
        };
    }
};

// A host that shows every surface in one window.
struct OneWindowHost {
    PlacedWindow* window;

    static PrAppWindow* PR_NULLABLE windowOf(void* context, PrUiViewport) {
        return static_cast<OneWindowHost*>(context)->window;
    }
};

TEST_CASE("a host shows each surface in a viewport of its own, which the pointer reaches from a capture elsewhere") {
    UiFixture f;
    PlacedWindow main{PrVec2{100, 100}};
    PlacedWindow popupWindow{PrVec2{400, 300}};
    OneWindowHost host{.window = &popupWindow};
    prUiSetWindow(f.ui, &main);
    PrUiHost desc{.window = OneWindowHost::windowOf, .context = &host};
    prUiSetHost(f.ui, &desc);
    PrLayoutNode handle = kPrLayoutNodeNone;
    PrLayoutNode item = kPrLayoutNodeNone;
    bool itemHovered = false;
    bool itemClicked = false;
    f.build = [&] {
        handle = f.box(f.root, 10, 10, 50, 50, PrUiFlag_Pressable);
        PrUiSurfaceDesc popup{.kind = PrUiSurfaceKind_Popup, .placement = {.anchor = PrRect{10, 10, 60, 60}}};
        PrLayoutNode surface = prUiSurface(f.ui, prUiKey(f.ui, kPrLayoutNodeNone, "menu"), &popup, 0);
        item = f.block(surface, 100, 40, PrUiFlag_Pressable | PrUiFlag_Focusable);
        itemHovered = prUiIsHovered(f.ui, item);
        itemClicked = itemClicked || prUiClicked(f.ui, item, 0);
        if (prUiFocus(f.ui) == item)
            prUiSetTextInput(f.ui, PrRect{5, 6, 6, 20}, PrAppTextInputKind_Text);
    };
    f.frame();
    PrUiViewportInfo info{};
    REQUIRE(prUiViewports(f.ui, &info, 1) == 1);
    CHECK(info.parent == kPrUiViewportMain);
    CHECK(info.extent.x == 100.0f);
    CHECK(info.extent.y == 40.0f);
    CHECK(prLayoutNodeIsValid(f.tree, info.root));
    CHECK(prUiNodeViewport(f.ui, item) == info.viewport);
    CHECK(prUiNodeViewport(f.ui, handle) == kPrUiViewportMain);

    // A move that the main viewport reports over the surface's viewport hovers the item, in the surface's px.
    f.move(320, 210);
    f.frame();
    CHECK(itemHovered);
    PrUiViewport over = kPrUiViewportMain;
    PrVec2 at{};
    CHECK(prUiPointer(f.ui, &over, &at));
    CHECK(over == info.viewport);
    CHECK(at.x == 20.0f);
    CHECK(at.y == 10.0f);
    // The main viewport's Leave after the pointer entered the surface's leaves the hover where it is.
    f.push(PrAppEvent{.kind = PrAppEventKind_PointerLeave, .pointer = {.position = PrVec2{801, 210}}});
    f.viewport = info.viewport;
    f.move(30, 10);
    f.frame();
    CHECK(itemHovered);

    // A press that the handle captures in the main viewport keeps its drag in the main viewport's px, while the pointer
    // hovers the item in the other.
    f.viewport = kPrUiViewportMain;
    f.down(30, 30);
    f.frame();
    f.move(330, 215);
    f.frame();
    PrUiDrag drag{};
    CHECK(prUiDrag(f.ui, handle, 0, 0, &drag));
    CHECK(drag.position.x == 330.0f);
    CHECK(drag.position.y == 215.0f);
    CHECK(itemHovered);
    f.up(330, 215);
    f.frame();
    CHECK_FALSE(itemClicked);

    // A click that the surface's viewport reports reaches the item, which takes the focus. The caret of its text is in
    // the surface's window.
    f.viewport = info.viewport;
    f.press(20, 10);
    f.frame();
    CHECK(itemClicked);
    f.frame();
    PrAppOutput output = prUiOutput(f.ui);
    CHECK(output.hasTextInput);
    CHECK(output.textInputWindow == &popupWindow);
    CHECK(output.textInput.left == 5.0f);
    prUiSetWindow(f.ui, nullptr);
}

TEST_CASE("with nothing focused, a key starts at the key root, and claims below it go unheard without one") {
    UiFixture f;
    bool keyRoot = false;
    uint32_t heard = 0;
    f.build = [&] {
        PrLayoutNode app = f.box(f.root, 0, 30, 800, 570);
        if (keyRoot)
            prUiSetKeyRoot(f.ui, app);
        if (prUiKeyPressed(f.ui, app, 'P', PrAppModifierFlag_Super | PrAppModifierFlag_Shift))
            ++heard;
    };
    f.frame();
    f.key(PrAppKey('P'), PrAppModifierFlag_Super | PrAppModifierFlag_Shift);
    f.frame();
    CHECK(heard == 0);
    keyRoot = true;
    f.frame();
    f.key(PrAppKey('P'), PrAppModifierFlag_Super | PrAppModifierFlag_Shift);
    f.frame();
    CHECK(heard == 1);
}

TEST_CASE("a control reports its gestures to the nearest sink above it, and a null sink passes them nowhere") {
    UiFixture f;
    Reports outer;
    Reports inner;
    PrLayoutNode controls[4] = {};
    f.build = [&] {
        PrLayoutNode panel = f.box(f.root, 0, 0, 400, 400);
        prUiSetEditSink(f.ui, panel, Reports::record, &outer);
        PrLayoutNode group = f.box(panel, 0, 0, 200, 200);
        prUiSetEditSink(f.ui, group, Reports::record, &inner);
        PrLayoutNode quiet = f.box(panel, 200, 0, 200, 200);
        prUiSetEditSink(f.ui, quiet, nullptr, nullptr);
        controls[0] = f.box(panel, 0, 300, 10, 10);
        controls[1] = f.box(group, 0, 0, 10, 10);
        controls[2] = f.box(quiet, 0, 0, 10, 10);
        controls[3] = f.box(f.root, 500, 0, 10, 10);
        for (PrLayoutNode control : controls)
            prUiEdit(f.ui, control, PrUiEditPhase_Commit);
    };
    f.frame(1.0 / 60, 1);
    REQUIRE(outer.seen.count() == 1);
    CHECK(outer.seen[0].control == controls[0]);
    REQUIRE(inner.seen.count() == 1);
    CHECK(inner.seen[0].control == controls[1]);
}

TEST_CASE("records are found by place in the round and groups by recipe, apart for each owner") {
    UiFixture f;
    Maker a;
    Maker b;
    PrLayoutStyleIndex styles[3] = {};
    f.build = [&] {
        Maker* owners[3] = {&a, &b, &a};
        for (uint32_t i = 0; i < 3; ++i) {
            styles[i] = widthStyle(f.ui, *owners[i], 40);
            prUiElement(f.ui, f.root, 0, styles[i], 0);
        }
    };
    f.frame(1.0 / 60, 1);
    // One owner's recipe makes its group once, wherever it is looked up, and another owner's makes its own.
    CHECK(a.made == 1);
    CHECK(b.made == 1);
    CHECK(styles[0] == styles[2]);
    CHECK(prLayoutStyleRecord(f.tree, styles[0]).groups[PrLayoutGroup_Box] !=
        prLayoutStyleRecord(f.tree, styles[1]).groups[PrLayoutGroup_Box]);
    // The first frame has no committed tree to place its lookups by, so the memo keeps them from the second.
    PrLayoutStyleIndex before[3] = {styles[0], styles[1], styles[2]};
    f.frame(1.0 / 60, 1);
    uint32_t missed = a.missed + b.missed;
    f.frame(1.0 / 60, 1);
    CHECK(a.missed + b.missed == missed);
    for (uint32_t i = 0; i < 3; ++i)
        CHECK(styles[i] == before[i]);
    // Forgetting an owner releases what it made, and its next lookups make them again.
    prUiForget(f.ui, &a);
    f.frame(1.0 / 60, 1);
    CHECK(a.made == 2);
    CHECK(b.made == 1);
}

TEST_CASE("a key that a frame gives twice is outlined on both nodes in the next frame, and the outlines go with it") {
    UiFixture f;
    bool twice = true;
    PrLayoutNode rows[2] = {};
    f.build = [&] {
        PrUiKey key = prUiKey(f.ui, kPrLayoutNodeNone, "row");
        rows[0] = f.box(f.root, 0, 0, 100, 20, 0, key);
        rows[1] = f.box(f.root, 0, 30, 100, 20, 0, twice ? key : 0);
    };
    // The outlines are the children of the overlay, which is the root's child other than the rows, if there is one.
    auto outlines = [&] {
        uint32_t count = 0;
        for (PrLayoutNode layer = prLayoutNodeFirstChild(f.tree, f.root); layer != kPrLayoutNodeNone;
                layer = prLayoutNodeNextSibling(f.tree, layer)) {
            if (layer == rows[0] || layer == rows[1])
                continue;
            for (PrLayoutNode n = prLayoutNodeFirstChild(f.tree, layer); n != kPrLayoutNodeNone;
                    n = prLayoutNodeNextSibling(f.tree, n))
                ++count;
        }
        return count;
    };
    f.frame();
    f.frame();
    CHECK(outlines() == 2);
    twice = false;
    f.frame();
    f.frame();
    CHECK(outlines() == 0);
}
