#include "pr/geom.h"

int prTestGeomCApi(void);

// The public header compiled as C. Values are plain structures.
int prTestGeomCApi(void) {
    PrRect rect = {0, 0, 4, 2};
    PrAffine2 scale = prAffine2Scale((PrVec2){2, 2});
    PrRect mapped = prAffine2MapRect(scale, rect);
    PrCornerRect shape = prCornerRectRound(rect, 1);
    PrAffine2 inverse = prAffine2Identity();
    int result = mapped.right != 8 || mapped.bottom != 4;
    result |= prCornerRectFit(shape).corners[PrRectCorner_BottomLeft].radii.x != 1;
    result |= !prAffine2Invert(scale, &inverse) || inverse.a != 0.5f;
    return result;
}
