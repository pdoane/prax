// Exif metadata: the camera settings that an image's Exif data records (PrImageInfo::exif).

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The camera settings of a photograph (prImageExifCapture). A text that the Exif data does not give is empty, and a
// number is zero.
typedef struct PrImageCapture {
    char make[64];          // the camera's maker
    char model[64];         // the camera's model
    char lens[64];          // the lens's model
    char taken[20];         // when it was taken, as Exif writes it: "YYYY:MM:DD HH:MM:SS"
    uint32_t exposure[2];   // the exposure time in seconds, as a numerator and a denominator
    float fNumber;          // the aperture's f-number
    float focalLength;      // in millimeters
    uint32_t iso;           // the sensitivity, as an ISO speed
} PrImageCapture;

// Reads the camera settings from the Exif data `exif` into `*out`. Returns true if the data gives any of them.
//
// Each text is printable UTF-8, cut to the size of its field. The call checks every offset in the data. Malformed data
// yields the fields that can be read, and the call never reads outside `exif`.
bool prImageExifCapture(PrByteView exif, PrImageCapture* out);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
