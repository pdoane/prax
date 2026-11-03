#include "pr/bit_cast.h"
#include "pr/color.h"
#include "pr/color_space.h"
#include "pr/float.h"

#include "doctest/doctest.h"

#include <cmath>
#include <initializer_list>

extern "C" int prTestColorCApi(void);

namespace pr_impl {

namespace {

// A color of 16-bit floats made at compile time.
constexpr PrColor16 kLiteral = PR_COLOR16(0.5f, -0.0f, 1, 0.25f);
static_assert(prFloat16ToFloat(kLiteral.r) == 0.5f);
static_assert(bitCast<uint16_t>(kLiteral.g) == 0);
static_assert(sizeof(PrColor16) == 8);

// Returns the color in `space` as absolute XYZ, in cd/m², with SDR white at 203.
void absoluteXyz(PrColorSpace space, double a, double b, double c, double out[3]) {
    const double in[3] = {a, b, c};
    prColorConvert(space, PrColorSpace_XyzD65, in, out);
    for (uint32_t i = 0; i < 3; ++i)
        out[i] *= 203;
}

// Returns the extended sRGB of a color in `space`, opaque.
PrColor srgbOf(PrColorSpace space, double a, double b, double c) {
    const double in[3] = {a, b, c};
    double out[3];
    prColorConvert(space, PrColorSpace_Srgb, in, out);
    return PrColor{float(out[0]), float(out[1]), float(out[2]), 1};
}

// Returns the color's channels in `space`.
void channelsOf(PrColor color, PrColorSpace space, double out[3]) {
    const double in[3] = {double(color.r), double(color.g), double(color.b)};
    prColorConvert(PrColorSpace_Srgb, space, in, out);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the public headers compile as C") {
    CHECK(prTestColorCApi() == 0);
}

TEST_CASE("every 8-bit channel survives a 16-bit float and back") {
    for (uint32_t code = 0; code < 256; ++code) {
        float channel = prColorFromColor8(PrColor8{uint8_t(code), 0, 0, 255}).r;
        float back = prColorFromColor16(prColor16FromColor(PrColor{channel, 0, 0, 1})).r;
        CHECK(uint32_t(back * 255 + 0.5f) == code);
    }
}

TEST_CASE("a color of 16-bit floats holds no -0 and no NaN, so equal colors have equal bits") {
    PrColor16 zero = prColor16FromColor(PrColor{0, 0, 0, 0});
    PrColor16 negative = prColor16FromColor(PrColor{-0.0f, -0.0f, kPrF32Nan, 0});
    CHECK(bitCast<uint64_t>(negative) == bitCast<uint64_t>(zero));
    // Extended sRGB keeps channels past [0, 1].
    PrColor wide = prColorFromColor16(prColor16FromColor(PrColor{-0.5f, 1.5f, 2, 1}));
    CHECK(wide.r == -0.5f);
    CHECK(wide.g == 1.5f);
    CHECK(wide.b == 2.0f);
}

TEST_CASE("the HDR spaces convert with SDR white at 203 cd/m2, and back") {
    double xyz[3];
    absoluteXyz(PrColorSpace_Rec2100Linear, 0.9, 1.0, 0.8, xyz);
    CHECK(xyz[0] == doctest::Approx(173.156).epsilon(1e-3));
    CHECK(xyz[1] == doctest::Approx(195.260).epsilon(1e-3));
    CHECK(xyz[2] == doctest::Approx(178.003).epsilon(1e-3));
    absoluteXyz(PrColorSpace_Ictcp, 0.5, -0.3, 0, xyz);
    CHECK(xyz[0] == doctest::Approx(63.6).epsilon(1e-2));
    CHECK(xyz[1] == doctest::Approx(96.53).epsilon(1e-2));
    CHECK(xyz[2] == doctest::Approx(8.558).epsilon(1e-2));
    absoluteXyz(PrColorSpace_Ictcp, 0.85, -0.35, 0.1, xyz);
    CHECK(xyz[0] == doctest::Approx(2521).epsilon(1e-2));
    CHECK(xyz[1] == doctest::Approx(2508).epsilon(1e-2));
    absoluteXyz(PrColorSpace_Jzczhz, 0.7, 0.14, 100, xyz);
    CHECK(xyz[0] == doctest::Approx(3776.1434).epsilon(1e-3));
    CHECK(xyz[1] == doctest::Approx(4362.407).epsilon(1e-3));
    CHECK(xyz[2] == doctest::Approx(1577.913).epsilon(1e-3));

    // SDR white is PQ's 0.5807, HLG's 0.75 and Jzazbz's lightness of 0.2220.
    const double white[3] = {1, 1, 1};
    double out[3];
    prColorConvert(PrColorSpace_Srgb, PrColorSpace_Rec2100Pq, white, out);
    CHECK(out[1] == doctest::Approx(0.5807).epsilon(1e-3));
    prColorConvert(PrColorSpace_Srgb, PrColorSpace_Rec2100Hlg, white, out);
    CHECK(out[1] == doctest::Approx(0.75).epsilon(1e-3));
    prColorConvert(PrColorSpace_Srgb, PrColorSpace_Jzazbz, white, out);
    CHECK(out[0] == doctest::Approx(0.2220).epsilon(1e-3));

    // Each space converts back to the color it came from.
    const double color[3] = {0.8, 0.3, 1.6};
    for (PrColorSpace space : {PrColorSpace_Rec2100Pq, PrColorSpace_Rec2100Hlg, PrColorSpace_Rec2100Linear,
        PrColorSpace_Ictcp, PrColorSpace_Jzazbz, PrColorSpace_Jzczhz}) {
        CAPTURE(space);
        double there[3];
        double back[3];
        prColorConvert(PrColorSpace_Srgb, space, color, there);
        prColorConvert(space, PrColorSpace_Srgb, there, back);
        for (uint32_t i = 0; i < 3; ++i)
            CHECK(back[i] == doctest::Approx(color[i]).epsilon(1e-6));
    }
}

TEST_CASE("scRGB converts to HDR10's PQ by Rec. 2020's primaries, then PQ's transfer") {
    // Rec. ITU-R BT.2087's matrix from Rec. 709's primaries to Rec. 2020's, row by row.
    const double kToRec2020[3][3] = {{0.6274, 0.3293, 0.0433}, {0.0691, 0.9195, 0.0114}, {0.0164, 0.0880, 0.8956}};
    const double colors[][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 1, 1}, {4, 2, 0.5}};
    for (const double* color : colors) {
        double out[3];
        prColorConvert(PrColorSpace_SrgbLinear, PrColorSpace_Rec2100Pq, color, out);
        for (uint32_t i = 0; i < 3; ++i) {
            double linear = kToRec2020[i][0] * color[0] + kToRec2020[i][1] * color[1] + kToRec2020[i][2] * color[2];
            CHECK(out[i] == doctest::Approx(prColorTransferEncodeDouble(PrColorTransfer_Pq, linear)).epsilon(1e-3));
        }
    }
    // PQ's 1 is 10,000 cd/m², which is 10000 / 203 times SDR white.
    CHECK(prColorTransferEncodeDouble(PrColorTransfer_Pq, 10000.0 / 203) == doctest::Approx(1).epsilon(1e-6));
    CHECK(prColorTransferEncodeDouble(PrColorTransfer_Pq, 0) == doctest::Approx(0).epsilon(1e-6));
}

TEST_CASE("an HDR color resolves to the geometric mean of its colors in absolute XYZ, weighted by headroom") {
    PrColorHdr hdr{
        .colors = {
            prColor16FromColor(srgbOf(PrColorSpace_Rec2100Linear, 0.9, 1.0, 0.8)),
            prColor16FromColor(srgbOf(PrColorSpace_Rec2100Linear, 1.8, 2.0, 1.5)),
        },
        .headrooms = {0, 2},
    };
    PrColor mixed = prColorHdrResolve(&hdr, 1);
    double xyz[3];
    absoluteXyz(PrColorSpace_Srgb, double(mixed.r), double(mixed.g), double(mixed.b), xyz);
    CHECK(xyz[0] == doctest::Approx(243.664).epsilon(1e-2));
    CHECK(xyz[1] == doctest::Approx(275.713).epsilon(1e-2));
    CHECK(xyz[2] == doctest::Approx(244.000).epsilon(1e-2));
    // At or past each headroom, its own color.
    PrColor low = prColorHdrResolve(&hdr, -1);
    PrColor high = prColorHdrResolve(&hdr, kPrF32Infinity);
    CHECK(low.g == prFloat16ToFloat(hdr.colors[0].g));
    CHECK(high.g == prFloat16ToFloat(hdr.colors[1].g));
}

TEST_CASE("tone mapping scales a color past the knee toward the headroom, keeping its hue") {
    // The knee for a headroom of 2 is 1.5 times SDR white, so SDR white is unchanged.
    const PrColor white{1, 1, 1, 0.5f};
    CHECK(prColorToneMap(white, 2).r == white.r);
    CHECK(prColorToneMap(white, 2).a == 0.5f);
    // Three times SDR white comes down to within the headroom, and a gray stays gray.
    float three = prColorTransferEncode(PrColorTransfer_Srgb, 3);
    PrColor mapped = prColorToneMap(PrColor{three, three, three, 1}, 2);
    float light = prColorTransferDecode(PrColorTransfer_Srgb, mapped.r);
    CHECK(light > 1.5f);
    CHECK(light < 2.0f);
    CHECK(mapped.g == mapped.r);
    // A headroom below 1 is 1, whose knee is SDR white.
    PrColor clipped = prColorToneMap(PrColor{three, three, three, 1}, 0.5f);
    CHECK(prColorTransferDecode(PrColorTransfer_Srgb, clipped.r) == doctest::Approx(1).epsilon(1e-4));
}

TEST_CASE("gamut mapping keeps a color within the gamut, and reduces the chroma of one outside it") {
    // Within the gamut, including a display's wider one, a color is unchanged.
    PrColor teal{0.2f, 0.6f, 0.5f, 0.5f};
    PrColor same = prColorGamutMap(teal, PrColorSpace_Srgb, 1);
    CHECK(same.r == teal.r);
    CHECK(same.a == teal.a);
    PrColor p3Red = srgbOf(PrColorSpace_DisplayP3, 1, 0, 0);
    CHECK(prColorGamutMap(p3Red, PrColorSpace_DisplayP3, 1).r == p3Red.r);

    // Outside it, the color lands within it, at nearly the same OKLCH lightness and hue, with less chroma.
    const PrColor outside[] = {
        p3Red, srgbOf(PrColorSpace_Rec2020, 0, 1, 0), srgbOf(PrColorSpace_Oklch, 0.7, 0.4, 250),
        srgbOf(PrColorSpace_Oklch, 0.95, 0.3, 100),
    };
    for (PrColor color : outside) {
        PrColor mapped = prColorGamutMap(color, PrColorSpace_Srgb, 1);
        CAPTURE(color.r);
        CAPTURE(color.g);
        CAPTURE(color.b);
        CHECK(mapped.r >= -1e-4f);
        CHECK(mapped.r <= 1 + 1e-4f);
        CHECK(mapped.g >= -1e-4f);
        CHECK(mapped.g <= 1 + 1e-4f);
        CHECK(mapped.b >= -1e-4f);
        CHECK(mapped.b <= 1 + 1e-4f);
        double before[3];
        double after[3];
        channelsOf(color, PrColorSpace_Oklch, before);
        channelsOf(mapped, PrColorSpace_Oklch, after);
        CHECK(after[0] == doctest::Approx(before[0]).epsilon(0.02));
        CHECK(after[1] < before[1]);
        double hue = after[2] - before[2];
        hue = hue > 180 ? hue - 360 : hue < -180 ? hue + 360 : hue;
        CHECK(std::fabs(hue) < 5);
    }

    // A P3 color maps into P3, within its own channels' range.
    PrColor rec2020Green = srgbOf(PrColorSpace_Rec2020, 0, 1, 0);
    double p3[3];
    channelsOf(prColorGamutMap(rec2020Green, PrColorSpace_DisplayP3, 1), PrColorSpace_DisplayP3, p3);
    for (double channel : p3) {
        CHECK(channel >= -1e-4);
        CHECK(channel <= 1 + 1e-4);
    }
}

TEST_CASE("gamut mapping reaches the limit's white, which a display's headroom raises") {
    // Twice SDR white is past an SDR gamut's white, and within a gamut of four times it.
    PrColor twice = srgbOf(PrColorSpace_SrgbLinear, 2, 2, 2);
    PrColor sdr = prColorGamutMap(twice, PrColorSpace_Srgb, 1);
    CHECK(sdr.r == doctest::Approx(1).epsilon(1e-4));
    CHECK(sdr.b == doctest::Approx(1).epsilon(1e-4));
    CHECK(prColorGamutMap(twice, PrColorSpace_Srgb, 4).r == twice.r);

    // A bright saturated red keeps more chroma under a higher limit, and stays within it in linear light.
    PrColor red = srgbOf(PrColorSpace_DisplayP3Linear, 3, 0, 0);
    double hdr[3];
    channelsOf(prColorGamutMap(red, PrColorSpace_Srgb, 4), PrColorSpace_SrgbLinear, hdr);
    for (double channel : hdr) {
        CHECK(channel >= -1e-4);
        CHECK(channel <= 4 + 1e-3);
    }
    double sdrChroma[3];
    double hdrChroma[3];
    channelsOf(prColorGamutMap(red, PrColorSpace_Srgb, 1), PrColorSpace_Oklch, sdrChroma);
    channelsOf(prColorGamutMap(red, PrColorSpace_Srgb, 4), PrColorSpace_Oklch, hdrChroma);
    CHECK(hdrChroma[1] > sdrChroma[1]);

    // Black and below map to black.
    PrColor below = prColorGamutMap(PrColor{-0.5f, -0.5f, -0.5f, 1}, PrColorSpace_Srgb, 1);
    CHECK(below.r == 0.0f);
    CHECK(below.g == 0.0f);
}
