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
