#include "pr/color.h"

PrColor prColorFromColor8(PrColor8 color) {
    return PrColor{color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, color.a / 255.0f};
}
