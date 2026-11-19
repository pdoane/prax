// Colors: four-channel values, and the color spaces, transfer functions and models they are in.

#pragma once

#include "pr/base.h"
#include "pr/float16.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

#if defined(__cplusplus)
struct PrColor16;
#endif

// A color of four channels. It carries no color space and no alpha mode: the code that holds it defines both. Channels
// are not clamped.
PR_NO_PADDING_BEGIN
typedef struct PrColor {
    float r;
    float g;
    float b;
    float a;

    PR_EQUALITY(PrColor)
#if defined(__cplusplus)
    // Narrows the color as prColor16FromColor does, so that a color passes where a PrColor16 is expected.
    PR_CONSTEXPR operator PrColor16() const;
#endif
} PrColor;
PR_NO_PADDING_END

// A color of four 8-bit channels. It carries no color space and no alpha mode.
typedef struct PrColor8 {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a;
} PrColor8;

// Returns `color` with each channel widened from [0, 255] to [0, 1].
PrColor prColorFromColor8(PrColor8 color);

// Returns `color` with each channel clamped to [0, 1] and narrowed to the nearest of [0, 255]. NaN narrows to 0.
PrColor8 prColor8FromColor(PrColor color);

// A color of four 16-bit float channels. It carries no color space and no alpha mode. A channel never holds -0 or NaN,
// so equal colors have equal bits.
PR_NO_PADDING_BEGIN
typedef struct PrColor16 {
    PrFloat16 r;
    PrFloat16 g;
    PrFloat16 b;
    PrFloat16 a;

#if defined(__cplusplus)
    // Widens the color exactly, as prColorFromColor16 does.
    PR_CONSTEXPR operator PrColor() const;

    // Compares the channels as floats. Written out, since a defaulted == does not compare a native 16-bit float.
    constexpr bool operator==(const PrColor16& other) const {
        return r == other.r && g == other.g && b == other.b && a == other.a;
    }
#endif
} PrColor16;
PR_NO_PADDING_END

// Convert between colors of floats and colors of 16-bit floats. Narrowing rounds each channel to the nearest 16-bit
// float, and maps -0 and NaN to 0. Widening is exact.
PR_CONSTEXPR PrFloat16 prColor16Channel(float value) {
    return prFloat16FromFloat(value != value ? 0.0f : value + 0.0f);
}
PR_CONSTEXPR PrColor16 prColor16FromColor(PrColor color) {
    PrColor16 narrow = {
        prColor16Channel(color.r), prColor16Channel(color.g), prColor16Channel(color.b),
        prColor16Channel(color.a),
    };
    return narrow;
}
PR_CONSTEXPR PrColor prColorFromColor16(PrColor16 color) {
    PrColor wide = {
        prFloat16ToFloat(color.r), prFloat16ToFloat(color.g), prFloat16ToFloat(color.b),
        prFloat16ToFloat(color.a),
    };
    return wide;
}

// Returns the sRGB color that `hex` writes as 0xRRGGBB, opaque, as CSS writes #RRGGBB.
PR_CONSTEXPR PrColor prColorFromHex(uint32_t hex) {
    PrColor color = {
        (float)((hex >> 16) & 0xff) / 255.0f, (float)((hex >> 8) & 0xff) / 255.0f, (float)(hex & 0xff) / 255.0f, 1.0f,
    };
    return color;
}

// Returns the sRGB color that `hex` writes as 0xRRGGBBAA, with straight alpha, as CSS writes #RRGGBBAA.
PR_CONSTEXPR PrColor prColorFromHexAlpha(uint32_t hex) {
    PrColor color = {
        (float)(hex >> 24) / 255.0f, (float)((hex >> 16) & 0xff) / 255.0f, (float)((hex >> 8) & 0xff) / 255.0f,
        (float)(hex & 0xff) / 255.0f,
    };
    return color;
}

// Returns `color` with its alpha replaced by `alpha`.
PR_CONSTEXPR PrColor prColorWithAlpha(PrColor color, float alpha) {
    PrColor result = {color.r, color.g, color.b, alpha};
    return result;
}

#if defined(__cplusplus)
// The conversions that PrColor and PrColor16 declare.
PR_CONSTEXPR PrColor::operator PrColor16() const {
    return prColor16FromColor(*this);
}
PR_CONSTEXPR PrColor16::operator PrColor() const {
    return prColorFromColor16(*this);
}
#endif

// Builds a color of 16-bit floats from literal channels. If 16-bit floats are native, the color is built at compile
// time.
#if PR_FLOAT16_NATIVE
#define PR_COLOR16(r, g, b, a)                                                                                    \
    PR_LITERAL(PrColor16) {                                                                                       \
        PR_VALUE_CAST(PrFloat16, (r) + 0.0f), PR_VALUE_CAST(PrFloat16, (g) + 0.0f),                             \
            PR_VALUE_CAST(PrFloat16, (b) + 0.0f), PR_VALUE_CAST(PrFloat16, (a) + 0.0f)                          \
    }
#else
#define PR_COLOR16(r, g, b, a) prColor16FromColor(PR_LITERAL(PrColor){(r), (g), (b), (a)})
#endif

// The weights of red, green and blue in a color's luminance.
PR_CONSTANT float kPrColorLuminanceRed = 0.2125f;
PR_CONSTANT float kPrColorLuminanceGreen = 0.7154f;
PR_CONSTANT float kPrColorLuminanceBlue = 0.0721f;

