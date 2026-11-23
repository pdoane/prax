// The built-in codec's Photoshop documents: what lib/psd's adapter reports of them, and their JPEG thumbnail as a
// preview.

#include "tools/harness/image_test.h"
#include "tools/harness/jpeg_vectors.h"
#include "tools/harness/psd_document.h"

#include "tools/harness/test.h"

using namespace harness;

namespace pr_impl {

namespace {

// Returns the thumbnail resource that holds `jpeg`.
Array<uint8_t> thumbnailResource(PrByteView jpeg) {
    Array<uint8_t> data;
    psdPut32(&data, 1);
    for (uint32_t i = 0; i < 6; ++i)
        psdPut32(&data, 0);
    data.append(jpeg);
    return psdResource(0x040c, data);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a psd reads as its merged image, with its thumbnail as a preview") {
    // A 16 by 16 RGB document of one color, and the 8 by 8 JPEG thumbnail.
    const uint32_t side = 16;
    Array<uint8_t> planes;
    for (uint32_t c = 0; c < 3; ++c) {
        for (uint32_t i = 0; i < side * side; ++i)
            planes.push(uint8_t(40 + c * 50));
    }
    Array<uint8_t> file = psdFile(PsdLayout{.width = side, .height = side},
        {}, thumbnailResource(PrByteView{kRgb444BaselineBytes, sizeof(kRgb444BaselineBytes)}), planes);
    CHECK(prImageCodecSetSupportsType(testCodec(), "image/vnd.adobe.photoshop"));
    PrImageDecoder* decoder = testImageOpen(file.data(), file.count());
    REQUIRE(decoder != nullptr);
    const PrImageInfo& info = *prImageDecoderInfo(decoder);
    CHECK(info.extent == PrUVec2{side, side});
    CHECK(info.format.model == PrColorModel_Rgb);
    CHECK(info.format.alpha == PrImageAlpha_None);
    REQUIRE(info.previewCount == 1);
    CHECK(prImageDecoderPreviewExtent(decoder, 0) == PrUVec2{8, 8});
    Array<uint8_t> rgba(side * side * 4);
    REQUIRE(testReadRgba8(decoder, 0, false, PrImageAlpha_Straight, rgba.data(), rgba.count()) == PrStatus_Ok);
    CHECK(rgba[0] == 40);
    CHECK(rgba[1] == 90);
    CHECK(rgba[2] == 140);
    Array<uint8_t> preview(8 * 8 * 4);
    REQUIRE(prImageDecoderReadPreviewRgba8(decoder, 0, PrImageAlpha_Straight, PrImageOrientation_Identity,
        PrPixels{preview.data(), 32, {8, 8}}) == PrStatus_Ok);
    CHECK(prMemEquals(preview.data(), kRgb444BaselinePixels, preview.count()));
    prImageDecoderDestroy(decoder);
}

TEST_CASE("a psd without a real merged image is not supported") {
    Array<uint8_t> version({0, 0, 0, 1, 0});
    Array<uint8_t> file = psdFile(PsdLayout{}, {}, psdResource(0x0421, version), Array<uint8_t>(12));
    PrImageSource source{};
    source.memory = file;
    PrImageDecoder* decoder = nullptr;
    CHECK(testDecoderOpen(testCodec(), source, nullptr, &decoder) == PrStatus_NotSupported);
}
