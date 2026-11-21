// Tests of Studio's applications.

#include "pr/array.h"
#include "pr/environment.h"
#include "pr/font_set.h"
#include "pr/studio_app.h"

#include "tools/harness/test.h"

extern "C" int prTestStudioAppCApi(void);

TEST_CASE("app: the public headers compile as C") {
    CHECK(prTestStudioAppCApi() == 0);
}

using namespace pr;

namespace pr_impl {
namespace {

// What the window of a run reported.
struct Reports {
    Array<PrStatus> drawStatuses;
    bool closed = false;
    PrStatus closeStatus = PrStatus_Ok;
};

} // namespace
} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("app: a window whose swap chain cannot be made says why it does not draw, and stays open") {
    // The memory device makes no swap chains, so the window can never draw.
    REQUIRE(prEnvironmentSetVariable("PR_BACKEND_GPU", "memory"));
    PrFontSet* fonts = prFontSetCreate(nullptr);
    Reports reports;
    PrStudioWindowDesc window{
        .title = "Draw status",
        .extent = {200, 100},
        .build = [](void*, PrStudioWindow*, PrLayoutNode, PrVec2) {},
        .drawStatus = [](void* context, PrStudioWindow* w, PrStatus status) {
            static_cast<Reports*>(context)->drawStatuses.push(status);
            prStudioAppStop(prStudioWindowStudioApp(w));
        },
        .close = [](void* context, PrStudioWindow*, PrStatus status) {
            Reports& r = *static_cast<Reports*>(context);
            r.closed = true;
            r.closeStatus = status;
        },
        .context = &reports,
    };
    PrStudioAppDesc app{.fonts = fonts, .window = &window};
    PrStatus status = prStudioAppRun(&app);
    prEnvironmentRemoveVariable("PR_BACKEND_GPU");
    prFontSetDestroy(fonts);
    if (status == PrStatus_NotSupported)
        return;     // the build has no application backend
    CHECK(status == PrStatus_Ok);
    // It is told once, and not again for each frame that still cannot draw.
    REQUIRE(reports.drawStatuses.count() == 1);
    CHECK(reports.drawStatuses[0] == PrStatus_NotSupported);
    // The window opened, so it closes without a failure of its own.
    CHECK(reports.closed);
    CHECK(reports.closeStatus == PrStatus_Ok);
}