// The channels of a color space, in order.
enum {
    PrColorModel_Gray,   // one intensity channel
    PrColorModel_Rgb,
    PrColorModel_Xyz,
    PrColorModel_Lab,    // lightness and two opponent axes
    PrColorModel_Lch,    // lightness, chroma and hue
    PrColorModel_Hsl,    // hue, saturation and lightness
    PrColorModel_Hwb,    // hue, whiteness and blackness
    PrColorModel_Cmyk,   // cyan, magenta, yellow and black ink
    PrColorModel_Count,
};
typedef uint8_t PrColorModel;

// Color spaces of three channels. In-gamut RGB channels are in [0, 1], and XYZ channels are tristimulus values.
// Lightness is in [0, 100] for Lab and Lch, and in [0, 1] for Oklab and Oklch. Saturation, lightness, whiteness and
// blackness are in [0, 100] for HSL and HWB. Hue is in degrees in every cylindrical space.
//
// The spaces from Rec2100Pq on hold HDR colors, and place SDR white at HDR Reference White, 203 cd/m² (Rec. ITU-R
// BT.2408). The Rec2100 spaces have Rec. ITU-R BT.2100's primaries. Rec2100Linear holds them in linear light with white
// at 1, Rec2100Pq encodes them with PQ, where 1 is 10,000 cd/m², and Rec2100Hlg with HLG, scaled so that white is 0.75.
// Ictcp (BT.2100) and Jzazbz and its polar form Jzczhz (Safdar et al., 2017) are perceptual spaces of absolute
// luminance.
enum {
    PrColorSpace_Srgb,
    PrColorSpace_SrgbLinear,
    PrColorSpace_DisplayP3,
    PrColorSpace_DisplayP3Linear,
    PrColorSpace_A98Rgb,
    PrColorSpace_ProPhotoRgb,
    PrColorSpace_Rec2020,
    PrColorSpace_XyzD50,
    PrColorSpace_XyzD65,
    PrColorSpace_Lab,
    PrColorSpace_Lch,
    PrColorSpace_Oklab,
    PrColorSpace_Oklch,
    PrColorSpace_Hsl,
    PrColorSpace_Hwb,
    PrColorSpace_Rec2100Pq,
    PrColorSpace_Rec2100Hlg,
    PrColorSpace_Rec2100Linear,
    PrColorSpace_Ictcp,
    PrColorSpace_Jzazbz,
    PrColorSpace_Jzczhz,
    PrColorSpace_Count,
};
typedef uint8_t PrColorSpace;

// How an RGB space encodes linear light in its channels. Linear light is relative to SDR white, so that a decoded
// channel of 1 is SDR white in every space. Each function extends past [0, 1], and mirrors below zero except PQ, which
// clamps there to black. A space that is not RGB counts as linear.
enum {
    PrColorTransfer_Linear,
    PrColorTransfer_Srgb,           // IEC 61966-2-1, which Display P3 shares
    PrColorTransfer_A98Rgb,         // a power of 563/256
    PrColorTransfer_ProPhotoRgb,    // ROMM RGB: a power of 1.8 above a linear segment
    PrColorTransfer_Rec2020,        // Rec. ITU-R BT.2020
    PrColorTransfer_Pq,             // SMPTE ST 2084, where 1 is 10,000 cd/m² and SDR white is 203 cd/m²
    PrColorTransfer_Hlg,            // ARIB STD-B67, scaled so that SDR white is 0.75
    PrColorTransfer_Count,
};
typedef uint8_t PrColorTransfer;

// Which way hues interpolate in a polar space: the shorter or the longer arc between the two hues, or the arc on which
// the hue increases or decreases.
enum {
    PrColorHueMethod_Shorter,
    PrColorHueMethod_Longer,
    PrColorHueMethod_Increasing,
    PrColorHueMethod_Decreasing,
};
typedef uint8_t PrColorHueMethod;

// An RGB space of linear light by its primaries and white: the CIE 1931 xy chromaticities of (1, 0, 0), (0, 1, 0), (0,
// 0, 1) and (1, 1, 1). A white of luminance 1 is SDR white.
typedef struct PrColorChromaticities {
    PrVec2 red;
    PrVec2 green;
    PrVec2 blue;
    PrVec2 white;
} PrColorChromaticities;

// Standard primaries and whites. Rec709 is sRGB's, and Rec2020 is that of Rec. ITU-R BT.2020 and BT.2100, each with the
// D65 white. DisplayP3 is DCI-P3's primaries with the D65 white. A98Rgb is Adobe RGB (1998)'s, and ProPhotoRgb is ROMM
// RGB's with the D50 white. AcesAp0 and AcesAp1 are the ACES primaries of SMPTE ST 2065-1 and of ACEScg (Academy
// S-2014-004), with the ACES white. Xyz is the space of CIE XYZ tristimulus values, whose white is equal energy.
enum {
    PrColorPrimaries_Rec709,
    PrColorPrimaries_DisplayP3,
    PrColorPrimaries_A98Rgb,
    PrColorPrimaries_ProPhotoRgb,
    PrColorPrimaries_Rec2020,
    PrColorPrimaries_AcesAp0,
    PrColorPrimaries_AcesAp1,
    PrColorPrimaries_Xyz,
    PrColorPrimaries_Count,
};
typedef uint8_t PrColorPrimaries;

// A color that depends on the display: two extended sRGB colors, each for a display of a headroom, and colors between
// them for the headrooms between. A headroom is in stops: the base-2 logarithm of the display's brightest color as a
// multiple of SDR white, so 0 is SDR.
typedef struct PrColorHdr {
    PrColor16 colors[2];                // straight alpha
    float headrooms[2];
} PrColorHdr;

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
