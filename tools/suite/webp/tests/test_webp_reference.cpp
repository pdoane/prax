// lib/webp's decoder beside libwebp's, over files that libwebp's encoders write for each feature: lossless images with
// each transform, color caches, palettes and prefix code groups that its methods and qualities choose; lossy images
// over the quality, the loop filter's type, strength and sharpness, segments, partitions and spatial noise shaping;
// lossy images with alpha raw or compressed, under each filter and with its levels reduced; animations of lossy,
// lossless and mixed frames, with the blending, disposal and key frames that the animation encoder chooses; and the
// extended format's chunks. Lossless and alpha decode to libwebp's pixels exactly. A lossy frame decodes to libwebp's
// planes exactly, and to its Rgba8 exactly, since both upsample the chroma alike.

#include "pr/array.h"
#include "pr/image.h"
#include "pr/mem.h"
#include "pr/string_ops.h"

#include "tools/harness/harness_codecs.h"
#include "tools/suite/webp/webp_reference.h"

#include "tools/harness/test.h"

using namespace harness;
using namespace pr;

namespace {

// A sequence of pseudo-random numbers, the same on every run.
struct Sequence {
    uint32_t state = 0x2545F491u;

    uint32_t next() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }
};

// What a picture shows.
enum {
    Content_Photo,      // smooth gradients, with noise and sharp shapes
    Content_Palette,    // a few colors in blocks, with some scattered
    Content_Alpha,      // the photo, under alpha that ranges from transparent to opaque
};
typedef uint8_t Content;

uint8_t clamp8(int32_t v) {
    return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v);
}

Array<uint8_t> picture(uint32_t width, uint32_t height, Content content, uint32_t colors, uint32_t seed) {
    Sequence random;
    random.state ^= seed * 0x9E3779B9u;
    Array<uint8_t> palette(colors * 4);
    for (uint32_t i = 0; i < colors * 4; ++i)
        palette[i] = uint8_t(random.next());
    Array<uint8_t> out(width * height * 4);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            uint8_t* p = out.data() + (size_t(y) * width + x) * 4;
            if (content == Content_Palette) {
                uint32_t index = (x / 5 + (y / 3) * 7) % colors;
                if (random.next() % 11 == 0)
                    index = random.next() % colors;
                prMemCopy(p, palette.data() + index * 4, 4);
                p[3] = colors > 2 ? p[3] | 0x80 : 0xFF;
                continue;
            }
            const int32_t noise = int32_t(random.next() % 25) - 12;
            const bool disc = (x - width / 3) * (x - width / 3) + (y - height / 2) * (y - height / 2) < (width * height) / 9;
            p[0] = clamp8(int32_t(x * 255 / (width == 1 ? 1 : width - 1)) + noise);
            p[1] = clamp8(int32_t(y * 255 / (height == 1 ? 1 : height - 1)) - noise);
            p[2] = disc ? 230 : clamp8(int32_t(((x * y) >> 3) & 0xFF) + noise / 2);
            p[3] = 0xFF;
            if ((x / 7 + y / 9) % 5 == 0)
                p[1] = uint8_t(255 - p[1]);
            if (content == Content_Alpha) {
                const uint32_t band = (x + 2 * y) % 64;
                p[3] = band < 12 ? 0 : band > 50 ? 255 : clamp8(int32_t(band * 4) + noise);
            }
        }
    }
    return out;
}

struct Size {
    uint32_t width;
    uint32_t height;
};

constexpr Size kSizes[] = {{1, 1}, {2, 3}, {17, 13}, {64, 48}, {129, 67}};

void checkSame(const Array<uint8_t>& file) {
    REQUIRE(file.count() != 0);
    webp::Frames reference;
    REQUIRE(webp::referenceFrames(file, &reference));
    webp::Frames prax;
    REQUIRE(webp::praxFrames(file, &prax) == PrStatus_Ok);
    CHECK(prax.width == reference.width);
    CHECK(prax.height == reference.height);
    CHECK(prax.playCount == reference.playCount);
    CHECK(prax.durations == reference.durations);
    REQUIRE(prax.rgba.count() == reference.rgba.count());
    uint32_t differ = 0;
    for (uint32_t i = 0; i < prax.rgba.count(); ++i)
        differ += prax.rgba[i] != reference.rgba[i] ? 1 : 0;
    CHECK(differ == 0);
}

void checkPlanes(const Array<uint8_t>& file) {
    webp::Planes reference;
    REQUIRE(webp::referencePlanes(file, &reference));
    webp::Planes prax;
    REQUIRE(webp::praxPlanes(file, &prax) == PrStatus_Ok);
    CHECK(prax.width == reference.width);
    CHECK(prax.height == reference.height);
    CHECK(prax.y == reference.y);
    CHECK(prax.u == reference.u);
    CHECK(prax.v == reference.v);
}

} // namespace

