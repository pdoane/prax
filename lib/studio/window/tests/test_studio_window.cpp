// Tests of Studio's window that need no platform window.

#include "pr/studio_window.h"

#include "tools/harness/test.h"

extern "C" int prTestStudioWindowCApi(void);

TEST_CASE("window: the public header compiles as C") {
    CHECK(prTestStudioWindowCApi() == 0);
}
