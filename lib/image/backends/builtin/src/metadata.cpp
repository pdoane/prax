#include "image/backends/builtin/src/metadata.h"

#include "pr/mem.h"

namespace pr_impl {
namespace {

constexpr float kMetersPerInch = 0.0254f;

// TIFF 6.0: the tags that this module reads from the first image file directory.
constexpr uint32_t kTagOrientation = 0x0112;
constexpr uint32_t kTagXResolution = 0x011A;
constexpr uint32_t kTagYResolution = 0x011B;
constexpr uint32_t kTagResolutionUnit = 0x0128;

struct TiffReader {
    const uint8_t* bytes;
    size_t size;
    bool bigEndian;

    uint32_t u16At(size_t at) const {
        return bigEndian ? readBeU16(bytes + at) : readLeU16(bytes + at);
    }

    uint32_t u32At(size_t at) const {
        return bigEndian ? readBeU32(bytes + at) : readLeU32(bytes + at);
    }
};

// Returns the value of a rational field, or zero if its offset runs past the header or its denominator is zero.
float rationalAt(const TiffReader& tiff, size_t at) {
    if (at + 8 > tiff.size) {
        return 0;
    }
    const uint32_t denominator = tiff.u32At(at + 4);
    return denominator == 0 ? 0 : float(double(tiff.u32At(at)) / double(denominator));
}

} // namespace

float imagePixelsPerMeterFromInch(float perInch) {
    return perInch / kMetersPerInch;
}

float imagePixelsPerMeterFromCm(float perCm) {
    return perCm * 100;
}

void imageReadExif(const uint8_t* tiff, size_t size, ImageMetadata* out) {
    if (size < 8) {
        return;
    }
    const bool bigEndian = tiff[0] == 'M' && tiff[1] == 'M';
    if (!bigEndian && (tiff[0] != 'I' || tiff[1] != 'I')) {
        return;
    }
    const TiffReader reader = {tiff, size, bigEndian};
    if (reader.u16At(2) != 42) {
        return;
    }
    const uint32_t directory = reader.u32At(4);
    if (size_t(directory) + 2 > size) {
        return;
    }
    const uint32_t count = reader.u16At(directory);
    if (size_t(directory) + 2 + size_t(count) * 12 > size) {
        return;
    }
    // The resolution uses two fields, and the unit may come after the value in the directory. Both are therefore
    // collected before either is converted.
    float resolutionX = 0;
    float resolutionY = 0;
    uint32_t unit = 2;
    for (uint32_t index = 0; index < count; ++index) {
        const size_t entry = size_t(directory) + 2 + size_t(index) * 12;
        const uint32_t tag = reader.u16At(entry);
        const size_t value = entry + 8;
        if (tag == kTagOrientation) {
            const uint32_t orientation = reader.u16At(value);
            if (orientation >= 1 && orientation <= 8) {
                out->orientation = PrImageOrientation(orientation - 1);
            }
        } else if (tag == kTagXResolution) {
            resolutionX = rationalAt(reader, reader.u32At(value));
        } else if (tag == kTagYResolution) {
            resolutionY = rationalAt(reader, reader.u32At(value));
        } else if (tag == kTagResolutionUnit) {
            unit = reader.u16At(value);
        }
    }
    if (unit == 2 || unit == 3) {
        if (resolutionX > 0) {
            if (unit == 3)
                out->pixelsPerMeterX = imagePixelsPerMeterFromCm(resolutionX);
            else
                out->pixelsPerMeterX = imagePixelsPerMeterFromInch(resolutionX);
        }
        if (resolutionY > 0) {
            if (unit == 3)
                out->pixelsPerMeterY = imagePixelsPerMeterFromCm(resolutionY);
            else
                out->pixelsPerMeterY = imagePixelsPerMeterFromInch(resolutionY);
        }
    }
}

} // namespace pr_impl