TEST_CASE("lossless webp decodes to libwebp's pixels") {
    const Content contents[] = {Content_Photo, Content_Palette, Content_Palette, Content_Palette, Content_Alpha};
    const uint32_t colors[] = {0, 2, 4, 16, 0};
    const int methods[] = {0, 3, 6};
    const float qualities[] = {0, 60, 100};
    uint32_t seed = 1;
    for (const Size& size : kSizes) {
        for (uint32_t c = 0; c < 5; ++c) {
            for (int method : methods) {
                for (float quality : qualities) {
                    CAPTURE(size.width);
                    CAPTURE(size.height);
                    CAPTURE(c);
                    CAPTURE(method);
                    CAPTURE(quality);
                    const Array<uint8_t> rgba = picture(size.width, size.height, contents[c], colors[c] + 1, seed++);
                    webp::Options options;
                    options.lossless = true;
                    options.method = method;
                    options.quality = quality;
                    options.exact = true;
                    checkSame(webp::encode(rgba.data(), size.width, size.height, options));
                }
            }
        }
    }
}

TEST_CASE("lossless webp of many colors, palettes and near-lossless preprocessing decodes to libwebp's pixels") {
    const uint32_t colors[] = {3, 5, 17, 200, 256};
    for (uint32_t count : colors) {
        CAPTURE(count);
        const Array<uint8_t> rgba = picture(97, 61, Content_Palette, count, count);
        webp::Options options;
        options.lossless = true;
        options.method = 6;
        options.quality = 100;
        checkSame(webp::encode(rgba.data(), 97, 61, options));
    }
    const int levels[] = {0, 40, 80};
    for (int level : levels) {
        CAPTURE(level);
        const Array<uint8_t> rgba = picture(200, 120, Content_Alpha, 1, uint32_t(level));
        webp::Options options;
        options.lossless = true;
        options.nearLossless = level;
        checkSame(webp::encode(rgba.data(), 200, 120, options));
    }
}

TEST_CASE("lossy webp decodes to libwebp's planes and pixels") {
    struct Case {
        float quality;
        int method;
        int filterType;
        int filterStrength;
        int sharpness;
        int segments;
        int partitions;
        int sns;
    };
    const Case cases[] = {
        {75, 4, 1, 60, 0, 4, 0, 50},
        {5, 0, 0, 100, 0, 1, 0, 0},
        {20, 2, 0, 40, 3, 2, 1, 80},
        {50, 6, 1, 100, 7, 4, 3, 100},
        {95, 3, 1, 20, 2, 3, 2, 25},
        {100, 6, 0, 0, 0, 1, 0, 0},
        {35, 5, 1, 80, 5, 4, 3, 70},
        {60, 1, 0, 70, 1, 3, 1, 10},
    };
    uint32_t seed = 100;
    for (const Size& size : kSizes) {
        for (const Case& c : cases) {
            CAPTURE(size.width);
            CAPTURE(size.height);
            CAPTURE(c.quality);
            CAPTURE(c.method);
            CAPTURE(c.filterType);
            const Array<uint8_t> rgba = picture(size.width, size.height, Content_Photo, 1, seed++);
            webp::Options options;
            options.quality = c.quality;
            options.method = c.method;
            options.filterType = c.filterType;
            options.filterStrength = c.filterStrength;
            options.filterSharpness = c.sharpness;
            options.segments = c.segments;
            options.partitions = c.partitions;
            options.sns = c.sns;
            const Array<uint8_t> file = webp::encode(rgba.data(), size.width, size.height, options);
            checkPlanes(file);
            checkSame(file);
        }
    }
}

TEST_CASE("lossy webp with automatic filtering and sharp chroma decodes to libwebp's pixels") {
    const Array<uint8_t> rgba = picture(320, 240, Content_Photo, 1, 7);
    webp::Options options;
    options.autofilter = true;
    options.sharpYuv = true;
    options.method = 6;
    const Array<uint8_t> file = webp::encode(rgba.data(), 320, 240, options);
    checkPlanes(file);
    checkSame(file);
}

TEST_CASE("lossy webp with alpha decodes to libwebp's pixels") {
    const int kAlphaQualities[] = {100, 40};
    uint32_t seed = 300;
    for (const Size& size : kSizes) {
        for (int compression = 0; compression < 2; ++compression) {
            for (int filtering = 0; filtering < 3; ++filtering) {
                for (int quality : kAlphaQualities) {
                    CAPTURE(size.width);
                    CAPTURE(size.height);
                    CAPTURE(compression);
                    CAPTURE(filtering);
                    CAPTURE(quality);
                    const Array<uint8_t> rgba = picture(size.width, size.height, Content_Alpha, 1, seed++);
                    webp::Options options;
                    options.alphaCompression = compression;
                    options.alphaFiltering = filtering;
                    options.alphaQuality = quality;
                    options.quality = 60;
                    checkSame(webp::encode(rgba.data(), size.width, size.height, options));
                }
            }
        }
    }
}

