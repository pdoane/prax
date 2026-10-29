#include "pr/kit.h"
#include "pr/kit_box.h"
#include "pr/kit_button.h"
#include "pr/kit_icon.h"
#include "pr/kit_surface.h"
#include "pr/kit_text.h"

#include "studio/kit/src/kit.h"
#include "tools/harness/kit_fixture.h"

#include "doctest/doctest.h"

#include <string>

using namespace harness;

extern "C" int prTestKitCApi(void);

namespace pr_impl {

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the public headers compile as C") {
    CHECK(prTestKitCApi() == 0);
}

TEST_CASE("a surface made as the root fills the viewport, and draws its ground or, on none, nothing") {
    KitFixture f;
    f.frame();
    PrRect bounds = f.bounds(f.root);
    CHECK(bounds.right == 800.0f);
    CHECK(bounds.bottom == 600.0f);
    auto background = [&] {
        PrLayoutStyle record = prLayoutStyleRecord(f.tree, prLayoutNodeStyle(f.tree, f.root));
        PrLayoutGroupIndex group = record.groups[PrLayoutGroup_Decoration];
        PrLayoutDecorationStyle decoration = prLayoutDecorationStyleRecord(f.tree, group);
        return prColorFromColor16(decoration.background);
    };
    CHECK(background().a != 0.0f);
    f.ground = PrKitGround_None;
    f.frame();
    CHECK(background().a == 0.0f);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a button is activated by a click, through the transparent overlay above it") {
    KitFixture f;
    PrLayoutNode button = kPrLayoutNodeNone;
    uint32_t activations = 0;
    f.build = [&](PrLayoutNode ground) {
        PrLayoutNode row = prKitBox(f.kit, ground, {}, {.direction = PrKitDirection_Row, .gap = kPrKitSpace4});
        PrKitButton content{.text = "Save", .icon = PrKitIconKind_Check};
        button = prKitButton(f.kit, row, content, {});
        activations += prKitActivated(f.kit, button) ? 1 : 0;
    };
    f.frame();
    PrRect box = f.bounds(button);
    CHECK(box.bottom - box.top == doctest::Approx(double(kPrKitHeightMd)));
    CHECK(box.right - box.left > 40.0f);
    f.click(f.center(button));
    f.frame();
    CHECK(activations == 1);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a box that scrolls in its block axis scrolls under the wheel") {
    KitFixture f;
    PrLayoutNode view = kPrLayoutNodeNone;
    f.build = [&](PrLayoutNode ground) {
        PrKitBoxStyle box{.overflow = PrKitOverflow_ScrollY};
        box.place.height = prKitPx(100);
        box.place.flex = PrKitFlex_Fixed;
        view = prKitBox(f.kit, ground, {}, box);
        for (uint32_t i = 0; i < 10; ++i) {
            PrKitBoxStyle row{};
            row.place.height = prKitPx(40);
            row.place.flex = PrKitFlex_Fixed;
            prKitBox(f.kit, view, {}, row);
        }
    };
    f.frame();
    f.move(f.center(view));
    f.push(PrAppEvent{.kind = PrAppEventKind_Scroll, .position = f.center(view), .delta = PrVec2{0, 60}});
    f.frame();
    PrLayoutVec offset = prLayoutNodeScroll(f.tree, view);
    CHECK(offset.y.raw != 0);
    CHECK(offset.x.raw == 0);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a frame of the same widgets makes no record, lays nothing out and records nothing") {
    KitFixture f;
    f.build = [&](PrLayoutNode ground) {
        PrLayoutNode panel = prKitBox(f.kit, ground, {}, {.gap = 0});
        prKitPanelHeader(f.kit, panel, {.title = "SCENE", .meta = "23"});
        for (uint32_t i = 0; i < 10; ++i) {
            PrLayoutNode row = prKitBox(f.kit, panel, {}, {.direction = PrKitDirection_Row, .gap = kPrKitSpace3});
            prKitIcon(f.kit, row, {.icon = PrKitIconKind_Cube}, {.size = 14, .tone = PrKitTone_Muted});
            PrKitTextStyle style{.wrap = PrKitWrap_Clip};
            prKitLabel(f.kit, row, {.text = "Entity"}, style);
            prKitBadge(f.kit, row, {.text = "mesh"}, {.variant = PrKitBadgeVariant_Neutral});
        }
        PrKitButton content{.text = "Add"};
        prKitButton(f.kit, panel, content, {});
    };
    f.frame();
    const PrLayoutScene* first = prLayoutTreeScene(f.tree);
    f.frame();
    PrLayoutTreeStats stats = prLayoutTreeStats(f.tree);
    CHECK(stats.frameNodesMade == 0);
    CHECK(stats.blocksLaidOut == 0);
    const PrLayoutScene* second = prLayoutTreeScene(f.tree);
    CHECK(second == first);
    CHECK(kitVerified(f.tree));
    prLayoutSceneRelease(first);
    prLayoutSceneRelease(second);
}

