#include "pr/draw_fragment.h"

int prTestDrawFragmentCApi(void);

// Tests that the public header compiles as C, with the options a caller fills in.
int prTestDrawFragmentCApi(void) {
    PrDrawFragmentOptions options = prDrawFragmentOptionsDefault();
    options.paths = PrDrawFragmentPaths_Strips;
    options.embeds = PrDrawFragmentEmbeds_Walk;
    PrStatus (*create)(PrGpuDevice*, const PrDrawFragmentOptions*, PrDrawRenderer**) = prDrawFragmentCreate;
    return create == NULL || options.atlasSize < kPrDrawFragmentAtlasMinSize;
}
