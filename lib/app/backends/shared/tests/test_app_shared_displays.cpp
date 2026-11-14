// The displays that a backend has reported: each keeps its name until the list is reset.

#include "app/backends/shared/src/app_shared_displays.h"

#include "tools/harness/test.h"

using namespace pr_impl;

TEST_CASE("a display keeps the name it was added with, and a reset forgets every display") {
    AppDisplayList list{};
    PrStringView found{};
    CHECK_FALSE(appDisplayListFind(list, 7, &found));
    PrStringView built = appDisplayListAdd(list, 7, "Built-in Display");
    PrStringView studio = appDisplayListAdd(list, 9, "Studio Display");
    CHECK(built == "Built-in Display");
    CHECK(studio == "Studio Display");
    // A display that is added again keeps its first name, and the views of the names stay valid as the list grows.
    CHECK(appDisplayListAdd(list, 7, "Renamed") == "Built-in Display");
    for (PrAppDisplayId id = 10; id < 40; ++id)
        appDisplayListAdd(list, id, "Other");
    REQUIRE(appDisplayListFind(list, 9, &found));
    CHECK(found == "Studio Display");
    CHECK(built == "Built-in Display");
    appDisplayListReset(list);
    CHECK_FALSE(appDisplayListFind(list, 7, &found));
}
