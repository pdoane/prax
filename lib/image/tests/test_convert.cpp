#include "pr/color_space.h"
#include "pr/image.h"

#include "tools/harness/test.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <type_traits>
#include <vector>

namespace pr_impl {

namespace {

PrImageFormat format(PrImageSample sample, PrImageAlpha alpha = PrImageAlpha_None) {
    return {PrColorModel_Rgb, sample, alpha, {PrImageColorEncoding_Named, PrColorSpace_Srgb, {nullptr, 0}}};
}

// Pixels and their format. Rows are `stride` bytes apart, or else divide the storage evenly.
struct Buffer {
    PrPixels pixels;
    PrImageFormat format;
};

template <typename T, size_t N>
Buffer buffer(T (&data)[N], PrImageFormat pixelFormat, uint32_t width = 1, uint32_t height = 1, size_t stride = 0) {
    uint8_t* bytes = reinterpret_cast<uint8_t*>(const_cast<std::remove_const_t<T>*>(data));
    return {{bytes, stride != 0 ? stride : sizeof(data) / height, {width, height}}, pixelFormat};
}

PrStatus convert(const Buffer& source, const Buffer& destination, PrImageConversionFlags policy) {
    return prImageConvert(prPixelsView(source.pixels), &source.format, destination.pixels, &destination.format, policy);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("image pixel formats size their pixels and reject invalid fields") {
    const PrImageFormat independent = format(PrImageSample_U16, PrImageAlpha_Independent);
    CHECK(prImageFormatSize(&independent) == 8);
    PrImageFormat bad = format(PrImageSample_U8);
    bad.alpha = 255;
    CHECK(prImageFormatSize(&bad) == 0);
    bad = format(PrImageSample_U8);
    bad.model = PrColorModel_Xyz;
    CHECK(prImageFormatSize(&bad) == 0);
    bad = format(PrImageSample_U8);
    bad.color.space = PrColorSpace_XyzD50;
    CHECK(prImageFormatSize(&bad) == 0);
}

TEST_CASE("shared color space models validate image channel meaning") {
    struct Mapping {
        PrColorSpace space;
        PrColorModel model;
    };
    const Mapping mappings[] = {
        {PrColorSpace_Srgb, PrColorModel_Rgb},
        {PrColorSpace_SrgbLinear, PrColorModel_Rgb},
        {PrColorSpace_DisplayP3, PrColorModel_Rgb},
        {PrColorSpace_DisplayP3Linear, PrColorModel_Rgb},
        {PrColorSpace_A98Rgb, PrColorModel_Rgb},
        {PrColorSpace_ProPhotoRgb, PrColorModel_Rgb},
        {PrColorSpace_Rec2020, PrColorModel_Rgb},
        {PrColorSpace_XyzD50, PrColorModel_Xyz},
        {PrColorSpace_XyzD65, PrColorModel_Xyz},
        {PrColorSpace_Lab, PrColorModel_Lab},
        {PrColorSpace_Lch, PrColorModel_Lch},
        {PrColorSpace_Oklab, PrColorModel_Lab},
        {PrColorSpace_Oklch, PrColorModel_Lch},
        {PrColorSpace_Hsl, PrColorModel_Hsl},
        {PrColorSpace_Hwb, PrColorModel_Hwb},
        {PrColorSpace_Rec2100Pq, PrColorModel_Rgb},
        {PrColorSpace_Rec2100Hlg, PrColorModel_Rgb},
        {PrColorSpace_Rec2100Linear, PrColorModel_Rgb},
        {PrColorSpace_Ictcp, PrColorModel_Lab},
        {PrColorSpace_Jzazbz, PrColorModel_Lab},
        {PrColorSpace_Jzczhz, PrColorModel_Lch},
    };
    for (const Mapping& mapping : mappings) {
        CAPTURE(mapping.space);
        CHECK(prColorSpaceModel(mapping.space) == mapping.model);
        PrImageFormat pixelFormat = format(PrImageSample_F32);
        pixelFormat.model = mapping.model;
        pixelFormat.color.space = mapping.space;
        CHECK(prImageFormatSize(&pixelFormat) == 12);
        pixelFormat.model = mapping.model == PrColorModel_Rgb ? PrColorModel_Lab : PrColorModel_Rgb;
        CHECK(prImageFormatSize(&pixelFormat) == 0);
    }
    CHECK(prColorSpaceModel(PrColorSpace_Count) == PrColorModel_Count);
    PrImageFormat invalid = format(PrImageSample_F32);
    invalid.model = PrColorModel_Count;
    CHECK(prImageFormatSize(&invalid) == 0);
}

TEST_CASE("image Lab and Lch conversions retain native ranges and independent alpha") {
    struct Pair {
        PrColorSpace rectangular;
        PrColorSpace polar;
        float lightness;
        float a;
        float b;
        float chroma;
    };
    const Pair pairs[] = {
        {PrColorSpace_Lab, PrColorSpace_Lch, 50.0f, 30.0f, 40.0f, 50.0f},
        {PrColorSpace_Oklab, PrColorSpace_Oklch, 0.5f, 0.03f, 0.04f, 0.05f},
    };
    for (const Pair& pair : pairs) {
        CAPTURE(pair.rectangular);
        float original[] = {pair.lightness, pair.a, pair.b, 0.0f};
        float polar[4] = {};
        float restored[4] = {};
        PrImageFormat lab = format(PrImageSample_F32, PrImageAlpha_Independent);
        lab.model = PrColorModel_Lab;
        lab.color.space = pair.rectangular;
        PrImageFormat lch = lab;
        lch.model = PrColorModel_Lch;
        lch.color.space = pair.polar;
        Buffer source = buffer(original, lab);
        Buffer intermediate = buffer(polar, lch);
        Buffer destination = buffer(restored, lab);
        REQUIRE(convert(source, intermediate, 0) == PrStatus_Ok);
        CHECK(polar[0] == doctest::Approx(pair.lightness));
        CHECK(polar[1] == doctest::Approx(pair.chroma));
        CHECK(polar[2] == doctest::Approx(53.130102));
        CHECK(polar[3] == 0.0f);
        REQUIRE(convert(intermediate, destination, 0) == PrStatus_Ok);
        for (size_t channel = 0; channel < 4; ++channel) {
            CHECK(restored[channel] == doctest::Approx(original[channel]).epsilon(0.0001));
        }
    }
}

TEST_CASE("image RGB and Lab conversions round trip characterized color") {
    float original[] = {0.25f, 0.5f, 0.75f};
    float labSamples[3] = {};
    float restored[3] = {};
    PrImageFormat lab = format(PrImageSample_F32);
    lab.model = PrColorModel_Lab;
    lab.color.space = PrColorSpace_Lab;
    Buffer source = buffer(original, format(PrImageSample_F32));
    Buffer intermediate = buffer(labSamples, lab);
    Buffer destination = buffer(restored, format(PrImageSample_F32));
    REQUIRE(convert(source, intermediate, 0) == PrStatus_Ok);
    CHECK(labSamples[0] > 1.0f);
    REQUIRE(convert(intermediate, destination, 0) == PrStatus_Ok);
    for (size_t channel = 0; channel < 3; ++channel) {
        CHECK(restored[channel] == doctest::Approx(original[channel]).epsilon(0.0001));
    }
}

TEST_CASE("image hue models use degrees and percentage channels") {
    for (PrColorSpace space : {PrColorSpace_Hsl, PrColorSpace_Hwb}) {
        float original[] = {
            120.0f, space == PrColorSpace_Hsl ? 100.0f : 0.0f,
            space == PrColorSpace_Hsl ? 50.0f : 0.0f,
        };
        float result[3] = {};
        PrImageFormat hue = format(PrImageSample_F32);
        hue.model = prColorSpaceModel(space);
        hue.color.space = space;
        Buffer source = buffer(original, hue);
        Buffer destination = buffer(result, format(PrImageSample_F32));
        REQUIRE(convert(source, destination, 0) == PrStatus_Ok);
        CHECK(result[0] == doctest::Approx(0.0).epsilon(0.0001));
        CHECK(result[1] == doctest::Approx(1.0).epsilon(0.0001));
        CHECK(result[2] == doctest::Approx(0.0).epsilon(0.0001));
    }
}

TEST_CASE("image conversion preserves U16 precision through floating point") {
    uint16_t original[] = {1, 32769, 65534};
    float linear[3] = {};
    uint16_t restored[3] = {};
    Buffer source = buffer(original, format(PrImageSample_U16));
    Buffer intermediate = buffer(linear, format(PrImageSample_F32));
    Buffer destination = buffer(restored, format(PrImageSample_U16));
    REQUIRE(convert(source, intermediate, 0) == PrStatus_Ok);
    CHECK(linear[0] == doctest::Approx(1.0 / 65535.0));
    CHECK(linear[1] == doctest::Approx(32769.0 / 65535.0));
    REQUIRE(convert(intermediate, destination, 0) == PrStatus_Ok);
    for (size_t channel = 0; channel < 3; ++channel) {
        CHECK(restored[channel] == original[channel]);
    }
}

TEST_CASE("image conversion honors signed normalized sample endpoints") {
    int16_t original[] = {-32768, 0, 32767};
    float result[3] = {};
    Buffer source = buffer(original, format(PrImageSample_I16));
    Buffer destination = buffer(result, format(PrImageSample_F32));
    REQUIRE(convert(source, destination, 0) == PrStatus_Ok);
    CHECK(result[0] == -1.0f);
    CHECK(result[1] == 0.0f);
    CHECK(result[2] == 1.0f);
}

TEST_CASE("image half float conversion preserves values beyond unit range") {
    float original[] = {-2.0f, 0.5f, 8.0f};
    uint16_t halves[3] = {};
    float result[3] = {};
    Buffer source = buffer(original, format(PrImageSample_F32));
    Buffer intermediate = buffer(halves, format(PrImageSample_F16));
    Buffer destination = buffer(result, format(PrImageSample_F32));
    REQUIRE(convert(source, intermediate, 0) == PrStatus_Ok);
    CHECK(halves[0] == 0xC000);
    CHECK(halves[1] == 0x3800);
    CHECK(halves[2] == 0x4800);
    REQUIRE(convert(intermediate, destination, 0) == PrStatus_Ok);
    for (size_t channel = 0; channel < 3; ++channel) {
        CHECK(result[channel] == original[channel]);
    }
}

TEST_CASE("independent alpha does not weight color at zero or fractional values") {
    float original[] = {0.25f, 0.5f, 1.0f, 0.0f, 0.5f, 0.25f, 0.75f, 0.125f};
    float result[8] = {};
    PrImageFormat inputFormat = format(PrImageSample_F32, PrImageAlpha_Independent);
    inputFormat.color.space = PrColorSpace_SrgbLinear;
    Buffer source = buffer(original, inputFormat, 2);
    Buffer destination = buffer(result, format(PrImageSample_F32, PrImageAlpha_Independent), 2);
    REQUIRE(convert(source, destination, 0) == PrStatus_Ok);
    CHECK(result[0] == doctest::Approx(0.5370987));
    CHECK(result[1] == doctest::Approx(0.735357));
    CHECK(result[2] == doctest::Approx(1.0));
    CHECK(result[3] == 0.0f);
    CHECK(result[4] == doctest::Approx(0.735357));
    CHECK(result[7] == 0.125f);
}

TEST_CASE("independent alpha cannot be dropped introduced or reinterpreted as opacity") {
    float original[] = {0.2f, 0.3f, 0.4f, 0.0f};
    float result[4] = {};
    for (PrImageAlpha alpha :
            {PrImageAlpha_None, PrImageAlpha_Straight, PrImageAlpha_Premultiplied}) {
        Buffer source = buffer(original, format(PrImageSample_F32, PrImageAlpha_Independent));
        Buffer destination = buffer(result, format(PrImageSample_F32, alpha));
        CHECK(convert(source, destination, 0) == PrStatus_NotSupported);
        source.format.alpha = alpha;
        destination.format.alpha = PrImageAlpha_Independent;
        CHECK(convert(source, destination, 0) == PrStatus_NotSupported);
    }
}

TEST_CASE("independent alpha preserves sample precision in representation conversion") {
    uint16_t original[] = {12345, 34567, 45678, 1};
    float result[4] = {};
    uint16_t restored[4] = {};
    Buffer source = buffer(original, format(PrImageSample_U16, PrImageAlpha_Independent));
    Buffer intermediate = buffer(result, format(PrImageSample_F32, PrImageAlpha_Independent));
    Buffer destination = buffer(restored, format(PrImageSample_U16, PrImageAlpha_Independent));
    REQUIRE(convert(source, intermediate, 0) == PrStatus_Ok);
    REQUIRE(convert(intermediate, destination, 0) == PrStatus_Ok);
    for (size_t channel = 0; channel < 4; ++channel) {
        CHECK(restored[channel] == original[channel]);
    }
}

TEST_CASE("straight and premultiplied alpha conversion associates stored color") {
    float original[] = {0.25f, 0.5f, 1.0f, 0.5f};
    float associated[4] = {};
    float restored[4] = {};
    Buffer source = buffer(original, format(PrImageSample_F32, PrImageAlpha_Straight));
    Buffer intermediate = buffer(associated, format(PrImageSample_F32, PrImageAlpha_Premultiplied));
    Buffer destination = buffer(restored, format(PrImageSample_F32, PrImageAlpha_Straight));
    REQUIRE(convert(source, intermediate, 0) == PrStatus_Ok);
    CHECK(associated[0] == 0.125f);
    CHECK(associated[1] == 0.25f);
    CHECK(associated[2] == 0.5f);
    CHECK(associated[3] == 0.5f);
    REQUIRE(convert(intermediate, destination, 0) == PrStatus_Ok);
    for (size_t channel = 0; channel < 4; ++channel) {
        CHECK(restored[channel] == original[channel]);
    }
}

TEST_CASE("unknown image color permits representation changes but needs cross-space policy") {
    uint16_t original[] = {0, 32768, 65535};
    float result[3] = {};
    Buffer source = buffer(original, format(PrImageSample_U16));
    Buffer destination = buffer(result, format(PrImageSample_F32));
    source.format.color = {};
    destination.format.color = {};
    REQUIRE(convert(source, destination, 0) == PrStatus_Ok);
    CHECK(result[1] == doctest::Approx(32768.0 / 65535.0));
    destination.format.color = {PrImageColorEncoding_Named, PrColorSpace_SrgbLinear, {nullptr, 0}};
    CHECK(convert(source, destination, 0) == PrStatus_NotSupported);
    PrImageConversionFlags conversion = PrImageConversionFlag_AssumeSrgb;
    REQUIRE(convert(source, destination, conversion) == PrStatus_Ok);
    CHECK(result[1] == doctest::Approx(0.2140482).epsilon(0.0001));
}

TEST_CASE("CMYK conversion requires explicit approximation and keeps K out of alpha") {
    float original[] = {0.25f, 0.5f, 0.75f, 0.5f};
    float result[4] = {};
    PrImageFormat cmyk = {PrColorModel_Cmyk, PrImageSample_F32, PrImageAlpha_None, {}};
    Buffer source = buffer(original, cmyk);
    Buffer destination = buffer(result, format(PrImageSample_F32, PrImageAlpha_Straight));
    CHECK(convert(source, destination, 0) == PrStatus_NotSupported);
    PrImageConversionFlags conversion = PrImageConversionFlag_AssumeSrgb;
    CHECK(convert(source, destination, conversion) == PrStatus_NotSupported);
    conversion |= PrImageConversionFlag_ApproximateCmyk;
    REQUIRE(convert(source, destination, conversion) == PrStatus_Ok);
    CHECK(result[0] == 0.375f);
    CHECK(result[1] == 0.25f);
    CHECK(result[2] == 0.125f);
    CHECK(result[3] == 1.0f);
}

TEST_CASE("ICC image color cannot silently use an sRGB fallback") {
    uint8_t profile[128] = {};
    uint8_t original[] = {10, 20, 30};
    float result[3] = {};
    Buffer source = buffer(original, format(PrImageSample_U8));
    source.format.color = {PrImageColorEncoding_Icc, 0, {profile, sizeof(profile)}};
    Buffer destination = buffer(result, format(PrImageSample_F32));
    PrImageConversionFlags conversion = PrImageConversionFlag_AssumeSrgb | PrImageConversionFlag_ApproximateCmyk;
    CHECK(convert(source, destination, conversion) == PrStatus_NotSupported);
    destination.format.color = source.format.color;
    REQUIRE(convert(source, destination, 0) == PrStatus_Ok);
    CHECK(result[1] == doctest::Approx(20.0 / 255.0));
}

TEST_CASE("ICC image color converts through its profile") {
    // An RGB profile with linear curves whose red and green colorants are sRGB's green and red, under D50.
    auto putU32 = [](std::vector<uint8_t>& out, uint32_t v) {
        for (int shift = 24; shift >= 0; shift -= 8)
            out.push_back(uint8_t(v >> shift));
    };
    auto text = [](std::vector<uint8_t>& out, const char* four) { out.insert(out.end(), four, four + 4); };
    auto fixed = [&](std::vector<uint8_t>& out, double v) { putU32(out, uint32_t(int32_t(v * 65536 + 0.5))); };
    const char* names[] = {"rXYZ", "gXYZ", "bXYZ", "rTRC", "gTRC", "bTRC"};
    const double columns[3][3] = {{0.3851, 0.7169, 0.0971}, {0.4361, 0.2225, 0.0139}, {0.1431, 0.0606, 0.7141}};
    std::vector<uint8_t> elements[6];
    for (uint32_t i = 0; i < 3; ++i) {
        text(elements[i], "XYZ ");
        putU32(elements[i], 0);
        for (double v : columns[i])
            fixed(elements[i], v);
        text(elements[3 + i], "curv");
        putU32(elements[3 + i], 0);
        putU32(elements[3 + i], 0);
    }
    std::vector<uint8_t> icc(128, 0);
    uint32_t at = 132 + 12 * 6;
    std::vector<uint8_t> table;
    putU32(table, 6);
    for (uint32_t i = 0; i < 6; ++i) {
        text(table, names[i]);
        putU32(table, at);
        putU32(table, uint32_t(elements[i].size()));
        at += uint32_t(elements[i].size());
    }
    icc.insert(icc.end(), table.begin(), table.end());
    for (const std::vector<uint8_t>& element : elements)
        icc.insert(icc.end(), element.begin(), element.end());
    std::vector<uint8_t> size;
    putU32(size, uint32_t(icc.size()));
    for (uint32_t i = 0; i < 4; ++i) {
        icc[i] = size[i];
        icc[16 + i] = uint8_t("RGB "[i]);
        icc[20 + i] = uint8_t("XYZ "[i]);
        icc[36 + i] = uint8_t("acsp"[i]);
    }
    uint8_t original[] = {255, 0, 0};
    float result[3] = {};
    Buffer source = buffer(original, format(PrImageSample_U8));
    source.format.color = {PrImageColorEncoding_Icc, 0, {icc.data(), icc.size()}};
    Buffer destination = buffer(result, format(PrImageSample_F32));
    REQUIRE(convert(source, destination, 0) == PrStatus_Ok);
    // Within the rounding of the profile's fixed-point colorants.
    CHECK(result[0] > -0.005f);
    CHECK(result[0] < 0.005f);
    CHECK(result[1] == doctest::Approx(1).epsilon(0.005));
    CHECK(result[2] > -0.005f);
    CHECK(result[2] < 0.005f);
}

TEST_CASE("image conversion respects independent source and destination strides") {
    uint8_t original[] = {10, 20, 30, 0xAA, 0xAA, 40, 50, 60};
    uint16_t result[8];
    for (uint16_t& sample : result)
        sample = 0xA5A5;
    Buffer source = buffer(original, format(PrImageSample_U8), 1, 2, 5);
    Buffer destination = buffer(result, format(PrImageSample_U16), 1, 2, 8);
    REQUIRE(convert(source, destination, 0) == PrStatus_Ok);
    CHECK(result[0] == 10 * 257);
    CHECK(result[2] == 30 * 257);
    CHECK(result[3] == 0xA5A5);
    CHECK(result[4] == 40 * 257);
    CHECK(result[6] == 60 * 257);
    CHECK(result[7] == 0xA5A5);
}

TEST_CASE("identical floating image formats preserve NaN payload and signed zero") {
    uint32_t patterns[] = {0x7FC12345u, 0x80000000u, 0x7F800000u};
    float original[3];
    float result[3] = {};
    std::memcpy(original, patterns, sizeof(original));
    Buffer source = buffer(original, format(PrImageSample_F32));
    Buffer destination = buffer(result, format(PrImageSample_F32));
    REQUIRE(convert(source, destination, 0) == PrStatus_Ok);
    CHECK(std::memcmp(original, result, sizeof(original)) == 0);
}

TEST_CASE("nonfinite floating image samples do not undergo undefined integer casts") {
    float original[] = {
        std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(),
    };
    uint8_t result[3] = {};
    Buffer source = buffer(original, format(PrImageSample_F32));
    Buffer destination = buffer(result, format(PrImageSample_U8));
    PrStatus status = convert(source, destination, 0);
    CHECK((status == PrStatus_Ok || status == PrStatus_InvalidData));
    if (status == PrStatus_Ok) {
        CHECK(result[1] == 255);
        CHECK(result[2] == 0);
    }
}

TEST_CASE("eight-bit premultiplying and unpremultiplying round the exact value half up") {
    // Every color and alpha, as one row of opaque-gray-to-RGBA pixels.
    static uint8_t straight[256 * 256 * 4];
    static uint8_t premultiplied[256 * 256 * 4];
    static uint8_t restored[256 * 256 * 4];
    for (uint32_t alpha = 0; alpha < 256; ++alpha) {
        for (uint32_t value = 0; value < 256; ++value) {
            uint8_t* p = straight + (alpha * 256 + value) * 4;
            p[0] = p[1] = p[2] = uint8_t(value);
            p[3] = uint8_t(alpha);
        }
    }
    Buffer from = buffer(straight, format(PrImageSample_U8, PrImageAlpha_Straight), 256, 256);
    Buffer to = buffer(premultiplied, format(PrImageSample_U8, PrImageAlpha_Premultiplied), 256, 256);
    REQUIRE(convert(from, to, 0) == PrStatus_Ok);
    Buffer back = buffer(restored, format(PrImageSample_U8, PrImageAlpha_Straight), 256, 256);
    REQUIRE(convert(to, back, 0) == PrStatus_Ok);
    uint32_t wrong = 0;
    for (uint32_t alpha = 0; alpha < 256; ++alpha) {
        for (uint32_t value = 0; value < 256; ++value) {
            const uint8_t* p = premultiplied + (alpha * 256 + value) * 4;
            const uint32_t expected = uint32_t(std::floor(value * alpha / 255.0 + 0.5));
            wrong += p[0] != expected || p[3] != alpha;
            // Unpremultiplying the premultiplied value, clamped to one.
            const uint32_t unpremultiplied = alpha == 0 ? 0 : std::min(255u, uint32_t(std::floor(p[0] * 255.0 / alpha + 0.5)));
            wrong += restored[(alpha * 256 + value) * 4] != unpremultiplied;
        }
    }
    CHECK(wrong == 0);
}
