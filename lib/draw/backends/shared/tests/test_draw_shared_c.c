#include "pr/draw_shared_atlas.h"

int prTestDrawSharedCApi(void);

// The public headers compiled as C. Makes an atlas, starts a frame, and reads it back empty.
int prTestDrawSharedCApi(void) {
    PrDrawAtlasOptions atlasOptions = {64, 1, 0, 0.05f};
    PrDrawAtlas* atlas = prDrawAtlasCreate(&atlasOptions);
    prDrawAtlasNext(atlas);
    int result = prDrawAtlasPageCount(atlas) != 0;
    result |= prDrawAtlasInfo(atlas).entryCount != 0;
    prDrawAtlasDestroy(atlas);
    return result;
}
