// Color spaces: their channels and transfer functions, conversion between them, HDR colors resolved for a display, and
// gamut mapping.

#pragma once

#include "pr/base.h"
#include "pr/color.h"
#include "pr/string.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Returns the color model of `space`, or PrColorModel_Count if `space` is not a color space.
PrColorModel prColorSpaceModel(PrColorSpace space);

// Returns the index of the channel that holds the hue in `space`, or 3 if the space has no hue.
uint32_t prColorSpaceHueChannel(PrColorSpace space);

// Returns the transfer function of `space`, which must be a color space.
PrColorTransfer prColorSpaceTransfer(PrColorSpace space);

// Converts a channel encoded by `transfer` to linear light, or linear light to its encoding. The float forms run sRGB's
// in floats, and the double forms run every function in doubles.
float prColorTransferDecode(PrColorTransfer transfer, float encoded);
float prColorTransferEncode(PrColorTransfer transfer, float linear);
double prColorTransferDecodeDouble(PrColorTransfer transfer, double encoded);
double prColorTransferEncodeDouble(PrColorTransfer transfer, double linear);

// Returns the name of `space`, or the null view if `space` is not a color space.
PrStringView prColorSpaceName(PrColorSpace space);

// Returns `degrees` normalized to [0, 360). Returns NaN if `degrees` is infinite.
double prColorHue(double degrees);

// Returns the sRGB color of an HSL color, with `hue` in degrees and `saturation` and `lightness` in [0, 100]. Values
// outside that range give channels outside [0, 1].
PrColor prColorFromHsl(double hue, double saturation, double lightness, double alpha);

// Returns the sRGB color of an HWB color, with `hue` in degrees and `whiteness` and `blackness` in [0, 100]. If
// `whiteness` and `blackness` sum to 100 or more, they are scaled to sum to 100, and the result is a gray.
PrColor prColorFromHwb(double hue, double whiteness, double blackness, double alpha);

// Converts the three channels at `in` from space `from` to space `to`, and writes them to `out`. Channels are in the
// units that PrColorSpace gives. Nothing is clamped, so a color outside the gamut of `to` keeps channels outside its
// range. An RGB transfer function extends to negative values by mirroring. XYZ D50, Lab, LCH and ProPhoto are relative
// to D50, and the other spaces to D65, with Bradford adaptation between the two. A hue is 0 if the chroma leaves it
// undefined. `in` and `out` may be the same.
void prColorConvert(PrColorSpace from, PrColorSpace to, const double in[PR_NONNULL 3], double out[PR_NONNULL 3]);

// Returns the color for a display of `headroom` stops: the first color at or below the first color's headroom, the
// second at or above the second's, and between them the two colors' weighted geometric mean in absolute XYZ, with SDR
// white at 203 cd/m² (Rec. ITU-R BT.2408). Alpha mixes by the same weights. Equal headrooms give the first color. An
// infinite headroom gives the color of the larger headroom.
PrColor prColorHdrResolve(const PrColorHdr* color, float headroom);

// Returns the extended sRGB color scaled down in linear light so that no channel exceeds `limit` times SDR white, which
// keeps its hue. A color within the limit, and a limit that is not above zero, leave it unchanged. Alpha is unchanged.
PrColor prColorLimit(PrColor color, float limit);

// Returns the extended sRGB color tone mapped for a display of `headroom`. A color whose luminance in linear light
// exceeds the knee, halfway from SDR white to the headroom, is scaled toward the headroom without reaching it, which
// keeps its hue. A color below the knee is unchanged. A headroom below 1 is 1. Alpha is unchanged.
PrColor prColorToneMap(PrColor color, float headroom);

// Returns the extended sRGB color mapped into the gamut of the RGB space `gamut`, whose white is `limit` times SDR
// white: a color whose channels in `gamut` lie in linear light within [0, `limit`]. A color within the gamut is
// unchanged. Any other keeps its OKLCH lightness and hue and loses chroma: a binary search on chroma finds the color
// whose clip to the gamut lies within a just noticeable difference of it, 0.02 in OKLab, and the result is that clip. A
// color at least as light as the gamut's white maps to that white, and one no lighter than black to black. A limit
// below 1 is 1. The rec2100 spaces name Rec. 2020's primaries. Alpha is unchanged.
PrColor prColorGamutMap(PrColor color, PrColorSpace gamut, float limit);

// Returns the most OKLCH chroma that the RGB space `gamut` holds at `lightness`, in [0, 1], and `hue` in degrees: the
// chroma of the gamut's surface there. A gamut whose white is `limit` times SDR white holds the chroma that this
// returns for lightness / cbrt(limit), times cbrt(limit), since OKLab scales by the cube root of linear light.
float prColorGamutMaxChroma(PrColorSpace gamut, float lightness, float hue);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
