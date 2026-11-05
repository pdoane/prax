// Studio's window, short of a window of the platform: a run whose fonts cannot be installed ends before it opens.

#include "pr/studio_window.h"

#include "tools/harness/test.h"

extern "C" int prTestStudioWindowCApi(void);

TEST_CASE("window: the public header compiles as C") {
    CHECK(prTestStudioWindowCApi() == 0);
}

TEST_CASE("window: a run whose fonts cannot be installed is NotFound, and calls none of its callbacks") {
    bool called = false;
    PrStudioWindowDesc desc{
        .title = "test",
        .extent = {100, 100},
        .fontDirectory = "/prax/no/such/fonts",
        .open = [](void* context, PrStudioWindow*) {
            *static_cast<bool*>(context) = true;
            return true;
        },
        .build = [](void*, PrStudioWindow*, PrLayoutNode, PrVec2) {},
        .close = [](void* context, PrStudioWindow*) { *static_cast<bool*>(context) = true; },
        .context = &called,
    };
    CHECK(prStudioWindowRun(&desc) == PrStatus_NotFound);
    CHECK_FALSE(called);
}
