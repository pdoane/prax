// The PSD codec: lib/psd's decoder (pr/psd.h) as a member of an image codec set.

#pragma once

#include "pr/base.h"
#include "pr/image.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates a codec that decodes the flattened composite of Photoshop documents (.psd, .psb), of the type
// image/vnd.adobe.photoshop, and encodes nothing. A document decodes to gray, RGB, CMYK ink or Lab, of 8 bits, 16 bits
// or floats, with straight alpha if the document has transparency. Its color is its ICC profile, or the named space
// that the profile is. A document has no layers here. The JPEG thumbnail that a document embeds is its preview, which
// opens through the set that opens the document, so a set without a JPEG codec has no previews. The codec has no state
// of its own, so one codec serves every thread.
PrImageCodec* prImagePsdCreate(void);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
