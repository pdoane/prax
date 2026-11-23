#include "pr/array.h"
#include "pr/blob.h"
#include "pr/checksum.h"
#include "pr/color_space.h"
#include "pr/compress_select.h"
#include "pr/float16.h"
#include "pr/gpu.h"
#include "pr/gpu_memory.h"
#include "pr/image_select.h"
#include "pr/image_texture_source.h"
#include "pr/mem.h"
#include "pr/texture.h"

#include "tools/harness/animated_gif.h"
#include "tools/harness/texture_await.h"

#include "tools/harness/test.h"

using namespace harness;

namespace pr_impl {

namespace {

// Returns true if `source` reformats to `format`.
bool produces(const PrTextureSource* source, PrGpuFormat format) {
    const PrTextureSource* other = nullptr;
    const bool made = prTextureSourceReformat(source, format, &other) == PrStatus_Ok;
    prTextureSourceRelease(other);
    return made;
}

// The codecs that the tests decode and encode images with.
struct Codecs {
    PrCompressCodecSet* compress = prCompressCodecSetCreateDefault();
    PrImageCodecSet* images = prImageCodecSetCreateDefault(compress);

    Codecs() = default;
    Codecs(const Codecs&) = delete;
    Codecs& operator=(const Codecs&) = delete;
    ~Codecs() {
        prImageCodecSetDestroy(images);
        prCompressCodecSetDestroy(compress);
    }

    // Returns a PNG of `extent` texels of four 8-bit channels each.
    Array<uint8_t> png(PrUVec2 extent, const uint8_t* texels) const {
        PrPixelView pixels{texels, size_t(extent.x) * 4, extent};
        size_t size = 0;
        prImageCodecSetEncode(images, "image/png", pixels, nullptr, 0, &size);
        Array<uint8_t> encoded;
        encoded.resize(uint32_t(size));
        REQUIRE(prImageCodecSetEncode(images, "image/png", pixels, encoded.begin(), size, &size) == PrStatus_Ok);
        return encoded;
    }
};

// A blob source over a file in memory. It counts the number of times its bytes are read.
struct CountedBytes {
    Array<uint8_t> file;
    uint32_t reads = 0;
    PrBlobSource* source = nullptr;

    explicit CountedBytes(Array<uint8_t> bytes) : file(static_cast<Array<uint8_t>&&>(bytes)) {
        PrBlobSourceDesc desc{.start = start, .context = this, .size = kPrBlobSizeUnknown};
        source = prBlobSourceCreate(&desc);
    }
    CountedBytes(const CountedBytes&) = delete;
    CountedBytes& operator=(const CountedBytes&) = delete;
    ~CountedBytes() { prBlobSourceRelease(source); }

    static void start(void* PR_NULLABLE context, PrBlobRead* read) {
        CountedBytes* counted = static_cast<CountedBytes*>(context);
        ++counted->reads;
        prBlobReadComplete(read, PrStatus_Ok, prBlobCopy(PrByteView{counted->file.begin(), counted->file.count()}), 0);
    }
};

// Writes OpenEXR files of one line of halves R, G and B, without compression.
struct ExrBytes {
    Array<uint8_t> out;

    void bytes(const void* data, size_t size) { out.append(PrByteView{static_cast<const uint8_t*>(data), size}); }

    void word(uint32_t value) {
        uint8_t b[4];
        prMemWriteLeU32(b, value);
        bytes(b, 4);
    }

    void real(float value) {
        uint32_t bits = 0;
        prMemCopy(&bits, &value, 4);
        word(bits);
    }

    void text(PrStringView value) {
        bytes(value.data, value.size);
        out.push(0);
    }

