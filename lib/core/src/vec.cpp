// Vectors.

#include "pr/vec.h"

#include "pr/float.h"

using namespace pr_impl;

PrVec2 prVec2Normalize(PrVec2 vector) {
    float length = sqrtF32(vector.x * vector.x + vector.y * vector.y);
    return length > 0 ? vector / length : PrVec2{0, 0};
}