TEST_CASE("animated webp composes as libwebp's animation decoder composes it") {
    constexpr uint32_t kWidth = 61;
    constexpr uint32_t kHeight = 45;
    constexpr uint32_t kFrames = 6;
    // A sprite that moves over a background that shows through, then a frame that covers the canvas, then the sprite
    // alone over a transparent canvas: the encoder chooses rectangles, blending and disposal among them.
    Array<uint8_t> frames(kWidth * kHeight * 4 * kFrames);
    const Array<uint8_t> photo = picture(kWidth, kHeight, Content_Photo, 1, 9);
    const Array<uint8_t> alpha = picture(kWidth, kHeight, Content_Alpha, 1, 10);
    for (uint32_t f = 0; f < kFrames; ++f) {
        uint8_t* canvas = frames.data() + size_t(f) * kWidth * kHeight * 4;
        const Array<uint8_t>& under = f == 3 ? photo : alpha;
        prMemCopy(canvas, under.data(), under.count());
        if (f >= 4)
            prMemZero(canvas, under.count());
        for (uint32_t y = 0; y < 12; ++y) {
            for (uint32_t x = 0; x < 14; ++x) {
                uint8_t* p = canvas + ((y + 3 * f) * kWidth + x + 7 * f) * 4;
                p[0] = uint8_t(40 * f);
                p[1] = 200;
                p[2] = uint8_t(x * 18);
                p[3] = uint8_t(f % 2 == 0 ? 255 : 128 + y * 8);
            }
        }
    }
    const uint32_t durations[kFrames] = {100, 40, 0, 250, 70, 1000};
    struct Case {
        bool lossless;
        bool mixed;
        bool minimize;
        int kmin;
        int kmax;
    };
    const Case cases[] = {
        {false, false, false, 0, 0},
        {true, false, false, 0, 0},
        {false, true, false, 0, 0},
        {true, false, true, 1, 2},
        {false, false, true, 2, 5},
        {true, true, true, 0, 0},
    };
    for (const Case& c : cases) {
        CAPTURE(c.lossless);
        CAPTURE(c.mixed);
        CAPTURE(c.minimize);
        webp::Options options;
        options.lossless = c.lossless;
        options.exact = c.lossless;
        options.quality = 70;
        webp::AnimationOptions animation;
        animation.allowMixed = c.mixed;
        animation.minimizeSize = c.minimize;
        animation.kmin = c.kmin;
        animation.kmax = c.kmax;
        animation.loopCount = c.lossless ? 3 : 0;
        checkSame(webp::encodeAnimation(frames.data(), durations, kFrames, kWidth, kHeight, options, animation));
    }
}

TEST_CASE("an extended webp's profile and orientation are its chunks'") {
    const Array<uint8_t> rgba = picture(33, 21, Content_Photo, 1, 11);
    webp::Options options;
    options.lossless = true;
    const Array<uint8_t> plain = webp::encode(rgba.data(), 33, 21, options);
    const uint8_t profile[] = {'n', 'o', 't', ' ', 'a', 'n', ' ', 'I', 'C', 'C', ' ', 'p', 'r', 'o', 'f', 'i', 'l', 'e'};
    // A TIFF structure of one entry: orientation 6, a quarter turn clockwise.
    const uint8_t exif[] = {'I', 'I', 42, 0, 8, 0, 0, 0, 1, 0, 0x12, 0x01, 3, 0, 1, 0, 0, 0, 6, 0, 0, 0, 0, 0, 0, 0};
    const uint8_t xmp[] = {'<', 'x', '/', '>'};
    Array<uint8_t> file = webp::withChunk(plain, "ICCP", PrByteView{profile, sizeof(profile)});
    file = webp::withChunk(file, "EXIF", PrByteView{exif, sizeof(exif)});
    file = webp::withChunk(file, "XMP ", PrByteView{xmp, sizeof(xmp)});
    REQUIRE(file.count() != 0);
    checkSame(file);
    PrImageDecoder* decoder = prImageDecoderCreate(harnessImages());
    PrImageSource source = {};
    source.memory = file;
    REQUIRE(prImageDecoderOpen(decoder, source, nullptr) == PrStatus_Ok);
    const PrImageInfo& info = *prImageDecoderInfo(decoder);
    CHECK(info.format.color.encoding == PrImageColorEncoding_Icc);
    CHECK(info.format.color.profile.size == sizeof(profile));
    CHECK(prMemEquals(info.format.color.profile.data, profile, sizeof(profile)));
    CHECK(info.orientation == PrImageOrientation_Rotate90);
    prImageDecoderDestroy(decoder);
}
