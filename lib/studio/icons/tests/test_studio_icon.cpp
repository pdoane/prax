// Studio's icons: every icon of the set has a name and path data that parses whole.

#include "pr/path.h"
#include "pr/path_data.h"
#include "pr/studio_icon.h"

#include "tools/harness/test.h"

TEST_CASE("every icon of the set has a name and path data that parses whole, and an icon out of it has neither") {
    PrPathCache* cache = prPathCacheCreate();
    for (uint32_t i = 1; i < PrStudioIconKind_Count; ++i) {
        PrStudioIconKind icon = PrStudioIconKind(i);
        CHECK(prStudioIconName(icon).size != 0);
        PrPath* path = prPathCreate();
        CHECK(prPathAppendData(path, prStudioIconPathData(icon)) == PrStatus_Ok);
        CHECK(prPathVerbCount(path) != 0);
        prPathDestroy(path);
        // The cache gives the same path for the same data, whose id is the same in every frame.
        CHECK(prPathCacheFind(cache, prStudioIconPathData(icon)) == prPathCacheFind(cache, prStudioIconPathData(icon)));
    }
    CHECK(prStudioIconName(PrStudioIconKind_Count).size == 0);
    CHECK(prStudioIconPathData(PrStudioIconKind_Count).size == 0);
    prPathCacheDestroy(cache);
}