TEST_CASE("a hovered button fades to its hover fill, and keeps one record once it is there") {
    KitFixture f;
    PrLayoutNode button = kPrLayoutNodeNone;
    f.build = [&](PrLayoutNode ground) {
        PrLayoutNode row = prKitBox(f.kit, ground, {}, {.direction = PrKitDirection_Row, .gap = 0});
        PrKitButton content{.text = "Hover"};
        button = prKitButton(f.kit, row, content, {});
    };
    f.frame();
    PrLayoutStyleIndex rest = prLayoutNodeStyle(f.tree, button);
    f.move(f.center(button));
    f.frame(0.01);
    f.frame(0.02);
    PrLayoutStyleIndex fading = prLayoutNodeStyle(f.tree, button);
    CHECK(fading != rest);
    CHECK(prUiWantsFrame(f.ui));
    f.frame(0.5);
    f.frame(0.5);
    PrLayoutStyleIndex hovered = prLayoutNodeStyle(f.tree, button);
    f.frame(0.5);
    CHECK(prLayoutNodeStyle(f.tree, button) == hovered);
    CHECK_FALSE(prUiWantsFrame(f.ui));
    // A fill change affects only painting.
    CHECK(prLayoutTreeStats(f.tree).blocksLaidOut == 0);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a widget that holds still keeps its record past the cache's age, when it moves to another place in the round") {
    KitFixture f;
    PrLayoutNode label = kPrLayoutNodeNone;
    bool shifted = false;
    f.build = [&](PrLayoutNode ground) {
        // The first widget of the round follows the overlay, which the screen made first, so the label is not first. A
        // box ahead of the label moves it to another position in the round, where the memo holds nothing for it.
        prKitBox(f.kit, ground, {.key = 1}, {});
        if (shifted)
            prKitBox(f.kit, ground, {.key = 2}, {});
        label = prKitLabel(f.kit, ground, {.key = 3, .text = "Still"}, {.tone = PrKitTone_Accent});
    };
    f.frame();
    PrLayoutStyleIndex record = prLayoutNodeStyle(f.tree, label);
    // The memo answers each round, so no lookup reaches the group cache while the record's groups age there.
    for (uint32_t i = 0; i < 3 * 64; ++i)
        f.frame();
    shifted = true;
    f.frame();
    CHECK(prLayoutNodeStyle(f.tree, label) == record);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a widget placed by its caller takes the placement, and keeps the placed record") {
    KitFixture f;
    PrLayoutNode button = kPrLayoutNodeNone;
    f.build = [&](PrLayoutNode ground) {
        PrLayoutNode row = prKitBox(f.kit, ground, {}, {.direction = PrKitDirection_Row, .gap = 0});
        PrKitButton content{.text = "Wide"};
        button = prKitButton(f.kit, row, content, {});
        PrKitPlace place{.width = prKitPx(200)};
        prKitPlace(f.kit, button, &place);
    };
    f.frame();
    CHECK(f.bounds(button).right - f.bounds(button).left == 200.0f);
    f.frame();
    CHECK(prLayoutTreeStats(f.tree).blocksLaidOut == 0);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a disabled container's buttons take no press and no focus") {
    KitFixture f;
    PrLayoutNode button = kPrLayoutNodeNone;
    uint32_t activations = 0;
    f.build = [&](PrLayoutNode ground) {
        PrKitButton content{.text = "Off"};
        button = prKitButton(f.kit, prKitBox(f.kit, ground, {.disabled = true}, {}), content, {});
        activations += prKitActivated(f.kit, button) ? 1 : 0;
    };
    f.frame();
    f.click(f.center(button));
    f.frame();
    CHECK(activations == 0);
    CHECK(prUiFocus(f.ui) == kPrLayoutNodeNone);
}

TEST_CASE("an icon draws its path, and a label its text") {
    KitFixture f;
    bool icon = true;
    f.build = [&](PrLayoutNode ground) {
        if (icon)
            prKitIcon(f.kit, ground, {.icon = PrKitIconKind_Settings}, {.size = 24, .tone = PrKitTone_Primary});
        else
            prKitLabel(f.kit, ground, {.text = "Inspector"}, {});
    };
    f.frame();
    uint32_t withIcon = f.paintedCommands();
    icon = false;
    f.frame();
    uint32_t withLabel = f.paintedCommands();
    CHECK(withIcon > 1);
    CHECK(withLabel > 1);
    CHECK(prKitIconName(PrKitIconKind_Settings).size != 0);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a fade from clear to a color keeps the color's channels, and only its alpha grows") {
    PrColor gray{0.6f, 0.6f, 0.6f, 1};
    PrColor clear{0, 0, 0, 0};
    PrColor half = kitMix(clear, gray, 0.5f);
    CHECK(half.r == doctest::Approx(0.6));
    CHECK(half.a == doctest::Approx(0.5));
    PrColor back = kitMix(gray, clear, 0.25f);
    CHECK(back.g == doctest::Approx(0.6));
    CHECK(back.a == doctest::Approx(0.75));
    // Between opaque colors, the blend is the plain one.
    PrColor mid = kitMix(PrColor{0, 0, 0, 1}, PrColor{1, 1, 1, 1}, 0.25f);
    CHECK(mid.r == doctest::Approx(0.25));
}
