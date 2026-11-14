// Studio Kit's title bar: its band drags the window, its controls take presses, and its window buttons are the
// system's.

#include "pr/kit_box.h"
#include "pr/kit_button.h"
#include "pr/kit_title_bar.h"
#include "pr/span.h"
#include "pr/ui.h"

#include "tools/harness/kit_fixture.h"

#include "tools/harness/test.h"

using namespace harness;

namespace pr_impl {

namespace {

// The title bar regions of the fixture's last frame.
Span<const PrAppTitleBarRegion> regionsOf(KitFixture& f) {
    PrAppOutput output = prUiOutput(f.ui);
    return Span<const PrAppTitleBarRegion>(output.titleBarRegions, output.titleBarRegionCount);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a title bar drags the window, a control within it takes presses, and the window buttons come last") {
    KitFixture f;
    PrLayoutNode button = kPrLayoutNodeNone;
    PrKitTitleBar titleBar{.bar = {.height = 32, .flags = PrAppTitleBarFlag_ProgramButtons}};
    f.build = [&](PrLayoutNode root) {
        PrLayoutNode row = prKitTitleBar(f.kit, root, titleBar);
        button = prKitButton(f.kit, row, {.text = "Menu"}, {});
    };
    f.frame();
    f.frame();
    Span<const PrAppTitleBarRegion> regions = regionsOf(f);
    REQUIRE(regions.count() == 5);
    CHECK(regions[0].kind == PrAppTitleBarRegionKind_Drag);
    CHECK(regions[0].rect.top == 0);
    CHECK(regions[0].rect.bottom == 32);
    CHECK(regions[0].rect.right == 800);
    CHECK(regions[1].kind == PrAppTitleBarRegionKind_Client);
    PrRect bounds = prLayoutRectToPx(prLayoutNodeBounds(f.tree, button, PrLayoutCoordinates_Viewport));
    CHECK(regions[1].rect.left == bounds.left);
    CHECK(regions[2].kind == PrAppTitleBarRegionKind_Minimize);
    CHECK(regions[3].kind == PrAppTitleBarRegionKind_Maximize);
    CHECK(regions[4].kind == PrAppTitleBarRegionKind_Close);
    // The window buttons are flush with the band's end, the close button last.
    CHECK(regions[4].rect.right == 800);
    CHECK(regions[4].rect.bottom == 32);
}

TEST_CASE("a title bar keeps its content out of the system's insets, and a window without one gets a plain row") {
    KitFixture f;
    PrLayoutNode row = kPrLayoutNodeNone;
    PrKitTitleBar titleBar{.bar = {.height = 28, .leading = 70}};
    f.build = [&](PrLayoutNode root) { row = prKitTitleBar(f.kit, root, titleBar); };
    f.frame();
    f.frame();
    Span<const PrAppTitleBarRegion> regions = regionsOf(f);
    REQUIRE(regions.count() == 1);
    CHECK(regions[0].kind == PrAppTitleBarRegionKind_Drag);
    CHECK(prLayoutRectToPx(prLayoutNodeBounds(f.tree, row, PrLayoutCoordinates_Viewport)).left == 70);
    titleBar.bar = PrAppTitleBar{};
    f.frame();
    f.frame();
    CHECK(regionsOf(f).isEmpty());
}
