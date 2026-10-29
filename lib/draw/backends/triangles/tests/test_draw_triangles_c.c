#include "pr/draw_triangles.h"

int prTestDrawTrianglesCApi(void);

// Tests that the public header compiles as C, with the options a caller fills in.
int prTestDrawTrianglesCApi(void) {
    PrDrawTrianglesOptions options = prDrawTrianglesOptionsDefault();
    options.atlasSize = 512;
    PrStatus (*create)(PrGpuDevice*, const PrDrawTrianglesOptions*, PrDrawRenderer**) = prDrawTrianglesCreate;
    return create == NULL || options.glyphDecoder != NULL;
}
