#include "pr/image_process.h"

#include "tools/harness/test.h"

#include <initializer_list>
#include <vector>

TEST_CASE("a blur keeps a constant field, and reads transparent beyond the edges without an edge mode") {
    std::vector<float> values(8 * 8, 1.0f);
    PrPixels pixels{reinterpret_cast<uint8_t*>(values.data()), 8 * sizeof(float), {8, 8}};
    prImageBlur(pixels, PrPixelFormat_RF32, PrVec2{3, 3}, PrImageEdgeMode_Duplicate);
    for (float v : values)
        CHECK(v == doctest::Approx(1.0f));
    values.assign(8 * 8, 1.0f);
    prImageBlur(pixels, PrPixelFormat_RF32, PrVec2{1, 1}, PrImageEdgeMode_None);
    CHECK(values[0] < values[4 * 8 + 4]);
    CHECK(values[0] > 0.0f);
}

TEST_CASE("a blur in mirror mode reads the pixels mirrored across the edge, the edge pixel first") {
    // A row of 1, 0, 0, 0 blurred by a narrow Gaussian. Mirror mode repeats the 1 beyond the left edge, wrap mode
    // brings in a 0 from the right edge, and no edge mode reads 0.
    std::vector<float> mirrored{1, 0, 0, 0};
    std::vector<float> wrapped{1, 0, 0, 0};
    std::vector<float> none{1, 0, 0, 0};
    PrPixels mirror{reinterpret_cast<uint8_t*>(mirrored.data()), 4 * sizeof(float), {4, 1}};
    PrPixels wrap{reinterpret_cast<uint8_t*>(wrapped.data()), 4 * sizeof(float), {4, 1}};
    PrPixels transparent{reinterpret_cast<uint8_t*>(none.data()), 4 * sizeof(float), {4, 1}};
    prImageBlur(mirror, PrPixelFormat_RF32, PrVec2{1, 0}, PrImageEdgeMode_Mirror);
    prImageBlur(wrap, PrPixelFormat_RF32, PrVec2{1, 0}, PrImageEdgeMode_Wrap);
    prImageBlur(transparent, PrPixelFormat_RF32, PrVec2{1, 0}, PrImageEdgeMode_None);
    CHECK(mirrored[0] > wrapped[0] + 0.1f);
    CHECK(wrapped[0] == doctest::Approx(none[0]).epsilon(0.01));
    // Beyond the right edge, mirror mode reads the 0s next to the edge, as no edge mode does.
    CHECK(mirrored[3] == doctest::Approx(none[3]).epsilon(0.05));
    // A constant field stays constant.
    std::vector<float> field(5, 0.5f);
    PrPixels constant{reinterpret_cast<uint8_t*>(field.data()), 5 * sizeof(float), {5, 1}};
    prImageBlur(constant, PrPixelFormat_RF32, PrVec2{3, 0}, PrImageEdgeMode_Mirror);
    for (float v : field)
        CHECK(v == doctest::Approx(0.5f));
}

TEST_CASE("a mip level is the rounded mean of each block, repeating the last row or column") {
    const uint8_t above[3 * 4] = {0, 10, 20, 255, 100, 30, 40, 255, 200, 50, 60, 255};
    uint8_t level[4] = {};
    prImageMipLevel(PrPixelView{above, sizeof(above), {3, 1}}, PrPixels{level, sizeof(level), {1, 1}},
        PrPixelFormat_Rgba8);
    CHECK(level[0] == 50);
    CHECK(level[1] == 20);
    CHECK(level[2] == 30);
    CHECK(level[3] == 255);
}

TEST_CASE("turbulence noise takes any finite seed") {
    PrImageNoise noise{};
    for (float seed : {1e30f, -1e30f, 3e9f, -3e9f})
        prImageNoise(seed, &noise);
    for (int32_t i = 0; i < kPrImageNoiseLattice; ++i) {
        CHECK(noise.lattice[i] >= 0);
        CHECK(noise.lattice[i] < kPrImageNoiseLattice);
        CHECK(noise.lattice[kPrImageNoiseLattice + i] == noise.lattice[i]);
    }
}