    void attribute(PrStringView name, PrStringView type, uint32_t size) {
        text(name);
        text(type);
        word(size);
    }
};

// Returns an OpenEXR file of one line of `width` pixels. Its chromaticities are those of `primaries`. Pixel x has the
// value `values[x]` in each channel.
Array<uint8_t> exrFile(uint32_t width, const float* values, PrColorPrimaries primaries) {
    ExrBytes w;
    w.word(20000630);
    w.word(2);
    w.attribute("channels", "chlist", 3 * 18 + 1);
    const PrStringView names[3] = {"B", "G", "R"};
    const uint32_t channel[4] = {1, 0, 1, 1};
    for (PrStringView name : names) {
        w.text(name);
        for (uint32_t value : channel)
            w.word(value);
    }
    w.out.push(0);
    w.attribute("compression", "compression", 1);
    w.out.push(0);
    const PrStringView windows[2] = {"dataWindow", "displayWindow"};
    const uint32_t box[4] = {0, 0, width - 1, 0};
    for (PrStringView name : windows) {
        w.attribute(name, "box2i", 16);
        for (uint32_t value : box)
            w.word(value);
    }
    w.attribute("lineOrder", "lineOrder", 1);
    w.out.push(0);
    w.attribute("pixelAspectRatio", "float", 4);
    w.real(1);
    w.attribute("screenWindowCenter", "v2f", 8);
    w.real(0);
    w.real(0);
    w.attribute("screenWindowWidth", "float", 4);
    w.real(1);
    PrColorChromaticities c = prColorPrimariesChromaticities(primaries);
    const float points[8] = {c.red.x, c.red.y, c.green.x, c.green.y, c.blue.x, c.blue.y, c.white.x, c.white.y};
    w.attribute("chromaticities", "chromaticities", 32);
    for (float value : points)
        w.real(value);
    w.out.push(0);
    uint8_t offset[8];
    prMemWriteLeU64(offset, w.out.count() + 8);
    w.bytes(offset, 8);
    w.word(0);
    w.word(width * 3 * 2);
    for (uint32_t i = 0; i < 3; ++i) {
        for (uint32_t x = 0; x < width; ++x) {
            PrFloat16 h = prFloat16FromFloat(values[x]);
            w.bytes(&h, 2);
        }
    }
    return static_cast<Array<uint8_t>&&>(w.out);
}

// Returns the 16-bit float at `index` of `texels`.
float halfAt(const Array<uint8_t>& texels, uint32_t index) {
    PrFloat16 h{};
    prMemCopy(&h, texels.begin() + index * 2, 2);
    return prFloat16ToFloat(h);
}

// Creates a source of the image that `bytes` provides with Stored color, within `limits`, or returns the failure.
PrStatus sourceOf(const Codecs& codecs, const PrBlobSource* bytes, const PrTextureSource* PR_NULLABLE* PR_NONNULL out,
        const PrTextureLimits* PR_NULLABLE limits = nullptr) {
    PrTextureFormat* format = prImageTextureFormatCreate(codecs.images, PrImageTextureColor_Stored);
    PrTextureFormatSet* set = prTextureFormatSetCreate(&format, 1);
    PrStatus status = textureOpenAwait(set, bytes, "", limits, out);
    prTextureFormatSetDestroy(set);
    return status;
}

// Reads every level and layer of `source` into `out`. Returns the status of the read.
PrStatus readAll(const PrTextureSource* source, Array<uint8_t>* out) {
    PrTextureReadDesc desc = prTextureReadDescAll(prTextureSourceDesc(source));
    PrTextureRead* read = prTextureReadCreate(source, &desc);
    PrStatus status = textureReadAwait(read);
    out->clear();
    if (status == PrStatus_Ok)
        out->append(PrByteView{prTextureReadStorage(read), prTextureReadSize(prTextureSourceDesc(source), &desc)});
    prTextureReadDestroy(read);
    return status;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_SUITE_BEGIN("image");

TEST_CASE("an image source decodes the pixels of its image, and fails for bytes that do not decode") {
    Codecs codecs;
    const uint8_t texels[8] = {255, 128, 0, 255, 10, 20, 30, 64};
    CountedBytes png(codecs.png({2, 1}, texels));
    const PrTextureSource* source = nullptr;
    REQUIRE(sourceOf(codecs, png.source, &source) == PrStatus_Ok);
    const PrTextureDesc& desc = *prTextureSourceDesc(source);
    CHECK(desc.extent == PrUVec2{2, 1});
    CHECK(desc.alpha == PrTextureAlpha_Straight);
    CHECK(desc.format == PrGpuFormat_Rgba8);
    CHECK(desc.levelCount == 1);
    Array<uint8_t> read;
    REQUIRE(readAll(source, &read) == PrStatus_Ok);
    REQUIRE(read.count() == 8);
    CHECK(prMemEquals(read.begin(), texels, 8));
    prTextureSourceRelease(source);

    // Bytes that are not an image are refused. A PNG that is cut short does not decode.
    const uint8_t garbage[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    PrBlob* blob = prBlobCopy(PrByteView{garbage, sizeof(garbage)});
    PrBlobSource* bytes = prBlobSourceCreateBlob(blob);
    prBlobRelease(blob);
    CHECK(sourceOf(codecs, bytes, &source) == PrStatus_NotSupported);
    CHECK(source == nullptr);
    prBlobSourceRelease(bytes);
    Array<uint8_t> cut;
    cut.append(PrByteView{png.file.begin(), png.file.count() - 20});
    CountedBytes cutBytes(static_cast<Array<uint8_t>&&>(cut));
    PrStatus status = sourceOf(codecs, cutBytes.source, &source);
    if (status == PrStatus_Ok) {
        CHECK(readAll(source, &read) == PrStatus_InvalidData);
        prTextureSourceRelease(source);
    } else {
        CHECK(status == PrStatus_InvalidData);
    }
}

TEST_CASE("an image over the limits is refused from its header when it opens, before any decoding") {
    Codecs codecs;
    const uint8_t texels[4] = {1, 2, 3, 255};
    // A PNG whose header states a width of one past the default extent, and a height of one, is past the limits on one
    // side alone. One whose width is the extent opens, though its data holds one texel, so it never decodes.
    auto statingWidth = [&](uint32_t width) {
        Array<uint8_t> file = codecs.png({1, 1}, texels);
        const uint8_t extent[8] = {
            uint8_t(width >> 24), uint8_t(width >> 16), uint8_t(width >> 8), uint8_t(width),
            0, 0, 0, 1,
        };
        prMemCopy(file.begin() + 16, extent, sizeof(extent));
        uint32_t crc = prCrc32Update(0, PrByteView{file.begin() + 12, 17});
        const uint8_t checked[4] = {uint8_t(crc >> 24), uint8_t(crc >> 16), uint8_t(crc >> 8), uint8_t(crc)};
        prMemCopy(file.begin() + 29, checked, sizeof(checked));
        return file;
    };
    const uint32_t most = prTextureLimitsDefault().extent;
    CountedBytes wide(statingWidth(most + 1));
    const PrTextureSource* source = nullptr;
    CHECK(sourceOf(codecs, wide.source, &source) == PrStatus_Limit);
    CHECK(source == nullptr);
    CountedBytes widest(statingWidth(most));
    REQUIRE(sourceOf(codecs, widest.source, &source) == PrStatus_Ok);
    CHECK(prTextureSourceDesc(source)->extent == PrUVec2{most, 1});
    prTextureSourceRelease(source);

    // The extent bounds each side of the image.
    const uint8_t clear[16] = {};
    CountedBytes small(codecs.png({2, 2}, clear));
    PrTextureLimits limits{.extent = 1};
    CHECK(sourceOf(codecs, small.source, &source, &limits) == PrStatus_Limit);
    limits.extent = 2;
    REQUIRE(sourceOf(codecs, small.source, &source, &limits) == PrStatus_Ok);
    prTextureSourceRelease(source);
    CountedBytes row(codecs.png({3, 1}, clear));
    CHECK(sourceOf(codecs, row.source, &source, &limits) == PrStatus_Limit);
}

TEST_CASE("a float image decodes to floats with its stored values, whatever chromaticities it states") {
    Codecs codecs;
    const float values[2] = {0.5f, 4.0f};
    CountedBytes file(exrFile(2, values, PrColorPrimaries_AcesAp1));
    const PrTextureSource* source = nullptr;
    REQUIRE(sourceOf(codecs, file.source, &source) == PrStatus_Ok);
    CHECK(prTextureSourceDesc(source)->format == PrGpuFormat_Rgba32F);
    // The values of an image of 16-bit floats are unchanged, including values above 1. The image is opaque.
    const PrTextureSource* halves = nullptr;
    REQUIRE(prTextureSourceReformat(source, PrGpuFormat_Rgba16F, &halves) == PrStatus_Ok);
    Array<uint8_t> read;
    REQUIRE(readAll(halves, &read) == PrStatus_Ok);
    REQUIRE(read.count() == 2 * 8);
    CHECK(halfAt(read, 4) == 4.0f);
    CHECK(halfAt(read, 3) == 1.0f);
    prTextureSourceRelease(halves);
    prTextureSourceRelease(source);
}

TEST_CASE("an image source reads the bytes from its provider again for each read, and keeps none of them") {
    Codecs codecs;
    const uint8_t texels[16] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 9, 8, 7, 255};
    CountedBytes png(codecs.png({2, 2}, texels));
    const PrTextureSource* source = nullptr;
    REQUIRE(sourceOf(codecs, png.source, &source) == PrStatus_Ok);
    CHECK(png.reads == 1);
    Array<uint8_t> read;
    for (uint32_t round = 0; round < 2; ++round) {
        REQUIRE(readAll(source, &read) == PrStatus_Ok);
        CHECK(png.reads == round + 2);
        CHECK(prMemEquals(read.begin(), texels, 16));
    }
    prTextureSourceRelease(source);
}

TEST_CASE("an image source reports the orientation stored in its file, and a texture format of the codec opens the "
        "codec's media types") {
    Codecs codecs;
    const uint8_t texels[4] = {1, 2, 3, 255};
    CountedBytes png(codecs.png({1, 1}, texels));
    const PrTextureSource* source = nullptr;
    REQUIRE(sourceOf(codecs, png.source, &source) == PrStatus_Ok);
    CHECK(prTextureSourceDesc(source)->orientation == 0);
    CHECK(produces(source, PrGpuFormat_Rgba32F));
    CHECK(!produces(source, PrGpuFormat_Bc7));
    prTextureSourceRelease(source);
    // A texture format of the codec opens what the codec decodes, chosen by the media type of the bytes.
    PrTextureFormat* format = prImageTextureFormatCreate(codecs.images, PrImageTextureColor_Stored);
    PrTextureFormatSet* opener = prTextureFormatSetCreate(&format, 1);
    CHECK(prTextureFormatSetSupportsType(opener, "image/png"));
    REQUIRE(textureOpenAwait(opener, png.source, "", nullptr, &source) == PrStatus_Ok);
    CHECK(prTextureSourceDesc(source)->extent == PrUVec2{1, 1});
    prTextureSourceRelease(source);
    prTextureFormatSetDestroy(opener);
}

TEST_SUITE_END();

TEST_CASE("an animated image is a source that changes over time: its frames show in turn, each for its duration") {
    Codecs codecs;
    Array<uint8_t> gif = animatedGif(3, 50, true);
    const PrTextureSource* source = nullptr;
    REQUIRE(prImageTextureSourceOpenBytes(PrByteView{gif.data(), gif.count()}, codecs.images, nullptr,
        PrImageTextureColor_Srgb, &source) == PrStatus_Ok);
    CHECK(prTextureSourceChanges(source));
    CHECK(prTextureSourceNotifier(source) == nullptr);
    CHECK(prTextureSourceDesc(source)->extent == PrUVec2{4, 2});
    // The animation starts at time zero of the timeline. The source itself is the first frame, which is red.
    PrTextureShow first{};
    prTextureSourceShow(source, 0, &first);
    CHECK(first.picture == source);
    CHECK(first.next == doctest::Approx(0.5));
    CHECK(first.complete);
    CHECK(!first.notifies);
    Array<uint8_t> texels;
    REQUIRE(readAll(first.picture, &texels) == PrStatus_Ok);
    CHECK(texels[0] == 255);
    CHECK(texels[1] == 0);
    // Half a second later the second frame shows, a still source with the same description. It is green.
    PrTextureShow second{};
    prTextureSourceShow(source, 0.6, &second);
    CHECK(second.picture != source);
    CHECK(second.serial != first.serial);
    CHECK(!prTextureSourceChanges(second.picture));
    CHECK(prTextureSourceDesc(second.picture)->extent == PrUVec2{4, 2});
    CHECK(second.next == doctest::Approx(1.0));
    REQUIRE(readAll(second.picture, &texels) == PrStatus_Ok);
    CHECK(texels[0] == 0);
    CHECK(texels[1] == 255);
    // The frames read in order, and a frame reads again after a later one.
    PrTextureShow third{};
    prTextureSourceShow(source, 1.2, &third);
    REQUIRE(readAll(third.picture, &texels) == PrStatus_Ok);
    CHECK(texels[2] == 255);
    REQUIRE(readAll(second.picture, &texels) == PrStatus_Ok);
    CHECK(texels[1] == 255);
    // After the last frame, the first shows again. The answer is the same whenever it is asked.
    PrTextureShow again{};
    prTextureSourceShow(source, 1.6, &again);
    CHECK(again.picture == source);
    CHECK(again.serial == first.serial);
    CHECK(again.next == doctest::Approx(2.0));
    PrTextureShow repeated{};
    prTextureSourceShow(source, 0.6, &repeated);
    CHECK(repeated.serial == second.serial);
    CHECK(repeated.next == second.next);
    prTextureSourceRelease(repeated.picture);
    prTextureSourceRelease(again.picture);
    prTextureSourceRelease(third.picture);
    prTextureSourceRelease(second.picture);
    prTextureSourceRelease(first.picture);
    prTextureSourceRelease(source);
}

TEST_CASE("frames read at two playback positions in turn each read as the frame they are") {
    Codecs codecs;
    Array<uint8_t> gif = animatedGif(3, 50, true);
    const PrTextureSource* source = nullptr;
    REQUIRE(prImageTextureSourceOpenBytes(PrByteView{gif.data(), gif.count()}, codecs.images, nullptr,
        PrImageTextureColor_Srgb, &source) == PrStatus_Ok);
    PrTextureShow second{};
    PrTextureShow third{};
    prTextureSourceShow(source, 0.6, &second);
    prTextureSourceShow(source, 1.2, &third);
    Array<uint8_t> texels;
    for (uint32_t round = 0; round < 6; ++round) {
        REQUIRE(readAll(third.picture, &texels) == PrStatus_Ok);
        CHECK(texels[2] == 255);
        CHECK(texels[1] == 0);
        REQUIRE(readAll(second.picture, &texels) == PrStatus_Ok);
        CHECK(texels[1] == 255);
        CHECK(texels[2] == 0);
    }
    prTextureSourceRelease(third.picture);
    prTextureSourceRelease(second.picture);
    prTextureSourceRelease(source);
}

TEST_CASE("an animation that plays once stays on its last frame, a frame too short to show takes a tenth of a second, "
        "and an image of one frame is still") {
    Codecs codecs;
    Array<uint8_t> once = animatedGif(2, 1, false);
    const PrTextureSource* source = nullptr;
    REQUIRE(prImageTextureSourceOpenBytes(PrByteView{once.data(), once.count()}, codecs.images, nullptr,
        PrImageTextureColor_Srgb, &source) == PrStatus_Ok);
    PrTextureShow frame{};
    prTextureSourceShow(source, 0, &frame);
    CHECK(frame.next == doctest::Approx(0.1));
    prTextureSourceRelease(frame.picture);
    prTextureSourceShow(source, 0.15, &frame);
    CHECK(frame.picture != source);
    CHECK(frame.next == doctest::Approx(0.2));
    uint64_t last = frame.serial;
    prTextureSourceRelease(frame.picture);
    // Once the frames have played, the last one stays, and nothing follows it.
    prTextureSourceShow(source, 4, &frame);
    CHECK(frame.serial == last);
    CHECK(frame.next == kPrF64Infinity);
    CHECK(!frame.notifies);
    prTextureSourceRelease(frame.picture);
    prTextureSourceRelease(source);

    Array<uint8_t> still = animatedGif(1, 50, true);
    REQUIRE(prImageTextureSourceOpenBytes(PrByteView{still.data(), still.count()}, codecs.images, nullptr,
        PrImageTextureColor_Srgb, &source) == PrStatus_Ok);
    CHECK(!prTextureSourceChanges(source));
    prTextureSourceShow(source, 1, &frame);
    CHECK(frame.picture == source);
    CHECK(frame.next == kPrF64Infinity);
    prTextureSourceRelease(frame.picture);
    prTextureSourceRelease(source);
}
