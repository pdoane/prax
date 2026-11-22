#include "pr/array.h"
#include "pr/image_process.h"

#include "tools/harness/test.h"

using namespace pr;

namespace pr_impl {

namespace {

// Returns an Rgba8 image of `extent`. `fill` gives each pixel.
template <typename Fill>
Array<uint8_t> imageOf(PrUVec2 extent, Fill fill) {
    Array<uint8_t> pixels(extent.x * extent.y * 4);
    for (uint32_t y = 0; y < extent.y; ++y) {
        for (uint32_t x = 0; x < extent.x; ++x)
            fill(x, y, &pixels[(y * extent.x + x) * 4]);
    }
    return pixels;
}

// Resizes `pixels` from the extent `from` to the extent `to`.
Array<uint8_t> resized(const Array<uint8_t>& pixels, PrUVec2 from, PrUVec2 to) {
    Array<uint8_t> out(to.x * to.y * 4);
    prImageResize(PrPixelView{pixels.data(), from.x * 4, from}, PrPixels{out.data(), to.x * 4, to});
    return out;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a resize to the same size keeps the pixels, and any resize keeps a constant color") {
    Array<uint8_t> noise = imageOf({7, 5}, [](uint32_t x, uint32_t y, uint8_t* p) {
        p[0] = uint8_t(x * 37 + y * 11);
        p[1] = uint8_t(x * 5 + y * 73);
        p[2] = uint8_t(x * y * 13);
        p[3] = 255;
    });
    CHECK(resized(noise, {7, 5}, {7, 5}) == noise);
    Array<uint8_t> flat = imageOf({9, 6}, [](uint32_t, uint32_t, uint8_t* p) {
        p[0] = 200;
        p[1] = 30;
        p[2] = 90;
        p[3] = 255;
    });
    const PrUVec2 sizes[] = {{3, 2}, {1, 1}, {20, 13}, {9, 1}};
    for (PrUVec2 to : sizes) {
        CAPTURE(to.x);
        CAPTURE(to.y);
        Array<uint8_t> out = resized(flat, {9, 6}, to);
        for (uint32_t i = 0; i < out.count(); i += 4) {
            CHECK(out[i] == 200);
            CHECK(out[i + 1] == 30);
            CHECK(out[i + 2] == 90);
            CHECK(out[i + 3] == 255);
        }
    }
}

TEST_CASE("reducing black and white pixels gives the gray of their mean in linear light") {
    // A checkerboard of single pixels, reduced to half size. The edges repeat. Each result pixel away from the edges
    // mixes black and white evenly. That is half the light, which is 188 in sRGB, not 128.
    Array<uint8_t> board = imageOf({16, 16}, [](uint32_t x, uint32_t y, uint8_t* p) {
        uint8_t v = (x + y) % 2 == 0 ? 255 : 0;
        p[0] = p[1] = p[2] = v;
        p[3] = 255;
    });
    Array<uint8_t> out = resized(board, {16, 16}, {8, 8});
    for (uint32_t y = 2; y < 6; ++y) {
        for (uint32_t x = 2; x < 6; ++x) {
            const uint8_t* p = &out[(y * 8 + x) * 4];
            CHECK(p[0] == doctest::Approx(188).epsilon(0.01));
            CHECK(p[3] == 255);
        }
    }
}

TEST_CASE("a resize does not bleed the color of transparent pixels into their neighbors") {
    // Opaque red beside transparent green. Where the resize blends the two, the color stays red and only the alpha
    // falls.
    Array<uint8_t> half = imageOf({8, 2}, [](uint32_t x, uint32_t, uint8_t* p) {
        p[0] = x < 4 ? 255 : 0;
        p[1] = x < 4 ? 0 : 255;
        p[2] = 0;
        p[3] = x < 4 ? 255 : 0;
    });
    Array<uint8_t> out = resized(half, {8, 2}, {3, 1});
    for (uint32_t i = 0; i < out.count(); i += 4) {
        if (out[i + 3] == 0)
            continue;
        CHECK(out[i] == 255);
        CHECK(out[i + 1] == 0);
    }
    CHECK(out[3] == 255);
    CHECK(out[7] > 0);
    CHECK(out[7] < 255);
}

TEST_CASE("enlarging a single pixel fills the output with that pixel") {
    Array<uint8_t> dot = imageOf({1, 1}, [](uint32_t, uint32_t, uint8_t* p) {
        p[0] = 10;
        p[1] = 120;
        p[2] = 250;
        p[3] = 128;
    });
    Array<uint8_t> out = resized(dot, {1, 1}, {5, 3});
    for (uint32_t i = 0; i < out.count(); i += 4) {
        CHECK(out[i] == 10);
        CHECK(out[i + 1] == 120);
        CHECK(out[i + 2] == 250);
        CHECK(out[i + 3] == 128);
    }
}

TEST_CASE("a blur keeps a constant field, and reads transparent beyond the edges without an edge mode") {
    Array<float> values(8 * 8, 1.0f);
    PrPixels pixels{reinterpret_cast<uint8_t*>(values.data()), 8 * sizeof(float), {8, 8}};
    prImageBlur(pixels, PrPixelFormat_RF32, PrVec2{3, 3}, PrImageEdgeMode_Duplicate);
    for (float v : values)
        CHECK(v == doctest::Approx(1.0f));
    for (float& v : values)
        v = 1.0f;
    prImageBlur(pixels, PrPixelFormat_RF32, PrVec2{1, 1}, PrImageEdgeMode_None);
    CHECK(values[0] < values[4 * 8 + 4]);
    CHECK(values[0] > 0.0f);
}

TEST_CASE("a blur in mirror mode reads the pixels mirrored across the edge, the edge pixel first") {
    // A row of 1, 0, 0, 0 blurred by a narrow Gaussian. Mirror mode repeats the 1 beyond the left edge, wrap mode
    // brings in a 0 from the right edge, and no edge mode reads 0.
    Array<float> mirrored({1, 0, 0, 0});
    Array<float> wrapped({1, 0, 0, 0});
    Array<float> none({1, 0, 0, 0});
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
    Array<float> field(5, 0.5f);
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
    const float seeds[] = {1e30f, -1e30f, 3e9f, -3e9f};
    for (float seed : seeds)
        prImageNoise(seed, &noise);
    for (int32_t i = 0; i < kPrImageNoiseLattice; ++i) {
        CHECK(noise.lattice[i] >= 0);
        CHECK(noise.lattice[i] < kPrImageNoiseLattice);
        CHECK(noise.lattice[kPrImageNoiseLattice + i] == noise.lattice[i]);
    }
}
