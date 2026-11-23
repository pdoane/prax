// Pixels in memory.

#include "pr/pixels.h"

uint32_t prPixelFormatSize(PrPixelFormat format) {
    switch (format) {
        case PrPixelFormat_Rgba8:
        case PrPixelFormat_Bgra8:
        case PrPixelFormat_Rgb10A2:
        case PrPixelFormat_RF32:
            return 4;
        case PrPixelFormat_RgbaF16:
            return 8;
        case PrPixelFormat_RgbaF32:
            return 16;
        case PrPixelFormat_R8:
            return 1;
    }
    return 0;
}

uint8_t prPixelChannel8(uint32_t value, uint32_t bits) {
    if (bits >= 8)
        return uint8_t(value >> (bits - 8));
    uint32_t out = 0;
    for (int32_t shift = 8 - int32_t(bits); shift > -int32_t(bits); shift -= int32_t(bits))
        out |= shift >= 0 ? value << shift : value >> -shift;
    return uint8_t(out);
}
