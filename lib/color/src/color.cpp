#include "pr/color.h"

#include "pr/float.h"

PrColor prColorFromColor8(PrColor8 color) {
    return PrColor{color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, color.a / 255.0f};
}

PrColor8 prColor8FromColor(PrColor color) {
    auto channel = [](float value) { return value > 0 ? uint8_t(prF32Round(prF32Min(value, 1) * 255)) : uint8_t(0); };
    return PrColor8{channel(color.r), channel(color.g), channel(color.b), channel(color.a)};
}
