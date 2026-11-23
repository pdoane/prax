#include "pr/pixels.h"

#include "tools/harness/test.h"

TEST_CASE("each pixel format occupies the bytes of its channels") {
    CHECK(prPixelFormatSize(PrPixelFormat_Rgba8) == 4);
    CHECK(prPixelFormatSize(PrPixelFormat_Bgra8) == 4);
    CHECK(prPixelFormatSize(PrPixelFormat_Rgb10A2) == 4);
    CHECK(prPixelFormatSize(PrPixelFormat_RgbaF16) == 8);
    CHECK(prPixelFormatSize(PrPixelFormat_RgbaF32) == 16);
    CHECK(prPixelFormatSize(PrPixelFormat_R8) == 1);
    CHECK(prPixelFormatSize(PrPixelFormat_RF32) == 4);
    for (uint32_t format = 0; format < PrPixelFormat_Count; ++format)
        CHECK(prPixelFormatSize(PrPixelFormat(format)) != 0);
}

TEST_CASE("a channel of 1 to 16 bits widens by repeating its bits, and narrows to its high 8 bits") {
    for (uint32_t bits = 1; bits <= 16; ++bits) {
        CAPTURE(bits);
        const uint32_t ones = (1u << bits) - 1;
        CHECK(prPixelChannel8(0, bits) == 0);
        CHECK(prPixelChannel8(ones, bits) == 255);
        for (uint32_t value = 0; value <= ones; value += bits > 8 ? 37 : 1) {
            uint32_t expected = value >> (bits > 8 ? bits - 8 : 0);
            if (bits < 8) {
                // The bits of the value, from its highest, repeated to fill the byte.
                expected = 0;
                for (uint32_t i = 0; i < 8; ++i)
                    expected |= (value >> (bits - 1 - i % bits) & 1) << (7 - i);
            }
            CHECK(prPixelChannel8(value, bits) == expected);
        }
    }
    // 5-bit 3 is 24, where rounding 3 * 255 / 31 would give 25.
    CHECK(prPixelChannel8(3, 5) == 24);
    CHECK(prPixelChannel8(11, 6) == 44);
}
