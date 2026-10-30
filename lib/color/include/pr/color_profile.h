// Color profiles: color spaces that ICC profiles define.

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/color.h"
#include "pr/status.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// How colors map between a profile's space and its connection space, as an ICC profile's rendering intents name them.
// Relative colorimetric keeps colors in gamut as they are, relative to the media's white. Absolute colorimetric keeps
// them relative to the connection space's white instead. Perceptual and saturation are the profile's own mappings.
enum {
    PrColorIntent_Perceptual,
    PrColorIntent_RelativeColorimetric,
    PrColorIntent_Saturation,
    PrColorIntent_AbsoluteColorimetric,
};
typedef uint8_t PrColorIntent;

// The most channels that a profile's color has.
enum { kPrColorProfileMaxChannels = 8 };

// A color space that an ICC profile defines: how the colors of a device, in one to eight channels, map to CIE XYZ under
// D50, which is the profile connection space. A profile is immutable once created, and its operations are thread-safe.
typedef struct PrColorProfile PrColorProfile;

// Creates a profile from the bytes of an ICC profile, which the profile copies. The profile describes gray, RGB, CMYK
// or another device space of up to eight channels, connects through XYZ or Lab, and maps device colors to the
// connection space with a matrix and curves or with a lookup table. Returns InvalidData for bytes that are no such
// profile, and sets `out` to null.
PrStatus prColorProfileCreate(PrByteView bytes, PrColorProfile* PR_NULLABLE* PR_NONNULL out);

// Frees the profile.
void prColorProfileDestroy(PrColorProfile* PR_NULLABLE profile);

// The model of the profile's device colors. PrColorModel_Count stands for a device space of another number of channels,
// which prColorProfileChannels gives.
PrColorModel prColorProfileModel(const PrColorProfile* profile);

// The number of channels of the profile's device colors.
uint32_t prColorProfileChannels(const PrColorProfile* profile);

// Converts a device color, one value in [0, 1] for each of the profile's channels, to CIE XYZ under D50 by `intent`,
// and writes it to `out`. A channel outside [0, 1] is clamped. A profile without a mapping for `intent` uses its
// default mapping.
void prColorProfileToXyzD50(const PrColorProfile* profile, PrColorIntent intent, const double* in, double out[PR_NONNULL 3]);

// Returns the named space that an RGB or gray profile describes, or PrColorSpace_Count if it describes none of the
// named RGB spaces within a fraction of an 8-bit step, as an RGB profile of another space does. A color that converts
// through a matching profile converts as its space does.
PrColorSpace prColorProfileNamedSpace(const PrColorProfile* profile);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
