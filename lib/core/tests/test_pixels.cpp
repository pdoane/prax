#include "pr/pixels.h"

#include "doctest/doctest.h"

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
