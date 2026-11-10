// The parts of a custom title bar: a later region takes a point from an earlier one.

#include "pr/app.h"

#include "app/backends/shared/src/app_shared_title_bar.h"

#include "tools/harness/test.h"

using namespace pr_impl;

TEST_CASE("a point takes the kind of the last region that holds it, and a point in none is the program's") {
    PrAppTitleBarRegion parts[] = {
        {.rect = {0, 0, 300, 30}, .kind = PrAppTitleBarRegionKind_Drag},
        {.rect = {40, 5, 80, 25}, .kind = PrAppTitleBarRegionKind_Client},
        {.rect = {260, 0, 300, 30}, .kind = PrAppTitleBarRegionKind_Close},
    };
    PrAppOutput output{.titleBarRegions = parts, .titleBarRegionCount = 3};
    AppTitleBarRegions regions{};
    appTitleBarRegionsSet(regions, output);
    CHECK(appTitleBarRegionAt(regions, PrVec2{10, 10}) == PrAppTitleBarRegionKind_Drag);
    CHECK(appTitleBarRegionAt(regions, PrVec2{50, 10}) == PrAppTitleBarRegionKind_Client);
    CHECK(appTitleBarRegionAt(regions, PrVec2{270, 10}) == PrAppTitleBarRegionKind_Close);
    CHECK(appTitleBarRegionAt(regions, PrVec2{10, 40}) == PrAppTitleBarRegionKind_Client);
    // A frame without regions leaves none.
    appTitleBarRegionsSet(regions, PrAppOutput{});
    CHECK(appTitleBarRegionAt(regions, PrVec2{10, 10}) == PrAppTitleBarRegionKind_Client);
    appTitleBarRegionsReset(regions);
}
