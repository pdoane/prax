// The built-in codec's WebP images: what lib/webp's adapter reports of them, and how they read through a decoder.

#include "pr/array.h"
#include "pr/mem.h"

#include "tools/harness/image_test.h"
#include "webp/tests/webp_vectors.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

using namespace harness;

const Vector& named(const char* name) {
    for (const Vector& vector : vectors()) {
        if (PrStringView(vector.name) == name)
            return vector;
    }
    FAIL("no vector named ", name);
    return vectors()[0];
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a webp is 8-bit sRGB, with straight alpha if it has alpha, and its frames' durations") {
    CHECK(prImageCodecSetSupportsType(testCodec(), "image/webp"));
    for (const Vector& vector : vectors()) {
        CAPTURE(vector.name);
        PrImageDecoder* decoder = testImageOpen(vector.bytes.data(), vector.bytes.count());
        REQUIRE(decoder != nullptr);
        const PrImageInfo& info = *prImageDecoderInfo(decoder);
        CHECK(info.extent.x == vector.width);
        CHECK(info.extent.y == vector.height);
        CHECK(info.format.model == PrColorModel_Rgb);
        CHECK(info.format.sample == PrImageSample_U8);
        CHECK(info.format.alpha == (vector.alpha ? PrImageAlpha_Straight : PrImageAlpha_None));
        CHECK(info.frameCount == vector.durations.count());
        CHECK(!info.hasCover);
        if (PrStringView(vector.name) != "extended metadata") {
            CHECK(info.format.color.encoding == PrImageColorEncoding_Named);
            CHECK(info.format.color.space == PrColorSpace_Srgb);
            CHECK(info.orientation == PrImageOrientation_Identity);
        }
        for (uint32_t frame = 0; frame < info.frameCount; ++frame) {
            PrImageFrameInfo frameInfo = {};
            REQUIRE(prImageDecoderFrameInfo(decoder, frame, &frameInfo) == PrStatus_Ok);
            CHECK(frameInfo.durationMs == vector.durations[frame]);
        }
        PrImageFrameInfo past = {};
        CHECK(prImageDecoderFrameInfo(decoder, info.frameCount, &past) != PrStatus_Ok);
        prImageDecoderDestroy(decoder);
    }
}

TEST_CASE("an extended webp's color is its profile, and its orientation its exif's") {
    const Vector& vector = named("extended metadata");
    PrImageDecoder* decoder = testImageOpen(vector.bytes.data(), vector.bytes.count());
    REQUIRE(decoder != nullptr);
    const PrImageInfo& info = *prImageDecoderInfo(decoder);
    const PrStringView profile = "not an ICC profile";
    CHECK(info.format.color.encoding == PrImageColorEncoding_Icc);
    REQUIRE(info.format.color.profile.size == profile.size);
    CHECK(prMemEquals(info.format.color.profile.data, profile.data, profile.size));
    CHECK(info.orientation == PrImageOrientation_Rotate90);
    uint32_t width = 0;
    uint32_t height = 0;
    REQUIRE(imageSize(vector.bytes.data(), vector.bytes.count(), Orientation_FromImage, &width, &height));
    CHECK(width == 2);
    CHECK(height == 3);
    prImageDecoderDestroy(decoder);
}

TEST_CASE("an animated webp reads its frames through a decoder, in any order") {
    const Vector& vector = named("animation");
    Animation* animation = animationOpen(vector.bytes.data(), vector.bytes.count(), Orientation_None);
    REQUIRE(animation != nullptr);
    CHECK(animationFrames(animation) == 3);
    CHECK(animationLoops(animation) == 2);
    CHECK(animationDuration(animation) == 240);
    const uint32_t order[] = {0, 1, 2, 2, 1, 0, 2};
    Array<uint8_t> pixels(size_t(vector.width) * vector.height * 4);
    for (uint32_t frame : order) {
        CAPTURE(frame);
        prMemFill(pixels.data(), 0xCD, pixels.count());
        REQUIRE(animationFrame(animation, frame, pixels.data(), pixels.count()));
        CHECK(pixels.span() == vector.frames[frame]);
    }
    CHECK(!animationFrame(animation, 3, pixels.data(), pixels.count()));
    animationClose(animation);
}

TEST_CASE("a webp past the decoder's limits is refused") {
    const Vector& vector = named("animation");
    PrImageLimits limits = prImageLimitsDefault();
    limits.frames = 2;
    PrImageDecoder* decoder = nullptr;
    CHECK(testDecoderOpen(testCodec(), PrImageSource{.memory = {vector.bytes.data(), vector.bytes.count()}}, &limits,
        &decoder) == PrStatus_Limit);
    CHECK(decoder == nullptr);
}
