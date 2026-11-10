// Tests of Studio's applications that need no platform window.

#include "pr/studio_app.h"

#include "tools/harness/test.h"

extern "C" int prTestStudioAppCApi(void);

TEST_CASE("app: the public headers compile as C") {
    CHECK(prTestStudioAppCApi() == 0);
}
